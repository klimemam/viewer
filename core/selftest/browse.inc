// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // The Browse panel's own behaviour, verifiable without a human. Connects a
    // LOCAL peer to <dir> and drives the same functions the panel draws with.
    if (!g_browseSelftest.empty()) {
        std::string dir = g_browseSelftest;
        std::replace(dir.begin(), dir.end(), '\\', '/');
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        // How many entries the SHARED fixture folder had before this test ran a
        // line. Compared again at the very bottom - see the note there: the next
        // test walks this folder by position, so a row this one leaves behind is
        // that test's failure and this test's fault.
        int fixtureEntriesAtStart = 0;
        {
            std::error_code fec;
            for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), fec))
                { (void)e; fixtureEntriesAtStart++; }
        }
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
            //
            // #148: this browse is of THIS machine (host empty), so the door
            // now goes to the extension-aware local scan - the same one a
            // dropped folder takes - and the picker is a LOCAL picker. The
            // claim being made here is unchanged and is about the scan's ROOT,
            // not about which scan ran: "all stacks below" has to mean this
            // folder AND everything under it, whichever side walks the tree.
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
            bool scanOk = app.folderPickOpen && !app.folderPickRemote && here >= 1 && below >= 1;
            fprintf(stderr, "browseselftest: open-folder on the current dir %s -> picker "
                            "open=%d remote=%d (0 = the local scan, #148), %d stack(s) in "
                            "this folder, %d below [%s]: %s\n",
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
        {   // ---- A REMOTE FOLDER WHOSE SHAPES DISAGREE IS NOT ONE STACK ------
            // The local twin of this is --picker-selftest UC7 (PR #116): four
            // numbered siblings group into one row, their shapes do not agree,
            // and the sequence loader refuses the odd ones BY NAME instead of
            // stacking four different pictures under one seqId. The frames that
            // arrive over the LINK never learned it - pumpRemoteFetch took
            // whatever the peer sent - so the SAME FOLDER was refused when it
            // sat on this disk and silently stacked when it was reached through
            // a peer. docs/terminology.md gives a stack a time axis either way,
            // and sigma_t, the FPN split and the per-frame table are quantities
            // only if every frame is the same picture measured again.
            //
            // Same fixtures as UC7, deliberately: (8,8), (8,8,3), (4,4), (8,8),
            // so channels and grid are each a way of disagreeing and a check on
            // one is not a check on the other, and so issue #71 - what a
            // trailing axis of 1 or 2 means - is neither answered nor depended
            // on, every reading of it agreeing about these three. Written into
            // the per-test TMP directory for the reasons the hlenbomb block
            // above gives: no other selftest scans it, and a fixture living
            // apart from the rule it pins drifts away from it.
            closeAll();
            std::error_code shec;
            std::filesystem::path shdir =
                std::filesystem::temp_directory_path(shec) / "viewer_remoteshape";
            std::filesystem::remove_all(shdir, shec);
            std::filesystem::create_directories(shdir, shec);
            std::vector<uint16_t> ramp(8 * 8 * 3);
            for (size_t i = 0; i < ramp.size(); i++) ramp[i] = (uint16_t)(i * 13 + 7);
            npyWriteFile((shdir / "rshape_1.npy").u8string(), "<u2",
                         { 8, 8 }, ramp.data(), 64 * 2);
            npyWriteFile((shdir / "rshape_2.npy").u8string(), "<u2",
                         { 8, 8, 3 }, ramp.data(), 64 * 3 * 2);
            npyWriteFile((shdir / "rshape_3.npy").u8string(), "<u2",
                         { 4, 4 }, ramp.data(), 16 * 2);
            npyWriteFile((shdir / "rshape_4.npy").u8string(), "<u2",
                         { 8, 8 }, ramp.data(), 64 * 2);
            std::string shpath = shdir.u8string();
            std::replace(shpath.begin(), shpath.end(), '\\', '/');
            remoteBrowseTo(rbMain(), shpath);
            double sht = nowSec();
            while (nowSec() - sht < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == shpath) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> shv = rbBuildView(rbMain(), &B.dir, B.entries, false, false);
            const RbRow* shg = nullptr;
            for (const auto& r : shv) if (r.isGroup()) { shg = &r; break; }
            // The GROUPER is not the fix and must not become it: the peer reads
            // names, four numbered files are one run, and .raw declares no
            // geometry at all. So this is asserted, exactly as UC7 asserts it.
            fprintf(stderr, "browseselftest: remote shape: peer groups %zu file(s) "
                            "into %d row(s)\n", shg ? shg->e->members.size() : 0,
                    shg ? 1 : 0);
            if (!shg || shg->e->members.size() != 4) {
                fprintf(stderr, "browseselftest: remote shape: four numbered files must "
                                "still LIST as one group - the fix is not a name rule "
                                "FAIL\n");
                ok = false;
            } else {
                std::vector<std::string> shfiles;
                for (const auto& m : shg->e->members) shfiles.push_back(shg->join(m));
                auto drainFetch = [&](double budget) {
                    double t = nowSec();
                    while (nowSec() - t < budget) {
                        pumpRemoteBrowse();
                        pumpRemoteFetch();
                        pumpRemoteOpenQueue();
                        pumpSequenceAndQueue();
                        if (loadingSettled() && app.rfPending == 0) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                };
                app.seqNote.clear();
                openRemoteStack(B.host, shfiles, stackNameFor(B.dir, shg->e->name));
                drainFetch(120.0);
                int shid = app.seqs.empty() ? 0 : app.seqs.back().id;
                std::vector<std::string> kept;
                for (int idx : shid ? framesOfSeq(shid) : std::vector<int>())
                    kept.push_back(baseName(app.images[idx]->src->path));
                std::string keptTxt;
                for (const auto& k : kept) keptTxt += (keptTxt.empty() ? "" : ",") + k;
                int shExp = shid ? seqInfo(shid)->expectedFrames : 0;
                fprintf(stderr, "browseselftest: remote shape: %zu doc(s), %zu stack(s), "
                                "%zu of %d frame(s) [%s]\n", app.images.size(),
                        app.seqs.size(), kept.size(), shExp, keptTxt.c_str());
                if (app.images.size() != 2 || app.seqs.size() != 1 || kept.size() != 2 ||
                    kept[0] != "rshape_1.npy" || kept[1] != "rshape_4.npy") {
                    fprintf(stderr, "browseselftest: remote shape: the stack must hold the "
                                    "two 8x8 1ch frames and NOTHING else FAIL\n");
                    ok = false;
                }
                // ONE FILE is dropped, not the stack - and the drop rides the
                // count that was already there. expectedFrames is what the Files
                // row and the canvas footer print "n of N" from, so a stack two
                // frames short says so on screen without a word being added.
                if (shExp != 4 || kept.size() >= (size_t)shExp) {
                    fprintf(stderr, "browseselftest: remote shape: the stack must still "
                                    "expect 4 and say so (n of N) FAIL\n");
                    ok = false;
                }
                // ...and it must SAY which files it left out, and why - the same
                // sentence the local half prints, because it is the same rule.
                const char* SHWANT[] = {
                    "rshape_2.npy: 8x8 3ch and rshape_1.npy is 8x8 1ch: a run of numbered "
                    "files is a stack, and a stack's frames are one shape - open it on "
                    "its own",
                    "rshape_3.npy: 4x4 1ch and rshape_1.npy is 8x8 1ch: a run of numbered "
                    "files is a stack, and a stack's frames are one shape - open it on "
                    "its own",
                };
                for (const char* w : SHWANT)
                    if (app.seqNote.find(w) == std::string::npos) {
                        fprintf(stderr, "browseselftest: remote shape: the refusal never "
                                        "said\n  want: %s\n  said: %s FAIL\n",
                                w, app.seqNote.c_str());
                        ok = false;
                    }
                // The way out the refusal names has to exist over the link too:
                // open the odd file itself and it opens as what it is.
                closeAll();
                openRemote(makeRemoteUrl(B.host, shfiles[1]));
                drainFetch(60.0);
                const ImageDoc* shalone = app.images.empty() ? nullptr : app.images[0].get();
                fprintf(stderr, "browseselftest: remote shape: \"open it on its own\" -> "
                                "%zu doc(s) %dx%d %dch\n", app.images.size(),
                        shalone ? shalone->w : 0, shalone ? shalone->h : 0,
                        shalone ? shalone->ch : 0);
                if (app.images.size() != 1 || !shalone || shalone->w != 8 ||
                    shalone->h != 8 || shalone->ch != 3) {
                    fprintf(stderr, "browseselftest: remote shape: the refused file must "
                                    "still open ALONE, as its own shape FAIL\n");
                    ok = false;
                }
            }
            closeAll();
            app.seqNote.clear();
            std::filesystem::remove_all(shdir, shec);    // ours: ours to remove
            remoteBrowseTo(rbMain(), dir);               // leave the panel where it was
            double shback = nowSec();
            while (nowSec() - shback < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == dir) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
            //
            // The job is enqueued HERE rather than through remoteScanFolder,
            // because since #148 that function produces no RbScan at all when
            // the host is empty (a folder on this disk goes to the local,
            // extension-aware scan). The subject of this assertion is the
            // REMOTE scan's cancel token, so the remote job is what has to be
            // in flight - built exactly as remoteScanFolder builds it.
            app.folderPick.clear();
            app.folderPickOpen = false;
            uint32_t genAt = ++rbMain().b.scanGen;
            {
                App::RbJob j;
                j.kind = App::RbScan;
                j.host = rbMain().b.host; j.port = rbMain().b.port; j.dir = B.dir;
                j.gen = genAt;
                rbEnqueue(rbMain(), std::move(j));
            }
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
        {   // CURSOR ACROSS A RE-LISTING. The keyboard cursor was keyed on
            // host|dir|REV, and B.rev bumps on every listing that arrives -
            // including a second listing of the folder the panel is already
            // standing in. The same nine rows would come back and the cursor
            // would be gone, which is the intermittent browse-keys failure
            // captured on 2026-08-05 (down worked; a duplicate listing landed
            // before the check and undid it). Driven through rbCursorFollow -
            // the function the panel calls - because a NOGL run cannot press a
            // key, and a flaky GUI test is what let this hide for four
            // sightings.
            std::string cdir = dir + "/scanroot/10lx";
            remoteBrowseTo(rbMain(), cdir);
            double tc = nowSec();
            while (nowSec() - tc < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == cdir && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            auto viewNow = [&]() {
                return rbBuildView(rbMain(), &B.dir, B.entries, rbMain().flat, rbMain().tree);
            };
            auto say = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& n : v) s += (s.empty() ? "" : " ") + n;
                return s;
            };
            std::vector<RbRow> v0 = viewNow();
            rbCursorFollow(rbMain(), v0);          // as the first panel frame does
            // Put the cursor on a REAL listing row - the first one that is not
            // the ".." parent. ".." exists in every listing of every folder, so
            // a cursor parked there would be "found again" by a match that
            // proves nothing about the entries the re-listing replaced.
            // Anti-vacuity: a listing with no such row cannot lose a cursor,
            // and would pass every check below without testing anything.
            int row0 = -1;
            for (size_t i = 0; i < v0.size(); i++) if (!v0[i].up) { row0 = (int)i; break; }
            bool haveRows = row0 >= 0;
            rbMain().cursor = row0;
            rbCursorFollow(rbMain(), v0);          // records the name under it
            std::string key0 = rbMain().cursorKey;
            std::string name0 = haveRows ? v0[row0].name() : std::string();
            uint64_t rev0 = B.rev;

            // the same place, listed again - a refresh, or the duplicate the
            // capture caught
            remoteBrowseTo(rbMain(), cdir);
            double tc2 = nowSec();
            while (nowSec() - tc2 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == cdir && !B.entries.empty() && B.rev != rev0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> v1 = viewNow();
            rbCursorFollow(rbMain(), v1);
            int keptIdx = rbMain().cursor;
            std::string keptName = keptIdx >= 0 && keptIdx < (int)v1.size()
                                 ? v1[keptIdx].name() : std::string();
            bool bumped = B.rev != rev0;

            // a DIFFERENT place must still clear it: a cursor on row 0 of the
            // folder you left is a leftover, not a cursor
            std::string other = dir + "/scanroot";
            remoteBrowseTo(rbMain(), other);
            double tc3 = nowSec();
            while (nowSec() - tc3 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == other && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> v2 = viewNow();
            rbCursorFollow(rbMain(), v2);
            int awayIdx = rbMain().cursor;

            // THE SELECTION HAS THE SAME DISEASE, and it is the other captured
            // browse-keys signature (docs/diagnostics/browse-keys-20260804.txt):
            // two stacks multi-selected and Opened, 36 of 44 images, seqs=3 of
            // 4 - one of the two opened. selSig was host|dir|rev as well, so a
            // duplicate listing emptied the ticks between the clicks and the
            // Open, and what Open then saw was the row under the cursor:
            // exactly one stack. Driven in a folder with enough rows to tick
            // TWO - one tick could be confused with "the cursor row".
            std::string mdir = dir;                 // tools/testdata/rb: 9 rows
            remoteBrowseTo(rbMain(), mdir);
            double tm = nowSec();
            while (nowSec() - tm < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == mdir && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> m0 = viewNow();
            rbSelFollow(rbMain(), m0);
            std::vector<std::string> ticked;
            for (size_t i = 0; i < m0.size() && ticked.size() < 2; i++)
                if (!m0[i].up) { rbMain().sel[i] = 1; ticked.push_back(m0[i].name()); }
            rbSelFollow(rbMain(), m0);              // records the names
            uint64_t mrev0 = B.rev;
            remoteBrowseTo(rbMain(), mdir);         // the same place, again
            double tm2 = nowSec();
            while (nowSec() - tm2 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == mdir && !B.entries.empty() && B.rev != mrev0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> m1 = viewNow();
            rbSelFollow(rbMain(), m1);
            std::vector<std::string> stillTicked;
            for (size_t i = 0; i < m1.size() && i < rbMain().sel.size(); i++)
                if (rbMain().sel[i]) stillTicked.push_back(m1[i].name());
            // and moving away must still empty it
            remoteBrowseTo(rbMain(), cdir);
            double tm3 = nowSec();
            while (nowSec() - tm3 < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == cdir && !B.entries.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::vector<RbRow> m2 = viewNow();
            rbSelFollow(rbMain(), m2);
            int awaySel = 0;
            for (size_t i = 0; i < rbMain().sel.size(); i++) if (rbMain().sel[i]) awaySel++;
            // anti-vacuity: TWO ticks had to go in, or "they survived" is a
            // statement about nothing
            bool selOk = ticked.size() == 2 && stillTicked == ticked && awaySel == 0;
            fprintf(stderr, "browseselftest: selection across a re-listing of %s: "
                            "ticked [%s] -> kept [%s]; after moving away: %d tick(s): %s\n",
                    mdir.c_str(), say(ticked).c_str(), say(stillTicked).c_str(),
                    awaySel, selOk ? "ok" : "FAIL");
            if (!selOk) ok = false;

            bool curOk = haveRows && bumped && !key0.empty() &&
                         keptIdx >= 0 && keptName == name0 && awayIdx < 0;
            fprintf(stderr, "browseselftest: cursor across a re-listing of %s: "
                            "rows=%zu row0=%d key='%s' rev %llu->%llu bumped=%d; after: "
                            "cursor=%d name='%s' (want '%s'); after moving away: "
                            "cursor=%d: %s\n",
                    cdir.c_str(), v0.size(), row0, key0.c_str(),
                    (unsigned long long)rev0, (unsigned long long)B.rev, bumped ? 1 : 0,
                    keptIdx, keptName.c_str(), name0.c_str(), awayIdx,
                    curOk ? "ok" : "FAIL");
            if (!curOk) ok = false;
            rbMain().cursor = -1;
            rbMain().cursorKey.clear();
        }
        {   // ---- BROWSE-SIDE POLLING (watch-design §2's second row, §5) -------
            // While an instance is BEING DRAWN, its current directory is
            // re-listed on a timer and the listing simply becomes the new one.
            // §5 is explicit that this is NOT a notification: a listing is
            // something you look at, not a measurement, so it may update
            // silently - which is only honest if the cursor and the ticks
            // survive it, and that is what this block exists for.
            //
            // NOTHING HERE SLEEPS. The interval is 3 s and the peer's 12 s; a
            // test that lived through either would be a slow test that fails on
            // a loaded machine (watch.h's own reason for existing). Every
            // decision below takes `now` as an argument, so the clock is SET.
            // `clk` is that clock: rbPollDue only ever compares `now` against
            // the instant of the last round, so a synthetic pair exercises a
            // three-second interval in a microsecond. It is deliberately not
            // nowSec() - which is seconds since THIS process started, so
            // "nowSec() - 99" is a negative instant a few seconds in.
            App::BrowseInstance& I = rbMain();
            double clk = 100000.0;
            auto say = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& n : v) s += (s.empty() ? "" : " ") + n;
                return s;
            };
            auto check = [&](bool cond, const char* what) {
                fprintf(stderr, "browseselftest: %-72s %s\n", what,
                        cond ? "PASS" : "FAIL");
                if (!cond) ok = false;
            };
            // A frame number to stand in for the real one: App::uiFrame is
            // bumped once per frame by the spine, and a NOGL run draws none.
            const uint64_t FR = 1000;
            app.uiFrame = FR;
            I.drawnFrame = FR;
            // Let this instance's worker come to rest, and swallow whatever it
            // produced. Called between cases that enqueue: a reply still in
            // flight from the case before would land in the middle of the next
            // one, which is the shape of the intermittent failures this panel
            // has already produced twice.
            auto settle = [&]() {
                double t0 = nowSec();
                while (nowSec() - t0 < 60.0) {
                    pumpRemoteBrowse();
                    bool quiet;
                    {
                        std::lock_guard<std::mutex> lk(I.mtx);
                        quiet = I.queue.empty() && I.done.empty();
                    }
                    if (quiet && !I.busy) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                I.pollPending = false;
            };
            // ---- the fixture ---------------------------------------------------
            // Its own folder, written by hand: the case is "a file appeared
            // while the panel was looking at it", and doing that inside
            // tools/testdata/rb would change what every other selftest sees
            // there.
            //
            // ...which is what it did, because a SUBDIRECTORY of rb is a row of
            // rb. It was `dir + "/bwatch-scratch"` and it is removed on the way
            // out, so a run that finishes leaves nothing - but a run that does
            // NOT (killed, interrupted, or the process ended from another test
            // still holding the tree) leaves an eleventh row in rb, and
            // --browse-keys-selftest is positional: `home` then eight `down`
            // lands one row short, every check after it fails, and the waits
            // spin out to the 300 s test budget. Measured: 28 FAIL / 300.8 s,
            // from four .npy files in a directory this test believed was its
            // own. So it goes where the per-test TMP is (the same place the
            // header-length bomb above uses, and the reason CMake gives every
            // selftest its own TMP/TEMP/TMPDIR): a folder no other test can see
            // whether this one cleans up or not.
            //
            // The bytes are arithmetic - a 4x4 f32 .npy is a 128-byte header
            // region + 4*4*4 = 192 bytes, the same sum --rwatch-selftest states
            // - and the names are deliberately NOT a numbered sequence: the peer
            // folds w_000..w_003 into ONE group row, and this case wants several
            // rows to put a cursor on and two to tick. (The group row is the
            // stack watcher's subject and is asserted over the link in
            // --rwatch-selftest R1.)
            std::error_code bec;
            std::string scratch =
                (std::filesystem::temp_directory_path(bec) / "viewer_bwatch").u8string();
            std::replace(scratch.begin(), scratch.end(), '\\', '/');
            std::filesystem::remove_all(pathFromUtf8(scratch), bec);
            std::filesystem::create_directories(pathFromUtf8(scratch), bec);
            auto writeNpy4 = [](const std::string& path, float v) {
                std::vector<float> px(16, v);
                std::vector<uint8_t> b = selftestNpyBytes("<f4", { 4, 4 }, px.data(),
                                                          px.size() * sizeof(float));
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            };
            const char* names4[4] = { "alpha.npy", "bravo.npy", "delta.npy", "echo.npy" };
            for (int i = 0; i < 4; i++) writeNpy4(scratch + "/" + names4[i], (float)i);
            remoteBrowseTo(I, scratch);
            double ts = nowSec();
            while (nowSec() - ts < 120.0) {
                pumpRemoteBrowse();
                if (B.dir == scratch && B.entries.size() == 4) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            settle();
            auto viewHere = [&]() {
                return rbBuildView(I, &B.dir, B.entries, I.flat, I.tree);
            };
            check(B.dir == scratch && B.entries.size() == 4,
                  "B0 (setup) the panel stands in a folder of 4 plain rows");

            {   // ---- B1: §2's two intervals, as a RATIO and not a second
                // constant. #161 computes the peer's interval from the two
                // numbers rather than typing it, so moving one of them can
                // never silently leave the peer polled at the local rate; the
                // Browse half is the same question and gets the same shape.
                //
                // The arithmetic, hand-derived: 3 s local and 10 s remote, so a
                // peer is asked every ceil(10 / 3) = 4th local round = every
                // 4 x 3 = 12 s. TWELVE, not ten: the ratio is a whole number of
                // local rounds, and that is what makes "never faster than the
                // local rate" true by construction (watch-design §14.3).
                const bool ratio = browseWatchEvery() == 4 &&
                                   app.browseWatchIntervalSec == 3.0 &&
                                   app.browseWatchRemoteIntervalSec == 10.0 &&
                                   browseWatchInterval(false) == 3.0 &&
                                   browseWatchInterval(true) == 12.0;
                check(ratio, "B1 3 s local / 10 s remote -> a peer every ceil(10/3)=4th "
                             "round = 12 s");
                // ANTI-VACUITY, and it is the whole point of computing it: move
                // the LOCAL interval past the remote one and the peer must slow
                // down with it, never speed up to the local rate.
                const double keep = app.browseWatchIntervalSec;
                app.browseWatchIntervalSec = 10.0;
                const bool never = browseWatchEvery() == 1 &&
                                   browseWatchInterval(true) == 10.0;
                app.browseWatchIntervalSec = keep;
                check(never, "B1b ...and a local interval raised to 10 s does not leave "
                             "the peer at 3 s");
            }
            {   // ---- B2: ONLY WHILE DRAWN -------------------------------------
                // §2: the poll runs for an instance that is actually being
                // drawn. drawPanelRemote writes the mark at its head and the
                // spine calls it only when ImGui::Begin answered true, so a
                // window that is closed, collapsed or on an unselected dock tab
                // simply stops advancing it. Asserted rather than assumed: the
                // mark IS the gate, and a gate nobody asks about is a comment.
                clk += 100.0;
                I.polledAt = clk - 99.0;             // long overdue either way
                I.drawnFrame = FR;
                const bool drawnNow = rbPollDue(I, clk, FR);
                I.drawnFrame = FR - 1;               // drawn in the frame that
                const bool drawnLast = rbPollDue(I, clk, FR);   // just finished
                I.drawnFrame = FR - 2;               // ...and then the window
                const bool gone = rbPollDue(I, clk, FR);        // went away
                I.drawnFrame = 0;                    // never drawn at all
                const bool never = rbPollDue(I, clk, FR);
                check(drawnNow && drawnLast && !gone && !never,
                      "B2 polled while drawn (this frame or last), never once the "
                      "panel stops");
                I.drawnFrame = FR;
                I.polledAt = 0;
            }
            {   // ---- B3: SKIP, NEVER QUEUE ------------------------------------
                // §2: if that instance's worker is busy, the round is skipped -
                // the next one comes. A queue of stale listings is worse than a
                // missed round: each of them replaces the listing when it lands,
                // so a slow link would redraw the panel N times with N answers
                // to the same question, the last N-1 already out of date when
                // they arrived.
                clk += 100.0;
                const int was = I.pollsIssued;
                I.polledAt = clk - 99.0;
                I.pollPending = false;
                I.busy = true;                       // a real job is running
                const bool whileBusy = rbPollRound(I, clk, FR);
                I.busy = false;
                I.pollPending = true;                // ...or a poll is already out
                const bool whilePolling = rbPollRound(I, clk, FR);
                I.pollPending = false;
                {   // ...or a navigation is waiting to go out
                    std::lock_guard<std::mutex> lk(I.mtx);
                    App::RbJob j;
                    j.kind = App::RbList;
                    j.host = I.b.host;
                    j.port = I.b.port;
                    j.dir = I.b.dir;
                    I.queue.push_back(std::move(j));
                }
                const bool whileQueued = rbPollRound(I, clk, FR);
                {
                    std::lock_guard<std::mutex> lk(I.mtx);
                    I.queue.clear();
                }
                check(!whileBusy && !whilePolling && !whileQueued &&
                      I.pollsIssued == was,
                      "B3 a busy worker, a poll in flight or a queued nav: the round "
                      "is SKIPPED");
                // ANTI-VACUITY: with the worker free the very same call DOES
                // issue one, and exactly one - "it never polls at all" passes
                // B3 on its own.
                bool quietBefore;
                {
                    std::lock_guard<std::mutex> lk(I.mtx);
                    quietBefore = I.done.empty();
                }
                I.polledAt = clk - 99.0;
                const bool free1 = rbPollRound(I, clk, FR);
                const bool free2 = rbPollRound(I, clk, FR);   // same instant: not due
                // WHAT went out is read off the ANSWER, not off the queue. The
                // enqueue wakes the worker, so by the time this thread looks the
                // job may already be gone from the queue and not yet in `done` -
                // which is exactly what happened on the Windows runner the first
                // time this was written (B3b FAIL with everything else green).
                // The reply carries the job's own kind and dir, so waiting for
                // it asks the same question with no race in it. `pollPending` is
                // still set, because only the pump clears it and this loop does
                // not pump: one poll issued, one outstanding.
                App::RbResult got;
                bool answered = false;
                double tb = nowSec();
                while (nowSec() - tb < 60.0) {
                    {
                        std::lock_guard<std::mutex> lk(I.mtx);
                        if (!I.done.empty()) { got = I.done.front(); answered = true; }
                    }
                    if (answered) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(free1 && !free2 && I.pollsIssued == was + 1 && I.pollPending &&
                      quietBefore && answered && got.kind == App::RbPoll &&
                      got.dir == I.b.dir,
                      "B3b ...and a free worker issues ONE round, of THIS dir, per "
                      "interval");
                settle();
            }
            {   // ---- B4: THE LOAD-BEARING ONE ---------------------------------
                // A poll-driven re-listing must leave the keyboard cursor and
                // the multi-selection exactly where a manual refresh leaves
                // them. `RemoteBrowse::rev` bumps on every listing that arrives,
                // and until PR #155 that bump wiped both; #155 made them follow
                // the row BY NAME (rbCursorFollow / rbSelFollow). Browse-side
                // polling re-lists every few seconds, so anything that regressed
                // there would turn an intermittent defect into a permanent one -
                // which is why this assertion is the reason the stage is safe to
                // build at all.
                std::vector<RbRow> v0 = viewHere();
                rbCursorFollow(I, v0);
                rbSelFollow(I, v0);
                int row0 = -1;
                std::vector<std::string> ticked;
                for (size_t i = 0; i < v0.size(); i++) {
                    if (v0[i].up) continue;          // ".." is in every listing
                    if (row0 < 0) row0 = (int)i;
                    if (ticked.size() < 2) { I.sel[i] = 1; ticked.push_back(v0[i].name()); }
                }
                I.cursor = row0;
                rbCursorFollow(I, v0);               // records the name under it
                rbSelFollow(I, v0);                  // ...and the ticked names
                const std::string want = row0 >= 0 ? v0[row0].name() : std::string();
                const uint64_t rev0 = B.rev;
                const size_t rows0 = v0.size();
                // ...and now a file appears in the folder the panel is looking
                // at. "charlie" sorts between bravo and delta, so it does not
                // merely lengthen the listing - it MOVES every row after it,
                // which is the case an index-keyed cursor cannot survive and a
                // name-keyed one must.
                writeNpy4(scratch + "/charlie.npy", 9.0f);
                clk += 100.0;
                I.polledAt = clk - 99.0;             // due: the clock is SET,
                const bool issued = rbPollRound(I, clk, FR);      // never waited on
                double tp = nowSec();
                while (nowSec() - tp < 120.0) {
                    pumpRemoteBrowse();
                    if (B.rev != rev0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                std::vector<RbRow> v1 = viewHere();
                rbCursorFollow(I, v1);
                rbSelFollow(I, v1);
                std::vector<std::string> kept;
                for (size_t i = 0; i < v1.size() && i < I.sel.size(); i++)
                    if (I.sel[i]) kept.push_back(v1[i].name());
                const int cur = I.cursor;
                const std::string curName = cur >= 0 && cur < (int)v1.size()
                                          ? v1[cur].name() : std::string();
                bool sawNew = false;
                for (const auto& e : B.entries) if (e.name == "charlie.npy") sawNew = true;
                // ANTI-VACUITY, and it is the whole guard: the listing really
                // WAS replaced. Without these four terms, "the cursor is still
                // on alpha.npy" is satisfied by a poll that never happened.
                check(issued && sawNew && B.rev == rev0 + 1 && v1.size() == rows0 + 1,
                      "B4 a poll-driven re-listing IS a new listing: +1 row, rev +1, "
                      "charlie.npy in it");
                check(!want.empty() && cur >= 0 && curName == want,
                      "B4b ...and the keyboard cursor is still on the SAME row, by name");
                check(ticked.size() == 2 && kept == ticked,
                      "B4c ...and both ticks are still on the SAME two rows");
                fprintf(stderr, "browseselftest: B4 detail: rows %zu->%zu rev %llu->%llu "
                                "cursor %d '%s' (want '%s') ticks [%s] -> [%s]\n",
                        rows0, v1.size(), (unsigned long long)rev0,
                        (unsigned long long)B.rev, cur, curName.c_str(), want.c_str(),
                        say(ticked).c_str(), say(kept).c_str());
                settle();
            }
            {   // ---- B5: a poll that found NOTHING costs the panel nothing ----
                // §5 lets the listing update silently; it does not ask for the
                // rows to be rebuilt every three seconds when the folder has not
                // moved. An answer identical to what is on screen is DROPPED -
                // no rev bump, so no cursor/selection carry, no recents churn
                // and no prefs write - which is what keeps a panel left open on
                // a quiet folder from spending §3's idle promise on nothing.
                const uint64_t rev0 = B.rev;
                const int applied0 = I.pollsApplied;
                const bool dirty0 = app.prefsDirty;
                clk += 100.0;
                I.polledAt = clk - 99.0;
                const bool issued = rbPollRound(I, clk, FR);
                double tq = nowSec();
                while (nowSec() - tq < 120.0) {
                    pumpRemoteBrowse();
                    if (!I.pollPending) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                check(issued && !I.pollPending && B.rev == rev0 &&
                      I.pollsApplied == applied0 && app.prefsDirty == dirty0,
                      "B5 a poll of an unchanged folder replaces nothing: rev, rows and "
                      "prefs all still");
                // ANTI-VACUITY: the comparator that decided that is not simply
                // saying yes to everything. One byte of one row's size, one
                // second of its mtime and one missing row are three different
                // listings.
                std::vector<remote::Entry> a = B.entries, b = B.entries;
                const bool same = rbSameListing(a, b);
                if (!b.empty()) b[0].size++;
                const bool notSize = !rbSameListing(a, b);
                b = a;
                if (!b.empty()) b[0].mtime++;
                const bool notTime = !rbSameListing(a, b);
                b = a;
                if (!b.empty()) b.pop_back();
                check(!a.empty() && same && notSize && notTime && !rbSameListing(a, b),
                      "B5b ...and 'nothing moved' notices one byte, one second and one "
                      "row");
                settle();
            }
            {   // ---- B6: THE COST, stated as a number -------------------------
                // §2 dedupes by (host, port, dir): three stacks open from one
                // folder cost ONE LIST (--rwatch-selftest R3). The Browse poll
                // cannot join that request - the stack watcher owns its own
                // Session on its own thread by §3's "one Session, one owning
                // thread", and for a LOCAL folder it makes no request at all (it
                // walks the directory with directory_iterator, where the panel
                // asks a peer over a pipe). So the honest statement is this one,
                // and it is what is asserted: the panel adds ONE listing per
                // interval per DRAWN INSTANCE - not one per stack, and not one
                // per row. See watch-design §14.4.
                std::vector<int> made;
                for (int k = 0; k < 3; k++) {
                    App::SeqInfo si;
                    si.id = app.nextSeqId++;
                    si.name = "peer stack " + std::to_string(si.id);
                    si.remoteHost = "kepler";
                    si.remotePort = 2222;
                    for (int f = 0; f < 4; f++)
                        si.remoteFiles.push_back(scratch + "/" + names4[f]);
                    si.expectedFrames = 4;
                    app.seqs.push_back(si);
                    made.push_back(si.id);
                }
                std::vector<App::WatchTarget> tg = watchTargetsNow();
                int mine = 0;
                bool oneKey = true;
                for (const auto& t : tg) {
                    bool ours = false;
                    for (int id : made) if (t.seqId == id) ours = true;
                    if (!ours) continue;
                    mine++;
                    if (t.host != "kepler" || t.port != 2222 || t.dir != scratch)
                        oneKey = false;
                }
                const int was = I.pollsIssued;
                clk += 100.0;
                I.polledAt = clk - 99.0;
                rbPollRound(I, clk, FR);
                check(mine == 3 && oneKey && I.pollsIssued == was + 1,
                      "B6 three stacks in the browsed folder are ONE watch key; the "
                      "panel adds ONE listing");
                for (int id : made)
                    for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
                        if (it->id == id) { app.seqs.erase(it); break; }
                settle();
            }
            I.cursor = -1;
            I.cursorKey.clear();
            I.sel.clear();
            I.selKeys.clear();
            I.polledAt = 0;
            app.uiFrame = 0;
            std::filesystem::remove_all(pathFromUtf8(scratch), bec);
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
        {   // ---- THE FIXTURE THIS TEST WAS HANDED IS NOT THIS TEST'S TO EDIT --
            // tools/testdata/rb is shared: --browse-keys-selftest navigates it by
            // POSITION (home, then N x down), so one extra row shifts every
            // gesture it makes and every check after the first fails - for 300 s,
            // in a different test, with nothing pointing here. That is not
            // hypothetical: the bwatch fixture above used to be a subdirectory of
            // this folder and a run that did not reach its own cleanup left one
            // behind, which cost 28 FAIL / 300.8 s in selftest.browse-keys and an
            // afternoon of looking at the wrong branch.
            //
            // So the count is taken at the end and compared with the one taken at
            // the top. It is deliberately about the DIRECTORY and not about any
            // particular fixture: whatever this test grows next, it cannot leak a
            // row into the folder the next test walks without saying so HERE,
            // where the cause is.
            int after = 0;
            std::error_code fec;
            for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(dir), fec))
                { (void)e; after++; }
            fprintf(stderr, "browseselftest: shared fixture %s: %d entr(ies) before, "
                            "%d after: %s\n", dir.c_str(), fixtureEntriesAtStart, after,
                    after == fixtureEntriesAtStart ? "ok" : "FAIL");
            if (after != fixtureEntriesAtStart) ok = false;
        }
        fprintf(stderr, "browseselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopSequenceLoader();
        return ok ? 0 : 1;
    }
