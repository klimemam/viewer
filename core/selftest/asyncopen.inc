// ---- --asyncopen-selftest: the open worker (issue #232 stage 2) -------------
//
// WHAT THIS CAN AND CANNOT FIX.
//
// It cannot fix the freeze. A headless process has no window, so nothing here
// can watch one fail to stop answering - that measurement needs a real GUI and a
// WM_NULL ping, and it lives in viewer_work/g232/tools/measure/freeze_probe.ps1
// with its numbers in PR #234's and this change's bodies.
//
// What it CAN fix is the structure the freeze was made of, and there are four
// parts to it:
//
//   (a) openPath RETURNS with nothing read. Before this change it came back with
//       the documents already built, which is another way of saying it had spent
//       the read on this thread.
//   (b) the frame loop RUNS WHILE the reading happens. A worker nobody pumps is
//       a worker that finished before anyone looked, and "it went to a thread"
//       would then be a claim about a variable rather than about a window. So the
//       pumps that found the job still working are counted, and there must be
//       more than one of them - which is why the .npy fixture here is 100 MB and
//       not eight pixels.
//   (c) pumping lands EXACTLY what the synchronous door lands - the documents,
//       the stack, and the source registry entries - because the two doors run
//       one landing (openLocalLand) over one decode (npyDecodeFrames).
//   (d) STOP LEAVES NOTHING. Not "cleans up afterwards": landing happens only in
//       pumpOpenJob and only for a job that was not cancelled, so there is
//       nothing to clean up. The assertion is the absence - no document, and no
//       trace in the source registry, which is the thing a partial landing would
//       leave behind that app.images would not show.
//
// ...and the case without which all four are vacuous: the SAME fixtures opened
// with the scripted default (synchronous) must land immediately and pump zero
// times. That is what --async-open exists for, and this test drives both sides
// of it in one process - the flag arrives from the command line (asserted), and
// the anti-vacuity case turns it off and back on again.
//
// NOGL: every assertion is a count, a pixel value or a registry lookup. No
// window, no texture, no ImGui frame.
static int asyncOpenSelftest(const std::string& root) {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "asyncopenselftest: %-66s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // The flag is the subject as much as the worker is: CMake registers this test
    // with --async-open, so if it did not arrive the rest of the run would pass
    // by being synchronous. Borrowed state, restored at the end (this process's
    // home is shared with every other selftest).
    const bool flagOnEntry = g_forceAsyncOpen;
    check(flagOnEntry, "A0 --async-open reached this run: the worker path is the one "
                       "under test");
    g_forceAsyncOpen = true;

    const std::string media = root + "/media";
    SharedFixture shared(media);          // handed, not ours to grow

    // ---- fixtures -----------------------------------------------------------
    const std::string dir = selftestScratch("viewer_asyncopen");
    // 100 MB, and the size is load-bearing (see (b) above): the worker has to
    // still be working when the pump comes round. (2, 4096, 6400) u16 is
    // 2*4096*6400*2 = 104,857,600 bytes - a TWO-frame stack, so the landing's
    // stack arithmetic and the per-frame registry tuple are under test as well
    // as the phases. Streamed row by row rather than built in memory: the file
    // is 100 MB and there is no reason for the test to hold it twice.
    const int64_t BF = 2, BH = 4096, BW = 6400;
    const std::string big = dir + "/big.npy";
    {
        uint8_t nothing = 0;
        std::vector<uint8_t> hdr = selftestNpyBytes("<u2", { BF, BH, BW }, &nothing, 0);
        std::ofstream f(pathFromUtf8(big), std::ios::binary);
        f.write((const char*)hdr.data(), (std::streamsize)hdr.size());
        std::vector<uint16_t> row((size_t)BW);
        for (int64_t fr = 0; fr < BF; fr++)
            for (int64_t y = 0; y < BH; y++) {
                for (int64_t x = 0; x < BW; x++)
                    row[(size_t)x] = (uint16_t)((fr * 10000 + y + x) & 0xffff);
                f.write((const char*)row.data(), (std::streamsize)(BW * 2));
            }
    }
    // Frame 0 sample (0,0) is 0 and frame 1's is 10000 - derived from the
    // generator above, not read back off the decode.
    const float BIG_F0 = 0.0f, BIG_F1 = 10000.0f;

    // A STORED .npz with exactly one image-shaped member: the case npzPlanOf
    // answers with no question asked, and therefore the only .npz case the
    // worker pre-decodes. (3, 16, 20) u16 - small, because what is under test
    // here is which door it took and not how long it took.
    const std::string onez = dir + "/one.npz";
    const int NZF = 3, NZH = 16, NZW = 20;
    {
        std::vector<uint16_t> px((size_t)NZF * NZH * NZW);
        for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(700 + i);
        selftestWriteNpz(onez, { { "arr", selftestNpyBytes("<u2", { NZF, NZH, NZW },
                                                           px.data(), px.size() * 2) } });
    }

    // ...and a multi-page TIFF, which is the third door: one file, N documents,
    // and a decoder that is neither .npy's nor a zip's. stack3.tif's three pages
    // are constant planes at 1000 / 2000 / 3000 (tools/gen_testdata.py), so
    // "which frame is which" is readable in a failure.
    const std::string tif = media + "/stack3.tif";

    // ---- the pump, bounded --------------------------------------------------
    // waitForOpenJob's loop with a ceiling on it. The ceiling is the difference
    // and it is the point: a worker that never finishes must fail this test
    // rather than hang the suite. It also counts what (b) is about - how many
    // times the loop came round and found the job still working - which is what
    // g_openPumps counts globally; taken as a delta so each case reads its own.
    auto drive = [&](int maxIters) -> bool {
        int it = 0;
        while ((app.openJob || !app.openQueue.empty()) && it++ < maxIters) {
            pumpOpenJob();
            if (app.openJob && !app.openJob->done.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !app.openJob && app.openQueue.empty();
    };
    // Is this tuple in the source registry? The probe is built the way every
    // pre-decode probe is (§6.2): the path, which frame of it, the member if
    // any, and the file's own stat.
    auto inRegistry = [&](const std::string& p, int frame, const std::string& member) {
        FrameSource probe;
        probe.path = p;
        probe.fileFrame = frame;
        probe.member = member;
        statSourceFile(probe);
        const std::string key = srcIdentityKey(probe);
        std::lock_guard<std::mutex> lk(g_srcRegMtx);
        return srcRegistryFindLocked(key) != nullptr;
    };

    // ---- A1: openPath returns with a job up and nothing read ----------------
    {
        closeAll();
        const int starts0 = g_openStarts, pumps0 = g_openPumps;
        openPath(big);
        const bool started = g_openStarts - starts0 == 1 && app.openJob != nullptr &&
                             app.images.empty();
        const std::string phase = app.openJob ? openJobPhase(*app.openJob) : std::string();
        fprintf(stderr, "asyncopenselftest: A1 after openPath: starts+%d job=%d %zu doc(s) "
                        "phase=\"%s\"\n", g_openStarts - starts0,
                (int)(app.openJob != nullptr), app.images.size(), phase.c_str());
        check(started, "A1 openPath STARTS the open and returns: nothing has been read");
        // The row's own words, while the row would be on screen. "reading" or
        // "decoding" - never a percentage, and never a phase the enum does not
        // have (openJobPhase says why the units cannot be mixed).
        check(phase.rfind("reading", 0) == 0 || phase == "decoding",
              "A1 ...and the row names the PHASE it is in, with no percentage");

        // ---- A2: the loop runs while the reading happens -------------------
        const bool drained = drive(4000);
        const int pumped = g_openPumps - pumps0;
        fprintf(stderr, "asyncopenselftest: A2 drained=%d pumps-during-job=%d %zu doc(s) "
                        "%zu seq\n", (int)drained, pumped, app.images.size(),
                app.seqs.size());
        check(drained, "A2 the job finishes and the pump lands it");
        check(pumped >= 2, "A2 ...and the frame loop came round at least twice WHILE it "
                           "was working");

        // ---- A3: it lands exactly what the synchronous door lands ----------
        const bool landed = app.images.size() == (size_t)BF && app.seqs.size() == 1 &&
                            app.openJob == nullptr && app.openQueue.empty();
        const float px0 = app.images.empty() ? -1.0f : app.images[0]->src->data[0];
        const float px1 = app.images.size() < 2 ? -1.0f : app.images[1]->src->data[0];
        fprintf(stderr, "asyncopenselftest: A3 %zu doc(s) %dx%d px[0]=%g px[1]=%g\n",
                app.images.size(), app.images.empty() ? 0 : app.images[0]->w,
                app.images.empty() ? 0 : app.images[0]->h, (double)px0, (double)px1);
        check(landed && app.images[0]->w == (int)BW && app.images[0]->h == (int)BH,
              "A3 a two-frame 100 MB .npy lands as one stack of two, at its own shape");
        check(px0 == BIG_F0 && px1 == BIG_F1,
              "A3 ...with frame 0's and frame 1's own pixels, not one twice");
        check(app.images[0]->seqId != 0 && app.images[0]->seqId == app.images[1]->seqId,
              "A3 ...in ONE stack");
        // The registry is the half app.images does not show: §6.2 says each
        // frame either joins a resident source or registers its own, and the
        // worker's decode must not have skipped it.
        const bool reg = inRegistry(big, 0, std::string()) && inRegistry(big, 1, std::string());
        fprintf(stderr, "asyncopenselftest: A3 registry: frame0=%d frame1=%d\n",
                (int)inRegistry(big, 0, std::string()), (int)inRegistry(big, 1, std::string()));
        check(reg, "A3 ...and both frames are in the source registry under their own "
                   "tuple");
        // The measured range came from the worker, and addImage was told so: a
        // source that reached the registry still showing the default 0/1 is the
        // failure that ordering exists to prevent.
        check(app.images[0]->src->vmin != app.images[0]->src->vmax &&
                  !(app.images[0]->src->vmin == 0 && app.images[0]->src->vmax == 1),
              "A3 ...measured: no source enters the registry showing the default 0/1");
    }

    // ---- A4: Stop leaves nothing -------------------------------------------
    {
        closeAll();
        const size_t regBefore = app.images.size();
        (void)regBefore;
        openPath(big);
        const bool up = app.openJob != nullptr;
        if (app.openJob) app.openJob->cancel.store(true);   // the Files row's Stop
        const bool drained = drive(4000);
        fprintf(stderr, "asyncopenselftest: A4 job-at-stop=%d drained=%d %zu doc(s) "
                        "%zu seq\n", (int)up, (int)drained, app.images.size(),
                app.seqs.size());
        check(up && drained && app.images.empty() && app.seqs.empty(),
              "A4 Stop: the job goes away and NO document lands");
        check(!inRegistry(big, 0, std::string()) && !inRegistry(big, 1, std::string()),
              "A4 ...and no trace of it in the source registry either");
    }

    // ---- A5: the anti-vacuity case ------------------------------------------
    // The same file, the same call, with the scripted default back in place.
    // Everything above would pass on a build that had never moved the read off
    // this thread, if openPath simply did it and returned - so the difference has
    // to be shown, in this process, on this fixture.
    {
        closeAll();
        g_forceAsyncOpen = false;
        const int starts0 = g_openStarts, pumps0 = g_openPumps;
        openPath(big);
        const bool immediate = app.images.size() == (size_t)BF && app.openJob == nullptr &&
                              g_openStarts == starts0;
        pumpOpenJob();                     // the loop's call, with nothing to land
        fprintf(stderr, "asyncopenselftest: A5 scripted default: starts+%d job=%d "
                        "%zu doc(s) pumps+%d\n", g_openStarts - starts0,
                (int)(app.openJob != nullptr), app.images.size(),
                g_openPumps - pumps0);
        check(immediate, "A5 the scripted DEFAULT still opens synchronously: the "
                         "documents are there on the line after openPath");
        check(g_openPumps - pumps0 == 0,
              "A5 ...and no pump was needed, which is what makes A1-A4 a difference");
        g_forceAsyncOpen = true;
    }

    // ---- A6: the .npz door -------------------------------------------------
    // One image-shaped member is the case the container answers with no question
    // asked, so the worker inflates and decodes it; npzPlanOf is the predicate
    // both sides ask, and if they disagreed the landing would decode a second
    // time and this count would still be right - so the pre-decode is checked
    // where it shows, in the member the documents carry.
    {
        closeAll();
        const int starts0 = g_openStarts;
        openPath(onez);
        const bool started = g_openStarts - starts0 == 1 && app.openJob != nullptr &&
                             app.images.empty();
        const bool drained = drive(4000);
        fprintf(stderr, "asyncopenselftest: A6 .npz: starts+%d started=%d drained=%d "
                        "%zu doc(s) member=\"%s\"\n", g_openStarts - starts0, (int)started,
                (int)drained, app.images.size(),
                app.images.empty() ? "" : app.images[0]->src->member.c_str());
        check(started && drained && app.images.size() == (size_t)NZF,
              "A6 a single-member .npz goes through the worker and lands its stack");
        check(!app.images.empty() && app.images[0]->src->member == "arr",
              "A6 ...naming the member it came from (identity the landing completes)");
        check(!app.images.empty() && app.images[0]->src->data[0] == 700.0f &&
                  app.images[1]->src->data[0] ==
                      (float)(700 + NZH * NZW),
              "A6 ...with each member frame's own pixels");
        check(inRegistry(onez, 0, "arr"),
              "A6 ...and the member is in the registry under the container's stat");
    }

    // ---- A7: the picture door ----------------------------------------------
    {
        closeAll();
        const int starts0 = g_openStarts;
        openPath(tif);
        const bool started = g_openStarts - starts0 == 1 && app.openJob != nullptr &&
                             app.images.empty();
        const bool drained = drive(4000);
        const float p0 = app.images.size() > 0 ? app.images[0]->src->data[0] : -1.0f;
        const float p1 = app.images.size() > 1 ? app.images[1]->src->data[0] : -1.0f;
        const float p2 = app.images.size() > 2 ? app.images[2]->src->data[0] : -1.0f;
        fprintf(stderr, "asyncopenselftest: A7 tif: started=%d drained=%d %zu doc(s) "
                        "px=%g/%g/%g\n", (int)started, (int)drained, app.images.size(),
                (double)p0, (double)p1, (double)p2);
        check(started && drained && app.images.size() == 3,
              "A7 a multi-page TIFF goes through the worker and lands three frames");
        check(p0 == 1000.0f && p1 == 2000.0f && p2 == 3000.0f,
              "A7 ...its pages in order, each with its own level");
    }

    // ---- A8: several files at once ------------------------------------------
    // A drop of three, or a multi-select: openPath in a loop, one worker. The
    // rest wait - and the same path asked for twice is one open, which is what a
    // double-click that landed twice looks like from here.
    {
        closeAll();
        const int starts0 = g_openStarts;
        openPath(onez);
        openPath(tif);
        openPath(onez);                    // asked for twice: still one open
        const bool queued = app.openJob != nullptr && app.openQueue.size() == 1 &&
                            g_openStarts - starts0 == 1;
        fprintf(stderr, "asyncopenselftest: A8 three opens -> starts+%d job=%d queued=%zu\n",
                g_openStarts - starts0, (int)(app.openJob != nullptr),
                app.openQueue.size());
        check(queued, "A8 a second file WAITS behind the first, and the same file asked "
                      "for twice is one open");
        const bool drained = drive(8000);
        fprintf(stderr, "asyncopenselftest: A8 drained=%d starts+%d %zu doc(s)\n",
                (int)drained, g_openStarts - starts0, app.images.size());
        check(drained && g_openStarts - starts0 == 2 &&
                  app.images.size() == (size_t)NZF + 3,
              "A8 ...and draining the queue opens BOTH of them");
    }

    // ---- A9: closeAll stops the worker too ----------------------------------
    // Review P2-2. closeAll's own comment says "FIRST: stop everything that
    // produces images, or Close All does not close" - and the open worker is a
    // thing that produces images. It was not in the list, so a file still being
    // read landed AFTER the close; and because loadSession closes before it
    // restores, a 480 MB open in flight when a session was restored put its
    // document into the restored session, in a batch that session never had.
    {
        closeAll();
        openPath(big);
        const bool up = app.openJob != nullptr;
        closeAll();                        // ...as File > Close All and loadSession do
        const bool cancelled = app.openJob && app.openJob->cancel.load();
        const bool queueGone = app.openQueue.empty();
        const bool drained = drive(4000);
        fprintf(stderr, "asyncopenselftest: A9 closeAll: job-up=%d cancelled=%d "
                        "queue-empty=%d drained=%d %zu doc(s) %zu seq\n", (int)up,
                (int)cancelled, (int)queueGone, (int)drained, app.images.size(),
                app.seqs.size());
        check(up && cancelled && queueGone && drained && app.images.empty() &&
                  app.seqs.empty(),
              "A9 closeAll cancels the open in flight: nothing lands after the close");
        check(!inRegistry(big, 0, std::string()),
              "A9 ...and leaves no trace of it in the source registry");
    }

    // ---- A10: Stop, then the same file again --------------------------------
    // Review P2-4. The one-at-a-time door folds a repeat of the path it is
    // already reading into the open in flight - that is what makes a
    // double-click that landed twice one open. A CANCELLED job lands nothing, so
    // folding into it answers "already opening" about an open that is on its way
    // to opening nothing: press Stop, drop the same file again before its phase
    // has finished, and the second drop used to vanish.
    {
        closeAll();
        openPath(onez);
        const bool up = app.openJob != nullptr;
        if (app.openJob) app.openJob->cancel.store(true);
        const int starts0 = g_openStarts;
        openPath(onez);                    // the same file, while the stopped one is up
        // Read at the moment it is claimed, not after the pump: by the time the
        // queue has drained it is empty again and the counter has moved, and a
        // line printed then would be evidence for a different sentence.
        const int startsAtAsk = g_openStarts - starts0;
        const size_t queuedAtAsk = app.openQueue.size();
        const bool notFolded = startsAtAsk == 1 || queuedAtAsk > 0;
        const bool drained = drive(8000);
        fprintf(stderr, "asyncopenselftest: A10 stop-then-reopen: up=%d at-ask starts+%d "
                        "queued=%zu -> drained=%d %zu doc(s)\n", (int)up,
                startsAtAsk, queuedAtAsk, (int)drained, app.images.size());
        check(up && notFolded, "A10 a re-drop is NOT folded into a STOPPED job");
        check(drained && app.images.size() == (size_t)NZF,
              "A10 ...and it opens: the stopped one lands nothing, the new one lands");
    }

    // ---- A11: the door with no frame loop ----------------------------------
    // waitForOpenJob is pumpOpenJob's twin for a caller that has no frame loop -
    // a selftest, or the command line before the window exists. It is asserted
    // here rather than deleted (review P2-6): it puts back ONLY the waiting, so
    // a test that uses it is testing the path the window drives, and an
    // unreferenced function is one nobody has ever run.
    {
        closeAll();
        openPath(big);
        const bool up = app.openJob != nullptr && app.images.empty();
        waitForOpenJob();
        fprintf(stderr, "asyncopenselftest: A11 waitForOpenJob: up=%d %zu doc(s) "
                        "job=%d\n", (int)up, app.images.size(),
                (int)(app.openJob != nullptr));
        check(up && app.openJob == nullptr && app.openQueue.empty() &&
                  app.images.size() == (size_t)BF,
              "A11 waitForOpenJob drives a started open to the end with no frame loop");
    }

    // ---- A12: --zoom / --center survive the worker --------------------------
    // Review P2-3. parseCli used to apply them at the end of itself, which was
    // right while openPath was synchronous: cur() was the file just opened, and
    // `fitRequested = false` said "the view the user named wins over
    // fit-on-load". With the read on a worker there is no cur() yet, so the
    // centre fell back to nothing AND the landing's addImage then raised
    // fitRequested - the fit overwriting the very view the command line asked
    // for. The deferral is asserted the way the frame loop runs it: pumpOpenJob
    // first, applyPendingView after, in the same turn.
    {
        closeAll();
        const float zoom0 = app.view.zoom;
        const ImVec2 center0 = app.view.center;
        const bool fit0 = app.fitRequested;
        app.view.zoom = 1.0f;
        app.view.center = ImVec2(0, 0);
        app.pendingViewZoom = true;    app.pendingZoom = 4.0f;
        app.pendingViewCenter = true;  app.pendingCenter = ImVec2(7, 9);
        openPath(big);
        applyPendingView();            // the call parseCli itself makes
        // Nothing to centre ON yet, so the centre waits. The ZOOM does not need
        // a document and is applied at once, which is what keeps `viewer
        // --zoom 2` with no file working.
        const bool waited = app.pendingViewCenter && app.view.center.x == 0.0f &&
                            app.view.zoom == 4.0f;
        int it = 0;
        while ((app.openJob || !app.openQueue.empty()) && it++ < 4000) {
            pumpOpenJob();
            applyPendingView();        // ...the frame loop's order, exactly
            if (app.openJob && !app.openJob->done.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        fprintf(stderr, "asyncopenselftest: A12 view: waited=%d zoom=%g center=%g,%g "
                        "fitRequested=%d pending=%d/%d %zu doc(s)\n", (int)waited,
                (double)app.view.zoom, (double)app.view.center.x,
                (double)app.view.center.y, (int)app.fitRequested,
                (int)app.pendingViewZoom, (int)app.pendingViewCenter,
                app.images.size());
        check(waited, "A12 the centre waits for the document instead of centring on "
                      "nothing");
        check(app.images.size() == (size_t)BF && app.view.zoom == 4.0f &&
                  app.view.center.x == 7.0f && app.view.center.y == 9.0f &&
                  !app.fitRequested && !app.pendingViewZoom && !app.pendingViewCenter,
              "A12 ...and once it lands the named view WINS over fit-on-load");
        app.view.zoom = zoom0;         // borrowed state, given back
        app.view.center = center0;
        app.fitRequested = fit0;
        app.pendingViewZoom = app.pendingViewCenter = false;
    }

    closeAll();
    g_forceAsyncOpen = flagOnEntry;        // borrowed state, given back
    if (!shared.intact("asyncopenselftest")) ok = false;
    std::error_code ec;
    std::filesystem::remove_all(pathFromUtf8(dir), ec);
    fprintf(stderr, "asyncopenselftest: %s\n", ok ? "ALL PASS" : "FAILURES ABOVE");
    return ok ? 0 : 1;
}
