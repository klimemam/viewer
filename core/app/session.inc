// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: session/prefs
static bool openRemote(const std::string& url, bool asPreview, int frame, int npyRead,
                       const rp::RawWire* rw);   // fwd (defaults: first decl)
static void openRemoteStack(const std::string& host, const std::vector<std::string>& files,
                            const std::string& name, int port, int token);   // defaults: first decl
// the two halves of restoring a REMOTE folder stack: ask the peer which files
// are its siblings, then build the stack around the frame that is already open
static bool remoteSequenceSiblings(const std::string& url,
                                   std::vector<std::string>& filesOut,
                                   std::string& nameOut, std::string& err);
static int attachRemoteStack(ImageDoc* first, const std::string& host,
                             const std::vector<std::string>& files,
                             const std::string& name, int port, int token);
// (makeRemoteUrl: extern since P7, declared in core/browse/host.h - which the
// spine includes before this fragment - so no local redeclaration here.)
// ---------------------------------------------------------------- session save/load
// (sequence helpers are defined further down; sessions can restore a stack)
static std::vector<std::string> findSequenceSiblings(const std::string& path,
                                                     std::string& patternOut);
static void startSequenceLoad(int imageIdx, const std::vector<std::string>& files,
                              const std::string& pattern);
static void stopSequenceLoader();
static std::vector<int> framesOfSeq(int seqId);
static App::SeqInfo* seqInfo(int id);   // never held across an open/close: the
                                        // rule is over its definition, sequence.inc
static void selectImage(int idx);
// Plain line-based text format (.vsession): view state + per-image reload recipes.
// Rendering is separate from writing so the crash handler can keep a pre-rendered
// copy and never has to build one while the process is already dying.
static void writeSessionTo(std::ostream& f, int* lostSeriesOut = nullptr,
                           int* lostMembersOut = nullptr,
                           int* lostSetsOut = nullptr,
                           int* lostSetRolesOut = nullptr) {
    f << "viewer-session 1\n";
    f << std::setprecision(9);        // 6 digits silently rounded measured values
    f << "gamma " << app.dispGamma << "\n";
    f << "grid " << (app.showGrid ? 1 : 0) << "\n";
    f << "zoom " << app.view.zoom << "\n";
    f << "center " << app.view.center.x << " " << app.view.center.y << "\n";
    // One image line per stack means saved-line order != app.images order, and a
    // restored stack expands to N frames, so a raw index cannot survive. Save the
    // ordinal of the line that carries the current image instead.
    // A stack contributes exactly one line: the frame it was left on.
    auto isSavedLine = [&](const ImageDoc* d) {
        // A preview is a look, not an open: it is not in the session, and
        // writing one must not turn it into an open. The autosave runs a few
        // seconds after any change and used to promote the preview first, so a
        // frame the user had merely clicked appeared under the batch by itself.
        if (d->preview) return false;
        if (d->src->path.empty()) return false;
        if (d->seqId == 0) return true;
        int rep = -1;
        if (App::SeqInfo* si = seqInfo(d->seqId))
            if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
                app.images[si->lastImageIdx]->seqId == d->seqId)
                rep = si->lastImageIdx;
        if (rep < 0) {                              // never navigated: first frame
            std::vector<int> fr = framesOfSeq(d->seqId);
            if (!fr.empty()) rep = fr.front();
        }
        return rep >= 0 && app.images[rep].get() == d;
    };
    auto savedLineOf = [&](const ImageDoc* d) {
        int ord = 0;
        for (const auto& o : app.images) {
            if (!isSavedLine(o.get())) continue;
            // the current image may not be its stack's saved frame, but it lands
            // on that stack's line either way
            if (o.get() == d || (d->seqId != 0 && o->seqId == d->seqId)) return ord;
            ord++;
        }
        return 0;
    };
    f << "current " << (cur() ? savedLineOf(cur()) : 0) << "\n";
    // display state that lives outside the per-image range
    f << "linkrange " << (app.linkRange ? 1 : 0) << " " << app.linkBlack << " "
      << app.linkWhite << "\n";
    f << "rangescope " << app.rangeScope << "\n";
    f << "cmpfollow " << (app.compareFollowFrame ? 1 : 0) << "\n";
    f << "cmprange " << app.compareRangeMode << "\n";
    // slots travel by the same NAME+frame pair B uses: uids do not survive a
    // restart, and a path alone cannot tell two frames of one stack apart.
    // `refmember` is the OTHER half a path cannot tell apart - two image
    // members of one .npz (or two layers of one .exr) are one path and two
    // pictures. See its parse in loadSession for why it is a companion line.
    for (uint64_t u : app.cmpExtra)
        for (const auto& d : app.images)
            if (d->uid == u) {
                f << "cmpslot " << (d->seqId != 0 ? d->seqIndex : -1) << " " << d->src->path << "\n";
                if (!d->src->member.empty()) f << "refmember " << d->src->member << "\n";
            }
    f << "abstats " << app.abStatsLayout << " " << app.histPlane << "\n";
    f << "roichannel " << app.roiChannel << "\n";
    f << "projstatlayout " << app.projStatLayout << " " << app.projDecimals << " "
      << (app.projAllRow ? 1 : 0) << " " << app.projStatOrder << "\n";
    f << "projection " << app.projMode << " " << app.projYMode << " " << app.projYLo << " "
      << app.projYHi << " " << (app.showProjH ? 1 : 0) << " " << (app.showProjV ? 1 : 0) << "\n";
    f << "analysis " << app.anaSel << " " << (app.anaAuto ? 1 : 0) << " "
      << (app.anaRefOn ? 1 : 0) << " " << app.anaRef << "\n";
    f << "histlog " << (app.histLog ? 1 : 0) << "\n";
    // Series Analysis panel settings (docs/analysis-layers.md §3.3 migration;
    // board row 104's 「設定のセッション未保存」): the fit method and window are
    // part of what a printed number MEANT, so they travel with the session
    // like every other panel's settings. tablePlane and the SNR dB toggle
    // ride along - same panel, same line.
    f << "seriespanel " << app.lin.tablePlane << " " << (app.lin.snrDb ? 1 : 0) << " "
      << app.lin.fitMethod << " " << app.lin.winMode << " "
      << app.lin.winLo << " " << app.lin.winHi << "\n";
    // "npyaxis" used to sit here: one global reading for a whole session
    // (§3.4). Its successor is the per-image "npyread" line below, so nothing
    // global is written any more. An OLD session's npyaxis is skipped like any
    // unknown key - and lands on the same pixels, because the reading it used
    // to force for (F,H,W) is now what native does anyway.
    f << "compare " << app.compareMode << " " << app.wipeFrac << " " << app.splitFrac << " "
      << app.diffGain << " " << (app.diffAbs ? 1 : 0) << " "
      << (app.flipAuto ? 1 : 0) << " " << app.flipPeriod << "\n";
    // uids are per-run, so the file carries name + frame index; the name is last
    // because it may contain spaces
    if (ImageDoc* b = resolveB())
        f << "compareb " << (b->seqId != 0 ? b->seqIndex : -1) << " " << b->name << "\n";
    f << "panels " << (app.showFiles ? 1 : 0) << " " << (app.showInspector ? 1 : 0) << " "
      << (app.showRois ? 1 : 0) << " " << (app.showAnalysis ? 1 : 0) << " "
      << (app.showHistogram ? 1 : 0) << " " << (app.showTemporal ? 1 : 0) << " "
      << (app.showProjection ? 1 : 0) << " " << (app.showSeriesAnalysis ? 1 : 0) << " "
      << (app.showRemote ? 1 : 0) << " " << (app.showSetAnalysis ? 1 : 0) << "\n";
    // Browse places (docs/todo-open.md item 10, user decision A5: auto-
    // reconnect): one "rbplace <num> <url>" line per instance that stands
    // somewhere. placeUrl keeps the port. On load each instance reconnects
    // THROUGH ITS OWN WORKER, asynchronously - the UI never blocks on ssh,
    // and a failure lands inline in that instance's error band. The num keeps
    // "Browse 2###Browse2" the same window the saved layout dock knows.
    // Unknown keys are skipped by older viewers, so this is additive.
    for (const auto& bp : app.browsePanels) {
        const App::RemoteBrowse& pb = bp->b;
        if (!pb.connected && pb.host.empty() && pb.dir == "~") continue;
        f << "rbplace " << bp->num << " "
          << placeUrl(pb.host, pb.port, pb.dir) << "\n";
    }
    // the whole dock arrangement, so a session reopens looking like it did
    if (const char* ini = ImGui::SaveIniSettingsToMemory()) {
        f << "layout_begin\n";
        for (const char* p = ini; *p; ) {          // indent so parsing stays trivial
            const char* e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            f << "|" << std::string(p, n) << "\n";
            if (!e) break;
            p = e + 1;
        }
        f << "layout_end\n";
    }
    // A derived image (a ROI montage) has no file behind it, so isSavedLine
    // drops it - correctly, since nothing could reload it. But dropping it in
    // silence is the one thing this format does not do: say how many.
    int derived = 0, avgs = 0;
    for (auto& d : app.images)
        if (d->src->path.empty()) { derived++; if (!d->avgOfPath.empty()) avgs++; }
    if (derived)
        fprintf(stderr, "session: %d derived image(s) not saved (no file to reload "
                        "them from); %d of those are frame averages, whose RECIPE "
                        "is saved instead\n", derived, avgs);
    for (auto& d : app.images) {
        // Sequences: one line per STACK (the frame that stack was left on), not
        // one per frame - and not only the stack that happens to be on screen.
        if (!isSavedLine(d.get())) continue;
        if (!d->src->member.empty()) f << "member " << d->src->member << "\n";
        // §3.3: the reading is the user's declaration and it sticks across
        // sessions. ADDITIVE, and written only when there IS a declaration - an
        // older viewer skips the unknown key and reloads natively, which is
        // exactly what it would have done with no key at all. It precedes the
        // image line because it decides how that line's file is decoded.
        if (d->src->npyRead != NR_NATIVE) f << "npyread " << d->src->npyRead << "\n";
        f << "lut " << d->displayLut << "\n";
        f << "image " << d->black << " " << d->white << " ";
        if (d->src->rawDtype >= 0) {
            int interp = d->src->rawInterp;
            if (RAW_INTERP_CH[interp] == 1)   // 1ch family: honor the CURRENT interpretation
                interp = d->cfa == 2 ? RI_QUAD : d->cfa == 1 ? RI_BAYER : RI_GRAY;
            f << "raw3 " << d->src->rawDtype << " " << interp << " "
              << (d->src->srcW > 0 ? d->src->srcW : d->w) << " " << (d->src->srcH > 0 ? d->src->srcH : d->h) << " "
              << d->src->rawOffset << " " << (d->src->rawLE ? 1 : 0) << " " << d->cfaPattern << " "
              << (d->cfaColorize ? 1 : 0) << " "
              << d->src->cropX << " " << d->src->cropY << " " << d->w << " " << d->h << " ";
        } else {
            f << "npy3 " << d->cfa << " " << d->cfaPattern << " " << (d->cfaColorize ? 1 : 0) << " "
              << d->src->cropX << " " << d->src->cropY << " " << d->w << " " << d->h << " ";
        }
        f << d->src->path << "\n";      // path last: may contain spaces
        // the batch travels BY NAME: ids do not survive sessions, and equal
        // names merging on restore is the least surprising failure mode
        for (const auto& b : app.batches)
            if (b.id == d->batchId) { f << "imgbatch " << b.name << "\n"; break; }
        if (d->seqId != 0) {
            f << "seqframe " << d->seqIndex << "\n";   // come back to the same frame
            // A folder sequence must rescan its siblings; an in-file frame axis
            // rebuilds the whole stack from the file itself, so asking for a
            // sibling scan there would split the stack in two.
            bool inFile = true;
            for (const auto& o : app.images)
                if (o->seqId == d->seqId && o->src->path != d->src->path) { inFile = false; break; }
            // ...but the head frame of the CURRENT rescan is also the only
            // image with its seqId until pumpSequence integrates the siblings
            // (4 per UI frame - the worker finishing first is the norm, so
            // seqRunning is no witness). That is not an in-file axis (loadNpy
            // builds those whole before anyone can save); it is the window
            // between seqRestore being consumed and integration, and the
            // autosave on quit and the crash snapshot both live inside it.
            // seqLoadingId is the durable marker: it stays on this stack until
            // the stack is closed or the next load starts - and by then the
            // siblings are integrated and !inFile carries the line instead.
            if (!inFile || (app.seqLoadingId != 0 && d->seqId == app.seqLoadingId))
                f << "seqload 1\n";
            // AFTER seqload: for a folder stack the SeqInfo only exists once the
            // rescan ran, and a name applied before that lands on nothing. The
            // name may be user-given ("25C dark") - it must survive the session.
            if (const App::SeqInfo* sqi = seqInfo(d->seqId)) {
                if (!sqi->name.empty()) f << "seqname " << sqi->name << "\n";
                // the per-frame x axis, additive keys an older viewer skips.
                // Values first and EXACTLY (fmtExact): this is an axis data is
                // plotted against. Name and unit last on their lines: spaces.
                if (!sqi->axisVals.empty()) {
                    f << "seqaxisvals";
                    for (double v : sqi->axisVals) f << " " << fmtExact(v);
                    f << "\n";
                    f << "seqaxisunit " << sqi->axisUnit << "\n";
                    f << "seqaxisname " << sqi->axisName << "\n";
                }
            }
        } else {
            // A stack whose sibling rescan is still QUEUED has no SeqInfo yet
            // (seqRestore is drained one at a time, over many frames), so the
            // branch above cannot fire and this head frame would be written as
            // a lone image - taking its series member down with it, because a
            // member resolves through a stack. "seqload 1" is exactly the
            // request that is still outstanding; ask for it again.
            for (const auto& r : app.seqRestore)
                if (r.uid == d->uid) { f << "seqload 1\n"; break; }
        }
    }
    // ---- computed frames: the RECIPE, never the pixels ----------------------
    // A frame average has no file, so isSavedLine dropped it above - correctly,
    // nothing could reload those pixels. What survives instead is the
    // derivation: the stack it averaged, named by the path of that stack's FIRST
    // FRAME (the key series members already travel by, for the same reasons -
    // ids do not survive a session and names are renameable and not unique), and
    // the mean is taken again on restore.
    //
    // The alternative was to give the frame its source's path so an ordinary
    // `image` line would carry it. That is what materializeDerivedFrame does and
    // it is why a derived stack comes back as its whole source; here it would be
    // worse still - the session would reload frame 0 from disk and hand it back
    // under the name "frame average", with the note that said otherwise gone.
    // A computed frame either comes back computed or does not come back.
    int avgLost = 0;
    for (auto& d : app.images) {
        if (d->avgOfPath.empty()) continue;
        bool live = false;                    // its stack must still be open, or
        for (auto& o : app.images)            // there is nothing left to average
            if (o->seqId != 0 && o->src->path == d->avgOfPath &&
                o->src->member == d->avgOfMember) { live = true; break; }
        if (!live) { avgLost++; continue; }
        // Two keys, not one key plus a flag: an older viewer skips lines it
        // does not know (the series block's rule), so a sum saved here simply
        // does not come back there - which is the right failure. A flag on the
        // stackavg line would instead be DROPPED by the old parser, and the
        // sum would come back as a mean: 1/n of itself under the same note.
        f << (d->avgFold == FOLD_SUM ? "stacksum " : "stackavg ")
          << d->avgOfPath << "\n";                  // path last: may contain spaces
        // ...and WHICH member of that file, when the file has members: the
        // recipe used to fold whichever stack of that path came first, so a
        // session holding the mean of `dark` came back holding the mean of
        // `bright` under dark's name. Directly after the line it qualifies.
        if (!d->avgOfMember.empty()) f << "refmember " << d->avgOfMember << "\n";
        // The GENERATION record (§10.2), on its own line right after its
        // recipe (the reader attaches it to the last recipe line): the digest
        // of the input tuples this picture was folded from, how many, and
        // when. The recipe alone recomputes silently from whatever the disk
        // holds next time; the record is what lets the restore SAY whether
        // that was still the same picture. An older viewer drops the unknown
        // key and restores the recipe with no generation claim - the right
        // failure, per the format's one rule. Note this is the DOC's record,
        // not today's disk: a stale average saves the tuples of the pixels it
        // is still showing, so the next restore states the change instead of
        // inheriting a silently refreshed record.
        if (d->avgSrcCount > 0) {
            char gb[64];
            snprintf(gb, sizeof gb, "stackavggen %d %016llx ", d->avgSrcCount,
                     (unsigned long long)d->avgSrcDigest);
            f << gb << d->avgWhen << "\n";          // timestamp last: has spaces
        }
    }
    if (avgLost)
        fprintf(stderr, "session: %d frame average(s) whose source stack is closed - "
                        "not saved, because the frames they averaged are gone\n", avgLost);
    // ---- preprocessor products: the RECIPE, for the same reason -------------
    // docs/analysis-layers.md §5 raised the frame average's rule to the rule for
    // every AnalysisPreProcessor product: no file, so the session carries the
    // derivation and recomputes. ONE line per product stack (not per frame) -
    // the record lives on the SeqInfo, keyed like an average's by the path of
    // the SOURCE stack's first frame.
    //
    // The four numbers before the path are the spec, spelled out rather than
    // packed: a session that could not say WHICH surface was removed would
    // bring the stack back under a name claiming a band it might not have.
    int dtLost = 0;
    for (const auto& s : app.seqs) {
        if (!s.dt.made()) continue;
        if (s.dt.ofPath.empty()) { dtLost++; continue; }
        bool live = false;
        for (auto& o : app.images)
            if (o->seqId != 0 && o->src->path == s.dt.ofPath &&
                o->src->member == s.dt.ofMember) { live = true; break; }
        if (!live) { dtLost++; continue; }
        f << "detrend " << s.dt.spec.method << " " << s.dt.spec.degree << " "
          << s.dt.spec.block << " " << (s.dt.spec.divide ? 1 : 0) << " "
          << s.dt.ofPath << "\n";                   // path last: may contain spaces
        if (!s.dt.ofMember.empty()) f << "refmember " << s.dt.ofMember << "\n";
        // The node is renameable (docs/reader-analysisset.md's rule for every
        // node), so the session owns the name the way seqname does.
        f << "detrendname " << s.name << "\n";
    }
    if (dtLost)
        fprintf(stderr, "session: %d detrend product(s) whose source stack is closed - "
                        "not saved, because the frames they were computed from are gone\n",
                dtLost);
    // ---- series (系列), after the image lines -------------------------------
    // A FLAT block with unique keys, on purpose: an older viewer skips every
    // line it does not know and still opens the session, and this one opens a
    // session with no block as zero series. Nothing else in the format moves.
    //
    // Members travel by the PATH OF THEIR FIRST FRAME. A stack NAME would be
    // the obvious choice and is the wrong one: names are renameable and not
    // unique, so two members could resolve to one stack or to none. A path is
    // what the session already proves it can round-trip: it is how the image
    // lines themselves work. (seqname itself DOES survive now - it rides on
    // the queued App::SeqRestore entry - but that does not make it a key.)
    int lostSeries = 0, lostMembers = 0;
    for (const auto& S : app.series) {
        std::string bn = batchNameOf(S.batchId);
        // no batch, nothing to restore it into - and nothing silently: the load
        // side counts every member it cannot resolve, so the save side owes the
        // same. A series that leaves the file is a sweep's worth of hand-typed
        // parameter values gone.
        if (bn.empty()) { lostSeries++; lostMembers += (int)S.members.size(); continue; }
        f << "series " << S.name << "\n";              // name last on its line: spaces
        f << "seriesbatch " << bn << "\n";             // by NAME, like imgbatch
        f << "seriesparam " << S.paramName << "\n";
        f << "seriesunit " << S.unit << "\n";          // empty line = unit not set
        f << "serieskind " << S.kind << "\n";
        for (const auto& m : S.members) {
            // Three keys, not one key plus flags (the stackavg/stacksum rule,
            // see App::SeriesRestore::M): a stack standing as mean keeps the
            // historic "seriesmember", a stack standing as sum is "seriessum"
            // - so the same line can never bring back a different QUANTITY -
            // and a frame member is "seriesframe", travelling by ITS OWN path
            // (the key a stack member's first frame already uses).
            const char* key2 = "seriesmember ";
            std::string path, member;
            if (m.seqId != 0) {
                std::vector<int> fr = framesOfSeq(m.seqId);
                if (fr.empty()) { lostMembers++; continue; }
                path = app.images[fr.front()]->src->path;
                member = app.images[fr.front()]->src->member;
                if (m.fold == FOLD_SUM) key2 = "seriessum ";
            } else {
                // a computed frame (frame average) has no file, so it cannot
                // be written - counted, like every loss this format admits to
                const ImageDoc* fd = frameMemberDoc(m.frameUid);
                if (!fd || fd->src->path.empty()) { lostMembers++; continue; }
                path = fd->src->path;
                member = fd->src->member;
                key2 = "seriesframe ";
            }
            f << key2;
            // EXACTLY, not to the file-wide 9 digits: this is the axis a fit is
            // taken against, and the modal no longer rounds it either.
            if (std::isfinite(m.value)) f << fmtExact(m.value);
            else f << "-";                             // "-" = unset, NOT zero
            f << " " << (m.include ? 1 : 0) << " " << path << "\n";   // path last: spaces
            // ...and which member of that file, when there is one: two 3-D
            // members of one .npz are one path, and a value that lands on the
            // wrong one is a misplaced point on the axis the fit is taken
            // against - which resolveOnePendingSeries calls worse than a loss.
            if (!member.empty()) f << "refmember " << member << "\n";
        }
        f << "seriesend\n";
    }
    // Series the LOADED file still owes: parsed into seriesRestore and waiting
    // for their stacks, because a member cannot be looked up before its stack
    // exists and that wait is minutes for a remote sweep. app.series is empty
    // for the whole of that window (closeAll cleared it), while Ctrl+S, the
    // 0.4 s crash snapshot, the debounced autosave and the autosave on quit are
    // all live - so a save inside it used to write the series out of existence
    // and toast "session saved" with nothing to report. They round-trip
    // VERBATIM: SeriesRestore holds exactly the fields this format has, members
    // by path included.
    for (const auto& R : app.seriesRestore) {
        lostMembers += R.truncated;      // unreadable in, unwritable out
        if (R.batchName.empty() || R.members.empty()) {
            lostSeries++;
            lostMembers += (int)R.members.size();
            continue;
        }
        f << "series " << R.name << "\n";
        f << "seriesbatch " << R.batchName << "\n";
        f << "seriesparam " << R.paramName << "\n";
        f << "seriesunit " << R.unit << "\n";
        f << "serieskind " << R.kind << "\n";
        for (const auto& m : R.members) {
            f << (m.kind == 2 ? "seriesframe " : m.kind == 1 ? "seriessum "
                                                             : "seriesmember ");
            if (std::isfinite(m.value)) f << fmtExact(m.value);
            else f << "-";
            f << " " << (m.include ? 1 : 0) << " " << m.path << "\n";
            if (!m.member.empty()) f << "refmember " << m.member << "\n";
        }
        f << "seriesend\n";
    }
    // A picker sweep that has not resolved yet cannot be written at all: its
    // members are GROUP names and creation tokens, and this format speaks paths
    // of frames that do not exist yet. Counted, so the toast warns instead of
    // reading clean - which is the whole point of these counters.
    for (const auto& P : app.seriesPending) {
        lostSeries++;
        lostMembers += (int)P.members.size();
    }
    // Legacy per-stack levels waiting on the same drain gate are in the same
    // position: not writable in this format (their stacks do not exist yet),
    // so they must at least be COUNTED, or a save in their window reads clean
    // while destroying every declared level.
    if (!app.seqLevelLegacy.empty()) {
        lostSeries++;
        lostMembers += (int)app.seqLevelLegacy.size();
    }
    if (lostSeries || lostMembers)
        fprintf(stderr, "series: NOT saved: %d series and %d member(s) had nothing to "
                        "restore them from\n", lostSeries, lostMembers);
    if (lostSeriesOut) *lostSeriesOut = lostSeries;
    if (lostMembersOut) *lostMembersOut = lostMembers;
    // ---- analysis sets (docs/reader-analysisset.md §5), AFTER the series ----
    // blocks: a set may bind a series, and the referenced side writes first.
    // The four rules are the series block's, by name (§5): flat unique keys;
    // members travel by first-frame path (stack) / path (frame) / (batch,
    // series name); what cannot be resolved is COUNTED; and the pending
    // blocks below round-trip verbatim while the restore is still waiting.
    int lostSets = 0, lostSetRoles = 0;
    auto writeSetRole = [&](const App::ASetRole& r) {
        if (r.seqId != 0) {
            std::vector<int> fr = framesOfSeq(r.seqId);
            const std::string p = fr.empty() ? std::string()
                                             : app.images[fr.front()]->src->path;
            if (p.empty()) {          // computed pixels: nothing reloads them
                f << "setrole " << r.role << " unbound the bound member has no "
                     "file to come back from\n";
                lostSetRoles++;
                return;
            }
            f << "setrole " << r.role << " stack "
              << batchNameOf(batchOfStack(r.seqId)) << "\t" << p << "\n";
            return;
        }
        if (r.frameUid != 0) {
            for (const auto& d : app.images)
                if (d->uid == r.frameUid) {
                    if (d->src->path.empty()) {
                        f << "setrole " << r.role << " unbound the bound member "
                             "has no file to come back from\n";
                        lostSetRoles++;
                        return;
                    }
                    f << "setrole " << r.role << " frame " << batchNameOf(d->batchId)
                      << "\t" << d->src->path << "\n";
                    return;
                }
            f << "setrole " << r.role << " unbound the bound frame is gone\n";
            lostSetRoles++;
            return;
        }
        if (r.seriesId != 0) {
            for (const auto& s : app.series)
                if (s.id == r.seriesId) {
                    f << "setrole " << r.role << " series " << batchNameOf(s.batchId)
                      << "\t" << s.name << "\n";
                    return;
                }
            f << "setrole " << r.role << " unbound the bound series is gone\n";
            lostSetRoles++;
            return;
        }
        // unbound roles are WRITTEN (§5.1): the declaration is a record, and
        // it does not vanish across a save. The reason rides as restOfLine.
        f << "setrole " << r.role << " unbound "
          << (r.reason.empty() ? std::string("no reason recorded") : r.reason) << "\n";
    };
    for (const auto& S : app.analysisSets) {
        std::string bn = batchNameOf(S.batchId);
        if (bn.empty()) { lostSets++; lostSetRoles += (int)S.roles.size(); continue; }
        f << "analysisset " << S.name << "\n";
        f << "setbatch " << bn << "\n";
        if (!S.readerSpec.empty()) f << "setreader " << S.readerSpec << "\n";
        if (!S.origin.empty())     f << "setorigin " << S.origin << "\n";
        for (const auto& r : S.roles) writeSetRole(r);
        f << "setend\n";
    }
    // Blocks still waiting for their stacks write back VERBATIM (§5 rule 4 -
    // the seriesRestore precedent above, and the same window: the autosave and
    // the crash snapshot are both live while a restore is still draining).
    for (const auto& R : app.aSetRestore) {
        lostSetRoles += R.truncated;
        if (R.batchName.empty()) {
            lostSets++;
            lostSetRoles += (int)R.roles.size();
            continue;
        }
        f << "analysisset " << R.name << "\n";
        f << "setbatch " << R.batchName << "\n";
        if (!R.readerSpec.empty()) f << "setreader " << R.readerSpec << "\n";
        if (!R.origin.empty())     f << "setorigin " << R.origin << "\n";
        for (const auto& rr : R.roles) {
            if (rr.kind == "unbound")
                f << "setrole " << rr.role << " unbound " << rr.reason << "\n";
            else
                f << "setrole " << rr.role << " " << rr.kind << " "
                  << rr.batchName << "\t" << rr.key << "\n";
        }
        f << "setend\n";
    }
    if (lostSets || lostSetRoles)
        fprintf(stderr, "analysisset: NOT saved whole: %d set(s) and %d role(s) "
                        "had nothing to restore them from\n", lostSets, lostSetRoles);
    if (lostSetsOut) *lostSetsOut = lostSets;
    if (lostSetRolesOut) *lostSetRolesOut = lostSetRoles;
    for (const auto& a : app.anns)   // label last: may contain spaces
        f << "ann " << a.type << " " << a.x << " " << a.y << " " << a.w << " " << a.h << " "
          << a.color << " " << (a.visible ? 1 : 0) << " "
          << a.prevX << " " << a.prevW << " " << a.prevY << " " << a.prevH << " "
          << a.label << "\n";     // label last: it may contain spaces
    {   // which annotation was selected, by position in the list above
        int selIdx = -1;
        for (int i = 0; i < (int)app.anns.size(); i++)
            if (app.anns[i].id == app.selectedAnn) selIdx = i;
        f << "selann " << selIdx << "\n";
    }
}

// ---- writing a file without destroying the one that is already there --------
// Opening the target with an ofstream TRUNCATES it. From that instant until the
// last byte lands there is no complete file on disk at all, so a crash, a full
// disk or a machine switched off mid-write leaves nothing where the previous
// session used to be. That is not a rare corner for this particular file:
// autosaveSession() goes through exactly the same call every few seconds,
// unattended, and what it destroys on the way down is the one copy of the
// arrangement the user would want back.
//
// So the target is never opened for writing at all. A NEW file is written
// beside it, put on the disk, and then moved over the target in one directory
// operation - after which a reader sees either the whole old file or the whole
// new one and never a half of either. Every failure leaves the OLD file exactly
// as it was and says, in the platform's own words, what went wrong.
//
// BESIDE the target, in the same directory, is load-bearing rather than tidy:
// rename is a single metadata operation only WITHIN one filesystem. A temp
// under %TEMP% or /tmp is usually another volume, where the rename degrades
// into copy-then-delete - the truncate-and-write this exists to avoid, with an
// extra pass over the data on top.
//
// WHY std::filesystem::rename and not ReplaceFileW. libstdc++ implements
// fs::rename as MoveFileExW(MOVEFILE_REPLACE_EXISTING) on Windows and rename(2)
// elsewhere, so this is one call for the whole matrix. Measured on this
// toolchain (MinGW/UCRT, Windows 11), not assumed:
//
//   case                              fs::rename            ReplaceFileW
//   target absent (any first save)    OK                    FAIL  err 2
//   target present                    OK                    OK
//   target open by this process       FAIL  err 5           FAIL  err 32
//   target open, FILE_SHARE_DELETE    FAIL  err 5           -
//   target FILE_ATTRIBUTE_READONLY    FAIL  err 5           FAIL  err 5
//
// In every failing row the target still held its old bytes - which is the whole
// property being bought here, and both calls have it. They also fail in the
// same places, so ReplaceFileW's one real advantage (the replacement inherits
// the target's ACL, creation time and object id instead of the temp's) buys
// nothing for a file this app creates in the user's own config directory, while
// costing a second code path for the save where the target does not exist yet.
//
// What Windows does that POSIX does not, and the user is therefore told rather
// than left guessing: a READ-ONLY target cannot be replaced (POSIX rename asks
// about the DIRECTORY's permissions, not the file's), and neither can one that
// any process still holds open - not even with FILE_SHARE_DELETE, measured
// above. Both surface as "session NOT saved: <reason>".
static unsigned long currentPid();                  // fwd (with the lock code below)

static std::string savingTempPath(const std::string& utf8Path) {
    // Named after the target so a leftover from a hard crash is obviously
    // attached to the file it was going to become. The pid is in it because two
    // viewers saving to ONE path (a share, "Save As" from both) must not write
    // into one temp: a shared temp would make the rename atomic and the CONTENT
    // interleaved, which is worse than the defect being fixed.
    return utf8Path + ".saving-" + std::to_string(currentPid());
}

// Push the bytes past the OS cache before the rename. The rename is what makes
// the new file THE file, and if the rename reaches the disk while the bytes
// have not, a power cut leaves a session file full of zeroes where a whole old
// one used to be. The DIRECTORY entry is deliberately not synced as well:
// without that, a power cut can leave the OLD name in place - and the old file,
// whole, is an outcome this is allowed to have. It is the only outcome it is
// allowed to have besides the new one.
static void syncFileToDisk(const std::string& utf8Path) {
#if defined(_WIN32)
    int fd = _wopen(pathFromUtf8(utf8Path).wstring().c_str(), _O_RDWR | _O_BINARY);
    if (fd >= 0) { _commit(fd); _close(fd); }   // _commit needs write access
#else
    int fd = open(pathFromUtf8(utf8Path).c_str(), O_WRONLY);
    if (fd >= 0) { if (fsync(fd) != 0) { /* best effort: the rename still gates */ } close(fd); }
#endif
}

// true on success. On failure nothing at utf8Path has been touched and *whyOut
// says why, ready to be shown to a person.
static bool writeFileAtomically(const std::string& utf8Path,
                                const std::function<void(std::ostream&)>& emit,
                                std::string* whyOut) {
    std::string tmp = savingTempPath(utf8Path);
    std::error_code ec;
    auto giveUp = [&](const std::string& why) {
        std::error_code rm;
        std::filesystem::remove(pathFromUtf8(tmp), rm);   // never leave a half one behind
        if (whyOut) *whyOut = why;
        return false;
    };
    {
        std::ofstream f(pathFromUtf8(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return giveUp("cannot create " + baseName(tmp));
        emit(f);
        f.flush();
        // A full disk does not announce itself at open() - it announces itself
        // here, or at close(), and an ofstream that is never asked swallows it.
        // Asking is the difference between "the write failed" and "a session
        // file of the right name and the wrong length".
        if (!f.good()) { f.close(); return giveUp("the write failed (disk full?)"); }
        f.close();
        if (f.fail()) return giveUp("the write could not be completed (disk full?)");
    }
    syncFileToDisk(tmp);
    std::filesystem::rename(pathFromUtf8(tmp), pathFromUtf8(utf8Path), ec);
    if (ec) return giveUp(ec.message());
    return true;
}

// The crash handler holds the autosave file OPEN for the life of the process
// (openCrashFile, further down: a SIGSEGV handler must not be opening files).
// That single fd is what makes the rename above a platform question:
//
//   * Windows refuses to replace a file anyone has open - ERROR_ACCESS_DENIED,
//     measured, and FILE_SHARE_DELETE does not rescue it - so the autosave, the
//     one save that runs unattended, could never complete;
//   * POSIX allows the rename and then leaves that fd pointing at the file that
//     was just replaced, so the next crash would write its snapshot into a file
//     with no name in any directory.
//
// One fix for both: drop the fd, replace the file, take a fresh one on the new
// file. In the window between them a crash writes nothing and the autosave on
// disk is still the previous one - which is the same amount of work a crash
// between two autosaves loses anyway.
static bool releaseCrashFileFor(const std::string& path);   // fwd
static void retakeCrashFile();                              // fwd

static void saveSession(std::string path, bool quiet = false) {
    // A saved session has no transients - but the way to honour that is to
    // leave the preview OUT of the file (isSavedLine), not to promote it into
    // a real open behind the user's back.
    if (path.find('.') == std::string::npos) path += ".vsession";
    int lostSeries = 0, lostMembers = 0, lostSets = 0, lostSetRoles = 0;
    bool retake = releaseCrashFileFor(path);
    std::string why;
    bool ok = writeFileAtomically(path, [&](std::ostream& f) {
        writeSessionTo(f, &lostSeries, &lostMembers, &lostSets, &lostSetRoles);
    }, &why);
    if (retake) retakeCrashFile();
    if (!ok) {
        // The news is not that a write failed; it is that nothing was lost by
        // it - the file they had is still the file they have. The autosave says
        // it in the Messages log instead of a toast: it runs every few seconds
        // with nobody watching, and logMsg drops a repeat of the line it just
        // wrote, so a disk that stays full says this once and not forever.
        std::string msg = "session NOT saved: " + why + " - the previous " +
                          baseName(path) + " is unchanged";
        if (quiet) logMsg(msg, true); else toast(msg, true);
        return;
    }
    if (quiet) return;
    std::string msg = "session saved: " + baseName(path);
    // the load side counts what it could not restore; so does this one now
    if (lostSeries || lostMembers)
        msg += " (" + std::to_string(lostSeries) + " series and " +
               std::to_string(lostMembers) + " series member(s) could NOT be saved)";
    if (lostSets || lostSetRoles)
        msg += " (" + std::to_string(lostSets) + " set(s) and " +
               std::to_string(lostSetRoles) + " role binding(s) could NOT be saved)";
    toast(msg, lostSeries > 0 || lostMembers > 0 || lostSets > 0 || lostSetRoles > 0);
}

static std::string loadNpy(const std::string& path, int npyRead);   // fwd (defined above)

// ---- instance identity (Open in new window, todo-open item 28 / A7) ---------
// Two viewers used to share ONE autosave file: whichever exited last overwrote
// the other's session. Every instance now claims a numbered slot at startup,
// via a lock file beside the autosave. The first instance keeps the historic
// name autosave.vsession (existing sessions and muscle memory unbroken); later
// ones get autosave-2.vsession, -3, ... The lock holds the owner's PID: a lock
// whose PID is no longer alive is stale - a crash leaves one behind on purpose,
// because "stale lock" is precisely what marks that autosave as reclaimable
// (and offerable by the recovery scan below).
static int g_instanceSlot = 1;            // which autosave-N this run owns
static std::string g_instanceLockPath;    // the lock we hold (empty = none)
// --secondary: this window was spawned by "Open in new window". Secondary
// windows READ prefs but never WRITE them - last-writer-wins over one file is
// how a scratch window would overwrite the layout the main one meant to keep.
// Two copies started BY HAND both stay "first" for prefs, which is exactly
// today's behaviour and accepted: the lock files still keep their AUTOSAVES
// apart, and that is the collision that eats data.
static bool g_secondary = false;

static std::string viewerConfigDir() {
    std::error_code ec;
    std::filesystem::path cfg;
#if defined(_WIN32)
    if (const char* ad = getenv("APPDATA")) cfg = std::filesystem::u8path(ad);
#else
    if (const char* hm = getenv("HOME")) cfg = std::filesystem::u8path(hm) / ".config";
#endif
    if (cfg.empty()) return {};
    cfg /= "viewer";
    std::filesystem::create_directories(cfg, ec);
    if (ec) return {};
    return cfg.u8string();
}

static unsigned long currentPid() {
#if defined(_WIN32)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static bool pidAlive(unsigned long pid) {
    if (!pid) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;  // alive, just not ours
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

// slot 1 = the name every existing session and script already knows
static std::string slotAutosaveName(int n) {
    return n <= 1 ? "autosave.vsession"
                  : "autosave-" + std::to_string(n) + ".vsession";
}
static std::string slotLockPath(const std::string& dir, int n) {
    return dir + "/autosave-" + std::to_string(n) + ".lock";
}
static unsigned long lockPid(const std::string& lockPath) {   // 0 = unreadable
    std::vector<uint8_t> b;
    if (!readFileBytes(lockPath, b)) return 0;
    return strtoul(std::string(b.begin(), b.end()).c_str(), nullptr, 10);
}
// O_EXCL IS the claim: two instances racing for one slot cannot both win it.
static bool createLockExcl(const std::string& lockPath, unsigned long pid) {
#if defined(_WIN32)
    int fd = _wopen(pathFromUtf8(lockPath).wstring().c_str(),
                    _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = open(pathFromUtf8(lockPath).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
#endif
    if (fd < 0) return false;
    std::string s = std::to_string(pid);
#if defined(_WIN32)
    _write(fd, s.data(), (unsigned)s.size());
    _close(fd);
#else
    ssize_t ign = write(fd, s.data(), s.size()); (void)ign;
    close(fd);
#endif
    return true;
}
// Claim the lowest free slot under dir. A slot is free when its lock is absent
// or stale (its PID is dead). Losing the re-claim race to another starting
// instance just moves on to the next slot.
static int claimAutosaveSlot(const std::string& dir, std::string& lockPathOut) {
    unsigned long self = currentPid();
    for (int n = 1; n <= 64; n++) {
        std::string lp = slotLockPath(dir, n);
        if (createLockExcl(lp, self)) { lockPathOut = lp; return n; }
        if (pidAlive(lockPid(lp))) continue;       // that instance is running
        std::error_code ec;                        // stale: reclaim it
        std::filesystem::remove(pathFromUtf8(lp), ec);
        if (createLockExcl(lp, self)) { lockPathOut = lp; return n; }
    }
    lockPathOut.clear();     // 64 live instances, or an unwritable config dir:
    return 1;                // behave like the historic single instance
}
static void releaseAutosaveLock() {
    if (g_instanceLockPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(g_instanceLockPath), ec);
    g_instanceLockPath.clear();
}

// ---- crash / exit safety net -------------------------------------------------
// The work that is expensive to redo is the arrangement (which files, which
// ROIs, which range), not the pixels: autosave it so a crash costs nothing.
static std::string autosavePath() {
    std::string dir = viewerConfigDir();
    if (dir.empty()) return {};
    return dir + "/" + slotAutosaveName(g_instanceSlot);
}

// The recovery offer: the NEWEST autosave under dir that no LIVE instance owns.
// A lock held by selfPid is ours - offering our own previous session is the
// historic "Restore last session"; a lock held by another live PID means that
// window is running and rewriting its file right now, and restoring it would
// fork a session that is still open. nameOut says WHICH file is being offered,
// because with several candidates "last session" alone no longer names one.
static std::string recoveryOfferIn(const std::string& dir, unsigned long selfPid,
                                   std::string* nameOut) {
    if (nameOut) nameOut->clear();
    if (dir.empty()) return {};
    std::string best, bestName;
    std::filesystem::file_time_type bestT{};
    std::error_code ec;
    for (int n = 1; n <= 64; n++) {
        std::string name = slotAutosaveName(n);
        std::filesystem::path p = pathFromUtf8(dir + "/" + name);
        if (!std::filesystem::exists(p, ec)) continue;
        unsigned long owner = lockPid(slotLockPath(dir, n));
        if (owner && owner != selfPid && pidAlive(owner)) continue;  // live, not us
        std::filesystem::file_time_type t = std::filesystem::last_write_time(p, ec);
        if (ec) continue;
        if (best.empty() || t > bestT) { best = (dir + "/" + name); bestName = name; bestT = t; }
    }
    if (nameOut) *nameOut = bestName;
    return best;
}

static void autosaveSession() {
    std::string p = autosavePath();
    if (!p.empty() && !app.images.empty()) saveSession(p, true);
}

// ---- preferences -------------------------------------------------------------
// How the app behaves, as opposed to what is open: it belongs to the user, not to
// a session file, and must survive a machine that never gets a clean shutdown.
static std::string prefsPath() {
    std::string p = autosavePath();
    if (p.empty()) return p;
    size_t slash = p.find_last_of("/\\");
    return p.substr(0, slash == std::string::npos ? 0 : slash + 1) + "prefs.txt";
}

// ---- the window's own geometry ----------------------------------------------
// The panels remember where they are (layout.ini); the window around them did
// not, so every start began by dragging it back to the size it had yesterday.
// It goes in prefs.txt with everything else that answers "how does this app
// come up" - one file, one writer, one place to look.
//
// UNITS, because the two disagree exactly when it matters. The SIZE is stored
// in LOGICAL pixels: screen coordinates divided by the same uiScale startup
// derives from the monitor's content scale (1.0 on macOS, where Cocoa's
// coordinates are already points). What the user chose when they sized the
// window is an amount of CONTENT - this many rows of the Files list beside a
// canvas this wide - and holding that same content at 200 % takes twice the
// physical pixels. Storing logical is what makes a size chosen on the 4K dock
// still mean the same window on the laptop panel. The POSITION is stored raw:
// desktop coordinates are a single global space, and scaling a location in it
// would only move the window somewhere nobody asked for.
struct WinRect { int x = 0, y = 0, w = 0, h = 0; };

static const int WIN_DEF_W  = 1600, WIN_DEF_H  = 1000;  // the size before any of this
static const int WIN_MIN_W  = 640,  WIN_MIN_H  = 400;   // a saved 1x1 is not a window
static const int WIN_GRAB_W = 160,  WIN_GRAB_H = 32;    // the strip you drag it by

static long long rectOverlap(const WinRect& a, const WinRect& b) {
    long long w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    long long h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return w > 0 && h > 0 ? w * h : 0;
}

// Monitors get unplugged. A window restored onto a screen that is no longer
// there cannot be reached with a mouse, and that is strictly worse than
// forgetting where it was - the user who hits it has no way back except editing
// prefs.txt by hand. So the saved rectangle has to earn its position:
//
//   * the monitor whose work area holds the most of it decides everything (work
//     area, so the taskbar or dock is already out of the way);
//   * the POSITION is kept only if at least a quarter of the window lands in
//     that work area AND a 160x32 strip along its top edge - the part you grab
//     to drag it - is inside as well. The strip is measured downwards from the
//     window's CONTENT top, which is what GLFW reports and sets; a system title
//     bar sits in the frame just above it, so "content top at or below the work
//     area top" is the honest form of "the title bar is reachable";
//   * failing either test the position is dropped and the window manager places
//     the window as it would on a first run. The SIZE still comes back: it is
//     what the user actually complained about, and a size cannot be off-screen;
//   * the size is clamped to that work area either way, so a window sized on a
//     4K screen never comes up larger than the panel it is restored onto.
//
// Returns true when the caller should apply r.x/r.y as well as r.w/r.h. Pure,
// and tested that way (--newwin-selftest N8): a monitor set that no longer
// contains the saved rectangle is the case nobody can reproduce by hand later.
static bool fitSavedWindow(WinRect& r, const std::vector<WinRect>& work) {
    r.w = std::max(r.w, WIN_MIN_W);
    r.h = std::max(r.h, WIN_MIN_H);
    if (work.empty()) return false;          // nothing answered: keep the size, place it nowhere
    size_t best = 0;
    long long bestOv = 0;
    for (size_t i = 0; i < work.size(); i++) {
        long long ov = rectOverlap(r, work[i]);
        if (ov > bestOv) { bestOv = ov; best = i; }
    }
    // No overlap anywhere: the primary still gets to bound the size.
    const WinRect& wa = work[bestOv > 0 ? best : 0];
    long long area = (long long)r.w * r.h;
    int grabW = std::min(r.x + r.w, wa.x + wa.w) - std::max(r.x, wa.x);
    int grabH = std::min(r.y + WIN_GRAB_H, wa.y + wa.h) - std::max(r.y, wa.y);
    bool keep = bestOv * 4 >= area && grabW >= WIN_GRAB_W && grabH >= WIN_GRAB_H;
    r.w = std::min(r.w, wa.w);
    r.h = std::min(r.h, wa.h);
    if (!keep) return false;
    // Kept, but not allowed to hang off the screen it was kept for.
    r.x = std::clamp(r.x, wa.x, wa.x + wa.w - r.w);
    r.y = std::clamp(r.y, wa.y, wa.y + wa.h - r.h);
    return true;
}

// Every preference the user has ever set, in the order loadPrefs reads them.
// Split out of savePrefs for the same reason writeSessionTo is split out of
// saveSession: the bytes are one thing and getting them onto the disk safely is
// another, and only the second one has a failure worth talking about.
static void writePrefsTo(std::ostream& f) {
    f << "viewer-prefs 1\n";
    f << "theme " << app.themeVariant << " " << app.themeAccent << "\n";
    f << "compact " << (app.compactUi ? 1 : 0) << "\n";
    f << "dragpans " << (app.dragPans ? 1 : 0) << "\n";
    f << "wheelzoom " << (app.wheelZoomPlain ? 1 : 0) << "\n";
    f << "fitonswitch " << (app.fitOnSwitch ? 1 : 0) << "\n";
    f << "watchfiles " << (app.watchEnabled ? 1 : 0) << "\n";
    f << "watchauto " << (app.watchAutoReload ? 1 : 0) << "\n";
    f << "seqload " << app.seqLoadMode << "\n";
    f << "showfps " << (app.showFps ? 1 : 0) << "\n";
    f << "lowbandwidth " << (app.lowBandwidth ? 1 : 0) << "\n";
    f << "membudget " << app.memBudgetGB << "\n";
    f << "procpolicy " << app.procPolicy << "\n";
    f << "linunit " << app.lin.unit << "\n";
    f << "gamma " << app.dispGamma << "\n";
    f << "grid " << (app.showGrid ? 1 : 0) << "\n";
    f << "frame " << app.frameMode << "\n";
    // The window: logical size, raw position, then the maximized flag (see
    // fitSavedWindow). Written only once a real window has reported a geometry,
    // so a run that never samples one - every scripted run - hands back exactly
    // the bytes it read.
    if (app.winW > 0 && app.winH > 0)
        f << "window " << app.winX << " " << app.winY << " "
          << app.winW << " " << app.winH << " " << (app.winMax ? 1 : 0) << "\n";
    f << "rbflat " << (app.rbFlat ? 1 : 0) << "\n";
    // ("rbadv" - the Browse drawer's open/closed state - is no longer written.
    //  A prefs file that still carries one is read and ignored, below.)
    f << "rbtree " << (app.rbTree ? 1 : 0) << "\n";
    f << "rbnatural " << (app.rbNatural ? 1 : 0) << "\n";
    // paths may contain spaces: value is the rest of the line
    if (!app.remoteExe.empty()) f << "remoteexe " << app.remoteExe << "\n";
    if (!app.lastRemoteUrl.empty()) f << "remoteurl " << app.lastRemoteUrl << "\n";
    for (const auto& b : app.rbBookmarks) f << "rbookmark " << b << "\n";
    for (const auto& r : app.rbRecents)   f << "rbrecent " << r << "\n";
    if (!app.pythonExe.empty()) f << "pythonexe " << app.pythonExe << "\n";
    // §4.13.2: the registered reader places, IN ORDER - the order is the setting
    // as much as the paths are, because first-match-wins is only answerable when
    // the order holds still. ("readerdir", the single folder these replaced, is
    // read once more below and then written out here as a place; it is never
    // written again, so an old prefs.txt is migrated by being loaded and saved.)
    for (const auto& p : app.readerPlaces) f << "readerplace " << p << "\n";
    // §4.12: path -> reader, most recent first. A TAB separates them because
    // both halves can contain spaces and only one of them can contain a tab
    // (no filesystem this tool runs on puts one in a path).
    for (const auto& m : app.readerMemo) f << "readerfor " << m.path << "\t" << m.spec << "\n";
}

static void savePrefs() {
    if (g_secondary) {
        // Secondary windows read prefs but never write them: prefs.txt is one
        // file and last-writer-wins, so a spawned scratch window saving on exit
        // would overwrite what the main window meant to keep. Said once, in the
        // log rather than a toast: it is an explanation, not an event.
        static bool said = false;
        if (!said) {
            said = true;
            logMsg("prefs are read-only in a secondary window");
        }
        return;
    }
    std::string p = prefsPath();
    if (p.empty()) return;
    // Beside the file, then over it - the same mechanism saveSession uses and
    // for a worse case. An ofstream on the TARGET truncates, so from that line
    // until the last byte lands there was no whole prefs.txt anywhere, and this
    // is the file that is rewritten on every toggle and once more on the way
    // out, i.e. during shutdown, which is precisely when a machine gets
    // switched off. There is also no second copy of it: a lost session is the
    // one the user saved by hand and can save again, a lost prefs.txt is every
    // preference they have ever set, their bookmarks and their recents.
    std::string why;
    if (!writeFileAtomically(p, [](std::ostream& f) { writePrefsTo(f); }, &why))
        // In the log, never a toast: this runs on every toggle and from the
        // exit path, where a toast would be drawn into a window that is already
        // going away. The news is the same as the session's - the file they had
        // is still the file they have - and logMsg drops an immediate repeat,
        // so a disk that stays full says it once.
        logMsg("preferences NOT saved: " + why +
               " - the previous prefs.txt is unchanged", true);
}

// Keys the last loadPrefs() did not recognise, in file order. A key that
// writePrefsTo emits and loadPrefs drops is a setting that silently reverts,
// which is a defect and not a compatibility case - N11 asserts this list is
// empty for the file the writer itself produces. Reading a key that is NOT
// written is the opposite and is legitimate (a migration): that direction is
// deliberately not policed here. Whether an unknown key should also be named
// on screen is #50 §9-6 and is not decided yet, so nothing outside the test
// looks at this.
static std::vector<std::string> g_prefsUnknownKeys;

static void loadPrefs() {
    g_prefsUnknownKeys.clear();
    std::vector<uint8_t> buf;
    std::string p = prefsPath();
    if (p.empty() || !readFileBytes(p, buf)) return;
    std::istringstream ss(std::string(buf.begin(), buf.end()));
    std::string line;
    std::getline(ss, line);                       // header
    if (line.compare(0, 13, "viewer-prefs ") != 0) return;
    while (std::getline(ss, line)) {
        std::istringstream ls(line);
        std::string key; ls >> key;
        int v = 0;
        if      (key == "theme")       { ls >> app.themeVariant >> app.themeAccent; }
        else if (key == "compact")     { ls >> v; app.compactUi = v != 0; }
        else if (key == "dragpans")    { ls >> v; app.dragPans = v != 0; }
        else if (key == "wheelzoom")   { ls >> v; app.wheelZoomPlain = v != 0; }
        else if (key == "fitonswitch") { ls >> v; app.fitOnSwitch = v != 0; }
        else if (key == "watchfiles")  { ls >> v; app.watchEnabled = v != 0; }
        // §9. A prefs.txt written before this existed has no such line, so the
        // in-class default (OFF) is what an upgrading user gets - which is the
        // only answer: nobody has opted in to anything by upgrading.
        else if (key == "watchauto")   { ls >> v; app.watchAutoReload = v != 0; }
        else if (key == "seqload")     { ls >> app.seqLoadMode; }
        else if (key == "showfps")     { ls >> v; app.showFps = v != 0; }
        else if (key == "lowbandwidth"){ ls >> v; app.lowBandwidth = v != 0; }
        else if (key == "membudget")   { ls >> app.memBudgetGB;
                                         if (app.memBudgetGB > 0)
                                             app.memBudgetGB = std::clamp(app.memBudgetGB, 0.5f, 4096.0f); }
        else if (key == "procpolicy")  { ls >> app.procPolicy;
                                         app.procPolicy = std::clamp(app.procPolicy, 0, 2);
                                         if (app.procPolicy == App::PolServerRetired)
                                             app.procPolicy = App::PolAuto; }
        // The PREFILL a newly created series starts from - not the unit of any
        // measurement (state.h says so where it is declared; what an axis
        // prints comes from Series::unit, where empty stays empty). prefs.txt
        // is its owner: writePrefsTo has always written it here, and nothing
        // read it back, so changing the unit and restarting gave "lx" again.
        // Value is the rest of the line - "cd/m2" has no space in it, but a
        // unit the user typed may.
        else if (key == "linunit")     {
            std::string u;
            std::getline(ls, u);
            while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(0, 1);
            while (!u.empty() && (u.back() == '\r' || u.back() == '\n' ||
                                  u.back() == ' '  || u.back() == '\t')) u.pop_back();
            // An empty value is not a unit. Leaving the default in place is the
            // same rule the series modal follows: an empty box stays empty and
            // never silently becomes "lx" on an axis.
            if (!u.empty())
                snprintf(app.lin.unit, sizeof app.lin.unit, "%s", u.c_str());
        }
        else if (key == "gamma")       { ls >> app.dispGamma; }
        else if (key == "grid")        { ls >> v; app.showGrid = v != 0; }
        else if (key == "frame")       { ls >> app.frameMode;
                                         app.frameMode = std::clamp(app.frameMode, 0, 1); }
        else if (key == "window")      {
            // All five or none: a truncated or hand-edited line means "nothing
            // saved", not a 0x0 window at wherever the reads gave up. The upper
            // bound is not a display size, it is the point past which the value
            // is certainly garbage - fitSavedWindow does the real clamping, and
            // it needs a monitor list this path does not have.
            int mx = 0;
            ls >> app.winX >> app.winY >> app.winW >> app.winH >> mx;
            app.winMax = mx != 0;
            if (ls.fail() || app.winW <= 0 || app.winH <= 0 ||
                app.winW > 32767 || app.winH > 32767) {
                app.winW = app.winH = 0;
                app.winMax = false;
            }
        }
        else if (key == "rbflat")      { ls >> v; app.rbFlat = v != 0; }
        else if (key == "rbadv")       { ls >> v; }   // the drawer is gone: read, drop
        else if (key == "rbtree")      { ls >> v; app.rbTree = v != 0; }
        // absent from a prefs file written before the toggle existed, which is
        // exactly right: the default this initialises to IS natural
        else if (key == "rbnatural")   { ls >> v; app.rbNatural = v != 0; }
        else if (key == "remoteexe" || key == "remoteurl" ||
                 key == "rbookmark" || key == "rbrecent" ||
                 key == "pythonexe" || key == "readerfor" || key == "readerdir" ||
                 key == "readerplace") {
            std::string s;
            std::getline(ls, s);
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            if (s.empty()) continue;
            if      (key == "remoteexe") app.remoteExe = s;
            else if (key == "remoteurl") app.lastRemoteUrl = s;
            else if (key == "rbookmark") app.rbBookmarks.push_back(s);
            else if (key == "pythonexe") app.pythonExe = s;
            // The one folder that "readerplace" replaced (§4.13.2). Still read,
            // and read as the FIRST place, so an existing prefs.txt keeps
            // finding the readers it was finding yesterday. It is not written
            // back under this key, so the migration completes on the next save
            // - and it is deduplicated, because loadPrefs is called twice in one
            // process by the selftests and appending would double the list.
            else if (key == "readerdir" || key == "readerplace") {
                bool dup = false;
                for (const auto& q : app.readerPlaces) if (q == s) dup = true;
                if (!dup && app.readerPlaces.size() < 32) app.readerPlaces.push_back(s);
            }
            else if (key == "readerfor") {
                // The cap is enforced on the way IN as well as on the way out:
                // a prefs file edited by hand (or written by a future version
                // with a larger cap) must not grow this list without bound.
                size_t tab = s.find('\t');
                if (tab == std::string::npos || app.readerMemo.size() >= 64) continue;
                app.readerMemo.push_back({ s.substr(0, tab), s.substr(tab + 1) });
            }
            else if (app.rbRecents.size() < 10) app.rbRecents.push_back(s);
        }
        else if (!key.empty()) g_prefsUnknownKeys.push_back(key);
    }
    app.themeVariant = std::clamp(app.themeVariant, 0, 1);
    app.themeAccent = std::clamp(app.themeAccent, 0, ui_theme::accentCount() - 1);
    app.seqLoadMode = std::clamp(app.seqLoadMode, 0, 2);
    app.dispGamma = app.dispGamma > 1.5f ? 2.2f : 1.0f;
}

// ---- input adapters: running one, and remembering which one ------------------
// docs/input-adapters.md §4.12 / §4.13. The viewer never goes looking for an
// adapter and never applies one it was not told to apply: everything below
// starts from a spec the user chose, or from the record of a choice they made
// earlier. That is the whole trust model, and it is why there is no search path.
static const size_t READER_MEMO_MAX = 64;      // §4.12: bounded, LRU
static std::string selfExePath();              // fwd: the harness sits beside the binary

// How many times Python was actually started for a load. The cache's promise is
// "a second open does not re-run it", and a promise about a thing that does not
// happen can only be tested by counting it.
static int g_adapterRuns = 0;

static std::string readerFileOf(const std::string& spec) {
    size_t c = spec.rfind(':');
    return c == std::string::npos ? spec : spec.substr(0, c);
}
static std::string readerFuncOf(const std::string& spec) {
    size_t c = spec.rfind(':');
    return c == std::string::npos ? std::string("load") : spec.substr(c + 1);
}

static std::string readerFor(const std::string& path) {
    for (const auto& m : app.readerMemo) if (m.path == path) return m.spec;
    return {};
}

static void rememberReader(const std::string& path, const std::string& spec) {
    for (size_t i = 0; i < app.readerMemo.size(); i++)
        if (app.readerMemo[i].path == path) { app.readerMemo.erase(app.readerMemo.begin() + (long)i); break; }
    app.readerMemo.insert(app.readerMemo.begin(), { path, spec });   // most recent first
    if (app.readerMemo.size() > READER_MEMO_MAX) app.readerMemo.resize(READER_MEMO_MAX);
    savePrefs();
}

static void forgetReader(const std::string& path) {
    for (size_t i = 0; i < app.readerMemo.size(); i++)
        if (app.readerMemo[i].path == path) {
            app.readerMemo.erase(app.readerMemo.begin() + (long)i);
            savePrefs();
            return;
        }
}

// The harness that runs adapters. Searched relative to the binary and the
// working directory - never guessed at, and its absence is reported as itself.
static std::string runAdapterScript() {
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path exe = pathFromUtf8(selfExePath());
    if (!exe.empty()) {
        std::filesystem::path d = exe.parent_path();
        for (int i = 0; i < 3 && !d.empty(); i++) { roots.push_back(d); d = d.parent_path(); }
    }
    roots.push_back(std::filesystem::current_path(ec));
    for (const auto& r : roots) {
        std::filesystem::path p = r / "tools" / "import" / "run_adapter.py";
        if (std::filesystem::exists(p, ec)) return p.u8string();
    }
    return {};
}

// ---- where readers are looked for (docs/input-adapters.md §4.13.2) ----------
// The ruling on issue #51 (2026-08-10): 「reader のファイルもしくはフォルダを
// 登録できるようにすればよいですかね.」 Before it the search path was ONE user
// folder plus one hard-coded folder beside the harness, so a person with
// readers in two places - a project's, a team share, the one odd file in a
// scratch directory - simply could not be found. Now the places are a LIST, a
// place is a file or a folder, and the list is the user's, in their order.
static const size_t READER_PLACES_MAX = 32;

struct ReaderPlace {
    std::string path;
    bool shipped = false;      // the adapters beside the harness; always last
    bool exists  = false;      // a registered place that has gone away is REPORTED
    bool isDir   = false;
    int  readers = 0;          // .py files in it, when it is a folder that is there
};

// What a place offers. A folder offers every .py in it (sorted, so the list a
// user reads is the list twice running); a file offers itself, whatever it is
// called - they registered that file, and second-guessing the name here would
// be this function inventing a rule the user did not ask for.
static std::vector<std::string> readersInPlace(const ReaderPlace& p) {
    std::vector<std::string> out;
    if (!p.exists) return out;                  // nothing, and the caller says so
    if (!p.isDir) { out.push_back(p.path); return out; }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(pathFromUtf8(p.path), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".py") continue;
        out.push_back(e.path().u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static ReaderPlace statReaderPlace(const std::string& path, bool shipped) {
    ReaderPlace p;
    p.path = path;
    p.shipped = shipped;
    std::error_code ec;
    auto st = std::filesystem::status(pathFromUtf8(path), ec);
    p.exists = std::filesystem::exists(st);
    p.isDir  = std::filesystem::is_directory(st);
    if (p.exists) p.readers = (int)readersInPlace(p).size();
    return p;
}

// Every place, IN SEARCH ORDER: the registered ones first, in the order the user
// registered them, and the folder that ships with the viewer last.
//
// Why registered-first. The shipped adapters are examples, and the tree they sit
// in is what an update replaces. If shipped won, an update that added a
// `npz_keys.py` would take that name away from the user's own `npz_keys.py` -
// their readings would change with nobody having touched a setting. The other
// way round an update can never shadow anything: yours beats ours, always. It
// is also the only order that leaves an override possible at all, since a
// packaged install may be read-only.
//
// Why the user's own order rather than alphabetical or newest-first: it is the
// only order they can both see AND control, and first-match-wins is only
// answerable when the order holds still.
static std::vector<ReaderPlace> readerSearchPath() {
    std::vector<ReaderPlace> out;
    for (const auto& p : app.readerPlaces) out.push_back(statReaderPlace(p, false));
    std::string script = runAdapterScript();
    if (!script.empty())
        out.push_back(statReaderPlace(
            (pathFromUtf8(script).parent_path() / "adapters").u8string(), true));
    return out;
}

// "Here is where I looked" - a LIST, not a single guess. §3.2's rule for
// anything that cannot be found is that the program says what it looked for, and
// for a search path that means every place, in order, with what is in each and
// WHICH ONES ARE NOT THERE ANY MORE. A registered folder that has gone away is
// the failure this report exists for: dropping it silently would leave a user
// staring at a viewer that stopped finding readers it found last week.
static std::string readerPlacesReport(const std::vector<ReaderPlace>& places) {
    size_t reg = 0;
    for (const auto& p : places) if (!p.shipped) reg++;
    std::string s = "  looked in " + std::to_string(places.size()) + " place(s): " +
                    std::to_string(reg) + " registered, in the order they were registered" +
                    (places.size() > reg ? ", then the one that ships with the viewer" : "") +
                    ":\n";
    if (places.empty()) s += "    (none - no place has been registered and this build "
                             "has no tools/import/adapters beside it)\n";
    int n = 0;
    for (const auto& p : places) {
        std::string what = !p.exists
            ? std::string("MISSING - registered, but not there now")
            : p.isDir ? ("folder, " + std::to_string(p.readers) + " reader(s)")
                      : std::string("file");
        if (p.shipped) what += ", ships with the viewer";
        s += "    " + std::to_string(++n) + ". " + dispPath(p.path) + "  (" + what + ")\n";
    }
    return s;
}

// A panel redraws at the frame rate, and a place can be a share that has gone
// away - a stat() on one of those blocks for seconds. Walking up to 32 of them
// sixty times a second would wedge the window on exactly the machine this
// feature exists for. So the DRAWING side reads a snapshot refreshed at most
// twice a second, or the moment the list itself changes: a reader saved in an
// editor still shows up within half a second, so §4.13.0's loop is untouched,
// and a button press is never waiting on a timer. Only the drawing uses this -
// resolving a reader for a run takes the live answer, every time.
static unsigned g_readerPlacesRev = 0;      // bumped by every edit to the list
struct ReaderPlaceView { ReaderPlace place; std::vector<std::string> readers; };
static const std::vector<ReaderPlaceView>& readerPlacesForUi() {
    static std::vector<ReaderPlaceView> cache;
    static double at = -1e9;
    static unsigned rev = (unsigned)-1;
    double t = nowSec();
    if (rev != g_readerPlacesRev || t - at > 0.5) {
        cache.clear();
        for (const auto& p : readerSearchPath()) cache.push_back({ p, readersInPlace(p) });
        at = t;
        rev = g_readerPlacesRev;
    }
    return cache;
}

// The result of looking a reader up by name. `shadowed` is not decoration: under
// first-match-wins the answer to "which acme.py ran?" is a fact about the ORDER,
// and an order that decides in silence is the guess this repo refuses everywhere
// else.
struct ReaderLookup {
    std::string file;                     // resolved; empty means it was not found
    std::string place;                    // the place it came from
    std::vector<std::string> shadowed;    // the same name, later in the order
    std::string why;                      // the whole refusal, when file is empty
};

static bool readerSpecNamesAPath(const std::string& f) {
    return f.find('/') != std::string::npos || f.find('\\') != std::string::npos;
}

// Turn the FILE half of a spec into a path that exists, or into the sentence
// saying why not.
//
// A spec that names a PATH is used as it stands. That is what the memory holds
// (§4.12), so registering a place can never change how an already-remembered
// file opens: registration is additive BY CONSTRUCTION, not by promise.
//
// A spec that is a BARE NAME is looked up along the search path, and the FIRST
// MATCH WINS. The alternative - refusing on a name collision - was rejected
// because it makes registering a place a DESTRUCTIVE act: add a folder that
// happens to hold a `util.py` and every already-working `util.py` stops. What
// the collision gets instead is a name: the loser is listed, here and in the
// panel, so "why did that one run?" always has an answer.
static ReaderLookup resolveReaderFile(const std::string& spec) {
    ReaderLookup r;
    std::string want = readerFileOf(spec);
    std::vector<ReaderPlace> places = readerSearchPath();
    std::error_code ec;
    if (readerSpecNamesAPath(want)) {
        if (std::filesystem::is_regular_file(pathFromUtf8(want), ec)) { r.file = want; return r; }
        // The remembered reader is gone - the other half of what issue #51 asked
        // ("リーダのファイルが消えたとき憶えている対応をどうするか"). We do NOT
        // quietly run a different file of the same name: §4.13 says only an
        // explicitly chosen reader runs, and a substitution made behind the
        // user's back is precisely that. Name the candidate and let them pick it
        // - one click, and the pick re-remembers this file against what ran.
        std::string base = baseName(want);
        std::vector<std::string> also;
        for (const auto& p : places)
            for (const auto& f : readersInPlace(p))
                if (baseName(f) == base) also.push_back(f);
        r.why = base + ": the reader this file was opened with is not there any more\n"
                "  " + dispPath(want) + "  (remembered, gone)\n" + readerPlacesReport(places);
        if (!also.empty()) {
            r.why += "  a file named " + base + " IS registered:\n";
            for (const auto& f : also) r.why += "    " + dispPath(f) + "\n";
            r.why += "  choose it in the Reader panel - nothing runs until you do";
        } else {
            r.why += "  register the file, or the folder it moved to: Reader panel > "
                     "where readers are looked for > Add file... / Add folder...";
        }
        return r;
    }
    for (const auto& p : places)
        for (const auto& f : readersInPlace(p))
            if (baseName(f) == want) {
                if (r.file.empty()) { r.file = f; r.place = p.path; }
                else r.shadowed.push_back(f);
            }
    if (!r.file.empty()) return r;
    r.why = want + ": no registered place holds a reader by that name\n" +
            readerPlacesReport(places) +
            "  register the file, or the folder it is in: Reader panel > "
            "where readers are looked for > Add file... / Add folder...";
    return r;
}

// Registering is additive and idempotent, and it refuses a path that is not
// there RIGHT NOW - the one moment we can tell the user something useful about
// it. (A place that vanishes later is reported by readerPlacesReport, not
// dropped: the user put it there, and a share that is offline this morning is
// back this afternoon.)
static bool registerReaderPlace(const std::string& path, std::string* why) {
    std::string p = dispPath(path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    std::error_code ec;
    if (p.empty() || !std::filesystem::exists(pathFromUtf8(p), ec)) {
        if (why) *why = "not there: " + dispPath(path);
        return false;
    }
    for (const auto& q : app.readerPlaces)
        if (q == p) { if (why) *why = "already registered: " + p; return false; }
    if (app.readerPlaces.size() >= READER_PLACES_MAX) {
        if (why) *why = "the search path holds " + std::to_string(READER_PLACES_MAX) +
                        " places already - remove one first";
        return false;
    }
    app.readerPlaces.push_back(p);
    g_readerPlacesRev++;
    savePrefs();
    return true;
}

static void unregisterReaderPlace(size_t i) {
    if (i >= app.readerPlaces.size()) return;
    app.readerPlaces.erase(app.readerPlaces.begin() + (long)i);
    g_readerPlacesRev++;
    savePrefs();
}

// The order is a setting, so it has to be editable where it is visible.
static void moveReaderPlace(size_t i, int delta) {
    long j = (long)i + delta;
    if (i >= app.readerPlaces.size() || j < 0 || j >= (long)app.readerPlaces.size()) return;
    std::swap(app.readerPlaces[i], app.readerPlaces[(size_t)j]);
    g_readerPlacesRev++;
    savePrefs();
}

static uint64_t adapterHash(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// §4.12's cache key, in full: the source and the adapter, each by identity AND
// by mtime, plus the function and the module's VERSION. Editing the adapter
// changes its mtime, so the next open re-runs it; nothing else has to be
// remembered to notice, and no timestamp comparison decides anything on its own.
static std::string adapterCacheKey(const std::string& src, const std::string& spec) {
    std::error_code ec;
    std::filesystem::path sp = pathFromUtf8(src);
    uint64_t srcMtime = 0, srcSize = 0;
    auto t = std::filesystem::last_write_time(sp, ec);
    if (!ec) srcMtime = (uint64_t)t.time_since_epoch().count();
    // A folder is a legal source (§4.1). Its size is meaningless, but adding or
    // removing a file inside it moves the directory's own mtime, which is the
    // signal that matters here.
    if (!std::filesystem::is_directory(sp, ec)) {
        auto s = std::filesystem::file_size(sp, ec);
        if (!ec) srcSize = (uint64_t)s;
    }
    std::string af = readerFileOf(spec);
    uint64_t adMtime = 0;
    auto at = std::filesystem::last_write_time(pathFromUtf8(af), ec);
    if (!ec) adMtime = (uint64_t)at.time_since_epoch().count();
    std::string key = src + "|" + std::to_string(srcMtime) + "|" + std::to_string(srcSize) +
                      "|" + af + "|" + std::to_string(adMtime) + "|" + readerFuncOf(spec) +
                      "|" + adapter::moduleVersion(af);
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)adapterHash(key));
    return hex;
}

static std::string adapterCacheDir() {
    std::string cfg = viewerConfigDir();
    if (cfg.empty()) return {};
    std::error_code ec;
    std::filesystem::path d = pathFromUtf8(cfg) / "adapter-cache";
    std::filesystem::create_directories(d, ec);
    return d.u8string();
}

// Run `spec` over `src` and open what it wrote. Returns "" on success; on
// failure the string is the adapter's OWN message wherever there is one (§4.9),
// because a message we invent in its place is a message the author cannot fix.
static std::string readerFinish(const std::string& src, const std::string& spec,
                                const std::string& out,
                                const std::vector<std::string>& argv,
                                const adapter::Run& r,
                                const FrameSource& originStat);   // fwd: the tail, shared
static std::string openWithReader(const std::string& src, const std::string& specIn,
                                  bool allowCache = true,
                                  App::ReaderJob* job = nullptr) {
    // Whatever happens, the panel gets the whole of it (§4.13.0).
    app.readerRan = true;
    app.readerLastOk = false;
    app.readerLastSpec = specIn;
    app.readerLastOut.clear();
    // §4.13.2: the reader is located BEFORE Python is started, so "there is no
    // such reader" arrives as a sentence naming every place we looked, rather
    // than as a traceback out of the harness about a module it could not import.
    ReaderLookup look = resolveReaderFile(specIn);
    if (look.file.empty()) { app.readerLastOut = look.why; return look.why; }
    const std::string spec = look.file + ":" + readerFuncOf(specIn);
    app.readerLastSpec = spec;
    if (!look.shadowed.empty()) {
        // First match won. Say which one, and which ones it beat - the durable
        // record of a decision the ORDER made rather than the user.
        std::string m = "reader " + baseName(look.file) + ": " + dispPath(look.file) +
                        " won (registered first); also in";
        for (const auto& s : look.shadowed) m += " " + dispPath(s);
        logMsg(m);
    }
    // identity BEFORE the bytes (§6.2): the reader is about to read the origin
    // file for however long it takes - the docs it produces bind to THIS stat
    FrameSource originStat;
    originStat.path = src;
    statSourceFile(originStat);
    std::string script = runAdapterScript();
    if (script.empty())
        return "cannot find tools/import/run_adapter.py - the adapter harness ships "
               "with the viewer and this build cannot see it";
    std::string why;
    std::string py = adapter::findPython(app.pythonExe, why);
    if (py.empty())
        return "no Python to run the reader with: " + why +
               " (set one in Settings, or install numpy for the one you have)";

    std::string cacheDir = adapterCacheDir();
    std::string cached;
    if (!cacheDir.empty())
        // .vstream, not .npz: the cache now holds the framed stream, and an npz
        // left by an older build must not be handed to the stream reader. A
        // stale one is simply not found, so the reader runs again.
        cached = (pathFromUtf8(cacheDir) / (adapterCacheKey(src, spec) + ".vstream")).u8string();
    std::error_code ec;
    if (allowCache && !cached.empty() && std::filesystem::exists(pathFromUtf8(cached), ec)) {
        // §7 stage 5: the second open does not start Python. The key already
        // covers the adapter's mtime, so "cached" cannot mean "stale".
        std::string lerr = loadViewerStream(cached, spec, src, originStat);
        if (lerr.empty()) {
            rememberReader(src, spec);
            app.readerLastOk = true;
            app.readerLastOut = "read from cache - the reader was not re-run.\n"
                                "Edit the reader and press Load again to re-run it.";
            return {};
        }
        std::filesystem::remove(pathFromUtf8(cached), ec);   // unreadable: earn it again
    }

    std::string out = cached;
    if (out.empty()) {
        std::filesystem::path t = std::filesystem::temp_directory_path(ec);
        char nm[64];
        snprintf(nm, sizeof nm, "viewer_adapter_%llx.vstream",
                 (unsigned long long)adapterHash(src + spec));
        out = (t / nm).u8string();
    }
    // -u: unbuffered. Without it Python holds print() in a buffer until it
    // exits, so a reader that reports its progress reports all of it at the end,
    // which is the one moment the progress is worthless.
    // --stream: the result comes back on stdout, framed, and the run captures
    // stdout straight to `out`. No npz is written and none is read: the zip
    // container cost 1.45 ms per MB of pixels for nothing.
    std::vector<std::string> argv = { py, "-u", script, spec, src, "--stream" };
    // §4.13: the exact command, before it runs - every time, not only the first.
    // The picker shows it in front of the user for a reader they have not used
    // before; this is the durable record, so "what did it actually start?" is
    // answerable after the fact as well as before it.
    logMsg("reader: " + adapter::showCommand(argv));
    g_adapterRuns++;
    std::error_code tec;
    std::filesystem::path td = std::filesystem::temp_directory_path(tec);
    char sn[64];
    snprintf(sn, sizeof sn, "viewer_reader_%llx", (unsigned long long)adapterHash(src + spec));
    // stdout IS the payload now, so it goes straight to `out` -- the cache
    // path when there is one, a temp otherwise. Nothing reads it back as text:
    // that would be the 755 MB std::string captureStdout=false exists to avoid.
    // stderr still lands in a file, because that is the summary and the
    // reader's own prints, which the panel shows and tails while it runs.
    const std::string& outFile = out;
    std::string errFile = (td / (std::string(sn) + ".err")).u8string();
    if (job) {
        // Asynchronous: the window keeps drawing and pollReader() picks this up.
        // Only the wait is off-thread - readerFinish touches images and GL.
        job->src = src; job->spec = spec; job->out = out; job->argv = argv;
        job->outFile = outFile; job->errFile = errFile;
        job->startedAt = nowSec();
        job->srcMtime = originStat.mtime;  // identity before bytes, carried
        job->srcFsize = originStat.fsize;  // across the asynchronous run
        app.readerLastOut = "running " + baseName(readerFileOf(spec)) + " ...\n";
        App::ReaderJob* j = job;
        j->th = std::thread([j]() {
            j->r = adapter::run(j->argv, 300000, &j->cancel, j->outFile, j->errFile, false);
            j->done.store(true);
        });
        return {};
    }
    adapter::Run r = adapter::run(argv, 300000, nullptr, outFile, errFile, false);
    std::string fin = readerFinish(src, spec, out, argv, r, originStat);
    std::filesystem::remove(pathFromUtf8(errFile), tec);
    return fin;
}

// Everything after the process has run. Shared by the synchronous path
// (selftests, session restore) and the asynchronous one the UI uses, so the two
// cannot drift into reporting the same failure differently.
static std::string readerFinish(const std::string& src, const std::string& spec,
                                const std::string& out,
                                const std::vector<std::string>& argv,
                                const adapter::Run& r,
                                const FrameSource& originStat) {
    if (r.cancelled) {
        app.readerLastOut += "\nstopped.\n";
        return "the reader was stopped";
    }
    if (!r.started) {
        app.readerLastOut = "could not start the reader: " + r.fail + "\n" +
                            adapter::showCommand(argv);
        return "could not start the reader: " + r.fail + "\n  " + adapter::showCommand(argv);
    }
    if (r.timedOut) {
        app.readerLastOut = "the reader did not finish (stopped after 5 minutes)\n" +
                            adapter::showCommand(argv) + "\n" + r.err;
        return "the reader did not finish (stopped after 5 minutes)\n  " +
               adapter::showCommand(argv);
    }
    if (r.exit != 0) {
        // The adapter's exception message, unchanged. run_adapter.py has already
        // printed the traceback with the failing line; passing it through is the
        // entire point of §4.9, and summarising it would remove the only part
        // that says which line of their file was wrong.
        std::string msg = r.err.empty() ? r.out : r.err;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        if (msg.empty()) msg = "the reader exited with status " + std::to_string(r.exit) +
                               " and said nothing";
        // The panel gets it WHOLE - every frame of the traceback, with the file
        // and line that raised. The toast gets the same string, but the panel is
        // where it can be read, and that is the point of §4.13.0.
        app.readerLastOut = msg;
        return msg;
    }
    std::string lerr = loadViewerStream(out, spec, src, originStat);
    if (!lerr.empty()) {
        // The one failure that used to leave the panel EMPTY. The reader ran and
        // exited 0, so it is not the reader's traceback that is wrong - what it
        // returned is something this viewer cannot read. Every other failure
        // above fills readerLastOut; this one returned without doing it, so the
        // panel said "refused it" over a blank box and Copy copied nothing.
        // That is also the path a first reader most often takes.
        app.readerLastOut = "the reader ran and returned, but the viewer could not read "
                            "what it produced:\n\n" + lerr +
                            "\n\nthe reader itself did not fail - check what load() returns "
                            "(the layer type, the shape, the dtype).\n" +
                            adapter::showCommand(argv);
        return lerr;
    }
    rememberReader(src, spec);
    app.readerLastOk = true;
    // §4.11: the harness writes one line to stderr counting what it read and
    // what it skipped, and the viewer relays it VERBATIM. It counts both
    // directions, so paraphrasing it is how a skipped axis becomes invisible.
    std::string sum = r.err;
    while (!sum.empty() && (sum.back() == '\n' || sum.back() == '\r')) sum.pop_back();
    // What the reader PRINTED, kept on a success as well as on a failure. print()
    // goes to stdout, which used to be read and then dropped whenever the run
    // worked - so the one thing an author adds to see what their reader is doing
    // was visible only when it crashed.
    std::string said = r.out;
    while (!said.empty() && (said.back() == '\n' || said.back() == '\r')) said.pop_back();
    app.readerLastOut = sum.empty() ? std::string("the reader wrote no summary") : sum;
    if (!said.empty())
        app.readerLastOut += "\n\n--- the reader printed ---\n" + said;
    size_t lastLine = sum.find_last_of('\n');
    if (lastLine != std::string::npos) sum = sum.substr(lastLine + 1);
    if (!sum.empty()) {
        fprintf(stderr, "adapter: %s\n", sum.c_str());
        toast(sum);
    }
    return {};
}

// Whatever is in the file now. The child is still writing it, so a short read
// is normal and not an error - this is a tail, not a load.
static std::string readerTail(const std::string& path) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

// Start a reader without waiting for it. The panel shows how long it has been
// running, what it has printed so far, and a Stop.
static void startReader(const std::string& src, const std::string& spec) {
    if (app.rdJob) { toast("a reader is already running", true); return; }
    app.rdJob.reset(new App::ReaderJob());
    std::string e = openWithReader(src, spec, true, app.rdJob.get());
    // A cache hit, or a failure before Python was reached, finishes with no
    // thread: there is nothing to wait for and nothing to stop.
    if (!app.rdJob->th.joinable()) {
        app.rdJob.reset();
        if (!e.empty()) toast(e, true);
    }
}

// Called once per frame. Until the child exits this only refreshes what it has
// printed; the loading itself happens here, on the UI thread, when it is done.
static void pollReader() {
    if (!app.rdJob) return;
    App::ReaderJob* j = app.rdJob.get();
    if (!j->done.load()) {
        // errFile only: outFile is the payload, and tailing binary into a
        // text panel would show the pixels as mojibake.
        std::string live = readerTail(j->errFile);
        if (live.size() != j->shown) {
            j->shown = live.size();
            app.readerLastOut = live.empty()
                ? ("running " + baseName(readerFileOf(j->spec)) + " ...\n") : live;
        }
        return;
    }
    j->th.join();
    FrameSource jStat;                     // the stat openWithReader took at start
    jStat.mtime = j->srcMtime;
    jStat.fsize = j->srcFsize;
    std::string e = readerFinish(j->src, j->spec, j->out, j->argv, j->r, jStat);
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(j->errFile), ec);
    app.rdJob.reset();
    if (!e.empty()) toast(e, true);
}

// §4.13: only an explicitly chosen adapter runs, and the first time a given one
// is used the user sees the EXACT command. Not a paraphrase - the thing that
// will be executed, so that "what is this about to run?" has a real answer.
static bool readerNeedsConfirming(const std::string& spec) {
    for (const auto& s : app.readerShown) if (s == spec) return false;
    return true;
}
static void readerConfirmed(const std::string& spec) {
    if (readerNeedsConfirming(spec)) app.readerShown.push_back(spec);
}
static std::string readerCommandLine(const std::string& src, const std::string& spec) {
    std::string why;
    std::string py = adapter::findPython(app.pythonExe, why);
    std::string script = runAdapterScript();
    if (py.empty()) return "(no Python found: " + why + ")";
    if (script.empty()) return "(run_adapter.py not found next to this build)";
    // The command shown must be the command run, so it carries the RESOLVED
    // reader (§4.13.2). A bare name here would show a line that is not what
    // starts, which is the one thing this display exists to prevent.
    ReaderLookup look = resolveReaderFile(spec);
    if (look.file.empty()) return "(no such reader - see the Reader panel)";
    return adapter::showCommand({ py, script, look.file + ":" + readerFuncOf(spec),
                                  src, "-o", "<cache>.npz" });
}

// ---- Open in new window (todo-open item 28) ---------------------------------
// A second, INDEPENDENT viewer: separate process, separate cur(), separate
// panels - the route the decision record chose over generalising cur(). The
// spawn is a pure argv builder plus a detached exec, kept apart so
// --newwin-selftest can assert the exact command line without starting one.
static std::string selfExePath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).u8string();
#else
    char buf[4096];                    // Windows-primary; keep the Linux path built
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; return buf; }
#endif
    return app.exePath;                // argv[0]: right unless the cwd changed
}

// The command line that makes "the same image look the same" in the child:
// - a stack passes its HEAD file, and --stack always regroups the numbered
//   siblings there without the picker;
// - the CFA interpretation rides along when one is set (pattern before --cfa,
//   which reads best; parseCli accepts either order);
// - --window-offset 40,40 cascades the child off its parent;
// - --secondary makes the child's prefs read-only (see savePrefs).
// Remote docs pass their ssh:// url - the CLI already parses it. Pure on
// purpose: the selftest asserts the exact argv this returns.
static std::vector<std::string> newWindowArgv(const ImageDoc* d) {
    if (!d) return {};
    const ImageDoc* head = d;
    if (d->seqId != 0)                 // a stack reopens from its head file
        for (const auto& o : app.images)
            if (o && o->seqId == d->seqId && o->seqIndex < head->seqIndex)
                head = o.get();
    std::string target = !head->src->remoteUrl.empty() ? head->src->remoteUrl : head->src->path;
    if (target.empty()) return {};     // nothing on disk behind this row
    std::vector<std::string> av{ selfExePath(), "--secondary",
                                 "--window-offset", "40,40" };
    if (d->seqId != 0) { av.push_back("--stack"); av.push_back("always"); }
    if (d->cfa != 0) {
        av.push_back("--bayer-pattern");
        av.push_back(CFA_PATTERNS[d->cfaPattern & 3]);
        av.push_back("--cfa");
        av.push_back(d->cfa == 2 ? "quad" : "bayer");
    }
    av.push_back(target);
    return av;
}

#if defined(_WIN32)
// CreateProcessW takes ONE string: quote an argument the way CommandLineToArgvW
// unquotes one (backslashes double before a quote; the quote itself escapes).
static std::wstring quoteArgW(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    r += L'"';
    return r;
}
#endif

// Detached on purpose: no inherited handles (a pipe rides along and ties the
// child's lifetime to ours), its own process group, no console tie - closing
// the window that spawned it must never take the child down. outPid is for the
// selftest, which has to prove the child actually starts under these flags.
static bool spawnDetached(const std::vector<std::string>& av,
                          unsigned long* outPid = nullptr) {
    if (av.empty()) return false;
#if defined(_WIN32)
    std::wstring cmd;
    for (const auto& a : av) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArgW(pathFromUtf8(a).wstring());
    }
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);                       // CreateProcessW may write into it
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(pathFromUtf8(av[0]).wstring().c_str(), cl.data(),
                        nullptr, nullptr, FALSE /* inherit NO handles */,
                        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi))
        return false;
    if (outPid) *outPid = (unsigned long)pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);              // hold nothing of the child's
    return true;
#else
    // double fork: the grandchild reparents to init, so no zombie ever waits on
    // this process and no signal of ours reaches it
    std::vector<char*> args;
    for (const auto& a : av) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t mid = fork();
    if (mid < 0) return false;
    if (mid == 0) {
        setsid();
        pid_t gc = fork();
        if (gc == 0) { execv(args[0], args.data()); _exit(127); }
        _exit(gc > 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(mid, &st, 0);
    if (outPid) *outPid = 0;               // the grandchild's pid is not observable
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#endif
}

static void openInNewWindow(const ImageDoc* d) {
    std::vector<std::string> av = newWindowArgv(d);
    if (av.empty()) {
        toast("no file behind this row - it cannot reopen in a new window", true);
        return;
    }
    if (!spawnDetached(av)) {
        toast("could not start a new window", true);
        return;
    }
    const std::string& target = av.back();
    if (target.compare(0, 6, "ssh://") == 0) {
        // the cost is visible: a second window is a second ssh session
        std::string host, rp;
        remote::parseUrl(target, host, rp);
        toast("new window: opens its own connection to " + host);
    } else {
        toast("opening " + baseName(target) + " in a new window");
    }
}

// Pre-rendered session text and a file already open for it. A SIGSEGV handler may
// be running on a corrupted heap: allocating, opening files, or touching ImGui
// there is how a crash-recovery feature turns into a hang. Everything expensive
// happens while the process is still healthy; the handler only write()s.
static std::string g_crashText;
static int g_crashFd = -1;

static void refreshCrashSnapshot() {
    if (app.images.empty()) { g_crashText.clear(); return; }
    std::ostringstream os;
    writeSessionTo(os);
    g_crashText = os.str();
}

static void openCrashFile() {
    std::string p = autosavePath();
    if (p.empty()) return;
#if defined(_WIN32)
    g_crashFd = _wopen(pathFromUtf8(p).wstring().c_str(),
                       _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    g_crashFd = open(pathFromUtf8(p).c_str(), O_CREAT | O_RDWR, 0644);
#endif
}

// The two halves of the handshake saveSession performs around the rename; the
// reasoning is written out at their forward declarations, beside the writer
// that needs them. Answering "is this the file we hold open" by PATH and not by
// identity is on purpose: a false negative costs a failed save that reports
// itself and leaves the old file whole, which is the outcome this whole change
// is about, while stat()-ing the target on every autosave would buy nothing.
static bool releaseCrashFileFor(const std::string& path) {
    if (g_crashFd < 0 || path.empty() || path != autosavePath()) return false;
#if defined(_WIN32)
    _close(g_crashFd);
#else
    close(g_crashFd);
#endif
    g_crashFd = -1;                 // the handler checks this: it now writes nothing
    return true;
}

static void retakeCrashFile() { openCrashFile(); }

static void crashHandler(int sig) {
    static volatile sig_atomic_t inHandler = 0;
    if (!inHandler) {
        inHandler = 1;
        if (g_crashFd >= 0 && !g_crashText.empty()) {
            const char* p = g_crashText.data();
            size_t n = g_crashText.size();
#if defined(_WIN32)
            _lseek(g_crashFd, 0, SEEK_SET);
            while (n) { int w = _write(g_crashFd, p, (unsigned)n); if (w <= 0) break; p += w; n -= (size_t)w; }
            _chsize(g_crashFd, (long)g_crashText.size());
            _commit(g_crashFd);
#else
            lseek(g_crashFd, 0, SEEK_SET);
            while (n) { ssize_t w = write(g_crashFd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
            if (ftruncate(g_crashFd, (off_t)g_crashText.size()) != 0) { /* best effort */ }
#endif
            static const char msg[] = "\nviewer crashed; session written to the autosave file\n";
#if defined(_WIN32)
            _write(2, msg, (unsigned)(sizeof msg - 1));
#else
            ssize_t ign = write(2, msg, sizeof msg - 1); (void)ign;
#endif
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// The doc a session line names, by §6.2's identity minus the disk baseline -
// (path, member). A container (and a reader's result) is ONE unit: the first
// line that names its file rebuilds the whole tree, and the loader leaves
// cur() on whichever doc it happened to finish with, so neither that line nor
// any later one can assume cur() is the doc it is about.
//
// `from` is what keeps a file legitimately opened TWICE in one session apart:
// a line that opened something looks only among the docs IT added, and only a
// line that opened nothing (the already-here dedupe) searches the whole list.
// A stack's frames all share both halves of the key; the first of them is
// where the line's display state goes, and loadSession spreads it over the
// stack from there. Previews are never in a session and never a target.
static int sessionDocAt(const std::string& path, const std::string& member, size_t from) {
    for (int i = (int)from; i < (int)app.images.size(); i++) {
        const ImageDoc* d = app.images[i].get();
        if (d->preview || d->src->path != path || d->src->member != member) continue;
        return i;
    }
    return -1;
}

// ---- session lines that have to wait for the doc they describe --------------
// See App::RestoreWait. One entry per doc, because both lines are about the
// same arrival: a second line for a doc already waiting fills in its other
// half rather than queueing behind it.
static App::RestoreWait& restoreWaitFor(const ImageDoc& im) {
    for (auto& w : app.restoreWait) if (w.uid == im.uid) return w;
    App::RestoreWait w;
    w.uid = im.uid;
    w.doc = im.name;
    app.restoreWait.push_back(std::move(w));
    return app.restoreWait.back();
}
static void parkRestoreWait(const ImageDoc& im, int seqFrame) {
    restoreWaitFor(im).seqFrame = seqFrame;
}
static void parkRestoreWait(const ImageDoc& im, int x, int y, int w, int h) {
    App::RestoreWait& e = restoreWaitFor(im);
    e.cx = x; e.cy = y; e.cw = w; e.ch = h;
}

// Drained from the frame loop, right after the remote fetch pump - so a frame
// that landed this frame is acted on in the same one, and a headless run
// (which advances only while something is pending) keeps turning until these
// are gone. See the `working |=` list in core/main.cpp.
static void pumpRestoreWaits() {
    for (size_t i = 0; i < app.restoreWait.size();) {
        App::RestoreWait& w = app.restoreWait[i];
        ImageDoc* im = nullptr;
        for (auto& q : app.images) if (q->uid == w.uid) { im = q.get(); break; }
        if (!im) { app.restoreWait.erase(app.restoreWait.begin() + i); continue; }  // closed: nothing to say
        // "nothing more is coming" is the fetcher's own idle, which is what
        // watch.inc's busy test already uses. Asked ONCE per entry and applied
        // to both halves, so a stack that stopped growing and a fetch that
        // never completed are given up on in the same frame rather than one
        // pump apart.
        const bool quiet = app.rfPending <= 0;
        bool done = true;

        if (w.seqFrame >= 0) {
            bool found = false;
            if (im->seqId != 0)
                for (int idx : framesOfSeq(im->seqId))
                    if (app.images[idx]->seqIndex == w.seqFrame) {
                        selectImage(idx); found = true; break;
                    }
            if (found) w.seqFrame = -1;
            else if (quiet) {
                toast(w.doc + ": this session was left on frame " +
                      std::to_string(w.seqFrame) + ", and that frame did not come "
                      "back - showing what did", true);
                w.seqFrame = -1;
            } else done = false;
        }

        if (w.cw > 0) {
            // Not `quiet`: this one has a precise signal of its own. The crop
            // is refused for exactly one reason (remoteStep > 1) and the fetch
            // that clears it either lands or records remoteErr, so waiting on
            // the fetcher's idle would give up on a doc whose frame arrives a
            // moment later in a busy queue.
            if (im->src->remoteStep <= 1) {
                if (cropInPlace(*im, w.cx, w.cy, w.cw, w.ch)) {
                    if (im == cur()) app.fitRequested = true;
                } else {
                    toast(w.doc + ": the crop this session saved could not be "
                          "put back", true);
                }
                w.cw = 0;
            } else if (!im->src->remoteErr.empty()) {
                toast(w.doc + ": the full frame never arrived, so the crop this "
                      "session saved was not put back", true);
                w.cw = 0;
            } else done = false;
        }

        if (done) app.restoreWait.erase(app.restoreWait.begin() + i);
        else i++;
    }
}

static std::string loadSession(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read session file";
    std::string text((const char*)buf.data(), buf.size());
    std::istringstream ss(text);
    std::string line;
    if (!std::getline(ss, line) || line.rfind("viewer-session", 0) != 0)
        return "not a viewer session file";
    closeAll();                    // stops the loader and empties every open
                                   // queue: orphan frames from a previous load
                                   // must not graft onto the restored list
    // A container/reader rebuild inside the replay re-declares its sets; their
    // Refs must wait for the gate (see g_sessionRestoring at its definition),
    // or every save/load cycle grows a twin of each referenced stack.
    g_sessionRestoring = true;
    app.anns.clear();
    app.selectedAnn = 0;
    app.annRev++;
    std::vector<std::string> failures;   // reported as one summary, not one toast each
    // ...and the other loss this file can suffer: the image line opened, but
    // the STACK around it could not be rebuilt, so what came back is one frame
    // where the session recorded many. Counted separately because it is a
    // different sentence - the picture is here, the stack is not.
    std::vector<std::string> stackFailures;
    int selAnnIndex = -1;
    float zoom = 0; ImVec2 center(0, 0); int current = 0;
    int pendingLut = -1;
    std::string pendingMember;         // .npz array name for the next image line
    int pendingRead = NR_NATIVE;       // §3.3 reading for the next image line
    bool lastImageOk = false;         // seqload applies to the image just loaded
    bool haveView = false;
    // "current" is an ordinal over saved image lines; a line can expand into a
    // whole stack, so resolve it to a real index once that line is fully applied
    // (seqframe, which picks the frame, comes after the image line).
    int lineOrd = -1, wantCurrent = -1, resolvedCurrent = -1;
    int curSeries = -1;               // index into app.seriesRestore, -1 = outside a block
    int curSet = -1;                  // index into app.aSetRestore, same shape
    bool capturing = false;
    // Which cross-reference line a `refmember` would qualify. The three lines
    // that name a doc from ELSEWHERE in the file - cmpslot, stackavg/stacksum,
    // seriesmember/seriessum/seriesframe - speak paths, and two image members
    // of one .npz are one path; the member is the other half of §6.2's identity
    // and the ONLY thing that tells them apart. It rides as a companion line
    // (stackavggen's shape) rather than as a field, because appending a field
    // to a line whose last field is a path would make an older viewer read
    // "path member" as the path and lose the reference outright - where an
    // unknown KEY is simply skipped, and that viewer resolves by path exactly
    // as it does today. Strictly the line DIRECTLY above: a qualifier that can
    // drift onto a different line is not a key.
    enum { RM_NONE, RM_SLOT, RM_AVG, RM_SERIES, RM_DETREND };
    int refFor = RM_NONE;
    auto settleCurrent = [&]() {
        if (capturing) { resolvedCurrent = app.current; capturing = false; }
    };
    auto restOfLine = [](std::istringstream& ls) {
        std::string p; std::getline(ls, p);
        while (!p.empty() && (p[0] == ' ' || p[0] == '\t')) p.erase(0, 1);
        while (!p.empty() && (p.back() == '\r' || p.back() == '\n')) p.pop_back();
        return p;
    };
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key; ls >> key;
        const int refTarget = refFor;   // only the line DIRECTLY above may be qualified
        refFor = RM_NONE;
        if      (key == "gamma")   ls >> app.dispGamma;
        else if (key == "grid")  { int g = 0; ls >> g; app.showGrid = g != 0; }
        else if (key == "zoom")  { ls >> zoom; haveView = true; }
        else if (key == "center")  ls >> center.x >> center.y;
        else if (key == "current") ls >> wantCurrent;
        else if (key == "linkrange") { int on = 0; ls >> on >> app.linkBlack >> app.linkWhite;
                                       app.linkRange = on != 0; }
        else if (key == "rangescope") { ls >> app.rangeScope; }
        else if (key == "cmpfollow") { int on = 1; ls >> on; app.compareFollowFrame = on != 0; }
        else if (key == "cmprange") { ls >> app.compareRangeMode; }
        else if (key == "cmpslot")  { int fi = -1; ls >> fi;
                                      std::string pth; std::getline(ls, pth);
                                      if (!pth.empty() && pth[0] == ' ') pth.erase(0, 1);
                                      if (!pth.empty()) {
                                          app.cmpSlotRestore.push_back({ fi, pth, {} });
                                          refFor = RM_SLOT;
                                      } }
        // The member half of the three path-keyed cross-references above/below
        // (see refFor): it qualifies the line it follows and nothing else.
        else if (key == "refmember") {
            std::string m = restOfLine(ls);
            if (m.empty()) continue;
            if (refTarget == RM_SLOT && !app.cmpSlotRestore.empty())
                app.cmpSlotRestore.back().member = m;
            else if (refTarget == RM_AVG && !app.avgRestore.empty())
                app.avgRestore.back().member = m;
            else if (refTarget == RM_DETREND && !app.dtRestore.empty())
                app.dtRestore.back().member = m;
            else if (refTarget == RM_SERIES && curSeries >= 0 &&
                     !app.seriesRestore[curSeries].members.empty())
                app.seriesRestore[curSeries].members.back().member = m;
        }
        // pre-tri-state prefs: the old bool maps onto "B uses A's" / "each own".
        // NOT onto the new default - it records a choice somebody actually made.
        else if (key == "cmpshare") { int on = 1; ls >> on; app.compareRangeMode = on ? 1 : 0; }
        else if (key == "abstats") { ls >> app.abStatsLayout >> app.histPlane;
                                     app.abStatsLayout = std::clamp(app.abStatsLayout, 0, 2);
                                     app.histPlane = std::clamp(app.histPlane, -1, 3); }
        else if (key == "roichannel") ls >> app.roiChannel;
        else if (key == "projstatlayout") { int dp = -1, ar = 0, so = 0;
                                            ls >> app.projStatLayout >> dp >> ar >> so;
                                            app.projStatLayout = std::clamp(app.projStatLayout, 0, 2);
                                            app.projDecimals = std::clamp(dp, -1, 6);
                                            app.projAllRow = ar != 0;
                                            app.projStatOrder = std::clamp(so, 0, 1); }
        else if (key == "projection") { int h = 1, v = 1;
                                        ls >> app.projMode >> app.projYMode >> app.projYLo
                                           >> app.projYHi >> h >> v;
                                        app.showProjH = h != 0; app.showProjV = v != 0; }
        else if (key == "analysis") {
            int a = 0, r = 0;
            float rv = 0.5f;
            ls >> app.anaSel >> a;
            app.anaAuto = a != 0;
            if (ls >> r >> rv) { app.anaRefOn = r != 0; app.anaRef = rv; }   // >= this rev
        }
        else if (key == "histlog") { int on = 1; ls >> on; app.histLog = on != 0; }
        else if (key == "seriespanel") {   // Series Analysis panel settings (§3.3)
            int tp = 0, db = 0, fm = 0, wm = 0;
            double lo = 0, hi = 0;
            ls >> tp >> db >> fm >> wm >> lo >> hi;
            app.lin.tablePlane = std::clamp(tp, 0, 3);
            app.lin.snrDb = db != 0;
            app.lin.fitMethod = std::clamp(fm, 0, 1);
            app.lin.winMode = std::clamp(wm, 0, 2);
            if (std::isfinite(lo)) app.lin.winLo = lo;
            if (std::isfinite(hi)) app.lin.winHi = hi;
        }
        // "npyaxis" is deliberately NOT parsed any more (§3.4): the flag it
        // restored no longer exists, and an old session's value falls through
        // the chain like any unknown key rather than being half-honoured.
        else if (key == "compare") { int ab = 0, fa = 0;
                                     ls >> app.compareMode >> app.wipeFrac >> app.splitFrac;
                                     if (ls >> app.diffGain >> ab) app.diffAbs = ab != 0;  // v2
                                     if (ls >> fa >> app.flipPeriod) app.flipAuto = fa != 0;  // v3
                                     app.compareMode = std::clamp(app.compareMode, 0, 4);
                                     app.flipPeriod = std::clamp(app.flipPeriod, 0.05f, 10.0f);
                                     app.wipeFrac = std::clamp(app.wipeFrac, 0.03f, 0.97f);
                                     app.splitFrac = std::clamp(app.splitFrac, 0.03f, 0.97f); }
        else if (key == "compareb") {   // "<frameIndex|-1> <name>"; v1 files: just a name
            std::streampos save = ls.tellg();
            int fi = -1;
            if (ls >> fi) { app.compareBSeq = fi; }
            else { ls.clear(); ls.seekg(save); app.compareBSeq = -1; }
            app.compareB = restOfLine(ls);
            app.compareBUid = 0;        // resolved by name+frame on first use
        }
        else if (key == "panels") {
            // trailing fields are optional: sessions written before a panel
            // existed keep their meaning, and its default stays put
            int a = 1, b = 1, c2 = 1, d2 = 1, e2 = 1, f2 = 1, g2 = 0, h2 = 0, i2 = 0, j2 = 0;
            ls >> a >> b >> c2 >> d2 >> e2 >> f2 >> g2 >> h2 >> i2 >> j2;
            app.showFiles = a != 0; app.showInspector = b != 0; app.showRois = c2 != 0;
            app.showAnalysis = d2 != 0; app.showHistogram = e2 != 0; app.showTemporal = f2 != 0;
            app.showProjection = g2 != 0; app.showSeriesAnalysis = h2 != 0; app.showRemote = i2 != 0;
            app.showSetAnalysis = j2 != 0;
        }
        else if (key == "rbplace") {
            // "<num> <url>": where a Browse instance stood (item 10, A5).
            // Reconnects asynchronously through that instance's own worker;
            // a failure shows inline in that instance, never a global dialog.
            int n = 1;
            ls >> n;
            sessionRestoreBrowsePlace(n, restOfLine(ls));
        }
        else if (key == "layout_begin") {
            std::string ini, l2;
            while (std::getline(ss, l2)) {
                if (l2.rfind("layout_end", 0) == 0) break;
                if (!l2.empty() && l2[0] == '|') l2.erase(l2.begin());
                while (!l2.empty() && (l2.back() == '\r' || l2.back() == '\n')) l2.pop_back();
                ini += l2;
                ini += "\n";
            }
            {   // sessions saved before the "Remote" -> "Browse###Remote"
                // rename: migrate the window entry or the panel undocks
                size_t p;
                while ((p = ini.find("[Window][Remote]")) != std::string::npos)
                    ini.replace(p, strlen("[Window][Remote]"), "[Window][Browse###Remote]");
                // ...and before "Linearity" -> "Series Analysis###Linearity"
                // (docs/analysis-layers.md §3.3 renamed the panel): same rule
                while ((p = ini.find("[Window][Linearity]")) != std::string::npos)
                    ini.replace(p, strlen("[Window][Linearity]"),
                                "[Window][Series Analysis###Linearity]");
            }
            // applied between frames: loading dock settings mid-frame is not safe
            app.pendingLayout = std::move(ini);
            app.resetLayout = false;   // otherwise the default rebuild wipes it
        }
        else if (key == "lut") ls >> pendingLut;   // applies to the next image line
        else if (key == "member") pendingMember = restOfLine(ls);
        else if (key == "npyread") {              // §3.3, applies to the next image line
            int r = NR_NATIVE;
            ls >> r;
            pendingRead = (r >= NR_NATIVE && r <= NR_CHW) ? r : NR_NATIVE;
        }
        else if (key == "selann") ls >> selAnnIndex;
        else if (key == "imgbatch") {   // batch of the image above, by name
            std::string nm = restOfLine(ls);
            if (!nm.empty() && lastImageOk && cur()) {
                int bid = batchReuse(nm);
                // frame ⊂ stack ⊂ batch: a stack the FILE carries is already
                // whole here (loadNpy/loadNpz/the container build it in one
                // call), and assigning cur() alone moved its representative
                // frame out on its own - the exact thing moveStackToBatch
                // exists to prevent. "close batch" then closed the whole stack
                // from either side, and the next autosave wrote the leaf batch
                // back, so one save/load cycle silently undid the move.
                // A FOLDER stack's siblings are not here yet (seqId == 0 at
                // this point); they inherit the head's batch at integration,
                // in pumpSequence.
                if (cur()->seqId != 0) moveStackToBatch(cur()->seqId, bid);
                else cur()->batchId = bid;
                app.imagesRev++;
            }
        }
        else if (key == "seqname") {    // user-given stack name (may contain spaces)
            std::string nm = restOfLine(ls);
            if (nm.empty() || !lastImageOk || !cur()) continue;
            if (cur()->seqId != 0) {
                if (App::SeqInfo* si2 = seqInfo(cur()->seqId)) si2->name = nm;
            } else {
                // a FOLDER stack: seqload queued a rescan and the SeqInfo will
                // not exist for many frames. Attach the name to that queued
                // entry instead of dropping it (which is what used to happen,
                // silently, for every renamed folder stack).
                for (auto it = app.seqRestore.rbegin(); it != app.seqRestore.rend(); ++it)
                    if (it->uid == cur()->uid) { it->name = nm; break; }
            }
        }
        // The per-frame x axis, three additive keys riding the image line above
        // exactly as seqname does (SeqInfo now, or the queued restore entry for
        // a folder stack). STRICT values: an unreadable token drops the whole
        // axis - a plotted axis with an invented 0 in it is worse than none.
        else if (key == "seqaxisvals" || key == "seqaxisunit" || key == "seqaxisname") {
            if (!lastImageOk || !cur()) continue;
            App::SeqInfo* si2 = cur()->seqId != 0 ? seqInfo(cur()->seqId) : nullptr;
            App::SeqRestore* sr = nullptr;
            if (!si2)
                for (auto it = app.seqRestore.rbegin(); it != app.seqRestore.rend(); ++it)
                    if (it->uid == cur()->uid) { sr = &*it; break; }
            if (!si2 && !sr) continue;
            if (key == "seqaxisvals") {
                std::vector<double> vals;
                bool bad = false;
                std::string tok;
                while (ls >> tok) {
                    char* end = nullptr;
                    double v = strtod(tok.c_str(), &end);
                    if (!end || *end != '\0' || !std::isfinite(v)) { bad = true; break; }
                    vals.push_back(v);
                }
                if (bad || vals.empty()) {
                    fprintf(stderr, "session: seqaxisvals unreadable - x axis dropped\n");
                    if (si2) si2->axisVals.clear(); else sr->axisVals.clear();
                } else if (si2) si2->axisVals = std::move(vals);
                else sr->axisVals = std::move(vals);
            } else if (key == "seqaxisunit") {
                std::string u = restOfLine(ls);
                if (si2) si2->axisUnit = u; else sr->axisUnit = u;
            } else {
                std::string n = restOfLine(ls);
                if (si2) si2->axisName = n; else sr->axisName = n;
            }
        }
        // MIGRATION of the old per-stack level. saveSession has never written
        // this key (verified over the whole history: no commit ever emitted
        // it), so it fires only for hand-written or third-party files. Note the
        // old reader also required cur()->seqId != 0, which is FALSE for a
        // folder stack at parse time - it could only ever have worked for an
        // in-file frame axis. This one keys on the PATH, which works for both,
        // and turns into a series only where there is a sweep to speak of.
        else if (key == "seqlevel") {
            // STRICTLY, like the seriesmember branch below. `ls >> lv` stores
            // 0.0 on a failed extraction (C++11 onward), so "notanumber", "1e",
            // "-" and a bare "seqlevel" all sailed through the isfinite guard as
            // a hard measurement at ZERO - and zero here is not a missing point,
            // it is the DARK stack: migrateLegacyLevels gives the series the
            // "lx" prefill so it is fittable out of the box, and linRecompute
            // then anchors the offset on it and MEASURES read noise in a stack
            // nobody declared dark. This branch exists ONLY for hand-written and
            // third-party files, which is precisely the input that carries
            // unparseable text. An unreadable legacy level is no legacy level;
            // a batch of them migrates to nothing, which is what the canon says
            // (値は「読めなければ未設定」... 0 ではない).
            double lv = parseSeriesValue(restOfLine(ls).c_str());
            if (lastImageOk && cur() && !cur()->src->path.empty() && std::isfinite(lv))
                app.seqLevelLegacy.push_back({ cur()->src->path, lv });
        }
        // A computed frame's RECIPE: "the mean of the stack whose first frame is
        // this path". Collected here and resolved after everything is open, for
        // exactly the reason the series block gives below - and taken again
        // rather than reloaded, because there is no file holding the answer.
        else if (key == "stackavg") {
            app.avgRestore.push_back({ restOfLine(ls), {}, FOLD_MEAN, false, 0, 0, {} });
            refFor = RM_AVG;                      // a `refmember` may follow
        }
        else if (key == "stacksum") {
            app.avgRestore.push_back({ restOfLine(ls), {}, FOLD_SUM, false, 0, 0, {} });
            refFor = RM_AVG;
        }
        // A preprocessor product's RECIPE (docs/analysis-layers.md §5): the
        // stage's spec, then the SOURCE stack's head path. Resolved after
        // everything is open, like the average above and for the same reason.
        // An older viewer skips the key and opens the session without the
        // product - which is the correct failure: it cannot make one.
        else if (key == "detrend") {
            App::DetrendRestore r;
            int div = 0;
            ls >> r.spec.method >> r.spec.degree >> r.spec.block >> div;
            r.spec.divide = div != 0;
            r.path = restOfLine(ls);
            if (!r.path.empty() && r.spec.method != DTR_NONE) {
                app.dtRestore.push_back(std::move(r));
                refFor = RM_DETREND;               // a `refmember` may follow
            }
        }
        else if (key == "detrendname") {
            if (!app.dtRestore.empty()) app.dtRestore.back().name = restOfLine(ls);
        }
        // The recipe's generation record (§10.2): count, input tuple digest,
        // and when the saved picture was computed. Attaches to the recipe line
        // just above (the writer keeps them adjacent); a record with no recipe
        // to attach to is dropped, exactly like any other unparseable line -
        // and a recipe WITHOUT a record (an older session) restores with no
        // generation claim, which is the correct silence: it cannot state a
        // change it never recorded.
        else if (key == "stackavggen") {
            if (!app.avgRestore.empty()) {
                auto& r = app.avgRestore.back();
                int cnt = 0; std::string hex;
                ls >> cnt >> hex;
                if (cnt > 0 && !hex.empty()) {
                    r.srcCount = cnt;
                    r.srcDigest = strtoull(hex.c_str(), nullptr, 16);
                    r.when = restOfLine(ls);
                    r.hasGen = true;
                }
            }
        }
        // ---- series (系列) block: collected here, RESOLVED later -------------
        // Nothing can be looked up yet - a folder stack is still one loose image
        // with a queued rescan behind it. See resolveSeriesRestore.
        else if (key == "series") {
            App::SeriesRestore r;
            r.name = restOfLine(ls);
            app.seriesRestore.push_back(std::move(r));
            curSeries = (int)app.seriesRestore.size() - 1;
        }
        else if (key == "seriesend") curSeries = -1;
        else if (key == "seriesbatch") {
            if (curSeries >= 0) app.seriesRestore[curSeries].batchName = restOfLine(ls);
        }
        else if (key == "seriesparam") {
            if (curSeries >= 0) app.seriesRestore[curSeries].paramName = restOfLine(ls);
        }
        else if (key == "seriesunit") {
            if (curSeries >= 0) app.seriesRestore[curSeries].unit = restOfLine(ls);
        }
        else if (key == "serieskind") {
            if (curSeries >= 0) { int k = 0; ls >> k; app.seriesRestore[curSeries].kind = k; }
        }
        else if (key == "seriesmember" || key == "seriessum" || key == "seriesframe") {
            // "<value|-> <0|1> <path>". Three keys, one shape: a stack member
            // standing as mean / as sum, and a standalone frame member (§3.3).
            // Both fields as TEXT. A value this program cannot read is UNSET,
            // not 0: atof() turns "notanumber", "1e" or a half-written line into
            // a hard measurement at zero, which the fit then treats as the dark
            // stack. A loader is exactly where that must not be assumed.
            std::string v, inc;
            ls >> v >> inc;
            std::string p = restOfLine(ls);
            if (curSeries < 0) {
                // nothing to attach it to; the block header is what was lost
            } else if (p.empty()) {
                // The path is the LAST field, so a line cut short loses it FIRST
                // (a power loss during the autosave, the crash handler's short-
                // write break, a hand-edit). Such a line never becomes a member,
                // so nothing downstream can count it - which made it the one
                // silent drop left in a loader whose whole contract is that
                // there are none. Count it here and let the toast say so.
                app.seriesRestore[curSeries].truncated++;
            } else {
                double val = parseSeriesValue(v.c_str());
                if (v != "-" && !std::isfinite(val)) app.seriesRestore[curSeries].badValues++;
                int mk = key == "seriessum" ? 1 : key == "seriesframe" ? 2 : 0;
                app.seriesRestore[curSeries].members.push_back({ val, inc != "0", p, mk, {} });
                refFor = RM_SERIES;               // a `refmember` may follow
            }
        }
        // ---- analysisset block (docs/reader-analysisset.md §5.1): collected
        // here, resolved at the drain gate (resolveASetRestore). A reader-read
        // origin re-runs its reader through the image lines above, so the
        // adopt rule (§5.2) has both declarations in hand by the time it fires.
        else if (key == "analysisset") {
            App::ASetRestore r;
            r.name = restOfLine(ls);
            app.aSetRestore.push_back(std::move(r));
            curSet = (int)app.aSetRestore.size() - 1;
        }
        else if (key == "setend") curSet = -1;
        else if (key == "setbatch") {
            if (curSet >= 0) app.aSetRestore[curSet].batchName = restOfLine(ls);
        }
        else if (key == "setreader") {
            if (curSet >= 0) app.aSetRestore[curSet].readerSpec = restOfLine(ls);
        }
        else if (key == "setorigin") {
            if (curSet >= 0) app.aSetRestore[curSet].origin = restOfLine(ls);
        }
        else if (key == "setrole") {   // "<role> <kind> <batch>\t<key>" | "<role> unbound <reason>"
            if (curSet < 0) continue;
            std::string role, kind;
            ls >> role >> kind;
            App::ASetRestore::R rr;
            rr.role = role;
            rr.kind = kind;
            std::string rest = restOfLine(ls);
            if (kind == "unbound") {
                rr.reason = rest;      // restOfLine: the reason may contain anything
            } else if (kind == "stack" || kind == "frame" || kind == "series") {
                // TAB separates batch name from path / series name: both halves
                // can hold spaces, only one can hold a tab (the readerfor rule)
                size_t tab = rest.find('\t');
                if (tab == std::string::npos || tab + 1 >= rest.size()) {
                    // the key is the LAST field, so a cut line loses it first -
                    // counted, the seriesmember truncation rule
                    app.aSetRestore[curSet].truncated++;
                    continue;
                }
                rr.batchName = rest.substr(0, tab);
                rr.key = rest.substr(tab + 1);
            } else {
                // a kind from a future build: counted, never guessed at
                app.aSetRestore[curSet].truncated++;
                continue;
            }
            app.aSetRestore[curSet].roles.push_back(std::move(rr));
        }
        // ("linunit" was read here and is not any more: it is a PREFERENCE -
        //  the prefill a new series starts from - and prefs.txt is its owner,
        //  which is where it has always been written. No writeSessionTo has
        //  ever emitted this key, so no session in existence carries one; a
        //  session that did would be a document changing an application
        //  preference on the way in.)
        else if (key == "seqframe") {   // select the frame this stack was left on
            int want = 0; ls >> want;
            if (lastImageOk && cur() && cur()->seqId != 0) {
                bool found = false;
                for (int idx : framesOfSeq(cur()->seqId))
                    if (app.images[idx]->seqIndex == want) { selectImage(idx); found = true; break; }
                // A stack whose frames live on a peer has ONE of them here: the
                // rest were enqueued by openRemote and arrive over the next
                // seconds, and this parser runs to the end of the file long
                // before they do. So the frame this session was left on was
                // simply not among the frames that existed when the line was
                // read, the loop matched nothing, and the stack came back on
                // frame 0 with nothing said (verify-matrix G5).
                if (!found) parkRestoreWait(*cur(), want);
            }
        }
        else if (key == "seqload") {   // applies to the image loaded just above
            int on = 0; ls >> on;
            // only if that image actually loaded, and not when it is already a
            // stack (an in-file frame axis) - that would split it in two
            if (on && lastImageOk && cur() && !cur()->src->path.empty() && cur()->seqId == 0) {
                const std::string p = cur()->src->path;
                const bool isRemote = hasUrlScheme(p);
                if (isRemote) {
                    // A folder stack whose frames live on a peer. Its siblings
                    // are a LISTING, and findSequenceSiblings below walks THIS
                    // filesystem: handed a url it returns nothing, so this
                    // branch used to fall through doing nothing and saying
                    // nothing. A thirty-frame sweep came back as one loose
                    // frame with its name and its hand-typed axis gone, its
                    // series member unresolvable, and a toast that read clean -
                    // while the same folder opened through File > Open restored
                    // perfectly, which made the difference look like the user's
                    // mistake. Ask the peer for the folder, exactly as the
                    // Browse row that opened it did, and hand the head to the
                    // one function that knows what a remote folder stack is.
                    std::vector<std::string> files;
                    std::string nm, why;
                    if (remoteSequenceSiblings(p, files, nm, why) && files.size() >= 2) {
                        std::string host, rp;
                        int port = 0;
                        remote::parseUrl(p, host, rp, &port);
                        attachRemoteStack(cur(), host, files, nm, port, 0);
                    } else {
                        stackFailures.push_back(why.empty() ? p + ": its folder could not "
                                                              "be listed" : why);
                    }
                } else {
                    std::string pat;
                    std::vector<std::string> files = findSequenceSiblings(p, pat);
                    // queued, not started: see App::seqRestore
                    if (files.size() >= 2)
                        app.seqRestore.push_back({ cur()->uid, std::move(files), pat,
                                                   {}, {}, {}, {} });
                    else
                        // the same silence, on this disk: the folder is gone or
                        // has lost its numbering, and the stack the file asked
                        // for is one frame now. Say which.
                        stackFailures.push_back(baseName(p) + ": no sibling frames beside "
                                                              "it any more");
                }
            }
        }
        else if (key == "ann") {
            App::Ann a; int vis = 1;
            ls >> a.type >> a.x >> a.y >> a.w >> a.h >> a.color >> vis;
            // v1 files stop here; v2 adds the band-toggle memory before the label
            std::streampos save = ls.tellg();
            if (!(ls >> a.prevX >> a.prevW >> a.prevY >> a.prevH)) {
                ls.clear(); ls.seekg(save);
                a.prevW = a.prevH = -1;
            }
            if (a.type < 0 || a.type > 1 || a.x < 0 || a.y < 0 || a.w < 0 || a.h < 0)
                continue;                 // reject malformed lines (would read out of bounds)
            a.color &= 7;
            a.visible = vis != 0;
            a.label = restOfLine(ls);
            if (a.label.empty()) a.label = a.type == 0 ? "ROI" : "P";
            a.id = app.nextAnnId++;
            app.anns.push_back(std::move(a));
            app.annRev++;
        }
        // legacy (pre-annotation sessions)
        else if (key == "pin") { int x = 0, y = 0; ls >> x >> y;
                                 if (x >= 0 && y >= 0) addAnn(1, x, y, 0, 0); }
        else if (key == "roi") { int x = 0, y = 0, w = 0, h = 0; ls >> x >> y >> w >> h;
                                 if (w > 0 && h > 0) addAnn(0, x, y, w, h); }
        else if (key == "image") {
            settleCurrent();              // the previous image line is done
            lineOrd++;
            float bk = 0, wt = 1; std::string kind;
            ls >> bk >> wt >> kind;
            std::string err;
            const size_t docsBefore = app.images.size();   // "did this line open anything"
            if (kind == "raw3" || kind == "raw2" || kind == "raw") {
                RawDialog d;
                int le = 1, col = 0;
                if (kind == "raw3") {
                    int ccx = 0, ccy = 0, ccw = 0, cch = 0;
                    ls >> d.dtype >> d.interp >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> col
                       >> ccx >> ccy >> ccw >> cch;
                    if (ccw > 0 && cch > 0 && (ccx != 0 || ccy != 0 || ccw != d.w || cch != d.h)) {
                        d.cropOn = true;
                        d.cropX = ccx; d.cropY = ccy; d.cropW = ccw; d.cropH = cch;
                    }
                } else if (kind == "raw2") {
                    ls >> d.dtype >> d.interp >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> col;
                } else {                     // legacy v1 line: combined format index + quad flag
                    int fmt = 0, quad = 0;
                    ls >> fmt >> d.w >> d.h >> d.offset >> le >> d.cfaPattern >> quad >> col;
                    if (fmt < 0 || fmt >= 12) { toast("session: bad raw format", true); continue; }
                    d.dtype = LEGACY_RAW_MAP[fmt][0];
                    d.interp = LEGACY_RAW_MAP[fmt][1];
                    if (quad && d.interp == RI_BAYER) d.interp = RI_QUAD;
                }
                d.littleEndian = le != 0;
                d.path = restOfLine(ls);
                err = loadRaw(d);
                if (err.empty()) cur()->cfaColorize = col != 0;
            } else if (kind == "npy2" || kind == "npy3") {
                int cfa = 0, pat = 0, col = 0, ccx = 0, ccy = 0, ccw = 0, cch = 0;
                ls >> cfa >> pat >> col;
                if (kind == "npy3") ls >> ccx >> ccy >> ccw >> cch;
                std::string p = restOfLine(ls);
                // an .npz member must go back through the zip reader; loadNpy
                // would fail the magic check and the array would silently vanish
                std::string lowp = p;
                std::transform(lowp.begin(), lowp.end(), lowp.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                bool isNpz = lowp.size() > 4 && lowp.compare(lowp.size() - 4, 4, ".npz") == 0;
                bool isRemote = hasUrlScheme(p);
                if (isRemote) {
                    // a remote image has no local file to decode: reconnect
                    // instead - carrying the §3.3 reading the line recorded,
                    // which since #124 is a thing a remote document can have.
                    // A restore that dropped it would silently hand back the
                    // peer's own reading of an array the user had said was
                    // being read wrong, which is the one outcome §3.3 exists to
                    // prevent ("以後その doc はその読み方で扱われる").
                    size_t before = app.images.size();
                    openRemote(p, false, 0, pendingRead);
                    err = app.images.size() > before ? "" : "cannot reopen " + p;
                } else {
                    // §4.12: a file that was read by an adapter is read by that
                    // adapter again when the session comes back. Without this
                    // the restore reaches for a member (__pixels_1) that only
                    // exists in the container the reader produces, and a session
                    // that cannot reopen what it saved is not a session.
                    std::string rspec = readerFor(p);
                    if (!rspec.empty()) {
                        // ONE reader run rebuilds every doc that file produced,
                        // but the session carries one line per doc. Run it for
                        // the first line and let the rest recognise their own
                        // pixels as already present - otherwise a six-frame
                        // series comes back six times over.
                        bool already = false;
                        for (const auto& d : app.images)
                            if (d->src->path == p) { already = true; break; }
                        err = already ? std::string() : openWithReader(p, rspec);
                    } else if (isNpz)
                        err = loadNpz(p, pendingMember, pendingRead);
                    else if (imagefile::forPath(p))
                        // a session that saved a .png must reopen a .png:
                        // loadNpy would fail its magic check and the document
                        // would come back as "cannot open", not as pixels.
                        // pendingMember carries the NAMED part when the format
                        // has any (an .exr layer), for the same reason loadNpz
                        // takes it: a session that saved one layer of six must
                        // not reopen six.
                        err = loadImageFile(p, pendingMember);
                    else
                        err = loadNpy(p, pendingRead);
                }
                // Point at the doc this line is ABOUT. For an ordinary file
                // that is the one doc (or the head frame) the call just added,
                // which is where cur() already is. For a container - or a
                // reader's result - one call rebuilds every doc the file holds,
                // and a later line adds nothing at all (the already-here
                // dedupe), so cur() is whichever doc the rebuild finished with:
                // without this, EVERY line of a two-stack container wrote its
                // range, LUT, mosaic reading and crop onto that same doc, the
                // last line won, and every other stack came back at the
                // container's defaults. A line whose member the rebuild did not
                // produce is a real loss - reported, never applied blind.
                if (err.empty()) {
                    if (app.images.size() > docsBefore) {
                        // it opened something: the line's doc is among what IT
                        // added, never among the docs already here (a file may
                        // legitimately be opened twice in one session).
                        int at = sessionDocAt(p, pendingMember, docsBefore);
                        if (at >= 0) selectImage(at);
                        else if (!pendingMember.empty()) {
                            // The line NAMED a member and that member is not
                            // among what this door produced. "Leave cur() where
                            // the loader put it" was the old answer and it is
                            // not good enough: everything below applies to
                            // cur(), so the line's range, LUT, mosaic reading
                            // and crop land on SOME OTHER DOCUMENT, and nothing
                            // says so. It is the same loss the branch below
                            // already reports; the two doors differ only in
                            // whether the file happened to be open already.
                            //
                            // The way this happens in practice (verify-matrix
                            // G6): the file was read by an ADAPTER, whose
                            // members are named __pixels_N, and the record of
                            // which adapter lives in prefs.txt - a bounded LRU
                            // on THIS machine. Move the session to another
                            // machine, or open 64 other files first, and the
                            // .npz behind it opens natively instead: real
                            // arrays, no __pixels_N, no error. So the reason
                            // is named when the member says so, because "that
                            // member is missing" would send the reader looking
                            // inside a file that is not the problem.
                            const bool fromReader =
                                pendingMember.rfind("__pixels_", 0) == 0;
                            err = "member " + pendingMember + " is not in what " +
                                  p + " rebuilt";
                            if (fromReader)
                                err += " - it was read by an input adapter, and this "
                                       "build has no record of which one (that record "
                                       "is per-machine and bounded). Open it with the "
                                       "reader again, and this session will find it";
                        }
                    } else {
                        // it opened NOTHING and did not fail: the already-here
                        // dedupe. Only a doc that is already open can be meant.
                        int at = sessionDocAt(p, pendingMember, 0);
                        if (at >= 0) selectImage(at);
                        else err = (pendingMember.empty() ? std::string("the pixels")
                                                          : "member " + pendingMember) +
                                   " named by this line is not in what " + p + " rebuilt";
                    }
                }
                if (err.empty()) {
                    cur()->cfa = std::clamp(cfa, 0, 2);
                    cur()->cfaPattern = pat & 3;
                    cur()->cfaColorize = col != 0;
                    if (ccw > 0 && cch > 0 && (ccx != 0 || ccy != 0 || ccw != cur()->w || cch != cur()->h))
                        // The return value MATTERS. cropInPlace refuses while
                        // the source is the decimated preview - the
                        // full-resolution landing replaces data/w/h wholesale
                        // and knows nothing of crop bookkeeping - and a remote
                        // image over 1600 px IS that preview at this moment.
                        // Ignoring the `false` meant the saved crop disappeared
                        // with no message, on exactly the large images where a
                        // crop is worth saving (verify-matrix G10).
                        if (!cropInPlace(*cur(), ccx, ccy, ccw, cch))
                            parkRestoreWait(*cur(), ccx, ccy, ccw, cch);
                }
            } else {                          // legacy "npy"
                std::string p = restOfLine(ls);
                err = loadNpy(p);
            }
            pendingMember.clear();
            pendingRead = NR_NATIVE;      // must not leak onto the next image
            if (!err.empty()) {
                failures.push_back(err);
                lastImageOk = false;
                pendingLut = -1;          // must not leak onto the next image
                continue;
            }
            lastImageOk = true;
            cur()->black = bk; cur()->white = wt; cur()->texDirty = true;
            cur()->displayLut = pendingLut; pendingLut = -1;
            // ONE line per stack, and the line was written from the frame the
            // stack was left on (isSavedLine) - so for a stack the FILE carries
            // (an .npy frame axis, an .npz member, a container node) everything
            // above has just landed on frame 0, which is where the rebuild
            // leaves cur(). `seqframe` then selects the representative frame,
            // which is still holding its decode-time defaults: the user comes
            // back to the wrong range, the wrong LUT and the wrong mosaic
            // reading, with the saved ones sitting invisibly on a frame nobody
            // is looking at.
            //
            // Give the line to the whole stack, which is what the OTHER half of
            // the format already does: pumpSequence copies exactly these fields
            // from a folder stack's head onto every sibling as it lands. The
            // format cannot express a per-frame range (it writes one line), so
            // making both halves agree is the only reading of it that survives
            // a round trip.
            if (cur()->seqId != 0) {
                const ImageDoc* ref = cur();
                for (int idx : framesOfSeq(ref->seqId)) {
                    ImageDoc* d = app.images[idx].get();
                    if (d == ref) continue;
                    d->black = ref->black; d->white = ref->white;
                    d->displayLut = ref->displayLut;
                    d->cfa = ref->cfa; d->cfaPattern = ref->cfaPattern;
                    d->cfaColorize = ref->cfaColorize;
                    d->texDirty = true;
                }
            }
            if (lineOrd == wantCurrent) capturing = true;
        }
    }
    settleCurrent();                      // last image line in the file
    if (resolvedCurrent >= 0) current = resolvedCurrent;
    if (current >= 0 && current < (int)app.images.size()) app.current = current;
    if (haveView && !app.images.empty()) {
        app.view.zoom = std::clamp(zoom, 1.0f / 512, 256.0f);
        app.view.center = center;
        app.fitRequested = false;      // restored view wins over fit-on-load
    }
    // label counters must continue past the restored names, or the next ROI
    // collides and the analysis grid gets two identical column headers
    for (const auto& a : app.anns) {
        int n = atoi(a.label.c_str() + (a.type == 0 ? std::min<size_t>(4, a.label.size())
                                                    : std::min<size_t>(1, a.label.size())));
        if (a.type == 0) app.roiSeq = std::max(app.roiSeq, n);
        else app.poiSeq = std::max(app.poiSeq, n);
    }
    if (selAnnIndex >= 0 && selAnnIndex < (int)app.anns.size())
        app.selectedAnn = app.anns[selAnnIndex].id;
    else
        app.selectedAnn = 0;
    if (!failures.empty()) {
        std::string msg = std::to_string(failures.size()) + " image(s) could not be restored:\n";
        for (size_t i = 0; i < failures.size() && i < 5; i++) msg += "  " + failures[i] + "\n";
        if (failures.size() > 5) msg += "  ...";
        toast(msg, true);
    }
    if (!stackFailures.empty()) {
        std::string msg = std::to_string(stackFailures.size()) +
                          " stack(s) came back as a single frame:\n";
        for (size_t i = 0; i < stackFailures.size() && i < 5; i++)
            msg += "  " + stackFailures[i] + "\n";
        if (stackFailures.size() > 5) msg += "  ...";
        toast(msg, true);
        fprintf(stderr, "session: %zu stack(s) could not be rebuilt; the first is: %s\n",
                stackFailures.size(), stackFailures.front().c_str());
    }
    g_sessionRestoring = false;        // deferred refs resolve at the drain gate
    markAllTexDirty();                 // gamma may have changed
    return {};
}
