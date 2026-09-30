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


    // ========================================================================
    //  R: the RELOAD worker (the 2026-09-28 report)
    //  ========================================================================
    //
    // Same four things the A group holds down for the open, and the same thing
    // it cannot do: a headless process has no window, so nothing here watches
    // one fail to stop answering (that is freeze_probe.ps1's, and its numbers
    // are in this change's PR body). What it CAN hold down:
    //
    //   (a) the RE-READ is one read. A frame-axis stack is one FrameSource per
    //       frame, and the old walk asked each of them to read the file for
    //       itself - three whole-file reads and six decodes to land three
    //       frames. g_fileReads is the counter; `reads == 1` is the assertion,
    //       and it is V22f's own pattern one gesture over.
    //   (b) the gesture RETURNS with nothing read, and the frame loop comes
    //       round WHILE the reading happens.
    //   (c) the decode ran on ANOTHER THREAD. The one direct statement a
    //       headless test can make about the freeze.
    //   (d) STOP LEAVES WHAT LANDED - the opposite of the open's (d), and the
    //       stack then says it holds two moments (#56's mark).
    //
    // ...and the anti-vacuity case, which is the same one A5 is: the identical
    // gesture with the scripted default back in place lands immediately and
    // pumps zero times.
    //
    // The fixture is its own file rather than big.npy: what is under test is a
    // stack of THREE memberships of ONE file, which is the shape the report is
    // about, and it has to be big enough that the worker is still working when
    // the pump comes round.
    const int64_t RF = 3, RH = 1400, RW = 4000;   // 3 * 1400 * 4000 * 2 = 33.6 MB
    const std::string rel = dir + "/rel.npy";
    // `frames` is a parameter because watch-design §12's frame-axis membership is
    // about the count the HEADER declares: R9/R9e rewrite the same file with a
    // different F and ask what the stack does about it.
    auto writeRelF = [&](int gen, int64_t frames) {
        uint8_t nothing = 0;
        std::vector<uint8_t> hdr = selftestNpyBytes("<u2", { frames, RH, RW }, &nothing, 0);
        std::ofstream f(pathFromUtf8(rel), std::ios::binary);
        f.write((const char*)hdr.data(), (std::streamsize)hdr.size());
        std::vector<uint16_t> row((size_t)RW);
        for (int64_t fr = 0; fr < frames; fr++)
            for (int64_t y = 0; y < RH; y++) {
                for (int64_t x = 0; x < RW; x++)
                    row[(size_t)x] = (uint16_t)((gen * 1000 + fr * 100 + y + x) & 0xffff);
                f.write((const char*)row.data(), (std::streamsize)(RW * 2));
            }
    };
    auto writeRel = [&](int gen) { writeRelF(gen, RF); };
    // sample (0,0) of frame fr in generation `gen`, derived from the generator
    // above rather than read back off a decode
    auto relPx = [&](int gen, int fr) { return (float)(gen * 1000 + fr * 100); };
    // A reload's own pump, bounded exactly as `drive` is and for the same
    // reason: a worker that never finishes must fail this test rather than hang
    // the suite. `stopAfter` >= 0 presses the Files row's Stop as soon as that
    // many items have landed.
    auto driveReload = [&](int maxIters, int stopAfter) -> bool {
        int it = 0;
        while ((app.reloadJob || !app.reloadQueue.empty()) && it++ < maxIters) {
            if (stopAfter >= 0 && app.reloadJob && app.reloadJob->landedN >= stopAfter)
                app.reloadJob->cancel.store(true);
            pumpReloadJob();
            if (app.reloadJob && !app.reloadJob->done.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !app.reloadJob && app.reloadQueue.empty();
    };
    // The three frames' current sample (0,0), in stack order.
    auto relSamples = [&](int seq) {
        std::vector<float> v;
        for (int idx : framesOfSeq(seq)) v.push_back(app.images[idx]->src->data[0]);
        return v;
    };

    // ---- R1: the gesture returns having read nothing ------------------------
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        const std::vector<float> was = relSamples(seq);
        check(seq != 0 && app.images.size() == (size_t)RF &&
                  was.size() == (size_t)RF && was[0] == relPx(1, 0) &&
                  was[2] == relPx(1, 2),
              "R1 (setup) a three-frame single-file stack, each frame its own");
        // three memberships, three DISTINCT sources of one file: the shape the
        // report is about, and the reason the old walk read the file three times
        bool distinct = true;
        const std::vector<int> fr = framesOfSeq(seq);
        for (size_t a = 0; a < fr.size(); a++)
            for (size_t b = a + 1; b < fr.size(); b++)
                if (app.images[fr[a]]->src == app.images[fr[b]]->src) distinct = false;
        check(distinct, "R1 ...and they are three sources of ONE file, not one shared");

        writeRel(2);                       // the file is rewritten under the stack
        const long long reads0 = g_fileReads;
        const int starts0 = g_reloadStarts, pumps0 = g_reloadPumps;
        watchReloadNow(seq, false);        // the Files row's own call
        const bool started = g_reloadStarts - starts0 == 1 && app.reloadJob != nullptr &&
                             app.reloadJob->landedN == 0 &&
                             relSamples(seq) == was;
        const std::string row = app.reloadJob ? reloadJobRowText(*app.reloadJob)
                                              : std::string();
        fprintf(stderr, "asyncopenselftest: R1 after the gesture: starts+%d job=%d "
                        "landed=%d row=\"%s\"\n", g_reloadStarts - starts0,
                (int)(app.reloadJob != nullptr),
                app.reloadJob ? app.reloadJob->landedN : -1, row.c_str());
        check(started, "R1 the gesture STARTS the reload and returns: not a pixel has "
                       "moved yet");
        check(row.find("reloading") == 0 && row.find(" / 3") != std::string::npos,
              "R1 ...and the row says reloading n / 3, a true fraction");

        // ---- R2: the loop runs while the re-reading happens ----------------
        const bool drained = driveReload(8000, -1);
        const long long reads = g_fileReads - reads0;
        const int pumped = g_reloadPumps - pumps0;
        const std::vector<float> now = relSamples(seq);
        fprintf(stderr, "asyncopenselftest: R2 drained=%d reads=%lld pumps-during=%d "
                        "px=%g/%g/%g\n", (int)drained, reads, pumped,
                now.size() > 0 ? (double)now[0] : -1.0,
                now.size() > 1 ? (double)now[1] : -1.0,
                now.size() > 2 ? (double)now[2] : -1.0);
        check(drained, "R2 the job finishes and the pump lands it");
        check(pumped >= 2, "R2 ...and the frame loop came round at least twice WHILE "
                           "it was working");
        // (a), and it is the half of the report the freeze measurement cannot see
        check(reads == 1, "R2 THREE memberships of one file are ONE read of it");
        // (c)
        check(g_reloadDecodeThread != std::this_thread::get_id(),
              "R2 ...and the decode did not run on this thread");
        check(now.size() == (size_t)RF && now[0] == relPx(2, 0) &&
                  now[1] == relPx(2, 1) && now[2] == relPx(2, 2),
              "R2 every frame holds the file's NEW bytes, each its own frame");
        // the landing's own half: the revision every cache is keyed on moved,
        // and the stack is of one moment (no #56 mark)
        bool revved = true;
        for (int idx : framesOfSeq(seq))
            if (app.images[idx]->src->rev == 0) revved = false;
        check(revved && !*seqReloadMark(seq) && seqInfo(seq) &&
                  seqInfo(seq)->reloadFailed == 0,
              "R2 ...the source revisions moved and the stack is of ONE moment");
        // and the registry moved with the bytes (§6.2's re-key): every frame is
        // findable under the file's CURRENT stat
        bool reg = true;
        for (int f = 0; f < (int)RF; f++) reg = reg && inRegistry(rel, f, std::string());
        check(reg, "R2 ...and each frame is in the registry under the NEW tuple");
    }

    // ---- R3: Stop leaves what landed ---------------------------------------
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeRel(2);
        watchReloadNow(seq, false);
        const bool up = app.reloadJob != nullptr;
        const bool drained = driveReload(8000, 1);   // Stop once ONE item has landed
        const std::vector<float> now = relSamples(seq);
        int fresh = 0;
        for (int f = 0; f < (int)RF && f < (int)now.size(); f++)
            if (now[(size_t)f] == relPx(2, f)) fresh++;
        const App::SeqInfo* si = seqInfo(seq);
        fprintf(stderr, "asyncopenselftest: R3 stop: up=%d drained=%d fresh=%d/%d "
                        "docs=%zu mark=\"%s\" err=\"%s\"\n", (int)up, (int)drained,
                fresh, (int)RF, app.images.size(), seqReloadMark(seq),
                si ? si->reloadFirstErr.c_str() : "");
        check(up && drained && app.images.size() == (size_t)RF,
              "R3 Stop: the job goes away and every frame is still a frame");
        check(fresh == 1, "R3 ...ONE frame landed and stayed landed - a stop is not "
                          "an undo");
        check(*seqReloadMark(seq) && si && si->reloadFailed == (int)RF - 1 &&
                  si->reloadOk == 1,
              "R3 ...and the stack says it holds two moments (#56's mark)");
        check(si && si->reloadFirstErr.find("stopped") != std::string::npos,
              "R3 ...for the reason it actually has: it was stopped, nothing refused");
        // no partial document: every frame's mirrors agree with its source, and
        // the un-landed ones still hold generation 1 rather than half of 2
        bool sane = true;
        for (int idx : framesOfSeq(seq)) {
            const ImageDoc& d = *app.images[idx];
            if (d.w != d.src->w || d.h != d.src->h || d.ch != d.src->ch) sane = false;
            if ((int)d.src->data.size() != d.w * d.h * d.ch) sane = false;
        }
        check(sane, "R3 ...and no frame is half-swapped");
    }

    // ---- R4: the anti-vacuity case ------------------------------------------
    // The same gesture on the same fixture with the scripted default back in
    // place. R1-R3 would all pass on a build that had never moved the read off
    // this thread, if watchReloadNow simply did it and returned - so the
    // difference has to be shown, in this process, on this fixture.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeRel(2);
        g_forceAsyncOpen = false;
        const int starts0 = g_reloadStarts, pumps0 = g_reloadPumps;
        watchReloadNow(seq, false);
        const std::vector<float> now = relSamples(seq);
        const bool immediate = app.reloadJob == nullptr &&
                               g_reloadStarts == starts0 &&
                               now.size() == (size_t)RF && now[2] == relPx(2, 2);
        pumpReloadJob();                   // the loop's call, with nothing to land
        fprintf(stderr, "asyncopenselftest: R4 scripted default: starts+%d job=%d "
                        "px2=%g pumps+%d\n", g_reloadStarts - starts0,
                (int)(app.reloadJob != nullptr),
                now.size() > 2 ? (double)now[2] : -1.0, g_reloadPumps - pumps0);
        check(immediate, "R4 the scripted DEFAULT still reloads synchronously: the new "
                         "pixels are there on the line after the gesture");
        check(g_reloadPumps - pumps0 == 0,
              "R4 ...and no pump was needed, which is what makes R1-R3 a difference");
        g_forceAsyncOpen = true;
    }

    // ---- R5: ONE frame takes the worker too ---------------------------------
    // The frame menu's Reload, and the Files row's. Its cost is the SAME cost -
    // a 480 MB frame is one whole-file read and one decode, which is what #232
    // stage 2 moved off this thread for an open of that very file - so it goes
    // through the same job, with one item and no membership.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const std::vector<int> fr = framesOfSeq(app.images.empty() ? 0
                                                                   : app.images[0]->seqId);
        writeRel(2);
        const long long reads0 = g_fileReads;
        const bool took = fr.size() > 1 && startFrameReload(fr[1]);
        const bool nothingYet = app.images[fr[1]]->src->data[0] == relPx(1, 1);
        const bool drained = driveReload(8000, -1);
        fprintf(stderr, "asyncopenselftest: R5 one frame: took=%d nothing-yet=%d "
                        "drained=%d reads=%lld px1=%g px0=%g\n", (int)took,
                (int)nothingYet, (int)drained, g_fileReads - reads0,
                (double)app.images[fr[1]]->src->data[0],
                (double)app.images[fr[0]]->src->data[0]);
        check(took && nothingYet && drained,
              "R5 a single frame's Reload starts a worker and returns having read "
              "nothing");
        check(app.images[fr[1]]->src->data[0] == relPx(2, 1) &&
                  app.images[fr[0]]->src->data[0] == relPx(1, 0),
              "R5 ...and lands THAT frame's new bytes and nobody else's");

        // ...and the ONE-FRAME door folds the way the stack's door does, for
        // the same reason: the question is WHEN the read started, and that does
        // not depend on how many items the job has (review P1-2's rule, applied
        // consistently). Without this the branch would have no caller at all -
        // which is the shape of review P2-8.
        writeRel(3);
        const int starts0 = g_reloadStarts;
        startFrameReload(fr[1]);           // gesture 1: in flight
        writeRel(4);                       // ...and the file moves again
        startFrameReload(fr[1]);           // gesture 2: folded
        const bool folded1 = app.reloadJob != nullptr &&
                             g_reloadStarts - starts0 == 1 && app.reloadJob->again;
        const bool drained2 = driveReload(16000, -1);
        fprintf(stderr, "asyncopenselftest: R5 one-frame fold: folded=%d drained=%d "
                        "starts+%d px1=%g\n", (int)folded1, (int)drained2,
                g_reloadStarts - starts0, (double)app.images[fr[1]]->src->data[0]);
        check(folded1 && drained2 && g_reloadStarts - starts0 == 2,
              "R5b a repeat of ONE frame folds, is remembered, and runs once more");
        check(app.images[fr[1]]->src->data[0] == relPx(4, 1),
              "R5c ...and the re-run reads the generation that was on disk when the "
              "repeat was asked for");
    }

    // ---- R6: ONE AT A TIME, and what a repeat of the same stack means ------
    // Two different claims, and review P1-2 separated them. A second gesture
    // for a stack already reloading starts NO SECOND CONCURRENT job - that is
    // the one-worker rule, and it is what the queue and the fold are for - but
    // it is not DISCARDED either: the read in flight began before the request
    // was made, so it cannot be an answer to it, and the stack runs once more
    // when that job lands. So three gestures over two stacks are one job plus
    // one queued plus one re-run = THREE flights, never three at once.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seqA = app.images.empty() ? 0 : app.images[0]->seqId;
        const size_t nA = app.images.size();
        openPath(big);                     // a second stack, of another file
        drive(8000);
        int seqB = 0;
        for (size_t i = nA; i < app.images.size(); i++)
            if (app.images[i]->seqId != seqA) { seqB = app.images[i]->seqId; break; }
        writeRel(2);
        const int starts0 = g_reloadStarts;
        watchReloadNow(seqA, false);
        watchReloadNow(seqB, false);
        watchReloadNow(seqA, false);       // asked for twice, mid-flight
        const bool queued = app.reloadJob != nullptr && app.reloadQueue.size() == 1 &&
                            g_reloadStarts - starts0 == 1;
        const bool again = app.reloadJob && app.reloadJob->again;
        fprintf(stderr, "asyncopenselftest: R6 three gestures -> starts+%d job=%d "
                        "queued=%zu again=%d\n", g_reloadStarts - starts0,
                (int)(app.reloadJob != nullptr), app.reloadQueue.size(), (int)again);
        check(seqB != 0 && queued,
              "R6 ONE job at a time: a second stack waits, a repeat starts nothing");
        check(again,
              "R6b ...and the repeat is REMEMBERED rather than dropped - the job in "
              "flight did not read it");
        const bool drained = driveReload(16000, -1);
        fprintf(stderr, "asyncopenselftest: R6 drained=%d starts+%d\n", (int)drained,
                g_reloadStarts - starts0);
        check(drained && g_reloadStarts - starts0 == 3,
              "R6c ...so draining gives THREE flights: the first, the queued stack, "
              "and the repeat re-run with a new plan");
    }

    // ---- R7: closeAll stops the reload too ----------------------------------
    // closeAll's rule, one worker over: "stop everything that produces images",
    // and a reload is a thing that REPLACES them. Without this a stack half-way
    // through re-reading would go on swapping frames into sources the close has
    // already thrown away.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeRel(2);
        watchReloadNow(seq, false);
        const bool up = app.reloadJob != nullptr;
        closeAll();                        // ...as File > Close All and loadSession do
        const bool cancelled = app.reloadJob && app.reloadJob->cancel.load();
        const bool queueGone = app.reloadQueue.empty();
        const bool drained = driveReload(8000, -1);
        fprintf(stderr, "asyncopenselftest: R7 closeAll: up=%d cancelled=%d "
                        "queue-empty=%d drained=%d %zu doc(s)\n", (int)up,
                (int)cancelled, (int)queueGone, (int)drained, app.images.size());
        check(up && cancelled && queueGone && drained && app.images.empty(),
              "R7 closeAll cancels the reload in flight and nothing lands after it");
    }

    // ---- R8: the door with no frame loop ------------------------------------
    // waitForReloadJob, waitForOpenJob's twin and A11's reason: it puts back
    // ONLY the waiting, so a caller that uses it is driving the path the window
    // drives.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeRel(2);
        watchReloadNow(seq, false);
        const bool up = app.reloadJob != nullptr;
        waitForReloadJob();
        const std::vector<float> now = relSamples(seq);
        fprintf(stderr, "asyncopenselftest: R8 waitForReloadJob: up=%d job=%d px=%g\n",
                (int)up, (int)(app.reloadJob != nullptr),
                now.empty() ? -1.0 : (double)now[0]);
        check(up && app.reloadJob == nullptr && app.reloadQueue.empty() &&
                  now.size() == (size_t)RF && now[0] == relPx(2, 0),
              "R8 waitForReloadJob drives a started reload to the end with no frame "
              "loop");
    }

    // ---- R9: §12's frame-axis plan survives the worker ----------------------
    // The merge of two changes that landed the same week (board 222 and this
    // one), and the one place they touch: watch-design §12's frame-axis
    // rebuild decides its plan BEFORE a byte is re-read and applies its
    // ARRIVALS after, and those two moments used to be two halves of one call.
    // They are not any more, so the plan travels on App::ReloadJob::axis. If it
    // did not, a .npy whose script wrote five frames where it had written three
    // would come back as three, silently, with two frames of the capture
    // outside the measurement - which is exactly the defect board 222 fixed for
    // the synchronous door.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        check(seq != 0 && framesOfSeq(seq).size() == (size_t)RF,
              "R9 (setup) a three-frame single-file stack");
        writeRelF(3, 5);                   // the same file, FIVE frames now
        watchReloadNow(seq, false);
        const bool took = app.reloadJob != nullptr;
        const bool drained = driveReload(16000, -1);
        const std::vector<float> now = relSamples(seq);
        fprintf(stderr, "asyncopenselftest: R9 grew 3 -> 5: took=%d drained=%d "
                        "frames=%zu px=%g/%g/%g/%g/%g\n", (int)took, (int)drained,
                now.size(),
                now.size() > 0 ? (double)now[0] : -1.0,
                now.size() > 1 ? (double)now[1] : -1.0,
                now.size() > 2 ? (double)now[2] : -1.0,
                now.size() > 3 ? (double)now[3] : -1.0,
                now.size() > 4 ? (double)now[4] : -1.0);
        check(took && drained && now.size() == 5,
              "R9 the frame-axis plan crosses the worker: F 3 -> 5 lands FIVE frames");
        bool own = now.size() == 5;
        for (int f = 0; f < 5 && own; f++) own = now[(size_t)f] == relPx(3, f);
        check(own, "R9b ...each one the FILE's own frame, in the file's order");
        // ...and the other direction, whose departures are applied BEFORE the
        // job starts (so the items are built from what is left).
        writeRel(2);                       // back to three frames
        watchReloadNow(seq, false);
        const bool drained2 = driveReload(16000, -1);
        const std::vector<float> back = relSamples(seq);
        fprintf(stderr, "asyncopenselftest: R9 shrank 5 -> 3: drained=%d frames=%zu "
                        "mark=\"%s\"\n", (int)drained2, back.size(), seqReloadMark(seq));
        check(drained2 && back.size() == (size_t)RF && back[0] == relPx(2, 0) &&
                  back[2] == relPx(2, 2),
              "R9c F 5 -> 3: the tail leaves and the survivors hold their own bytes");
        check(!*seqReloadMark(seq),
              "R9d ...and a frame that is GONE is not a frame that failed to read");
    }

    // ---- R9e: a SECOND gesture while one is in flight ----------------------
    // Review P1-2, written fail-first. The frame-axis plan used to be taken at
    // the GESTURE (watchReloadNow) and handed to startStackReload - and a
    // gesture for a stack that is ALREADY reloading is folded into the job in
    // flight, which threw the plan away. So the shrink side applied its
    // departures and then never ran the arrivals or the bookkeeping, and the
    // grow side asked for six frames and got four: exactly the defect board 222
    // fixed for the synchronous door, back through the folding path.
    //
    // It is reachable by hand: the "Reload from disk" MenuItem is enabled on
    // `anyLocal` alone and never asks whether a reload is running, so the second
    // press during a twelve-frame reload is one click. R6 does not see it (it
    // never changes a frame count) and R9 does not (it never folds).
    //
    // THE RULING (P1-2): the plan and its departures belong to the job that
    // does the READING, so they are taken where the job is created; and a folded
    // gesture is not discarded - the job re-runs once it has landed, with a NEW
    // plan and a NEW stat. "Your request was already read" is only true when the
    // read STARTED AFTER the request, so when it did not, running again is the
    // only correct answer.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        check(seq != 0 && framesOfSeq(seq).size() == (size_t)RF,
              "R9e (setup) a three-frame single-file stack");
        // ---- the GROW side, folded ------------------------------------------
        watchReloadNow(seq, false);                  // gesture 1: job in flight
        const bool flying = app.reloadJob != nullptr;
        writeRelF(3, 5);                             // ...and NOW F becomes 5
        const int starts0 = g_reloadStarts;
        watchReloadNow(seq, false);                  // gesture 2: folded
        const bool folded = app.reloadJob != nullptr &&
                            g_reloadStarts == starts0 && app.reloadQueue.empty();
        const bool drained = driveReload(24000, -1);
        std::vector<float> now = relSamples(seq);
        const App::SeqInfo* si = seqInfo(seq);
        fprintf(stderr, "asyncopenselftest: R9e grow folded: flying=%d folded=%d "
                        "drained=%d frames=%zu expected=%d starts+%d\n",
                (int)flying, (int)folded, (int)drained, now.size(),
                si ? si->expectedFrames : -1, g_reloadStarts - starts0);
        check(flying && folded && drained,
              "R9e a second gesture for a stack already reloading is folded in");
        check(now.size() == 5,
              "R9e2 ...and it is NOT thrown away: F 3 -> 5 still lands FIVE frames");
        bool own = now.size() == 5;
        for (int f = 0; f < 5 && own; f++) own = now[(size_t)f] == relPx(3, f);
        check(own, "R9e3 ...each one the FILE's own frame, from the CURRENT generation");
        check(si && si->expectedFrames == 5,
              "R9e4 ...and the stack's own count says five, not the count it had");

        // ---- the SHRINK side, folded ----------------------------------------
        // The half R9 does not cover at all. Its departures are the dangerous
        // ones: applied at the gesture, they used to leave the stack shorter
        // with nothing re-issuing seqIndex, expectedFrames or the stack name.
        const uint64_t avgRev0 = seqInfo(seq) ? (uint64_t)seqInfo(seq)->stackRev : 0;
        watchReloadNow(seq, false);
        const bool flying2 = app.reloadJob != nullptr;
        writeRelF(4, 2);                             // F 5 -> 2, mid-flight
        watchReloadNow(seq, false);                  // folded again
        const bool drained2 = driveReload(24000, -1);
        now = relSamples(seq);
        si = seqInfo(seq);
        fprintf(stderr, "asyncopenselftest: R9e shrink folded: flying=%d drained=%d "
                        "frames=%zu expected=%d stackRev %llu -> %llu mark=\"%s\"\n",
                (int)flying2, (int)drained2, now.size(), si ? si->expectedFrames : -1,
                (unsigned long long)avgRev0,
                (unsigned long long)(si ? (uint64_t)si->stackRev : 0),
                seqReloadMark(seq));
        check(flying2 && drained2 && now.size() == 2,
              "R9e5 F 5 -> 2 through a FOLDED gesture: the tail leaves");
        check(now.size() == 2 && now[0] == relPx(4, 0) && now[1] == relPx(4, 1),
              "R9e6 ...and the survivors hold the CURRENT generation's bytes");
        check(si && si->expectedFrames == 2,
              "R9e7 ...and the bookkeeping ran: the stack's own count says two");
        // The member LIST moved, so everything keyed on the stack AS A SET has
        // to have moved with it - sigma_t of two frames is not sigma_t of five.
        check(si && (uint64_t)si->stackRev > avgRev0,
              "R9e8 ...and the stack's data revision moved, so its numbers unlatch");
        check(!*seqReloadMark(seq),
              "R9e9 ...and a frame that is GONE is not a frame that failed to read");
    }

    // ========================================================================
    //  R9f..R9k: §12.6 - the ARRIVALS are a SECOND STAGE of the same job
    // ========================================================================
    //
    // Board row 319. PR #275 moved the RE-READ of a frame-axis stack off this
    // thread and left one path behind it: the frames that JOIN when the file
    // grew were decoded by watchReloadFinish, here, one whole-file read PER
    // frame. So the Reload of a capture that wrote five frames where it had
    // written three reproduced the very freeze #275 had just removed.
    //
    // Measured before it was fixed, with freeze_probe.ps1 and the gesture
    // really made (watchFiles + watchAutoReload on, the file replaced from the
    // side): a 480 MB .npy at 3 -> 5 frames froze the pump for 641 ms and at
    // 3 -> 10 frames for 1797 ms, median of 3. This is what a headless run can
    // hold down about that, and it is four separate statements:
    //
    //   (a) THE ARRIVALS DECODED ON THE JOB'S THREAD, not this one. rreader's
    //       R24b is the pattern and its reason: the id is recorded where the
    //       work is DECIDED and read after the job has finished, so there is
    //       nothing left to race with. A SEPARATE probe from the re-read's,
    //       because an assert reading g_reloadDecodeThread would go green on a
    //       build where the arrivals had come back here - which is exactly the
    //       state the board row describes.
    //   (b) ONE READ for any number of joining frames. They are one group of
    //       one file, which is R2's own statement one stage over; before this
    //       they were one whole-file read each.
    //   (c) THE SHAPE IS A RECORD, not a look. The joining frames are measured
    //       against the head's shape AS IT LANDED, written down once. R9h
    //       makes that observable: with the head CROPPED, the full-size
    //       arrivals must be refused against the CROPPED shape - and be said,
    //       with a count, not dropped.
    //   (d) A GEOMETRY THAT MOVED IS SAID. R9i grows the file AND changes its
    //       frame shape; the new frames may join (they are the frames of the
    //       file that is there now) but the sentence has to carry it.
    //
    // The fixture is its own file again, and its shape is a PARAMETER: R9i
    // needs the same path to declare a different frame shape on its second
    // generation, which `rel` cannot do (its H and W are constants).
    const std::string arr = dir + "/arr.npy";
    auto writeArr = [&](int gen, int64_t frames, int64_t h, int64_t w) {
        uint8_t nothing = 0;
        std::vector<uint8_t> hdr = selftestNpyBytes("<u2", { frames, h, w }, &nothing, 0);
        std::ofstream f(pathFromUtf8(arr), std::ios::binary);
        f.write((const char*)hdr.data(), (std::streamsize)hdr.size());
        std::vector<uint16_t> row((size_t)w);
        for (int64_t fr = 0; fr < frames; fr++)
            for (int64_t y = 0; y < h; y++) {
                for (int64_t x = 0; x < w; x++)
                    row[(size_t)x] = (uint16_t)((gen * 1000 + fr * 100 + y + x) & 0xffff);
                f.write((const char*)row.data(), (std::streamsize)(w * 2));
            }
    };
    auto arrPx = [&](int gen, int fr) { return (float)(gen * 1000 + fr * 100); };
    const int64_t AH = 700, AW = 900;      // 3 * 700 * 900 * 2 = 3.8 MB a frame

    {   // ---- R9f/R9g: the arrivals go to the WORKER, and cost ONE read ------
        closeAll();
        writeArr(1, 3, AH, AW);
        openPath(arr);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        check(seq != 0 && framesOfSeq(seq).size() == 3,
              "R9f (setup) a three-frame single-file stack");
        const std::thread::id uiThread = std::this_thread::get_id();
        writeArr(2, 5, AH, AW);                  // the same file, FIVE frames now
        const long long reads0 = g_fileReads;
        const int stages0 = g_reloadArrivalStages, starts0 = g_reloadStarts;
        watchReloadNow(seq, false);
        // The gesture returns having grown NOTHING: the arrivals belong to the
        // job, so not one of them can have joined yet. It is the frame COUNT
        // that is asked and not g_fileReads, and that is review P2-1's lesson
        // (rreader R24): the worker may perfectly well have started reading on
        // the line after the call, so a counter compared across a live thread
        // is not a measurement of anything. A JOIN is this thread's, always, so
        // "three frames" is a completed fact.
        const bool clean = app.reloadJob != nullptr && framesOfSeq(seq).size() == 3;
        const bool drained = driveReload(24000, -1);
        // BOTH ids are read after the job has finished - rreader's R24 review
        // P2-1, and the same reason: a counter or an id compared across a live
        // thread is not a measurement of anything.
        const std::thread::id arrivedOn = g_reloadArrivalThread;
        const std::thread::id reReadOn = g_reloadDecodeThread;
        const long long reads = g_fileReads - reads0;
        const std::vector<float> now = relSamples(seq);
        const App::SeqInfo* si = seqInfo(seq);
        fprintf(stderr, "asyncopenselftest: R9f grew 3 -> 5 off-thread: clean=%d "
                        "drained=%d frames=%zu stages+%d starts+%d reads=%lld "
                        "arrivalOnUi=%d reReadOnUi=%d arrivalRecorded=%d "
                        "expected=%d px=%g/%g/%g/%g/%g\n",
                (int)clean, (int)drained, now.size(),
                g_reloadArrivalStages - stages0, g_reloadStarts - starts0, reads,
                (int)(arrivedOn == uiThread), (int)(reReadOn == uiThread),
                (int)(arrivedOn != std::thread::id()), si ? si->expectedFrames : -1,
                now.size() > 0 ? (double)now[0] : -1.0,
                now.size() > 1 ? (double)now[1] : -1.0,
                now.size() > 2 ? (double)now[2] : -1.0,
                now.size() > 3 ? (double)now[3] : -1.0,
                now.size() > 4 ? (double)now[4] : -1.0);
        check(clean, "R9f the gesture STARTS the grow and returns: nothing read, "
                     "nothing joined");
        check(drained && now.size() == 5,
              "R9f2 ...and draining it lands FIVE frames");
        // (a) - THE statement about the freeze, and both halves of it. The id
        // must not be this thread's, and it must have been WRITTEN: it is
        // cleared at the start of every gesture, so a default-constructed id
        // means stage 2 never ran and the assert must go red rather than green
        // on a vacuous comparison.
        check(g_reloadArrivalStages - stages0 == 1 &&
                  arrivedOn != std::thread::id() && arrivedOn != uiThread,
              "R9f3 the ARRIVALS were decoded on the JOB's thread, not the UI's");
        check(reReadOn != std::thread::id() && reReadOn != uiThread &&
                  arrivedOn != reReadOn,
              "R9f4 ...and that is a SECOND stage: the re-read's own thread is "
              "recorded, is not this one either, and is not the same one");
        // ...and ONE gesture is ONE start, whatever it does in stages: the
        // queue, the fold and §9's drain all count reloads, not passes.
        check(g_reloadStarts - starts0 == 1,
              "R9f5 ...and a two-stage reload is still ONE reload");
        // (b) - 3 memberships are one read (R2), and 2 arrivals are one more.
        // Before this change the arrivals were one whole-file read EACH, so
        // this same fixture read the file 1 + 2 = 3 times.
        check(reads == 2,
              "R9g the whole grow is TWO reads: one for the re-read, one for "
              "every arrival together");
        bool own = now.size() == 5;
        for (int f = 0; f < 5 && own; f++) own = now[(size_t)f] == arrPx(2, f);
        check(own, "R9g2 ...each frame the FILE's own, in the file's order");
        check(si && si->expectedFrames == 5 && !*seqReloadMark(seq),
              "R9g3 ...and the bookkeeping ran, with nothing refused");
    }

    {   // ---- R9h: the SHAPE is a RECORD, and a frame that cannot join says so
        // The head frame is CROPPED before the file grows. Its landed shape is
        // therefore the CROP's, the arrivals decode at full size, and the join
        // must refuse them against the record - by name, with a count, and
        // without shortening or silently re-measuring anything.
        //
        // This is the case that makes "the shape is decided once" observable at
        // all: every frame of one .npy array has the same shape by
        // construction, so a crop on the head is the one way a joining frame
        // can disagree with it. It is reachable by hand in one gesture - crop
        // frame 0, then the script writes two more frames.
        closeAll();
        writeArr(1, 3, AH, AW);
        openPath(arr);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        const std::vector<int> fr0 = framesOfSeq(seq);
        const bool cropped = !fr0.empty() &&
                             cropInPlace(*app.images[fr0.front()], 0, 0, 100, 80);
        check(seq != 0 && fr0.size() == 3 && cropped &&
                  app.images[framesOfSeq(seq).front()]->w == 100,
              "R9h (setup) three frames, and the HEAD is cropped to 100x80");
        writeArr(2, 5, AH, AW);
        const int msgs0 = (int)app.msgLog.size();
        watchReloadNow(seq, false);
        const bool drained = driveReload(24000, -1);
        std::string said;
        for (int i = msgs0; i < (int)app.msgLog.size(); i++)
            if (app.msgLog[(size_t)i].text.find("new frame(s) joined") !=
                std::string::npos)
                said = app.msgLog[(size_t)i].text;
        const App::SeqInfo* si = seqInfo(seq);
        fprintf(stderr, "asyncopenselftest: R9h cropped head: drained=%d frames=%zu "
                        "expected=%d said=\"%s\"\n",
                (int)drained, framesOfSeq(seq).size(), si ? si->expectedFrames : -1,
                said.c_str());
        check(drained && framesOfSeq(seq).size() == 3,
              "R9h a frame that disagrees with the RECORDED head shape does not "
              "join - and the frames it already had are untouched");
        check(said.find("0 of 2 new frame(s) joined") != std::string::npos,
              "R9h2 ...and the summary says HOW MANY joined, which is none");
        check(said.find("frame 3 did not: ") != std::string::npos &&
                  said.find("100x80 1ch") != std::string::npos,
              "R9h3 ...and WHY, measured against the recorded 100x80 head rather "
              "than against the file's own shape");
        check(said.find("the 1 frame(s) after it are not in the measurement") !=
                  std::string::npos,
              "R9h4 ...and that the frame behind the refusal is out too");
        // §12.4: the header's count is still the header's count, and n-of-N is
        // what carries the difference. Nothing is clamped to what fitted.
        check(si && si->expectedFrames == 5,
              "R9h5 ...and the stack's own count is the FILE's 5, not the 3 that "
              "fitted - a refusal is not a smaller capture");
    }

    {   // ---- R9i: the stack's GEOMETRY moved, and the screen says so --------
        // The file grows AND its frames change shape. The arrivals are measured
        // against the new head and may join - they are the frames of the file
        // that is there now - but a stack that quietly became a stack of a
        // different picture is the one outcome that is not allowed.
        closeAll();
        writeArr(1, 3, AH, AW);
        openPath(arr);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        check(seq != 0 && framesOfSeq(seq).size() == 3 &&
                  app.images[framesOfSeq(seq).front()]->w == (int)AW,
              "R9i (setup) three frames of 900x700");
        writeArr(3, 5, AH / 2, AW / 2);      // five frames, and HALF the size
        const int msgs0 = (int)app.msgLog.size();
        watchReloadNow(seq, false);
        const bool drained = driveReload(24000, -1);
        std::string said;
        for (int i = msgs0; i < (int)app.msgLog.size(); i++)
            if (app.msgLog[(size_t)i].text.find("geometry changed") !=
                std::string::npos)
                said = app.msgLog[(size_t)i].text;
        const std::vector<int> fr = framesOfSeq(seq);
        bool allNew = fr.size() == 5;
        for (size_t k = 0; k < fr.size() && allNew; k++)
            allNew = app.images[fr[k]]->w == (int)(AW / 2) &&
                     app.images[fr[k]]->h == (int)(AH / 2);
        fprintf(stderr, "asyncopenselftest: R9i geometry moved: drained=%d "
                        "frames=%zu allNew=%d said=\"%s\"\n",
                (int)drained, fr.size(), (int)allNew, said.c_str());
        check(drained && fr.size() == 5 && allNew,
              "R9i the arrivals join against the NEW head shape, and every frame "
              "of the stack is that shape");
        check(said.find("this stack's geometry changed: its frames are "
                        "450x350 1ch now, was 900x700 1ch") != std::string::npos,
              "R9i2 ...and the stack SAYS its geometry moved, with both shapes");
        check(said.find("the joining frame(s) were measured against the new "
                        "shape") != std::string::npos,
              "R9i3 ...and says which shape the new frames were measured against");
    }

    {   // ---- R9j: a grow that did NOT move the geometry says nothing about it
        // The anti-vacuity of R9i, and R9g's fixture is already this case: a
        // five-frame rewrite at the same shape must not carry the sentence.
        closeAll();
        writeArr(1, 3, AH, AW);
        openPath(arr);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeArr(2, 5, AH, AW);
        const int msgs0 = (int)app.msgLog.size();
        watchReloadNow(seq, false);
        const bool drained = driveReload(24000, -1);
        bool quiet = true;
        for (int i = msgs0; i < (int)app.msgLog.size(); i++) {
            const std::string& t = app.msgLog[(size_t)i].text;
            if (t.find("geometry changed") != std::string::npos ||
                t.find("new frame(s) joined") != std::string::npos)
                quiet = false;
        }
        fprintf(stderr, "asyncopenselftest: R9j same shape: drained=%d frames=%zu "
                        "quiet=%d\n", (int)drained, framesOfSeq(seq).size(),
                (int)quiet);
        check(drained && framesOfSeq(seq).size() == 5 && quiet,
              "R9j a grow at the SAME shape with every frame joining says neither "
              "sentence - both are about a difference");
    }

    {   // ---- R9k: STOP during the arrivals leaves what joined, and says so --
        // §18.2's rule for stage 1 ("Stop leaves what landed") reaches stage 2
        // unchanged, and the half that is new is what the summary owes: a
        // stopped grow must not report the header's count as though it were in
        // the measurement, and it must NOT quietly fall back to reading the
        // rest on this thread.
        closeAll();
        writeArr(1, 3, AH, AW);
        openPath(arr);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        writeArr(2, 6, AH, AW);
        const int msgs0 = (int)app.msgLog.size();
        const long long reads0 = g_fileReads;
        watchReloadNow(seq, false);
        // Stop on the first frame of stage 2 that has landed. The counter is
        // the ARRIVALS' - j->landedN belongs to the re-read, which by then has
        // finished, so driveReload's own stopAfter cannot express this.
        int it = 0;
        while ((app.reloadJob || !app.reloadQueue.empty()) && it++ < 24000) {
            if (app.reloadJob && app.reloadJob->stage == 2 &&
                app.reloadJob->arrivalsLanded >= 1)
                app.reloadJob->cancel.store(true);
            pumpReloadJob();
            if (app.reloadJob && !app.reloadJob->done.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const long long reads = g_fileReads - reads0;
        const size_t left = framesOfSeq(seq).size();
        std::string said;
        for (int i = msgs0; i < (int)app.msgLog.size(); i++)
            if (app.msgLog[(size_t)i].text.find("new frame(s) joined") !=
                std::string::npos)
                said = app.msgLog[(size_t)i].text;
        fprintf(stderr, "asyncopenselftest: R9k stop in stage 2: frames=%zu "
                        "reads=%lld said=\"%s\"\n", left, reads, said.c_str());
        check(!app.reloadJob && left >= 4 && left < 6,
              "R9k Stop during the arrivals: what joined stays joined, and the "
              "rest never join");
        check(said.find("new frame(s) joined") != std::string::npos &&
                  said.find("was stopped before this frame was read") !=
                      std::string::npos,
              "R9k2 ...and the summary names the frames that are not in the "
              "measurement, and says it was a Stop");
        // THE ONE THAT MATTERS: a stopped grow must not make this thread read
        // the file. Two reads is the whole gesture (re-read + arrivals); a
        // fall-back to watchFrameAxisGrow here would add one per missing frame.
        check(reads == 2,
              "R9k3 ...and a stopped grow does NOT fall back to reading the rest "
              "on this thread");
    }

    // ---- R10: the AUTOMATIC door through the worker, and what it blocks -----
    // Review P2-8. Two things had no caller in the whole suite:
    //
    //   watchAutoBusy's newest term (app.reloadJob). --watch and --rwatch are
    //   NOGL runs without --async-open, so startStackReload always declines
    //   there and no reload is ever in flight while they ask; the R group had
    //   never asked at all. The term is what stops §9's drain from queueing a
    //   second stack every frame while the first is still re-reading, and it is
    //   what makes the Files line SAY why it is holding off rather than just
    //   being silent.
    //
    //   ...and `automatic = true` reaching the worker at all. Every R case above
    //   presses the row's own Reload. §9's drain is the caller with nobody's
    //   click behind it, so it is the one whose record has to be there
    //   afterwards - and the record is now a token on the row
    //   (seqAutoReloadMark), which means the whole of §9 rule 3 travels through
    //   this path.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seqA = app.images.empty() ? 0 : app.images[0]->seqId;
        const size_t nA = app.images.size();
        openPath(big);                     // a second stack, of another file
        drive(8000);
        int seqB = 0;
        for (size_t i = nA; i < app.images.size(); i++)
            if (app.images[i]->seqId != seqA) { seqB = app.images[i]->seqId; break; }
        // Borrowed prefs - this process's config home is shared with every other
        // selftest, so the two watch keys are given back at the end of the block.
        const bool wasOn = app.watchEnabled, wasAuto = app.watchAutoReload;
        app.watchEnabled = true;
        app.watchAutoReload = true;
        // seqB armed: a confirmed finding, as watchIntegrate lands one.
        if (App::SeqInfo* si = seqInfo(seqB)) {
            si->watchFound = watch::Finding{};
            si->watchFound.changed = 1;
            si->watchFound.firstChanged = "big.npy";
            si->watchToasted = true;
        }
        // ANTI-VACUITY FIRST: with nothing in flight the drain IS due, or the
        // assertion below is about an unarmed stack and says nothing.
        const bool dueIdle = watchAutoDue();
        const char* busyIdle = watchAutoBusy(seqB);
        writeRel(2);
        watchReloadNow(seqA, false);       // ...and now one is in flight
        const bool flying = app.reloadJob != nullptr;
        const char* busyFly = watchAutoBusy(seqB);
        const bool dueFly = watchAutoDue();
        fprintf(stderr, "asyncopenselftest: R10 armed: due-idle=%d busy-idle=\"%s\" -> "
                        "flying=%d busy=\"%s\" due=%d\n", (int)dueIdle,
                busyIdle ? busyIdle : "", (int)flying, busyFly ? busyFly : "",
                (int)dueFly);
        check(seqB != 0 && dueIdle && !busyIdle,
              "R10 (anti-vacuity) an armed stack with nothing in flight IS due");
        check(flying && busyFly &&
                  std::string(busyFly).find("reload") != std::string::npos,
              "R10b a reload in flight makes every armed stack WAIT, and says so");
        check(!dueFly,
              "R10c ...so the idle-skip chain does not spin while one runs");
        const bool drained = driveReload(16000, -1);
        check(drained && !watchAutoBusy(seqB),
              "R10d ...and the wait ends when the job lands");

        // ---- R10e: automatic = true, all the way through the worker ---------
        writeRel(3);
        watchReloadNow(seqA, true);        // §9's drain's own call
        const bool tookAuto = app.reloadJob != nullptr && app.reloadJob->automatic;
        const bool drained2 = driveReload(16000, -1);
        const App::SeqInfo* sa = seqInfo(seqA);
        const std::string mark = seqAutoReloadMark(seqA);
        fprintf(stderr, "asyncopenselftest: R10e automatic: took=%d drained=%d "
                        "note=\"%s\" when=\"%s\" mark=\"%s\"\n", (int)tookAuto,
                (int)drained2, sa ? sa->autoReloadNote.c_str() : "",
                sa ? sa->autoReloadWhen.c_str() : "", mark.c_str());
        check(tookAuto && drained2 && relSamples(seqA)[0] == relPx(3, 0),
              "R10e §9's drain takes the worker too, and its frames land");
        check(sa && !sa->autoReloadNote.empty() &&
                  sa->autoReloadNote.rfind("auto-reloaded ", 0) == 0,
              "R10f ...and the record §9 rule 3 owes is latched on the stack");
        // ...and the TOKEN the row now carries, built by the real path rather
        // than by a test writing autoReloadWhen by hand (review P2-9). The
        // clock itself is the wall clock, so what is pinned is the shape.
        check(sa && sa->autoReloadWhen.size() == 5 && sa->autoReloadWhen[2] == ':' &&
                  mark == "  (auto-reloaded " + sa->autoReloadWhen + ")",
              "R10g ...and the row's token is that record's clock, to the minute");
        // state.h declares the pair as an invariant: set and cleared together.
        check(sa && !sa->autoReloadWhen.empty() == !sa->autoReloadNote.empty(),
              "R10h ...the note and its clock are set together, as state.h says");

        if (App::SeqInfo* si = seqInfo(seqB)) {
            si->watchFound = watch::Finding{};
            si->watchToasted = false;
        }
        app.watchEnabled = wasOn;          // borrowed prefs, given back
        app.watchAutoReload = wasAuto;
    }

    // ---- R11: Stop before ANY frame landed ----------------------------------
    // The ruling that goes with R3: the behaviour does not change - the stack
    // really is not of one moment and #56's mark is right to be up - but what
    // it SAYS when n is zero was nowhere pinned, so a later reader could not
    // tell the sentence from a bug. seqReloadNote has two halves and this is
    // the one that says nothing was replaced at all.
    {
        closeAll();
        writeRel(1);
        openPath(rel);
        drive(8000);
        const int seq = app.images.empty() ? 0 : app.images[0]->seqId;
        const std::vector<float> was = relSamples(seq);
        writeRel(2);
        watchReloadNow(seq, false);
        const bool drained = driveReload(16000, 0);   // Stop before item 1 lands
        const std::vector<float> now = relSamples(seq);
        const App::SeqInfo* si = seqInfo(seq);
        const std::string note = seqReloadNote(seq);
        fprintf(stderr, "asyncopenselftest: R11 stop-at-zero: drained=%d ok=%d "
                        "failed=%d note=\"%s\"\n", (int)drained,
                si ? si->reloadOk : -1, si ? si->reloadFailed : -1, note.c_str());
        check(drained && now == was,
              "R11 Stop before the first landing leaves every frame exactly as it was");
        check(si && si->reloadOk == 0 && si->reloadFailed == (int)RF,
              "R11b ...and the latch counts all three as not re-read, none re-read");
        check(note.find("no frame could be re-read") != std::string::npos &&
                  note.find("stopped") != std::string::npos,
              "R11c ...and the sentence says BOTH: nothing was replaced, and it was "
              "stopped rather than refused");
    }

    closeAll();
    g_forceAsyncOpen = flagOnEntry;        // borrowed state, given back
    if (!shared.intact("asyncopenselftest")) ok = false;
    std::error_code ec;
    std::filesystem::remove_all(pathFromUtf8(dir), ec);
    fprintf(stderr, "asyncopenselftest: %s\n", ok ? "ALL PASS" : "FAILURES ABOVE");
    return ok ? 0 : 1;
}
