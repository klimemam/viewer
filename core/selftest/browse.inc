// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // The Browse panel's own behaviour, verifiable without a human. Connects a
    // LOCAL peer to <dir> and drives the same functions the panel draws with.
    if (!g_browseSelftest.empty()) {
        std::string dir = g_browseSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        startRemote(rbMain(), "local://" + dir);
        double t0 = nowSec();
        while (nowSec() - t0 < 120.0) {
            pumpRemoteBrowse();
            if (rbMain().b.connected && !rbMain().b.entries.empty()) break;
            if (!rbMain().b.err.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const App::RemoteBrowse& B = rbMain().b;
        if (!B.connected || B.entries.empty()) {
            fprintf(stderr, "browseselftest: no listing for %s (%s)\n",
                    dir.c_str(), B.err.c_str());
            stopRbWorker(); stopRemoteFetcher(); stopMeasureWorker();
            return 1;
        }
        bool ok = true;
        {   // Grouped <-> flat is a CLIENT-SIDE view over one reply: the flat
            // listing must be exactly the group's members, in the peer's order,
            // and the grouped one exactly one row for them.
            const remote::Entry* g = nullptr;
            int nGroups = 0, nMembers = 0;
            for (const auto& e : B.entries) {
                if (!e.group) continue;
                nGroups++;
                nMembers += (int)e.members.size();
                if (!g) g = &e;
            }
            std::vector<RbRow> gv = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            std::vector<RbRow> fv = rbBuildView(rbMain(), &B.dir, B.entries, true, false);
            // ...plus the one ".." row the listing always carries (row 0,
            // under every sort - it is navigation, not a listed entry)
            int gUp = 0, fUp = 0;
            for (const auto& r : gv) if (r.up) gUp++;
            for (const auto& r : fv) if (r.up) fUp++;
            bool sizes = gUp == 1 && fUp == 1 &&
                         gv.size() == B.entries.size() + 1 &&
                         fv.size() == B.entries.size() + 1 - nGroups + nMembers;
            // one grouped row for the sequence, and it is the group entry
            int gRows = 0;
            for (const auto& r : gv) if (r.isGroup()) gRows++;
            // the expanded rows, in order, ARE the members
            std::vector<std::string> expanded;
            bool memberCells = true;
            for (const auto& r : fv) {
                if (r.e != g || r.member < 0) continue;
                expanded.push_back(r.name());
                // an expanded frame has no size / mtime of its own, but keeps
                // the shape/dtype the group shares by construction
                if (r.ownFile() || r.isGroup() || r.isDir()) memberCells = false;
                if (r.e->hasMeta != g->hasMeta || r.e->dtype != g->dtype) memberCells = false;
            }
            bool same = g && expanded == g->members;
            bool nogroups = true;
            for (const auto& r : fv) if (r.isGroup()) nogroups = false;
            fprintf(stderr, "browseselftest: view %s: grouped %d row(s) [%d stack row(s), "
                            "'%s' x%d], flat %d row(s), members match=%d, no group rows "
                            "when flat=%d, member size/mtime blank=%d: %s\n",
                    dir.c_str(), (int)gv.size(), gRows, g ? g->name.c_str() : "?",
                    g ? (int)g->members.size() : 0, (int)fv.size(), same ? 1 : 0,
                    nogroups ? 1 : 0, memberCells ? 1 : 0,
                    (sizes && gRows == nGroups && nGroups >= 1 && same && nogroups &&
                     memberCells) ? "ok" : "FAIL");
            if (!(sizes && gRows == nGroups && nGroups >= 1 && same && nogroups && memberCells))
                ok = false;
        }
        {   // The deferred-action invariant. Every row the listing draws is a
            // pair of raw pointers into B.entries; the Places combo used to
            // replace the whole browse state from inside the draw, and the
            // table below it then read nine destroyed entries (SIGSEGV in
            // strlen). Nothing that replaces that state may run while a row
            // list is alive - it is queued and runs when the rows are gone.
            int ranAt = -1, marker = 0;
            bool queued = false, aliveOk = false;
            size_t pend0 = rbDeferredPending();
            {
                RbDeferredActions guard(rbMain());
                std::vector<RbRow> rows = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
                rbDefer([&] { ranAt = marker; });     // what picking a place queues
                marker = 1;
                queued = rbDeferredPending() == pend0 + 1 && ranAt == -1;
                // ...and the rows are still readable, which is the whole point
                // rows[0] is "..", whose entry is a static, not one of
                // B.entries: the row that must survive is the first LISTED one
                const RbRow* first = nullptr;
                for (const auto& r : rows) if (!r.up) { first = &r; break; }
                aliveOk = first && first->e == &B.entries[0] &&
                          first->e->name == B.entries[0].name;
                marker = 2;                            // the row list dies here
            }
            bool defOk = queued && aliveOk && ranAt == 2 && rbDeferredPending() == 0;
            fprintf(stderr, "browseselftest: deferred actions: queued while the rows "
                            "were alive=%d, rows valid throughout=%d, ran only after "
                            "they died=%d, queue drained=%d: %s\n",
                    queued ? 1 : 0, aliveOk ? 1 : 0, ranAt == 2 ? 1 : 0,
                    rbDeferredPending() == 0 ? 1 : 0, defOk ? "ok" : "FAIL");
            if (!defOk) ok = false;
        }
        {   // Tree mode, lazily: expanding a node issues exactly ONE LIST, on
            // the worker; the children appear under the folder at depth+1;
            // collapsing keeps the cache, so re-expanding issues nothing.
            std::string sub;
            for (const auto& e : B.entries)
                if (e.dir && (sub.empty() || e.name == "scanroot")) sub = e.name;
            if (sub.empty()) {
                fprintf(stderr, "browseselftest: tree: no subfolder in %s to expand\n",
                        dir.c_str());
                ok = false;
            } else {
                std::string subPath = dir + "/" + sub;
                int before = rbMain().treeLists;
                // nothing is listed until it is asked for
                bool lazy0 = rbMain().treeCache.empty();
                rbTreeExpand(rbMain(), subPath);
                double t1 = nowSec();
                while (nowSec() - t1 < 120.0) {
                    pumpRemoteBrowse();
                    if (rbMain().treeCache.count(subPath)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                int afterExpand = rbMain().treeLists;
                std::vector<RbRow> tv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
                // the children sit directly under their folder, one level in
                int at = -1;
                for (int i = 0; i < (int)tv.size(); i++)
                    if (tv[i].isDir() && tv[i].name() == sub) { at = i; break; }
                size_t nKids = rbMain().treeCache.count(subPath)
                             ? rbMain().treeCache[subPath].size() : 0;
                int kidsUnder = 0;
                bool contiguous = at >= 0;
                for (int i = at + 1; i < (int)tv.size() && tv[i].depth > tv[at].depth; i++)
                    kidsUnder++;
                if (at < 0 || (size_t)kidsUnder != nKids) contiguous = false;
                // ...and they know which directory they came from
                bool paths = at >= 0;
                for (int i = at + 1; i <= at + kidsUnder && i < (int)tv.size(); i++)
                    if (tv[i].full() != subPath + "/" + tv[i].name()) paths = false;
                // collapse: rows go, the cache stays
                rbTreeCollapse(rbMain(), subPath);
                std::vector<RbRow> cv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
                // ...back to the listed entries, plus the ".." row every shape
                // carries (it is navigation, not a listed entry)
                size_t cvListed = 0;
                for (const auto& r : cv) if (!r.up) cvListed++;
                bool collapsed = cvListed == B.entries.size() &&
                                 rbMain().treeCache.count(subPath) == 1;
                // re-expand: no second round trip
                rbTreeExpand(rbMain(), subPath);
                int afterAgain = rbMain().treeLists;
                std::vector<RbRow> rv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
                bool freeAgain = afterAgain == afterExpand && rv.size() == tv.size();
                // and a grandchild: the recursion must indent, not flatten
                int deep = 0, atDeep = -1;
                if (at >= 0)
                    for (int i = at + 1; i <= at + kidsUnder && i < (int)tv.size(); i++)
                        if (tv[i].isDir()) { atDeep = i; break; }
                if (atDeep >= 0) {
                    rbTreeExpand(rbMain(), rv[atDeep].full());
                    double t2 = nowSec();
                    while (nowSec() - t2 < 120.0) {
                        pumpRemoteBrowse();
                        if (rbMain().treeCache.count(rv[atDeep].full())) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    std::vector<RbRow> dv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
                    for (const auto& q : dv) if (q.depth == 2) deep++;
                }
                bool treeOk = lazy0 && afterExpand == before + 1 && nKids > 0 &&
                              contiguous && paths && collapsed && freeAgain &&
                              (atDeep < 0 || deep > 0);
                fprintf(stderr, "browseselftest: tree lazy: nothing cached before=%d, "
                                "expand %s -> %d LIST(s) (%d children at depth %d, paths "
                                "ok=%d), collapse -> %d row(s) with the cache kept=%d, "
                                "re-expand -> %d extra LIST(s), grandchild rows at "
                                "depth 2 = %d: %s\n",
                        lazy0 ? 1 : 0, sub.c_str(), afterExpand - before, kidsUnder,
                        at >= 0 && at + 1 < (int)tv.size() ? tv[at + 1].depth : -1,
                        paths ? 1 : 0, (int)cv.size(), collapsed ? 1 : 0,
                        afterAgain - afterExpand, deep, treeOk ? "ok" : "FAIL");
                if (!treeOk) ok = false;
                rbTreeForget(rbMain());
            }
        }
        {   // ".." belongs to the LISTING only. In a tree the parent is already
            // on screen, so a row pointing at it is a duplicate - and the user
            // said so explicitly. It is row 0 whatever the sort, and a filter
            // narrows the listing without ever hiding the way out: the version
            // that appeared only outside the home directory is what shifted
            // every row by one on the way in and out.
            auto rows = [&](bool flat, bool tree) {
                return rbBuildView(rbMain(), &B.dir, B.entries, flat, tree);
            };
            std::vector<RbRow> lv = rows(false, false), tv = rows(false, true);
            std::vector<RbRow> fv2 = rows(true, false);
            bool listHas = !lv.empty() && lv[0].up && lv[0].name() == "..";
            bool flatHas = !fv2.empty() && fv2[0].up;
            int treeUps = 0;
            for (const auto& r : tv) if (r.up) treeUps++;
            bool treeHas = !tv.empty() && tv[0].up && treeUps == 1;
            // ...and it is a directory row, so the arrow keys step onto it
            // without previewing it and Enter leaves rather than opening
            bool asDir = listHas && lv[0].isDir() && !lv[0].isGroup();
            int ups = 0;
            for (const auto& r : lv) if (r.up) ups++;
            bool upOk = listHas && flatHas && treeHas && asDir && ups == 1;
            fprintf(stderr, "browseselftest: \"..\": listing row0=%d flat row0=%d "
                            "tree row0=%d (x%d) draws as a folder=%d exactly one=%d: %s\n",
                    listHas ? 1 : 0, flatHas ? 1 : 0, treeHas ? 1 : 0, treeUps,
                    asDir ? 1 : 0, ups, upOk ? "ok" : "FAIL");
            if (!upOk) ok = false;
        }
        {   // ---- the ancestors of the row at the top of the viewport --------
            // The arithmetic half of the pinned band. It belongs here rather
            // than in --browse-keys-selftest because it needs no frame at all:
            // rbAncestorRows reads the flattened row run, and the run is what
            // rbBuildView hands the panel. --browse-keys-selftest owns the
            // other half - that the rows it names really are laid out at the
            // top of the panel and really are clickable.
            //
            // The claim is not "it returns some earlier rows". It is that for
            // every row in a tree of any depth, what comes back is EXACTLY that
            // row's chain of parents: one per level, outermost first, each one
            // a path prefix of the row - checked against full(), which is built
            // by the tree walker and not by this function. A depth-0 row has
            // none, and ".." is never one, at any position.
            //
            // rb/expset/zdeep is what makes this worth asserting: five levels,
            // so a walk that stopped one level short, or one that took a
            // sibling instead of a parent, is visible here and would not be in
            // a tree two deep.
            std::string zroot = dir + "/expset";
            rbTreeExpand(rbMain(), zroot);
            {
                double t0z = nowSec();
                while (nowSec() - t0z < 120.0) {
                    pumpRemoteBrowse();
                    if (rbMain().treeCache.count(zroot)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            zroot += "/zdeep";
            const char* chain[] = { "", "/a", "/a/b", "/a/b/c" };
            for (const char* c : chain) {
                rbTreeExpand(rbMain(), zroot + c);
                double t1 = nowSec();
                while (nowSec() - t1 < 120.0) {
                    pumpRemoteBrowse();
                    if (rbMain().treeCache.count(zroot + c)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            std::vector<RbRow> tv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
            std::vector<int> shown((size_t)tv.size());
            for (int i = 0; i < (int)tv.size(); i++) shown[(size_t)i] = i;   // a tree is not re-sorted
            int maxDepth = 0, checked = 0, deepRows = 0;   // depth 4 = a shot under c
            bool chainOk = true, upNever = true, rootNone = true;
            std::vector<int> anc;
            for (int f = 0; f < (int)shown.size(); f++) {
                const RbRow& r = tv[(size_t)shown[(size_t)f]];
                maxDepth = std::max(maxDepth, r.depth);
                if (r.depth >= 4) deepRows++;
                rbAncestorRows(tv, shown, f, anc);
                for (int p : anc) if (tv[(size_t)shown[(size_t)p]].up) upNever = false;
                if (r.depth == 0 && !anc.empty()) rootNone = false;
                if (r.up || r.ph) continue;
                checked++;
                // one per level, outermost first, and each really a parent
                if ((int)anc.size() != r.depth) { chainOk = false; continue; }
                for (int k = 0; k < (int)anc.size(); k++) {
                    const RbRow& a2 = tv[(size_t)shown[(size_t)anc[(size_t)k]]];
                    std::string pre = a2.full() + "/";
                    if (a2.depth != k || anc[(size_t)k] >= f ||
                        r.full().compare(0, pre.size(), pre) != 0) chainOk = false;
                }
            }
            bool ancOk = chainOk && upNever && rootNone && maxDepth >= 4 && deepRows > 0;
            fprintf(stderr, "browseselftest: pinned ancestors: %d row(s) over a tree %d "
                            "level(s) deep (%d of them at depth 4+), every chain is the "
                            "row's own parents=%d, \"..\" never pinned=%d, top level pins "
                            "nothing=%d: %s\n",
                    checked, maxDepth, deepRows, chainOk ? 1 : 0, upNever ? 1 : 0,
                    rootNone ? 1 : 0, ancOk ? "ok" : "FAIL");
            if (!ancOk) ok = false;
            rbTreeForget(rbMain());
        }
        {   // "Open folder" on the folder being browsed: the same call the
            // toolbar button and the breadcrumb menu make. The scan must
            // include the CURRENT directory itself (a stack whose name has no
            // folder prefix), which is exactly what the old row-only entry
            // could not reach without first going up a level.
            remoteScanFolder(rbMain(), B.dir);
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                pumpRemoteBrowse();
                if (app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            int here = 0, below = 0;
            std::string names;
            for (const auto& fp : app.folderPick) {
                if (fp.g.name.find('/') == std::string::npos) here++;
                else below++;
                if (names.size() < 160)
                    names += (names.empty() ? "" : ",") + fp.g.name;
            }
            bool scanOk = app.folderPickOpen && app.folderPickRemote && here >= 1 && below >= 1;
            fprintf(stderr, "browseselftest: open-folder on the current dir %s -> picker "
                            "open=%d remote=%d, %d stack(s) in this folder, %d below "
                            "[%s]: %s\n",
                    B.dir.c_str(), app.folderPickOpen ? 1 : 0, app.folderPickRemote ? 1 : 0,
                    here, below, names.c_str(), scanOk ? "ok" : "FAIL");
            if (!scanOk) ok = false;
            app.folderPick.clear();          // Cancel: nothing must be opened
            app.folderPickOpen = false;
        }
        {   // SESSION OWNERSHIP. openRemote must talk over the UI thread's OWN
            // remote::Session, never the browse worker's - two threads framing
            // requests into one ssh stdin desynchronise the stream permanently
            // (rp::Header has no request id), and the worker's stop() frees the
            // pipe under a UI-thread read. The proof is on the wire: the open
            // moves bytes on app.uiSession and NOT ONE byte on the worker's.
            std::string file;
            for (const auto& e : B.entries) {
                if (e.dir) continue;
                if (e.group && !e.members.empty()) { file = e.members[0]; break; }
                if (peerServesName(e.name)) { file = e.name; break; }
            }
            if (file.empty()) {
                fprintf(stderr, "browseselftest: sessions: no .npy under %s to open\n",
                        B.dir.c_str());
                ok = false;
            } else {
                bool uiIdle0 = !app.uiSession.alive() && app.uiSession.bytesReceived() == 0;
                // safe to read the worker's session directly HERE and only
                // here: the selftest drives the pump by hand and the queue is
                // drained, so rbWorker is parked in its 50 ms sleep
                uint64_t rb0 = rbMain().session ? rbMain().session->bytesReceived() : 0;
                size_t nimg0 = app.images.size();
                openRemote("local://" + B.dir + "/" + file, true);
                pumpRemoteBrowse();          // pick up anything the worker said
                uint64_t uiRx = app.uiSession.bytesReceived();
                uint64_t rbRx = rbMain().session ? rbMain().session->bytesReceived() : 0;
                bool sepOk = uiIdle0 && app.uiSession.alive() && uiRx > 0 &&
                             rbRx == rb0 && app.images.size() == nimg0 + 1 &&
                             (void*)rbMain().session.get() != (void*)&app.uiSession;
                fprintf(stderr, "browseselftest: session ownership: ui session idle "
                                "before=%d, open %s -> ui rx %llu B (alive=%d), browse "
                                "worker rx %llu -> %llu B (unmoved=%d), distinct "
                                "objects=%d: %s\n",
                        uiIdle0 ? 1 : 0, file.c_str(), (unsigned long long)uiRx,
                        app.uiSession.alive() ? 1 : 0, (unsigned long long)rb0,
                        (unsigned long long)rbRx, rbRx == rb0 ? 1 : 0,
                        (void*)rbMain().session.get() != (void*)&app.uiSession ? 1 : 0,
                        sepOk ? "ok" : "FAIL");
                if (!sepOk) ok = false;
                dropPreview();
            }
        }
        {   // STACK IDENTITY. A Browse open must name the stack the way the
            // canon does - `folder/pattern` (docs/terminology.md) - and the
            // same way the SCAN picker does, or one folder has two identities
            // depending on which door it was opened through. It is not only
            // cosmetic: extractLevelFromName strips a frame-axis extent run
            // only from a name that HAS a folder part, so a bare member name
            // parses its first digit run as a LEVEL, and a lit stack gets
            // proposed at 0 - which is the dark-reference value.
            remoteBrowseTo(rbMain(), dir + "/scanroot/10lx");
            double t1 = nowSec();
            while (nowSec() - t1 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == dir + "/scanroot/10lx" && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> gv = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            const RbRow* grp = nullptr;
            for (const auto& r : gv) if (r.isGroup()) { grp = &r; break; }
            if (!grp) {
                fprintf(stderr, "browseselftest: identity: no group row in %s\n",
                        B.dir.c_str());
                ok = false;
            } else {
                closeAll();
                std::vector<std::string> files;
                for (const auto& m : grp->e->members) files.push_back(grp->join(m));
                std::string want = stackNameFor(*grp->dir, grp->e->name);
                openRemoteStack(B.host, files, want);       // exactly what a row click does
                std::string got = app.seqs.empty() ? std::string() : app.seqs.back().name;
                std::string src;
                double lvl = extractLevelFromName(got, &src);
                // ...and the multi-select leg, which has only file names
                std::vector<std::string> bases;
                for (const auto& f : files) bases.push_back(baseName(f));
                std::string multi = stackNameFor(B.dir, patternOfNames(bases));
                bool idOk = got.rfind("10lx/", 0) == 0 &&
                            got.find(grp->e->name) != std::string::npos &&
                            lvl == 10.0 && multi == want;
                fprintf(stderr, "browseselftest: stack identity: group open -> '%s', "
                                "level %.6g (from '%s'), multi-select name '%s': %s\n",
                        got.c_str(), lvl, src.c_str(), multi.c_str(),
                        idOk ? "ok" : "FAIL");
                if (!idOk) ok = false;

                {   // ---- THE SAME STACK, THROUGH A SESSION --------------------
                    // A folder stack opened from Browse is one doc per FILE, and
                    // its membership lives only in SeqInfo::remoteFiles, which
                    // the session never wrote. On restore the image line reopened
                    // ONE url and `seqload 1` handed the head to
                    // findSequenceSiblings - a local directory scan, which
                    // returns nothing for an ssh:// or local:// url. So a
                    // thirty-frame sweep came back as one loose frame, with its
                    // name and its hand-typed axis gone and a series member
                    // silently unresolvable - and nothing anywhere said so. The
                    // same folder opened through File > Open restores correctly,
                    // which is what made it read as the user's mistake.
                    //
                    // Whether the peer is reached over ssh or over a pipe is not
                    // what this is about: local:// is the same url shape, the
                    // same code path and the same listing, which is why the
                    // whole remote side of the suite is tested through it.
                    auto drainRemote = [&](double budget) {
                        double t = nowSec();
                        while (nowSec() - t < budget) {
                            pumpRemoteBrowse();
                            pumpRemoteFetch();
                            pumpRemoteOpenQueue();
                            pumpSequenceAndQueue();
                            if (loadingSettled() && app.rfPending == 0 &&
                                app.seqRestore.empty()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    };
                    drainRemote(120.0);
                    int sid0 = app.seqs.empty() ? 0 : app.seqs.back().id;
                    size_t n0 = sid0 ? framesOfSeq(sid0).size() : 0;
                    const char* RNAME = "10lx dark";
                    if (sid0) seqInfo(sid0)->name = RNAME;      // the user's own name
                    // ...and the frame the stack was left on, which is not the head
                    if (n0 >= 3) selectImage(framesOfSeq(sid0)[2]);
                    std::error_code rec;
                    const std::string rsess =
                        (std::filesystem::temp_directory_path(rec) /
                         "viewer_browse_remotestack.vsession").u8string();
                    saveSession(rsess, true);
                    std::string rerr = loadSession(rsess);
                    drainRemote(180.0);
                    int sid1 = app.seqs.empty() ? 0 : app.seqs.back().id;
                    std::vector<int> fr1 = sid1 ? framesOfSeq(sid1) : std::vector<int>();
                    std::string name1 = sid1 ? seqInfo(sid1)->name : std::string();
                    int urls = 0;
                    for (const auto& d : app.images)
                        if (d->src->path.rfind("local://", 0) == 0) urls++;
                    fprintf(stderr, "browseselftest: remote folder stack through a "
                                    "session: %zu frame(s) saved -> err=\"%s\", %zu doc(s) "
                                    "(%d remote), %zu stack(s), %zu frame(s) in it, "
                                    "name '%s'\n",
                            n0, rerr.c_str(), app.images.size(), urls, app.seqs.size(),
                            fr1.size(), name1.c_str());
                    if (n0 < 3) {
                        fprintf(stderr, "browseselftest: remote session: the fixture "
                                        "gave only %zu frame(s)\n", n0);
                        ok = false;
                    }
                    bool backOk = rerr.empty() && app.seqs.size() == 1 &&
                                  fr1.size() == n0;
                    fprintf(stderr, "browseselftest: remote folder stack restored whole: "
                                    "%s\n", backOk ? "ok" : "FAIL");
                    if (!backOk) ok = false;
                    if (name1 != RNAME) {
                        fprintf(stderr, "browseselftest: remote stack name after restore: "
                                        "'%s' (wanted '%s') FAIL\n",
                                name1.c_str(), RNAME);
                        ok = false;
                    }
                    {   // frame ⊂ stack ⊂ batch, and the frame it was left on
                        int offBatch = 0;
                        for (int idx : fr1)
                            if (!fr1.empty() && app.images[idx]->batchId !=
                                                app.images[fr1.front()]->batchId) offBatch++;
                        const ImageDoc* c = cur();
                        bool posOk = c && c->seqId == sid1 && c->seqIndex == 2;
                        fprintf(stderr, "browseselftest: restored remote stack: %d frame(s) "
                                        "off-batch, standing on frame %d: %s\n",
                                offBatch, c ? c->seqIndex : -1,
                                (offBatch == 0 && posOk) ? "ok" : "FAIL");
                        if (offBatch || !posOk) ok = false;
                    }
                    {   // ---- and when the folder CANNOT give the stack back ---
                        // The other half of this defect was the silence: the old
                        // code did nothing and said nothing, so a stack that came
                        // back as one frame was indistinguishable from a stack
                        // that had always been one frame. Point a session at a
                        // remote file the peer groups with nothing (rb/dark.npy
                        // is a lone file) and ask for a stack around it.
                        const std::string lone =
                            (std::filesystem::temp_directory_path(rec) /
                             "viewer_browse_lonestack.vsession").u8string();
                        {
                            std::ofstream lf(pathFromUtf8(lone), std::ios::binary);
                            lf << "viewer-session 1\n"
                               << "image 0 1 npy3 0 0 0 0 0 0 0 local://" << dir
                               << "/dark.npy\n"
                               << "seqload 1\n";
                        }
                        size_t logBefore = app.msgLog.size();
                        closeAll();
                        std::string lerr2 = loadSession(lone);
                        drainRemote(60.0);
                        bool said = false;
                        for (size_t i = logBefore; i < app.msgLog.size(); i++)
                            if (app.msgLog[i].text.find("came back as a single frame") !=
                                    std::string::npos &&
                                app.msgLog[i].text.find("dark.npy") != std::string::npos &&
                                app.msgLog[i].err)
                                said = true;
                        fprintf(stderr, "browseselftest: a remote stack that cannot be "
                                        "rebuilt: err=\"%s\" %zu doc(s), %zu stack(s), "
                                        "reported=%d\n", lerr2.c_str(), app.images.size(),
                                app.seqs.size(), (int)said);
                        if (!lerr2.empty() || app.images.size() != 1 ||
                            !app.seqs.empty() || !said) {
                            fprintf(stderr, "browseselftest: the loss must be SAID, not "
                                            "silently restored as one frame FAIL\n");
                            ok = false;
                        }
                        std::filesystem::remove(std::filesystem::u8path(lone), rec);
                    }
                    std::filesystem::remove(std::filesystem::u8path(rsess), rec);
                }
                closeAll();
            }
        }
        {   // ---- #81: A MULTI-ROW SELECTION IS A SET OF STACKS ----------------
            // The reported defect, in the reporter's own numbers: eight frames
            // from each of three stacks came out as ONE average of twenty-four.
            // rb/scanroot is exactly that shape - 10lx, 20lx, 40lx, eight frames
            // each - and it is also the cruellest version of it, because all
            // twenty-four members are called frame_000.npy..frame_007.npy. The
            // old code took patternOfNames over those basenames and got
            // "frame_???.npy", so three stacks came out under one plausible
            // name: the picture did not merely mean nothing, it said it came
            // from somewhere it did not.
            //
            // Driven through the same functions the panel draws with -
            // rbBuildView for the rows, rbSelectionStacks for the partition,
            // openStackForAverage for each open - so this passes only if the
            // menu item does.
            // Wait for `want` averages - and STOP WAITING once the loaders have
            // been idle a while with fewer than that, or a failing run spends
            // the whole budget proving something it already knows.
            auto settle = [&](int want) {
                double t = nowSec(), calm = 0;
                while (nowSec() - t < 120.0) {
                    pumpRemoteBrowse();
                    pumpRemoteFetch();
                    pumpRemoteOpenQueue();
                    pumpSequenceAndQueue();     // pumpStackAverages rides in here
                    int n = 0;
                    for (const auto& d : app.images) if (!d->avgOfPath.empty()) n++;
                    bool idle = loadingSettled() && app.pendingAvg.empty();
                    if (idle && n >= want) break;
                    if (!idle) calm = 0;
                    else if (!calm) calm = nowSec();
                    else if (nowSec() - calm > 2.0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            auto averages = [&]() {
                std::vector<const ImageDoc*> v;
                for (const auto& d : app.images) if (!d->avgOfPath.empty()) v.push_back(d.get());
                return v;
            };
            auto say = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& n : v) s += (s.empty() ? "" : ", ") + n;
                return s;
            };
            // Three stacks in ONE view: the tree, with the three level folders
            // expanded. (Three group rows cannot appear in a flat LISTING -
            // they live in three directories - and the tree is how a user
            // reaches them without navigating away from the first two.)
            std::string scanroot = dir + "/scanroot";
            remoteBrowseTo(rbMain(), scanroot);
            double tS = nowSec();
            while (nowSec() - tS < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == scanroot && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const std::vector<std::string> lvs = { "10lx", "20lx", "40lx" };
            for (const auto& lv : lvs) {
                rbTreeExpand(rbMain(), scanroot + "/" + lv);
                double t1 = nowSec();
                while (nowSec() - t1 < 120.0) {
                    pumpRemoteBrowse();
                    if (rbMain().treeCache.count(scanroot + "/" + lv)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            std::vector<RbRow> tv = rbBuildView(rbMain(), &B.dir, B.entries, false, true);
            std::vector<char> sel(tv.size(), 0);
            int ticked = 0;
            for (size_t i = 0; i < tv.size(); i++)
                if (tv[i].isGroup()) { sel[i] = 1; ticked++; }
            std::vector<RbAvgStack> parts = rbSelectionStacks(tv, sel);
            // What the partition claims, before a single pixel moves.
            std::vector<std::string> pNames;
            int totalFiles = 0;
            bool eight = parts.size() == lvs.size(), homed = !parts.empty();
            for (size_t k = 0; k < parts.size(); k++) {
                pNames.push_back(parts[k].name + " x" + std::to_string(parts[k].files.size()));
                totalFiles += (int)parts[k].files.size();
                if (parts[k].files.size() != 8) eight = false;
                // every file of a part comes from that part's OWN folder: this
                // is the assertion the old one-list code cannot satisfy at any
                // size, because it had one list
                if (k < lvs.size() && parts[k].name.rfind(lvs[k] + "/", 0) != 0) homed = false;
                for (const auto& f : parts[k].files)
                    if (k < lvs.size() &&
                        f.rfind(scanroot + "/" + lvs[k] + "/", 0) != 0) homed = false;
            }
            bool distinct = true;
            for (size_t a = 0; a < parts.size(); a++)
                for (size_t b = a + 1; b < parts.size(); b++)
                    if (parts[a].name == parts[b].name) distinct = false;
            bool partOk = ticked == 3 && parts.size() == 3 && eight && homed &&
                          distinct && totalFiles == 24;
            fprintf(stderr, "browseselftest: #81 partition: %d group row(s) ticked in "
                            "%s -> %d stack(s) [%s], %d file(s) total, names distinct=%d, "
                            "each under its own folder=%d: %s\n",
                    ticked, scanroot.c_str(), (int)parts.size(), say(pNames).c_str(),
                    totalFiles, distinct ? 1 : 0, homed ? 1 : 0, partOk ? "ok" : "FAIL");
            if (!partOk) ok = false;
            // ...and now open them, because a partition that nothing acts on is
            // not the bug. THREE averages must appear, each over its own eight
            // frames, each named for the stack it is the mean of.
            closeAll();
            for (const auto& s : parts) openStackForAverage(B.host, s.files, s.name, B.port);
            settle((int)parts.size());
            std::vector<const ImageDoc*> avg = averages();
            std::vector<std::string> aNames;
            bool counts = avg.size() == 3, named = avg.size() == 3;
            std::vector<int> srcSeqs;
            for (const auto* d : avg) {
                aNames.push_back(d->name + " [" + std::to_string(d->avgSrcCount) + "]");
                if (d->avgSrcCount != 8) counts = false;
                srcSeqs.push_back(d->avgSeqId);
            }
            for (const auto& lv : lvs) {              // one average per level, by name
                int hits = 0;
                for (const auto* d : avg) if (d->name.rfind(lv + "/", 0) == 0) hits++;
                if (hits != 1) named = false;
            }
            // ...and each of them is the mean of a DIFFERENT stack: the #82
            // provenance every average carries is what makes that checkable.
            bool ownStack = srcSeqs.size() == 3;
            for (size_t a = 0; a < srcSeqs.size(); a++) {
                if (!srcSeqs[a] || !seqInfo(srcSeqs[a])) ownStack = false;
                for (size_t b = a + 1; b < srcSeqs.size(); b++)
                    if (srcSeqs[a] == srcSeqs[b]) ownStack = false;
            }
            bool openOk = counts && named && ownStack && (int)app.seqs.size() == 3;
            fprintf(stderr, "browseselftest: #81 opens: 3 stacks selected -> %d average(s) "
                            "[%s] over %d stack(s), 8 frames each=%d, one per level=%d, "
                            "each from its own stack=%d: %s\n",
                    (int)avg.size(), say(aNames).c_str(), (int)app.seqs.size(),
                    counts ? 1 : 0, named ? 1 : 0, ownStack ? 1 : 0,
                    openOk ? "ok" : "FAIL");
            if (!openOk) ok = false;
            closeAll();
            rbTreeForget(rbMain());
            // The case that was ALREADY right, pinned so the fix cannot
            // overshoot into it: N frames picked inside ONE stack are ONE
            // average, of exactly those frames, and they still are.
            remoteBrowseTo(rbMain(), scanroot + "/10lx");
            double t2 = nowSec();
            while (nowSec() - t2 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == scanroot + "/10lx" && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> fv3 = rbBuildView(rbMain(), &B.dir, B.entries, true, false);
            std::vector<char> sel1(fv3.size(), 0);
            int members = 0;
            for (size_t i = 0; i < fv3.size(); i++)
                if (fv3[i].member >= 0) { sel1[i] = 1; members++; }
            std::vector<RbAvgStack> one = rbSelectionStacks(fv3, sel1);
            for (const auto& s : one) openStackForAverage(B.host, s.files, s.name, B.port);
            settle((int)one.size());
            std::vector<const ImageDoc*> a1 = averages();
            bool oneOk = one.size() == 1 && members == 8 && one[0].files.size() == 8 &&
                         a1.size() == 1 && a1[0]->avgSrcCount == 8 &&
                         a1[0]->name.rfind("10lx/", 0) == 0;
            fprintf(stderr, "browseselftest: #81 one stack stays one: %d frame row(s) of "
                            "10lx ticked -> %d stack(s) of %d file(s) -> %d average(s) "
                            "'%s' over %d frame(s): %s\n",
                    members, (int)one.size(), one.empty() ? 0 : (int)one[0].files.size(),
                    (int)a1.size(), a1.empty() ? "" : a1[0]->name.c_str(),
                    a1.empty() ? 0 : a1[0]->avgSrcCount, oneOk ? "ok" : "FAIL");
            if (!oneOk) ok = false;
            closeAll();
            // THE MIXED SELECTION, which the issue left open and this change
            // decides: a row that is not part of a stack is its OWN stack, and
            // is averaged the way its own right-click menu would average it.
            // rb's root is the fixture for it - one 24-frame group plus the two
            // loose .npy files - and the answer is three parts, not one heap and
            // not two. Nothing here is merged that the listing did not already
            // show as one thing.
            remoteBrowseTo(rbMain(), dir);
            double t3 = nowSec();
            while (nowSec() - t3 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == dir && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> rv2 = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            std::vector<char> selM(rv2.size(), 0);
            int grp2 = 0, loose = 0, other = 0;
            for (size_t i = 0; i < rv2.size(); i++) {
                if (rv2[i].up || rv2[i].isDir()) continue;
                if (rv2[i].isGroup()) { selM[i] = 1; grp2++; }
                // the PEER's question (#111): rbSelectionStacks partitions for
                // openStackForAverage, which is served over the protocol
                else if (peerServesName(rv2[i].name())) { selM[i] = 1; loose++; }
                else other++;                       // notes.txt: never ticked
            }
            std::vector<RbAvgStack> mix = rbSelectionStacks(rv2, selM);
            std::vector<std::string> mNames;
            int fromGroup = 0, fromLoose = 0;
            for (const auto& s : mix) {
                mNames.push_back(s.name + " x" + std::to_string(s.files.size()));
                if (s.files.size() == 24) fromGroup++;
                else if (s.files.size() == 1) fromLoose++;
            }
            bool mixOk = grp2 == 1 && loose == 2 && other == 1 && mix.size() == 3 &&
                         fromGroup == 1 && fromLoose == 2;
            fprintf(stderr, "browseselftest: #81 mixed selection in %s: 1 group row + "
                            "%d loose .npy ticked (%d non-.npy row left out) -> %d stack(s) "
                            "[%s] (%d whole group, %d on their own): %s\n",
                    dir.c_str(), loose, other, (int)mix.size(), say(mNames).c_str(),
                    fromGroup, fromLoose, mixOk ? "ok" : "FAIL");
            if (!mixOk) ok = false;
            closeAll();
        }
        {   // A CANCELLED SCAN'S LATE REPLY. openPickerWith does not merely
            // raise a modal: it clears app.folderPick and resets the picker's
            // root, filter, merge and sweep fields. So the reply from an
            // abandoned remote scan destroyed whatever the picker held -
            // including a local Open Folder selection being filtered - and
            // re-raised the modal over it. RbGlob has carried a generation
            // token since it was written; RbScan carried none at all.
            app.folderPick.clear();
            app.folderPickOpen = false;
            remoteScanFolder(rbMain(), B.dir);
            uint32_t genAt = rbMain().b.scanGen;
            rbMain().b.scanGen++;            // exactly what the cancel button does
            double t3 = nowSec();
            bool sawScan = false;
            while (nowSec() - t3 < 60.0) {
                {   // wait for the worker to actually produce the scan result
                    std::lock_guard<std::mutex> lk(rbMain().mtx);
                    for (const auto& rr : rbMain().done)
                        if (rr.kind == App::RbScan && rr.gen == genAt) sawScan = true;
                }
                pumpRemoteBrowse();
                if (sawScan) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (int k = 0; k < 20; k++) {    // and a few more frames of pumping
                pumpRemoteBrowse();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            bool cancelOk = sawScan && !app.folderPickOpen && app.folderPick.empty();
            fprintf(stderr, "browseselftest: cancelled scan: reply gen %u arrived=%d, "
                            "picker opened anyway=%d (%d group(s)): %s\n",
                    genAt, sawScan ? 1 : 0, app.folderPickOpen ? 1 : 0,
                    (int)app.folderPick.size(), cancelOk ? "ok" : "FAIL");
            if (!cancelOk) ok = false;
            app.folderPick.clear();
            app.folderPickOpen = false;
        }
        {   // MALFORMED .npy HEADER LENGTH. The peer sized a std::string from a
            // raw u32 taken straight off the file, with no bound and no read
            // check, so a 12-byte file whose header length reads 0xffffffff
            // cost 4 GB of commit and ~1.8 s on EVERY LIST of its directory -
            // up to 256 of them in one LIST, 512 in one SCAN. The client's own
            // .npy readers have always bounded it; the peer had the guard
            // missing in the wrong binary.
            std::error_code bec;
            std::filesystem::path bomb =
                std::filesystem::temp_directory_path(bec) / "viewer_hlenbomb";
            std::filesystem::remove_all(bomb, bec);
            std::filesystem::create_directories(bomb, bec);
            std::filesystem::copy_file(pathFromUtf8(dir + "/dark.npy"), bomb / "good.npy",
                                       std::filesystem::copy_options::overwrite_existing, bec);
            // lettered, not numbered: a numbered run would group into ONE row
            for (int i = 0; i < 8; i++) {
                std::ofstream bf(bomb / (std::string("bomb") + (char)('a' + i) + ".npy"),
                                 std::ios::binary);
                bf.write("\x93NUMPY\x02\x00\xff\xff\xff\xff", 12);
            }
            std::string bdir = bomb.u8string();
            std::replace(bdir.begin(), bdir.end(), '\\', '/');
            double bt0 = nowSec();
            remoteBrowseTo(rbMain(), bdir);
            double t2 = nowSec();
            while (nowSec() - t2 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == bdir) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            double secs = nowSec() - bt0;
            int meta = 0, nb = 0;
            for (const auto& e : B.entries) {
                if (e.name.rfind("bomb", 0) == 0) nb++;
                if (e.hasMeta) meta++;
            }
            // Measured: 0.06 s with the guard, 3.49 s for four bombs without
            // it. Eight bombs and a 2 s bound leaves ~30x headroom over a
            // healthy LIST and still fails the unbounded allocation by ~3x.
            bool bombOk = B.dir == bdir && nb == 8 && meta == 1 && secs < 2.0;
            fprintf(stderr, "browseselftest: malformed .npy headers: listed %d entr(ies) "
                            "(%d bombs, %d with metadata) in %.2f s: %s\n",
                    (int)B.entries.size(), nb, meta, secs, bombOk ? "ok" : "FAIL");
            if (!bombOk) ok = false;
            std::filesystem::remove_all(bomb, bec);
        }
        {   // NAME ORDER (user decision, 2026-08-05: the default is natural).
            //
            // The listing used to sort names as text while the stack it opened
            // was built with rp::naturalLess, so the order on screen was not
            // the order sigma_t is computed over - and a tooltip in the same
            // panel already claimed numeric name order. rb/unpadded is the one
            // fixture that can tell the two apart: every other numbered set
            // here is zero-padded, and for those the two orders agree.
            //
            // Asserted, in order: the listing's own sort (rbSortShown, the
            // exact function the panel calls), the TREE's per-level sort
            // (inside the builder, a different code path that has to reach the
            // same comparator), that the toggle actually still reaches text
            // order - a test that only checks the default cannot tell a
            // working switch from a hardwired one - and last, the one that
            // matters: the frames in the order the panel SHOWS them are the
            // frames in the order the stack is BUILT from.
            remoteBrowseTo(rbMain(), dir + "/unpadded");
            double t4 = nowSec();
            while (nowSec() - t4 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == dir + "/unpadded" && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // exactly what the panel lays out: build the view, then sort the
            // shown-index list with the panel's own sort (a tree is sorted per
            // level while it is built, so it never comes through rbSortShown)
            auto onScreen = [&](bool natural, bool flat, bool tree) {
                rbMain().nameNatural = natural;
                std::vector<RbRow> v = rbBuildView(rbMain(), &B.dir, B.entries, flat, tree);
                std::vector<int> idx(v.size());
                for (int i = 0; i < (int)v.size(); i++) idx[i] = i;
                if (!tree) rbSortShown(rbMain(), v, idx);
                std::vector<std::string> names;
                for (int i : idx) names.push_back(v[i].name());
                return names;
            };
            auto say = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& n : v) s += (s.empty() ? "" : " ") + n;
                return s;
            };
            // the folder rows, which are what a listing sort can be read off:
            // directories never collapse into a group, so they ARE the rows
            auto dirsOf = [](const std::vector<std::string>& v) {
                std::vector<std::string> d;
                for (const auto& n : v) if (n.rfind("lv", 0) == 0) d.push_back(n);
                return d;
            };
            const std::vector<std::string> wantNat = { "lv1", "lv2", "lv10" };
            const std::vector<std::string> wantTxt = { "lv1", "lv10", "lv2" };
            std::vector<std::string> listNat = onScreen(true, false, false);
            std::vector<std::string> treeNat = onScreen(true, false, true);
            std::vector<std::string> listTxt = onScreen(false, false, false);
            std::vector<std::string> treeTxt = onScreen(false, false, true);
            // ...and the frames, which only exist as rows when the group is
            // expanded (flat). The natural listing must agree with the stack.
            rbMain().nameNatural = true;
            std::vector<RbRow> fv = rbBuildView(rbMain(), &B.dir, B.entries, true, false);
            std::vector<int> fidx(fv.size());
            for (int i = 0; i < (int)fv.size(); i++) fidx[i] = i;
            rbSortShown(rbMain(), fv, fidx);
            std::vector<std::string> shownFrames;
            for (int i : fidx) if (fv[i].member >= 0) shownFrames.push_back(fv[i].name());
            std::vector<std::string> stacked = shownFrames;
            sortFramesNumerically(stacked);          // what openRemoteStack gets
            // the same question asked of TEXT order, which must fail it - the
            // disagreement is the defect this whole change exists to remove,
            // so a test that cannot still see it is not testing anything
            std::vector<std::string> txtFrames;
            {
                rbMain().nameNatural = false;
                std::vector<RbRow> tf = rbBuildView(rbMain(), &B.dir, B.entries, true, false);
                std::vector<int> ti(tf.size());
                for (int i = 0; i < (int)tf.size(); i++) ti[i] = i;
                rbSortShown(rbMain(), tf, ti);
                for (int i : ti) if (tf[i].member >= 0) txtFrames.push_back(tf[i].name());
            }
            rbMain().nameNatural = true;             // leave the panel as found
            bool listOk  = dirsOf(listNat) == wantNat && dirsOf(listTxt) == wantTxt;
            bool treeOk2 = dirsOf(treeNat) == wantNat && dirsOf(treeTxt) == wantTxt;
            bool framesOk = shownFrames.size() == 3 && shownFrames == stacked &&
                            txtFrames != stacked;
            bool ordOk = listOk && treeOk2 && framesOk;
            fprintf(stderr, "browseselftest: name order in %s: listing natural [%s] "
                            "text [%s]; tree natural [%s] text [%s]; frames shown [%s] "
                            "vs stacked [%s] agree=%d, text order [%s] differs=%d: %s\n",
                    B.dir.c_str(), say(dirsOf(listNat)).c_str(), say(dirsOf(listTxt)).c_str(),
                    say(dirsOf(treeNat)).c_str(), say(dirsOf(treeTxt)).c_str(),
                    say(shownFrames).c_str(), say(stacked).c_str(),
                    shownFrames == stacked ? 1 : 0, say(txtFrames).c_str(),
                    txtFrames != stacked ? 1 : 0, ordOk ? "ok" : "FAIL");
            if (!ordOk) ok = false;
        }
        {   // PORT. A non-default ssh port used to survive only inside the
            // browse worker: makeRemoteUrl dropped it, and that string is what
            // becomes ImageDoc::path, RFetchJob::url, MJob::url and the session
            // file's npy3 line. Everything downstream then ran plain `ssh host`
            // - which is a DIFFERENT MACHINE if one answers on 22, and the
            // install fallback would deploy the peer onto it.
            const int P = 2222;
            std::string u = makeRemoteUrl("user@host", "/data/f.npy", P);
            std::string uh = makeRemoteUrl("user@host", "~/data/f.npy", P);
            std::string h1, p1, h2, p2;
            int gp1 = 0, gp2 = 0;
            bool r1 = remote::parseUrl(u, h1, p1, &gp1);
            bool r2 = remote::parseUrl(uh, h2, p2, &gp2);
            // the places url and the open url are the same function now, so a
            // bookmark and an open of the same place cannot disagree
            bool same = placeUrl("user@host", P, "/data/f.npy") == u;
            bool portOk = r1 && r2 && gp1 == P && gp2 == P && h1 == "user@host" &&
                          p1 == "/data/f.npy" && p2 == "~/data/f.npy" && same &&
                          u.find(":2222/") != std::string::npos;
            fprintf(stderr, "browseselftest: port in the locator: '%s' -> host '%s' "
                            "path '%s' port %d; '%s' -> port %d; placeUrl identical=%d: %s\n",
                    u.c_str(), h1.c_str(), p1.c_str(), gp1, uh.c_str(), gp2,
                    same ? 1 : 0, portOk ? "ok" : "FAIL");
            if (!portOk) ok = false;
        }
        fprintf(stderr, "browseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
