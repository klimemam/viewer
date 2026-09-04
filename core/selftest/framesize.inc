// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
// ---- --framesize-selftest: stepping frames never changes the picture's size --
// Issue #250「フレーム送りで画像が一段毎小さくなる」. `app.view` is ONE view shared
// by a whole stack, while a remote stack's frames sit at two resolution LEVELS
// at once: each frame arrives DECIMATED (remoteStep 4, so w/4) and is replaced
// by its full-resolution bytes when they land, frame by frame. Stepping across
// a level boundary changes what "one image pixel" means underneath a zoom that
// did not change, so the picture jumps - reported as "one step smaller per
// frame".
//
// The invariant asserted here IS the specification, in one line:
//
//     within one stack, app.view.zoom * cur()->w does not change on a step
//
// That product is the picture's width in CANVAS pixels, so holding it constant
// is exactly "the picture keeps the size it had on screen", written as a number
// two stops of a walk can be compared on.
//
// Its own test, fixture-free and NOGL, on the histhl pattern and for the same
// reasons: every fixture is a SIZE built in memory, every expected number is
// derived by hand beside the assert, and none of it goes near a GL context.
// Deliberately not a section of range.inc - the nearest neighbour, same 担当
// ImageView and the other place selectImage's per-step policies are asserted -
// because range.inc's subject is the DISPLAY RANGE and its fixtures are a real
// two-stack folder on disk, and because #248 is extending that file at the same
// time. A failure here should name the invariant it is about.
//
// NOTHING is left in app.images, in app.seqs or in a borrowed mode: one
// selftest home is shared by every test in the suite.
static int frameSizeSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "framesizeselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    // A frame of a stack and nothing else. The pixels are never read - what is
    // under test is arithmetic on w - but they are allocated all the same, so
    // that what walks through selectImage is a document rather than a shape.
    auto mk = [](const char* nm, int seq, int idx, int W) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = W; d->src->ch = 1;
        d->src->dtype = "u16";
        d->src->data.assign((size_t)W * W, 0.0f);
        d->syncMirrors();
        d->uid = app.nextUid++;
        d->seqId = seq;
        d->seqIndex = idx;
        app.images.push_back(std::move(d));
        app.imagesRev++;
        return (int)app.images.size() - 1;
    };
    auto mkSeq = [](const char* nm) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = nm;
        app.seqs.push_back(si);
        return si.id;
    };
    // What the remote fetcher does when the full-resolution bytes land: the
    // same picture, now through finer data (core/app/remote_client.inc). Called
    // here while the frame is NOT the one on screen, which is the case #250 is
    // about - the on-screen swap corrects the shared view on the spot and was
    // never the half that was wrong.
    auto swapToFull = [](int idx, int W) {
        ImageDoc& d = *app.images[idx];
        d.src->w = W; d.src->h = W;
        d.src->data.assign((size_t)W * W, 0.0f);
        d.preview = false;
        d.syncMirrors();
    };
    // The picture's width on the canvas: zoom (canvas px per image px) times the
    // image's own width. THE quantity under test.
    auto onScreenW = []() { return cur() ? app.view.zoom * (float)cur()->w : 0.0f; };

    // Borrowed, and given back at the end. selectImage's OTHER per-step policy
    // is the range one, and auto-per-frame would re-fit documents this test
    // does not care about; per stack (the default) touches nothing here.
    const int   saveScope = app.rangeScope;
    const bool  saveLink  = app.linkRange;
    const float saveZoom  = app.view.zoom;
    const ImVec2 saveCtr  = app.view.center;
    app.rangeScope = 1;
    app.linkRange  = false;

    // 64 px frames decimated by 4 -> 16 px previews. Powers of two, so every
    // product below is exact in float and can be compared without a tolerance
    // budget doing the test's work for it.
    const int   FULL = 64, STEP = 4, PREV = FULL / STEP;   // 64, 4, 16
    const float Z0   = 2.0f;                               // 2 canvas px per image px
    const float WANT = Z0 * (float)FULL;                   // 128 canvas px wide, by hand

    {   // ---- F1: the report --------------------------------------------------
        // Three frames of one stack. Frame 1 was already full when the view was
        // set (its own swap happened on screen and corrected the shared view);
        // frames 2 and 3 arrived as previews and were swapped to full OFF
        // SCREEN. Walk 1 -> 2 -> 3 -> 1.
        //
        // Every frame is 64 px wide by the time it is stepped onto, so the
        // picture is 2 * 64 = 128 canvas px wide at all four stops.
        //
        // NON-VACUITY, MEASURED on this exact fixture against the old code,
        // with swapToFull also leaving pendingViewScale = 4 the way the remote
        // path did - selectImage then divided the SHARED zoom by it once per
        // step, on a view the on-screen swap had already calibrated:
        //     stop 0 (frame 1): zoom 2      -> 128 px
        //     stop 1 (frame 2): zoom 0.5    ->  32 px   FAIL
        //     stop 2 (frame 3): zoom 0.125  ->   8 px   FAIL
        //     stop 3 (frame 1): zoom 0.125  ->   8 px   FAIL
        // 1/step, then 1/step², and it stays there: "一段毎小さく", to the digit.
        const int seq = mkSeq("f250");
        const int i1 = mk("f250_1.npy", seq, 0, FULL);
        const int i2 = mk("f250_2.npy", seq, 1, PREV);
        const int i3 = mk("f250_3.npy", seq, 2, PREV);
        app.current    = i1;
        app.view.zoom  = Z0;
        app.view.center = ImVec2(FULL * 0.5f, FULL * 0.5f);   // the middle: 32, 32
        swapToFull(i2, FULL);
        swapToFull(i3, FULL);
        const int walk[4] = { i1, i2, i3, i1 };
        bool held = true;
        for (int k = 0; k < 4; k++) {
            selectImage(walk[k]);
            const float got = onScreenW();
            fprintf(stderr, "framesizeselftest: F1 stop %d = frame %d  w %d  zoom %.6g"
                            "  on screen %.6g px (want %.6g)\n",
                    k, cur()->seqIndex + 1, cur()->w, app.view.zoom, got, WANT);
            if (fabsf(got - WANT) > 1e-3f) held = false;
        }
        check(held, "F1 1->2->3->1 over an off-screen preview->full swap");
        closeAll();
    }

    {   // ---- F2: the other direction ----------------------------------------
        // Frame 1 is full and the view is calibrated on it; frame 2 is STILL a
        // preview, because its full-resolution bytes have not landed yet. Step
        // 1 -> 2 -> 1. No swap happens anywhere in this case, which is the
        // point: a remembered swap-time flag has nothing to say here, and the
        // dimensions do.
        //     1 -> 2: r = 16/64 = 0.25, zoom = 2 / 0.25 = 8, and 8 * 16 = 128
        //     2 -> 1: r = 64/16 = 4,    zoom = 8 / 4    = 2, and 2 * 64 = 128
        // The centre travels with it: the middle of the frame is 32 of 64 and
        // 8 of 16 - the same point of the same picture, so center.x / w is 0.5
        // at every stop.
        //
        // NON-VACUITY, MEASURED against the old code, which had no correction
        // in this direction at all: stop 1 came out at zoom 2 x w 16 = 32 px -
        // the frame drawn a quarter of the size of the one beside it - with the
        // centre at 32 of a 16 px frame, which is off the picture entirely.
        // Stop 2 read 128 again only because nothing had moved.
        const int seq = mkSeq("f250_rev");
        const int i1 = mk("rev_1.npy", seq, 0, FULL);
        const int i2 = mk("rev_2.npy", seq, 1, PREV);      // left as a preview
        app.images[i2]->preview = true;
        app.current     = i1;
        app.view.zoom   = Z0;
        app.view.center = ImVec2(FULL * 0.5f, FULL * 0.5f);
        const int walk[3] = { i1, i2, i1 };
        bool held = true, centred = true;
        for (int k = 0; k < 3; k++) {
            selectImage(walk[k]);
            const float got = onScreenW();
            const float rel = app.view.center.x / (float)cur()->w;
            fprintf(stderr, "framesizeselftest: F2 stop %d = frame %d  w %d  zoom %.6g"
                            "  on screen %.6g px (want %.6g)  centre %.6g of w (want 0.5)\n",
                    k, cur()->seqIndex + 1, cur()->w, app.view.zoom, got, WANT, rel);
            if (fabsf(got - WANT) > 1e-3f) held = false;
            if (fabsf(rel - 0.5f) > 1e-4f)  centred = false;
        }
        check(held,    "F2 full -> still-preview -> full, both ways");
        check(centred, "F2 ...and the point at the centre stays at the centre");
        closeAll();
    }

    {   // ---- F3: and it stops at the stack boundary --------------------------
        // The invariant is about ONE stack, where frames are the same size by
        // contract and a width difference can only be a resolution level.
        // Another stack is another picture and a standalone image is another
        // picture again; whatever sizes those get is decided elsewhere and must
        // not be second-guessed from here. The widths below differ by exactly
        // the amount that WOULD move the view inside one stack, so this is the
        // over-reach guard for the fix rather than a restatement of it.
        const int sA = mkSeq("f250_A"), sB = mkSeq("f250_B");
        const int a = mk("A_0.npy", sA, 0, FULL);          // 64
        const int b = mk("B_0.npy", sB, 0, FULL / 2);      // 32
        const int lone = mk("lone.npy", 0, 0, PREV);       // 16, seqId 0 = standalone
        app.current     = a;
        app.view.zoom   = Z0;
        app.view.center = ImVec2(FULL * 0.5f, FULL * 0.5f);
        selectImage(b);
        const float zAB = app.view.zoom, cAB = app.view.center.x;
        selectImage(lone);                                  // stack -> standalone
        const float zBL = app.view.zoom, cBL = app.view.center.x;
        selectImage(a);                                     // standalone -> stack
        const float zLA = app.view.zoom, cLA = app.view.center.x;
        fprintf(stderr, "framesizeselftest: F3 zoom %.6g -> %.6g -> %.6g -> %.6g, "
                        "centre.x %.6g -> %.6g -> %.6g -> %.6g (all want %.6g / %.6g)\n",
                Z0, zAB, zBL, zLA, FULL * 0.5f, cAB, cBL, cLA, Z0, FULL * 0.5f);
        check(zAB == Z0 && zBL == Z0 && zLA == Z0 &&
              cAB == FULL * 0.5f && cBL == FULL * 0.5f && cLA == FULL * 0.5f,
              "F3 crossing a stack boundary leaves the shared view alone");
        closeAll();
    }

    // The home is shared: give back every mode and the view itself.
    app.rangeScope  = saveScope;
    app.linkRange   = saveLink;
    app.view.zoom   = saveZoom;
    app.view.center = saveCtr;
    app.current     = -1;
    fprintf(stderr, "framesizeselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
