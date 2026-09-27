// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
    // ---- side-by-side tiles: the pane geometry, asserted ----------------------
    // A layout cannot be verified by looking at it here: this machine captures a
    // pure white client area for anything OpenGL draws. So the layout is verified
    // as arithmetic. Four slots (A, B, C, D) are built, and the rectangles
    // tileLayout() hands drawCanvas are checked for count, equal width, no
    // overlap, staying inside the canvas, slot order, and ONE zoom across all of
    // them - the last being the rule that matters most, because two images of one
    // scene at two magnifications side by side is how a reader compares sharpness
    // and concludes the wrong thing.
    if (!g_tileSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 300.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "tileselftest: %-64s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        openFolder(g_tileSelftest);
        loadAll();
        if (app.images.size() < 4) {
            fprintf(stderr, "tileselftest: need 4 images under %s\n", g_tileSelftest.c_str());
            stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
            return 1;
        }

        // ---- T1: the slot list is A, then B, then the extras in order --------
        selectImage(0);
        setCompareB(app.images[1].get());
        app.cmpExtra.clear();
        addCompareSlot(app.images[2].get());
        addCompareSlot(app.images[3].get());
        app.compareMode = App::CmpSplit;
        std::vector<ImageDoc*> ex = compareExtras();
        check(ex.size() == 2 && ex[0] == app.images[2].get() && ex[1] == app.images[3].get(),
              "T1 two extra slots, in the order they were added");
        check(slotOf(app.images[2].get()) == "C" && slotOf(app.images[3].get()) == "D",
              "T1 the extras are lettered C and D");
        std::vector<TileSlot> sl = tileSlots();
        fprintf(stderr, "tileselftest: T1 slots =");
        for (const TileSlot& t : sl)
            fprintf(stderr, "  %s:%s", t.tag.c_str(), t.doc->name.c_str());
        fputc('\n', stderr);
        check(sl.size() == 4, "T1 four panes: A, B and the two extras");
        check(sl.size() == 4 && sl[0].tag == "A" && sl[1].tag == "B" &&
              sl[2].tag == "C" && sl[3].tag == "D",
              "T1 the badges are in slot order A B C D");
        check(sl.size() == 4 && sl[0].doc == cur() && sl[1].doc == cmpB() &&
              sl[2].doc == ex[0] && sl[3].doc == ex[1],
              "T1 the documents are in slot order too");
        {   // One pane per LETTER. Four distinct documents are armed here, so
            // four distinct documents is what must come out - but the reason is
            // that the SEATS are distinct, not that an image may never appear
            // twice. Two letters resolving onto one document (A walked onto a
            // slot, or follow-frame landing both on one sibling) now get a pane
            // each, deliberately: side by side is where you see them coincide.
            bool dupTag = false;
            for (size_t i = 0; i < sl.size(); i++)
                for (size_t j = i + 1; j < sl.size(); j++)
                    if (sl[i].tag == sl[j].tag) dupTag = true;
            check(!dupTag, "T1 no letter occupies two panes");
            bool dupDoc = false;
            for (size_t i = 0; i < sl.size(); i++)
                for (size_t j = i + 1; j < sl.size(); j++)
                    if (sl[i].doc == sl[j].doc) dupDoc = true;
            check(!dupDoc, "T1 four distinct seats give four distinct documents");
        }

        // ---- T2: which modes tile, and which four must not -------------------
        // Wipe, difference and blink each mean exactly two images. Tiling them
        // would be a third thing on a canvas that only has room for a pair.
        check(tileEngaged(), "T2 Split with extras tiles");
        // OFF means off. It used to tile whenever extras existed, so there was
        // no state showing A alone and no way OUT (「A,B,Cから抜けられない」).
        app.compareMode = App::CmpOff;
        check(!tileEngaged(), "T2 compare OFF shows A alone - extras do not override off");
        app.compareMode = App::CmpWipe;
        check(!tileEngaged(), "T2 wipe does not tile: it compares exactly two");
        app.compareMode = App::CmpDiff;
        check(!tileEngaged(), "T2 difference does not tile: it compares exactly two");
        app.compareMode = App::CmpFlip;
        check(!tileEngaged(), "T2 blink does not tile: it compares exactly two");
        app.compareMode = App::CmpSplit;
        {   // and with no extras at all, the plain A/B split is untouched
            std::vector<uint64_t> keep = app.cmpExtra;
            app.cmpExtra.clear();
            check(!tileEngaged(), "T2 two slots: the A/B split keeps the canvas");
            app.cmpExtra = keep;
            check(tileEngaged(), "T2 fixture: the extras are back");
        }

        // ---- T3: the rectangles ----------------------------------------------
        const float X0 = 100.0f, W = 1600.0f, GUT = 3.0f, MINW = 110.0f;
        std::vector<TilePane> pn;
        bool laid = tileLayout(X0, W, (int)sl.size(), GUT, MINW, pn);
        check(laid, "T3 four panes fit a 1600 px canvas");
        check(pn.size() == sl.size(), "T3 one pane per slot, no more and no fewer");
        for (const TilePane& t : pn)
            fprintf(stderr, "tileselftest: T3 pane x0=%.3f w=%.3f x1=%.3f\n",
                    t.x0, t.w, t.x0 + t.w);
        if (laid && pn.size() == 4) {
            float wmin = pn[0].w, wmax = pn[0].w;
            for (const TilePane& t : pn) { wmin = std::min(wmin, t.w); wmax = std::max(wmax, t.w); }
            check(wmax - wmin <= 1e-3f, "T3 the panes are equal width");
            bool ordered = true, gaps = true, disjoint = true;
            for (size_t i = 0; i + 1 < pn.size(); i++) {
                if (!(pn[i].x0 < pn[i + 1].x0)) ordered = false;
                if (pn[i].x0 + pn[i].w > pn[i + 1].x0 + 1e-3f) disjoint = false;
                if (fabsf((pn[i + 1].x0 - (pn[i].x0 + pn[i].w)) - 2.0f * GUT) > 1e-3f) gaps = false;
            }
            check(ordered, "T3 the panes run left to right in slot order");
            check(disjoint, "T3 no two panes overlap");
            check(gaps, "T3 one gutter band between neighbours, all the same width");
            check(fabsf(pn.front().x0 - X0) <= 1e-3f, "T3 the first pane starts at the canvas edge");
            check(pn.back().x0 + pn.back().w <= X0 + W + 1e-3f,
                  "T3 the last pane ends inside the canvas");
            float used = (float)(pn.size() - 1) * 2.0f * GUT;
            for (const TilePane& t : pn) used += t.w;
            check(fabsf(used - W) <= 1e-2f, "T3 panes plus gutters account for the whole width");
            check(pn.front().w >= MINW, "T3 every pane is at least the usable minimum");
        }

        // ---- T4: ONE view. Not one per pane ----------------------------------
        if (laid && pn.size() == 4) {
            const ImVec2 CEN(321.5f, 214.25f);
            const float Z = 2.75f, PY = 40.0f, PH = 900.0f;
            bool zoomSame = true, centreSame = true, sameOffset = true, roundTrip = true;
            ImVec2 ref0(0, 0);
            for (size_t i = 0; i < pn.size(); i++) {
                ImVec2 p0(pn[i].x0, PY), sz(pn[i].w, PH);
                // the scale this pane draws at, read off the mapping itself
                ImVec2 a = tileMap(p0, sz, CEN, Z, 100.0f, 100.0f);
                ImVec2 b = tileMap(p0, sz, CEN, Z, 110.0f, 130.0f);
                if (fabsf((b.x - a.x) / 10.0f - Z) > 1e-3f) zoomSame = false;
                if (fabsf((b.y - a.y) / 30.0f - Z) > 1e-3f) zoomSame = false;
                // the view centre sits at the pane centre in every pane
                ImVec2 c = tileUnmap(p0, sz, CEN, Z, ImVec2(p0.x + sz.x * 0.5f, PY + PH * 0.5f));
                if (fabsf(c.x - CEN.x) > 1e-3f || fabsf(c.y - CEN.y) > 1e-3f) centreSame = false;
                // ...so one image point lands at the same place WITHIN each pane
                ImVec2 off(a.x - p0.x, a.y - p0.y);
                if (i == 0) ref0 = off;
                else if (fabsf(off.x - ref0.x) > 1e-3f || fabsf(off.y - ref0.y) > 1e-3f)
                    sameOffset = false;
                ImVec2 back = tileUnmap(p0, sz, CEN, Z, a);
                if (fabsf(back.x - 100.0f) > 1e-3f || fabsf(back.y - 100.0f) > 1e-3f)
                    roundTrip = false;
            }
            check(zoomSame, "T4 every pane draws at the view zoom, in x and in y");
            check(centreSame, "T4 every pane is centred on the view centre");
            check(sameOffset, "T4 an image point lands identically inside every pane");
            check(roundTrip, "T4 screen -> image -> screen round trips per pane");
        }

        // ---- T5: splitFrac belongs to the A/B pair, and only to it -----------
        {
            std::vector<TilePane> before, after;
            app.splitFrac = 0.5f;
            tileLayout(X0, W, 4, GUT, MINW, before);
            app.splitFrac = 0.9f;                 // the divider dragged hard right
            tileLayout(X0, W, 4, GUT, MINW, after);
            bool same = before.size() == after.size();
            for (size_t i = 0; same && i < before.size(); i++)
                if (fabsf(before[i].x0 - after[i].x0) > 1e-4f ||
                    fabsf(before[i].w - after[i].w) > 1e-4f) same = false;
            fprintf(stderr, "tileselftest: T5 splitFrac 0.5 -> 0.9: pane0 w %.3f -> %.3f\n",
                    before.empty() ? 0.0f : before[0].w, after.empty() ? 0.0f : after[0].w);
            check(same, "T5 the divider cannot make one of four panes a sliver");
            app.splitFrac = 0.5f;
        }

        // ---- T6: degrade by saying so, not by drawing slivers ----------------
        {
            std::vector<TilePane> p2;
            check(!tileLayout(X0, 300.0f, 4, GUT, MINW, p2) && p2.empty(),
                  "T6 four panes in 300 px: refused, and no rectangles handed back");
            check(!tileLayout(X0, W, 16, GUT, MINW, p2) && p2.empty(),
                  "T6 sixteen panes in 1600 px: refused");
            check(tileLayout(X0, W, 8, GUT, MINW, p2) && p2.size() == 8,
                  "T6 eight panes in 1600 px still fit");
            check(!tileLayout(X0, W, 0, GUT, MINW, p2), "T6 zero panes is not a layout");
            check(tileLayout(X0, W, 1, GUT, MINW, p2) && p2.size() == 1 &&
                  fabsf(p2[0].w - W) <= 1e-3f && fabsf(p2[0].x0 - X0) <= 1e-3f,
                  "T6 one pane is the whole canvas, no gutter");
            bool counts = true;
            for (int n = 1; n <= 8; n++)
                if (!tileLayout(X0, W, n, GUT, MINW, p2) || (int)p2.size() != n) counts = false;
            check(counts, "T6 one to eight slots: the pane count always equals the slot count");
            // the exact width where it gives up, stated rather than implied
            float edge = (float)(4 - 1) * 2.0f * GUT + 4.0f * MINW;
            check(tileLayout(X0, edge + 0.5f, 4, GUT, MINW, p2) &&
                  !tileLayout(X0, edge - 0.5f, 4, GUT, MINW, p2),
                  "T6 the cutoff is exactly n*minPane + gutters");
            check(tileMinPaneW(1.0f) == MINW && tileMinPaneW(2.0f) == 2.0f * MINW,
                  "T6 the minimum pane width scales with the UI, like every other size");
        }

        // ---- T7: a closed slot stops existing, and the rest keep their letters
        {
            const uint64_t uidA = sl[0].doc->uid, uidB = sl[1].doc->uid, uidD = sl[3].doc->uid;
            int di = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->uid == uidD) di = i;
            check(di >= 0, "T7 fixture: D is a live image");
            if (di >= 0) {
                selectImage(di);
                closeCurrent(true);               // that image, and only that one
                int ai = -1, bi = -1;
                for (int i = 0; i < (int)app.images.size(); i++) {
                    if (app.images[i]->uid == uidA) ai = i;
                    if (app.images[i]->uid == uidB) bi = i;
                }
                if (ai >= 0) selectImage(ai);
                if (bi >= 0) setCompareB(app.images[bi].get());
                app.compareMode = App::CmpSplit;
                std::vector<TileSlot> s2 = tileSlots();
                fprintf(stderr, "tileselftest: T7 after closing D:");
                for (const TileSlot& t : s2) fprintf(stderr, "  %s", t.tag.c_str());
                fputc('\n', stderr);
                check(s2.size() == 3, "T7 closing D leaves three panes");
                check(s2.size() == 3 && s2[0].tag == "A" && s2[1].tag == "B" &&
                      s2[2].tag == "C", "T7 ...and A, B, C keep their letters");
                std::vector<TilePane> p3;
                check(tileLayout(X0, W, (int)s2.size(), GUT, MINW, p3) && p3.size() == 3,
                      "T7 the layout follows the slot list down to three");
            }
        }

        // ---- T8: cycling C out of the tiles must actually LEAVE ---------------
        // Split -> Diff -> Flip -> Off. Off used to keep tiling (see T2), so
        // the last press appeared to do nothing and there was no exit.
        {
            app.compareMode = App::CmpSplit;
            check(tileEngaged(), "T8 fixture: Split with extras tiles");
            cycleCompare();      // -> difference
            cycleCompare();      // -> blink
            cycleCompare();      // -> off
            fprintf(stderr, "tileselftest: T8 cycled to mode=%d tileEngaged=%d\n",
                    app.compareMode, tileEngaged() ? 1 : 0);
            check(app.compareMode == App::CmpOff, "T8 three C presses from Split reach OFF");
            check(!tileEngaged(), "T8 OFF is A alone: the exit exists");
        }

        // ---- T9: wipe / diff / flip are A against B, never A against a slot --
        // With B unset and a slot present, the default-B fallback used to grab
        // the adjacent image even when it held a letter - so leaving the tiles
        // through the two-image modes silently became an "A,C compare"
        // (「A,C比較になっちゃう」), with the same pixels in two slots.
        {
            int aIdx = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->uid == sl[0].doc->uid) aIdx = i;
            check(aIdx >= 0, "T9 fixture: A is a live image");
            selectImage(aIdx);
            app.cmpExtra.clear();
            setCompareB(nullptr);
            app.lastCompareBUid = 0;
            app.prevImageUid = 0;
            ImageDoc* adjacent = app.images[(app.current + 1) % app.images.size()].get();
            addCompareSlot(adjacent);                 // the image the fallback reaches for
            ensureCompareB();
            ImageDoc* b = resolveB();
            fprintf(stderr, "tileselftest: T9 default B = %s (holds slot '%s')\n",
                    b ? b->name.c_str() : "(none)", b ? slotOf(b).c_str() : "");
            check(b && slotOf(b).empty(), "T9 a default B never lands on a lettered image");
            // choosing a lettered image as B EXPLICITLY takes the letter away:
            // one image, one letter, or the same pixels hold two panes
            setCompareB(adjacent);
            fprintf(stderr, "tileselftest: T9 set-as-B: slot '%s', B=%s\n",
                    slotOf(adjacent).c_str(),
                    resolveB() == adjacent ? "the chosen image" : "someone else");
            check(slotOf(adjacent).empty(), "T9 set-as-B releases the slot letter");
            check(resolveB() == adjacent, "T9 ...and the chosen image IS B");
            // ...and B cannot take a letter while it is B
            addCompareSlot(adjacent);
            check(slotOf(adjacent).empty(), "T9 B refuses a slot letter");
        }

        // ---- T10: fit spans the LARGER dimensions of what is on screen -------
        // (A3, user decision 2026-07-30.) The sides share one view anchored at
        // (0,0), so fitting A alone crops a bigger B; the extras join the union
        // exactly when the tiles have them on screen.
        {
            auto mkDoc = [&](const char* nm, int w, int h) {
                auto d = std::make_unique<ImageDoc>();
                d->name = nm;
                d->src->w = w; d->src->h = h; d->src->ch = 1;
                d->src->dtype = "f32";
                d->src->data.assign((size_t)w * h, 0.f);
                d->syncMirrors();
                d->uid = app.nextUid++;
                app.images.push_back(std::move(d));
                return app.images.back().get();
            };
            ImageDoc* a = cur();
            ImageDoc* big = mkDoc("fitbig.npy", a->w * 2, a->h * 3);
            app.cmpExtra.clear();
            setCompareB(big);
            app.compareMode = App::CmpWipe;
            fitToCanvas(ImVec2(800.0f, 600.0f));
            float ez = std::min(800.0f / big->w, 600.0f / big->h) * 0.97f;
            fprintf(stderr, "tileselftest: T10 A %dx%d B %dx%d: zoom %.5f want %.5f, "
                            "center (%.1f,%.1f)\n", a->w, a->h, big->w, big->h,
                    app.view.zoom, ez, app.view.center.x, app.view.center.y);
            check(fabsf(app.view.zoom - ez) <= 1e-4f,
                  "T10 fit spans the larger side when B is bigger than A");
            check(fabsf(app.view.center.x - big->w * 0.5f) <= 0.5f &&
                  fabsf(app.view.center.y - big->h * 0.5f) <= 0.5f,
                  "T10 ...and centres the union, so all of B is on screen");
            // tiled: a slot bigger still joins the union
            ImageDoc* huge = mkDoc("fithuge.npy", a->w * 4, a->h * 2);
            addCompareSlot(huge);
            app.compareMode = App::CmpSplit;
            fitToCanvas(ImVec2(500.0f, 400.0f));
            float ez2 = std::min(500.0f / huge->w, 400.0f / big->h) * 0.97f;
            fprintf(stderr, "tileselftest: T10 tiled with %dx%d slot: zoom %.5f want %.5f\n",
                    huge->w, huge->h, app.view.zoom, ez2);
            check(fabsf(app.view.zoom - ez2) <= 1e-4f,
                  "T10 the tiled fit spans the extras too");
            // compare OFF: fit is A's alone, exactly as it always was
            app.compareMode = App::CmpOff;
            app.cmpExtra.clear();
            fitToCanvas(ImVec2(800.0f, 600.0f));
            float ez3 = std::min(800.0f / a->w, 600.0f / a->h) * 0.97f;
            check(fabsf(app.view.zoom - ez3) <= 1e-4f &&
                  fabsf(app.view.center.x - a->w * 0.5f) <= 0.5f,
                  "T10 compare off: fit is A's, unchanged");
        }

        // ---- T11: the display-range link spans EVERY pane --------------------
        // (report 2026-07-30: 「表示値域がAuto FitするのがA,BまででCが変わら
        // ない」.) Two images stretched differently cannot be compared - with
        // tiles that goes for every pane. Union mode must fit A, B AND each
        // slot to one union; "B uses A's" must hand A's range to the slots
        // too (the reference side dictates); per-own stays per-own.
        {
            app.rangeScope = 1;
            app.linkRange = false;
            bool saveFollow = app.compareFollowFrame;
            app.compareFollowFrame = false;   // pin = resolved doc, so the
                                              // asserts read the pinned images
            ImageDoc *ta = nullptr, *tb = nullptr, *tc = nullptr;
            for (auto& d : app.images) {      // three distinct open images
                if (!ta) { ta = d.get(); continue; }
                if (!tb) { tb = d.get(); continue; }
                tc = d.get(); break;
            }
            check(ta && tb && tc, "T11 fixture: three images");
            if (ta && tb && tc) {
                int ai = -1;
                for (int i = 0; i < (int)app.images.size(); i++)
                    if (app.images[i].get() == ta) ai = i;
                selectImage(ai);
                app.cmpExtra.clear();
                setCompareB(tb);
                addCompareSlot(tc);
                app.compareMode = App::CmpSplit;
                check(cur() == ta && cmpB() == tb && compareExtras().size() == 1 &&
                      compareExtras()[0] == tc, "T11 fixture: A, B and slot C armed");
                // distinct content ranges and distinct own stretches, so a pane
                // left on its own range cannot pass by luck
                ta->vmin = 10;  ta->vmax = 100;  ta->black = 12;  ta->white = 90;
                tb->vmin = 0;   tb->vmax = 50;   tb->black = 1;   tb->white = 40;
                tc->vmin = -5;  tc->vmax = 200;  tc->black = 0;   tc->white = 1;
                for (ImageDoc* t : { ta, tb, tc })   // forced range = mirror write: set both sides
                    { t->src->vmin = t->vmin; t->src->vmax = t->vmax; }
                app.compareRangeMode = 2;               // union auto
                fprintf(stderr, "tileselftest: T11 union: A %.6g..%.6g  B %.6g..%.6g  "
                                "C %.6g..%.6g  (want -5..200 on all three)\n",
                        effBlack(*ta), effWhite(*ta), effBlack(*tb), effWhite(*tb),
                        effBlack(*tc), effWhite(*tc));
                check(effBlack(*ta) == -5 && effWhite(*ta) == 200 &&
                      effBlack(*tb) == -5 && effWhite(*tb) == 200,
                      "T11 union mode: A and B span the union of all three");
                check(effBlack(*tc) == -5 && effWhite(*tc) == 200,
                      "T11 union mode: slot C spans the SAME union, not its own");
                app.compareRangeMode = 1;               // the reference dictates
                fprintf(stderr, "tileselftest: T11 B-uses-A: A %.6g..%.6g  B %.6g..%.6g  "
                                "C %.6g..%.6g  (want 12..90 on all three)\n",
                        effBlack(*ta), effWhite(*ta), effBlack(*tb), effWhite(*tb),
                        effBlack(*tc), effWhite(*tc));
                check(effBlack(*tb) == 12 && effWhite(*tb) == 90,
                      "T11 B-uses-A: B is stretched to A's black-white");
                check(effBlack(*tc) == 12 && effWhite(*tc) == 90,
                      "T11 B-uses-A: slot C uses A's range too");
                app.compareRangeMode = 0;               // each keeps its own
                check(effBlack(*ta) == 12 && effWhite(*ta) == 90 &&
                      effBlack(*tb) == 1 && effWhite(*tb) == 40 &&
                      effBlack(*tc) == 0 && effWhite(*tc) == 1,
                      "T11 per-own mode: every pane keeps its own stretch");
                // ...and the label names what the range covers: a label that
                // undersells what it covers is wrong the moment C joins
                app.compareRangeMode = 2;
                std::string said = abRangeSaid(true);
                fprintf(stderr, "tileselftest: T11 label with slot C: '%s'\n", said.c_str());
                check(said.find("compared images") != std::string::npos,
                      "T11 the union label names the slots, not just A and B");
                app.cmpExtra.clear();
                said = abRangeSaid(true);
                check(said == std::string("A and B combined range"),
                      "T11 ...and stays 'A and B' when only A and B compare");
            }
            app.compareFollowFrame = saveFollow;
        }

        // ---- T12: what the pane badges SAY - two lines, letter+batch / name --
        // (user report 2026-07-30: 「一行目にBatch名，二行目がファイル名」.)
        // Real ImGui frames through drawCanvas; the composition is asserted
        // from g_paneBadgeProbe exactly as the geometry is from
        // g_tilePanesDrawn - this machine cannot screenshot GL.
        {
            bool saveFollow = app.compareFollowFrame;
            app.compareFollowFrame = false;   // pin = resolved doc, as in T11
            ImageDoc *ta = nullptr, *tb = nullptr, *tc = nullptr;
            for (auto& d : app.images) {
                if (!ta) { ta = d.get(); continue; }
                if (!tb) { tb = d.get(); continue; }
                tc = d.get(); break;
            }
            int ai = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i].get() == ta) ai = i;
            selectImage(ai);
            app.cmpExtra.clear();
            setCompareB(tb);
            addCompareSlot(tc);
            app.compareMode = App::CmpSplit;
            if (!needWindow("--tile-selftest (T12, the canvas probe)")) return 1;
            auto canvasFrame = [&](float w, float h) {
                for (int pass = 0; pass < 2; pass++) {
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui_ImplGlfw_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                    ImGui::Begin("CanvasProbe", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoDocking);
                    drawCanvas(ImGui::GetContentRegionAvail());
                    ImGui::End();
                    ImGui::EndFrame();
                }
            };
            canvasFrame(1720.0f, 980.0f);
            fprintf(stderr, "tileselftest: T12 tiled badges: '%s'\n",
                    g_paneBadgeProbe.c_str());
            std::string wa = "A|" + batchNameOf(ta) + "|" + ta->name + ";";
            std::string wb = "B|" + batchNameOf(tb) + "|" + tb->name + ";";
            std::string wc = "C|" + batchNameOf(tc) + "|" + tc->name + ";";
            check(!batchNameOf(ta).empty(),
                  "T12 fixture: the images belong to a named batch");
            check(g_paneBadgeProbe.find(wa) != std::string::npos &&
                  g_paneBadgeProbe.find(wb) != std::string::npos &&
                  g_paneBadgeProbe.find(wc) != std::string::npos,
                  "T12 tiles: every pane reads letter+batch over the file name");
            // the wipe's two overlay labels follow the same two-line format
            app.compareMode = App::CmpWipe;
            canvasFrame(1720.0f, 980.0f);
            fprintf(stderr, "tileselftest: T12 wipe badges: '%s'\n",
                    g_paneBadgeProbe.c_str());
            check(g_paneBadgeProbe.find(wa) != std::string::npos &&
                  g_paneBadgeProbe.find(wb) != std::string::npos,
                  "T12 wipe: both sides carry the same two-line identity");
            // a NARROW pane elides from the front (elideFront) or drops the
            // line - it never overflows into the neighbour's pixels
            canvasFrame(220.0f, 500.0f);
            size_t ap = g_paneBadgeProbe.find("A|");
            std::string aline2;
            if (ap != std::string::npos) {
                std::string entry = g_paneBadgeProbe.substr(
                    ap, g_paneBadgeProbe.find(';', ap) - ap);
                size_t p1 = entry.find('|');
                size_t p2 = entry.find('|', p1 + 1);
                aline2 = entry.substr(p2 + 1);
            }
            fprintf(stderr, "tileselftest: T12 narrow wipe: A's name line drawn "
                            "as '%s' (full '%s')\n", aline2.c_str(), ta->name.c_str());
            check(ap != std::string::npos, "T12 narrow: the A badge itself survives");
            check(aline2 != ta->name &&
                  (aline2.empty() || aline2.compare(0, 3, "...") == 0),
                  "T12 narrow: the name elides from the front or drops, never overflows");

            // ---- T13: the footer strip, through the same real frames --------
            // E3 in docs/verification/results/20260803-ui.md; the probe's
            // standing contract is docs/verification/ui.md §3.
            // Everything here goes straight to AddText
            // and was kept nowhere, so the whole strip was untestable and the
            // row had to be moved out of the A section. g_footerProbe records
            // the strings as drawn, and these are the claims about them.
            app.compareMode = App::CmpOff;
            float saveZoom = app.view.zoom;
            app.view.zoom = 1.0f;
            canvasFrame(1720.0f, 980.0f);
            fprintf(stderr, "tileselftest: T13 footer at zoom 1: '%s'\n",
                    g_footerProbe.c_str());
            // (a) the two-line identity leads with the CONTEXT - the batch, and
            // the series when there is one. Same rule the pane badges follow.
            check(g_footerProbe.find("ctx=" + batchNameOf(cur()) + ";") != std::string::npos,
                  "T13 the footer's context line names the batch");
            check(g_footerProbe.find("zoom=zoom 100%;") != std::string::npos,
                  "T13 zoom 1 reads as 'zoom 100%'");
            // (b) a magnification below the whole-percent threshold must still
            // name a magnification. %.0f would print "zoom 0%" here, and 0% is
            // not small - it is nothing, which is a different claim about the
            // pixels on screen. This is the reason the readout has two formats
            // and the only check that can tell them apart.
            app.view.zoom = 0.005f;
            canvasFrame(1720.0f, 980.0f);
            fprintf(stderr, "tileselftest: T13 footer at zoom 0.005: '%s'\n",
                    g_footerProbe.c_str());
            check(g_footerProbe.find("zoom=zoom 0.5%;") != std::string::npos &&
                  g_footerProbe.find("zoom 0%") == std::string::npos,
                  "T13 a sub-percent zoom says 0.5%, never 0%");
            app.view.zoom = saveZoom;
            // (c) a partially loaded stack says n of N (docs/terminology.md).
            // The counter had no reader at all, so the rule was carried only by
            // the comment beside it; expectedFrames is forced here the way a
            // stack still loading would have it.
            if (App::SeqInfo* t13s = cur() ? seqInfo(cur()->seqId) : nullptr) {
                int saveExp = t13s->expectedFrames;
                int resident = (int)framesOfSeq(t13s->id).size();
                t13s->expectedFrames = resident + 4;
                canvasFrame(1720.0f, 980.0f);
                fprintf(stderr, "tileselftest: T13 footer, %d of %d resident: '%s'\n",
                        resident, t13s->expectedFrames, g_footerProbe.c_str());
                check(g_footerProbe.find("count=1/" + std::to_string(resident) +
                                         " of " + std::to_string(resident + 4) + ";")
                      != std::string::npos,
                      "T13 a part-loaded stack's counter says n of N, not n of n");
                t13s->expectedFrames = saveExp;
            }
            app.compareMode = App::CmpSplit;
            app.compareFollowFrame = saveFollow;
        }

        // ---- T14: "Copy view (as displayed)" - the framebuffer readback ------
        // Board 96 (「項目18 表示どおりコピー」), Fable 2026-09-27. Two things
        // are on trial and they are different in kind: WHICH PIXELS came back -
        // the canvas rectangle, at framebuffer resolution, nothing else - and
        // WHAT THE ANNOUNCEMENT SAID about them, because the facts that make the
        // picture reproducible (zoom, filter, panes, overlays) are deliberately
        // not drawn into it.
        //
        // This is the one case in this file that is NOT arithmetic. The frames
        // above stop at ImGui::EndFrame and assert what drawCanvas wrote down;
        // this one renders for real (ImGui::Render + RenderDrawData) and then
        // reads the framebuffer, so the rectangle is checked against pixels: a
        // capture offset by the ruler, or by a pane, or flipped, lands a known
        // colour somewhere it is not.
        //
        // NOT verified here, and the reason is deliberate: the CLIPBOARD.
        // g_copyViewProbeOnly keeps the buffer and leaves the clipboard alone,
        // because a test has no business emptying the clipboard of the machine
        // it runs on. clipboardPutImage past that point is the same call the
        // dot-by-dot copy has made since it shipped.
        {
            if (!needWindow("--tile-selftest (T14, the framebuffer readback)")) return 1;
            const int saveMode = app.compareMode;
            const float saveZoom = app.view.zoom;
            const ImVec2 saveCenter = app.view.center;
            const bool saveGrid = app.showGrid, saveHl = app.highlight.on;
            const bool saveProbe = g_copyViewProbeOnly;
            const std::vector<App::Ann> saveAnns = app.anns;
            const int saveSel = app.selectedAnn;
            const std::vector<uint64_t> saveExtra = app.cmpExtra;
            const uint64_t saveB = app.compareBUid;
            ImGuiIO& t14io = ImGui::GetIO();
            // A frame that is actually DRAWN, and then the same call the frame
            // loop makes at the same point in it (core/main.cpp, right after
            // RenderDrawData and before glfwSwapBuffers). Going through
            // pumpCopyViewRequest rather than copyViewAsDisplayed is the point:
            // the request-flag wiring is exercised, not written out twice.
            auto shotFrame = [&]() {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                // Park the cursor off the canvas. The GLFW backend rewrites
                // MousePos from the REAL cursor every frame (verify.inc:1300),
                // and a cursor that happens to rest over the canvas draws the
                // hovered-pixel outline at zoom >= 2 - orange, 1.5 px, exactly
                // over whichever pixel this test was about to sample.
                t14io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(t14io.DisplaySize, ImGuiCond_Always);
                ImGui::Begin("CopyViewProbe", nullptr,
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoDocking);
                drawCanvas(ImGui::GetContentRegionAvail());
                ImGui::End();
                ImGui::Render();
                const int fw = (int)lroundf(t14io.DisplaySize.x * t14io.DisplayFramebufferScale.x);
                const int fh = (int)lroundf(t14io.DisplaySize.y * t14io.DisplayFramebufferScale.y);
                glViewport(0, 0, fw, fh);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                pumpCopyViewRequest();
            };
            // ...and this is the menu item: file the request, then let frames
            // run. Two of them, because the request deliberately skips the frame
            // it was made on (g_copyViewRequest) - on the real menu that frame
            // still has the open popup lying across the canvas.
            auto shot = [&]() { g_copyViewRequest = 2; shotFrame(); shotFrame(); };
            g_copyViewProbeOnly = true;
            app.compareMode = App::CmpOff;   // one picture: a second pane's badge
            app.cmpExtra.clear();            // would sit on the corner sampled below
            app.compareBUid = 0;
            app.anns.clear(); app.selectedAnn = 0;
            app.highlight.on = false;
            app.showGrid = false;
            selectImage(0);
            ImageDoc* cv = cur();
            // The lattice check below is only as strong as the fixture's SPREAD:
            // multi's frames sit near 1000 DN and the load-time default maps a
            // float image to 0..1, so every pixel renders as 255 and "the capture
            // equals the render" would be white equals white. Fitting the range to
            // this frame's own min..max makes the nine samples nine different
            // bytes, which is what makes a one-pixel error detectable at all.
            const bool saveLink = app.linkRange;
            const float saveBlk = cv->black, saveWht = cv->white;
            app.linkRange = false;
            {
                float lo = cv->px()[0], hi = lo;
                for (size_t i = 1; i < (size_t)cv->w * cv->h; i++) {
                    lo = std::min(lo, cv->px()[i]); hi = std::max(hi, cv->px()[i]);
                }
                cv->black = lo; cv->white = hi > lo ? hi : lo + 1.0f;
                cv->texDirty = true;
            }
            app.view.zoom = 2.0f;
            app.view.center = ImVec2(cv->w * 0.5f, cv->h * 0.5f);
            app.fitRequested = false;        // a pending fit would rewrite the zoom
            app.annRev++;                    // the cleared list, for anything caching on it
            shotFrame();                     // settle the window, build the texture
            shotFrame();
            // The skipped frame, asserted rather than assumed: a capture on the
            // click frame is a picture of the context menu, and the only way to
            // see that this code waits is to look after exactly one frame.
            g_copyViewW = 0; g_copyViewSaid.clear(); g_copyViewPixels.clear();
            g_copyViewRequest = 2;
            shotFrame();
            check(g_copyViewW == 0 && g_copyViewPixels.empty(),
                  "T14 the frame the menu was clicked on is not the one copied");
            shotFrame();                     // ...this one is
            check(g_copyViewW > 0, "T14 ...the next frame, with the menu gone, is");
            const float sx = t14io.DisplayFramebufferScale.x > 0
                           ? t14io.DisplayFramebufferScale.x : 1.0f;
            const float sy = t14io.DisplayFramebufferScale.y > 0
                           ? t14io.DisplayFramebufferScale.y : 1.0f;
            const int wantW = (int)lroundf(g_canvasRect[2] * sx);
            const int wantH = (int)lroundf(g_canvasRect[3] * sy);
            fprintf(stderr, "tileselftest: T14 canvas %.1fx%.1f at (%.1f,%.1f), fb scale %.2fx%.2f "
                            "-> captured %dx%d (want %dx%d), %zu byte(s)\n",
                    g_canvasRect[2], g_canvasRect[3], g_canvasRect[0], g_canvasRect[1],
                    sx, sy, g_copyViewW, g_copyViewH, wantW, wantH, g_copyViewPixels.size());
            fprintf(stderr, "tileselftest: T14 said: '%s'\n", g_copyViewSaid.c_str());
            check(g_copyViewW == wantW && g_copyViewH == wantH && wantW > 1 && wantH > 1,
                  "T14 the capture is the canvas rect x the framebuffer scale");
            check(g_copyViewPixels.size() == (size_t)g_copyViewW * g_copyViewH * 4,
                  "T14 ...and the buffer is that many RGBA pixels");
            // The two sample points. Both are KNOWN before the frame is drawn:
            //   - the canvas corner is the flat plate drawCanvas fills the
            //     rectangle with (IM_COL32(12,14,16,255)), and at zoom 2 an 80x64
            //     fixture is 160x128 px in the middle of a canvas hundreds wide,
            //     so the corner is plate and nothing else. It pins the OFFSET:
            //     four pixels in from the ruler is the ruler, and from a panel
            //     is the panel.
            //   - the middle pixel of the image is renderDocRGBA's byte for that
            //     pixel, because rebuildTexture uploads exactly that buffer and
            //     GL_NEAREST at zoom 2 magnifies it without inventing anything.
            //     It pins the ORIENTATION and the SCALE: the fixture is gaussian
            //     noise, so a one-pixel error anywhere lands a different colour.
            auto at = [&](int x, int y, int c) -> int {
                if (x < 0 || y < 0 || x >= g_copyViewW || y >= g_copyViewH) return -1;
                return g_copyViewPixels[((size_t)y * g_copyViewW + x) * 4 + c];
            };
            auto isPlate = [&](int x, int y) {
                return at(x, y, 0) == 12 && at(x, y, 1) == 14 && at(x, y, 2) == 16 &&
                       at(x, y, 3) == 255;
            };
            const int inset = (int)lroundf(4 * std::max(sx, sy));
            const int rx = g_copyViewW - 1 - inset, ry = g_copyViewH - 1 - inset;
            fprintf(stderr, "tileselftest: T14 plate at the four corners (%d,%d)/(%d,%d)/"
                            "(%d,%d)/(%d,%d): %d%d%d%d (top-left reads %d,%d,%d,%d)\n",
                    inset, inset, rx, inset, inset, ry, rx, ry,
                    isPlate(inset, inset), isPlate(rx, inset), isPlate(inset, ry), isPlate(rx, ry),
                    at(inset, inset, 0), at(inset, inset, 1),
                    at(inset, inset, 2), at(inset, inset, 3));
            // All four, not one: a capture one ruler too far left still has plate
            // at its top-left, and a capture one row too short still has plate at
            // its top. Four corners pin the offset AND the extent.
            check(isPlate(inset, inset) && isPlate(rx, inset) &&
                  isPlate(inset, ry) && isPlate(rx, ry),
                  "T14 all four corners are the canvas plate, not a ruler or a panel");
            {
                // ...and the IMAGE, where drawCanvas put it. The bytes are
                // renderDocRGBA's own, because rebuildTexture uploads exactly
                // that buffer and GL_NEAREST at zoom 2 magnifies it without
                // inventing anything - so this is the strongest statement
                // available here: the readback is the render, at the position the
                // shared map says, over a noise fixture where a one-pixel error
                // lands a different colour.
                std::vector<uint8_t> want;
                renderDocRGBA(*cv, want);
                const ImVec2 rp0(g_canvasRect[0], g_canvasRect[1]);
                const ImVec2 rsz(g_canvasRect[2], g_canvasRect[3]);
                auto capOf = [&](float ix, float iy) {
                    ImVec2 sc = tileMap(rp0, rsz, app.view.center, app.view.zoom, ix, iy);
                    return ImVec2((sc.x - g_canvasRect[0]) * sx, (sc.y - g_canvasRect[1]) * sy);
                };
                int hit = 0, miss = 0;
                std::string line;
                for (int gy = 1; gy <= 3; gy++) for (int gx = 1; gx <= 3; gx++) {
                    const int ix = cv->w * gx / 4, iy = cv->h * gy / 4;
                    ImVec2 q = capOf(ix + 0.5f, iy + 0.5f);
                    const int qx = (int)floorf(q.x), qy = (int)floorf(q.y);
                    const size_t o = ((size_t)iy * cv->w + ix) * 4;
                    char b[96];
                    snprintf(b, sizeof b, " (%d,%d)=%d/%d", ix, iy, at(qx, qy, 0), (int)want[o]);
                    line += b;
                    // Tolerance 1, not 0: the quad's edges need not land on integer
                    // framebuffer pixels, so the sampled texel is the right one but
                    // its neighbour is one unit away in principle. A WRONG
                    // rectangle is not one unit away - it is a different colour.
                    bool one = true;
                    for (int c = 0; c < 3; c++)
                        if (std::abs(at(qx, qy, c) - (int)want[o + c]) > 1) one = false;
                    if (one) hit++; else miss++;
                }
                fprintf(stderr, "tileselftest: T14 image lattice (capture/renderDocRGBA):%s\n",
                        line.c_str());
                check(hit == 9 && miss == 0,
                      "T14 nine image pixels read back as renderDocRGBA's own bytes");
                // ...and that those nine bytes are not nine of the same byte. A
                // uniform lattice would pass the check above against a uniform
                // capture of anything at all, including a plate or a panel.
                int distinct = 0;
                for (int gy = 1; gy <= 3; gy++) for (int gx = 1; gx <= 3; gx++) {
                    const size_t o = ((size_t)(cv->h * gy / 4) * cv->w + cv->w * gx / 4) * 4;
                    bool seen = false;
                    for (int hy = 1; hy <= gy; hy++) for (int hx = 1; hx <= 3; hx++) {
                        if (hy == gy && hx >= gx) continue;
                        const size_t p = ((size_t)(cv->h * hy / 4) * cv->w + cv->w * hx / 4) * 4;
                        if (want[p] == want[o]) seen = true;
                    }
                    if (!seen) distinct++;
                }
                fprintf(stderr, "tileselftest: T14 the lattice carries %d distinct value(s)\n",
                        distinct);
                check(distinct >= 4, "T14 fixture: the lattice is not one flat value");
                // The image's EDGE, from both sides, one pixel apart at the scale
                // the view is drawn at. This is what says the zoom was baked in
                // rather than ignored: at 1x the edge would be 80 px further in.
                ImVec2 e0 = capOf(0, cv->h * 0.5f);
                const int ex = (int)floorf(e0.x), ey = (int)floorf(e0.y);
                fprintf(stderr, "tileselftest: T14 image left edge at capture x=%d: "
                                "outside plate=%d, inside plate=%d\n",
                        ex, isPlate(ex - 3, ey), isPlate(ex + 3, ey));
                check(isPlate(ex - 3, ey) && !isPlate(ex + 3, ey),
                      "T14 the image's left edge is where this zoom puts it");
            }
            // ---- and the sentence, which is where the facts live -------------
            char zbuf[32]; snprintf(zbuf, sizeof zbuf, "zoom %.2fx", app.view.zoom);
            check(g_copyViewSaid.find(zbuf) != std::string::npos,
                  "T14 the announcement states the zoom it baked in");
            check(g_copyViewSaid.find("copied view " + std::to_string(g_copyViewW) + "x" +
                                      std::to_string(g_copyViewH)) != std::string::npos,
                  "T14 ...and the size it actually copied");
            check(g_copyViewSaid.find("nearest") != std::string::npos,
                  "T14 ...and the interpolation (zoom 2 is nearest)");
            check(g_copyViewSaid.find("overlays: none") != std::string::npos,
                  "T14 ...and that nothing was over the image");
            // An ARMED overlay that was not DRAWN must not be claimed: the grid
            // is only drawn past 8x, so at 2x the sentence must stay silent
            // about it. This is the rule that makes the sentence worth reading.
            app.showGrid = true;
            shot();
            fprintf(stderr, "tileselftest: T14 grid on at zoom 2: '%s'\n", g_copyViewSaid.c_str());
            check(g_copyViewSaid.find("grid") == std::string::npos,
                  "T14 a grid that is on but below its zoom threshold is not claimed");
            app.view.zoom = 16.0f;
            shot();
            fprintf(stderr, "tileselftest: T14 grid on at zoom 16: '%s'\n", g_copyViewSaid.c_str());
            check(g_copyViewSaid.find("overlays: grid") != std::string::npos,
                  "T14 ...and IS claimed once it is being drawn");
            // A ROI is an overlay by name, and it is in the pixels either way.
            app.view.zoom = 2.0f;
            app.showGrid = false;
            addAnn(0, 4, 4, 16, 16);
            shot();
            fprintf(stderr, "tileselftest: T14 with one ROI: '%s'\n", g_copyViewSaid.c_str());
            check(g_copyViewSaid.find("overlays: ROI") != std::string::npos,
                  "T14 a drawn ROI frame is named in the announcement");
            // ---- both panes ARE what is displayed (board 96, item 2) ---------
            // A split canvas holds two pictures and the copy takes the whole
            // rectangle, so the sentence has to say so - a reader handed a
            // two-pane image with no note cannot tell which half is which.
            app.anns.clear(); app.selectedAnn = 0;
            setCompareB(app.images[1].get());
            app.compareMode = App::CmpSplit;
            shot();
            fprintf(stderr, "tileselftest: T14 split: '%s'\n", g_copyViewSaid.c_str());
            check(g_copyViewSaid.find(" · A|B") != std::string::npos,
                  "T14 a split canvas is copied whole and announced as A|B");
            const int splitW = g_copyViewW;
            app.compareMode = App::CmpOff;
            shot();
            fprintf(stderr, "tileselftest: T14 compare off: '%s' (%d px wide, split was %d)\n",
                    g_copyViewSaid.c_str(), g_copyViewW, splitW);
            check(g_copyViewSaid.find("A|B") == std::string::npos,
                  "T14 one picture names no panes");
            check(g_copyViewW == splitW,
                  "T14 the rectangle is the canvas either way - panes do not resize it");
            // Everything borrowed goes back: this home is SHARED with every
            // other test in the run (docs/verification/functional.md E-5).
            g_copyViewProbeOnly = saveProbe;
            g_copyViewPixels.clear();
            g_copyViewRequest = 0;
            app.compareMode = saveMode;
            app.cmpExtra = saveExtra;
            app.compareBUid = saveB;
            app.view.zoom = saveZoom;
            app.view.center = saveCenter;
            app.showGrid = saveGrid;
            app.highlight.on = saveHl;
            app.anns = saveAnns;
            app.selectedAnn = saveSel;
            app.annRev++;
            cv->black = saveBlk; cv->white = saveWht; cv->texDirty = true;
            app.linkRange = saveLink;
        }

        // ---- T15: where the FLOATING panels open on a first launch (board 258)
        // The defect: main.cpp's DockBuilder branch placed "ROIs" and "Analysis"
        // with the name-addressed ImGui::SetWindowPos, which goes through
        // FindWindowByName - and neither window had ever been Begin'd when that
        // line ran, so both calls returned having moved nothing and both panels
        // opened at ImGui's default (60,60), overlapping (measured ROIs (60,60)
        // 690x450 / Analysis (60,60) 630x390).
        //
        // WHY THE ARMING IS CALLED HERE RATHER THAN REACHED THROUGH THE APP: the
        // DockBuilder branch lives inside the frame loop in main(), and a
        // selftest returns from main() before that loop is entered. There is no
        // fresh-layout path to drive from here at all. What IS reachable is the
        // whole of the fix - floatPanelPlacement is pure, and floatPlaceApply
        // plus a real Begin is exactly what the two panel sites do - so the work
        // area is this test's own choice and every expected number below is
        // arithmetic rather than a function of this machine's display.
        {
            const ImVec2 wp(0.0f, 26.0f), ws(1600.0f, 1000.0f);
            auto overlap = [](const FloatPlace& a, const FloatPlace& b) {
                return a.pos.x < b.pos.x + b.size.x && b.pos.x < a.pos.x + a.size.x &&
                       a.pos.y < b.pos.y + b.size.y && b.pos.y < a.pos.y + a.size.y;
            };
            FloatPlace r, a2;
            floatPanelPlacement(wp, ws, 1.0f, r, a2);
            const float wantX = wp.x + ws.x * 0.34f;
            fprintf(stderr, "tileselftest: T15 ROIs (%.0f,%.0f) %.0fx%.0f  Analysis "
                            "(%.0f,%.0f) %.0fx%.0f  (work %.0fx%.0f at %.0f,%.0f, "
                            "x=34%% is %.0f)\n",
                    r.pos.x, r.pos.y, r.size.x, r.size.y,
                    a2.pos.x, a2.pos.y, a2.size.x, a2.size.y,
                    ws.x, ws.y, wp.x, wp.y, wantX);
            check(r.armed && a2.armed, "T15 the layout reset arms both panels");
            check(r.pos.x == wantX && a2.pos.x == wantX,
                  "T15 both open at the intended x = 34% of the work width");
            check(!overlap(r, a2), "T15 ...and they do not overlap");
            check(r.pos.x >= wp.x && r.pos.y >= wp.y &&
                  a2.pos.x >= wp.x && a2.pos.y >= wp.y,
                  "T15 neither is placed above or left of the work area");
            // ANTI-VACUOUS: the geometry this replaced - 8% and 42% of the work
            // HEIGHT - overlaps at this very work size, so "they do not overlap"
            // is a claim about the new arithmetic and not about the test.
            {
                FloatPlace o1, o2;
                o1.pos = ImVec2(wantX, wp.y + ws.y * 0.08f);
                o1.size = ImVec2(620, 360);
                o2.pos = ImVec2(wantX, wp.y + ws.y * 0.42f);
                o2.size = ImVec2(560, 420);
                check(overlap(o1, o2),
                      "T15 the OLD 8%/42% pair overlaps here - T15 is not vacuous");
            }
            // A short work area: the pair is lifted back inside it, and still
            // must not overlap - the two are placed relative to each other, so
            // no clamp can push one into the other.
            {
                FloatPlace s1, s2;
                floatPanelPlacement(wp, ImVec2(1600.0f, 700.0f), 1.0f, s1, s2);
                fprintf(stderr, "tileselftest: T15 short work area: ROIs y=%.0f "
                                "Analysis y=%.0f (work y %.0f..%.0f)\n",
                        s1.pos.y, s2.pos.y, wp.y, wp.y + 700.0f);
                check(!overlap(s1, s2) && s1.pos.y >= wp.y && s2.pos.y >= wp.y,
                      "T15 a short work area lifts the pair, never overlaps it");
            }
            // ...and at 150%, where the OLD geometry was worst.
            {
                FloatPlace h1, h2;
                floatPanelPlacement(wp, ws, 1.5f, h1, h2);
                check(!overlap(h1, h2) && h1.size.x == 620 * 1.5f,
                      "T15 the UI scale scales the boxes and still separates them");
            }

            // The ONE SHOT, through real frames: armed, spent by the first Begin,
            // and NOT re-applied afterwards - a placement that ran every frame
            // would drag a window the user has since moved back under the cursor.
            FloatPlace oneShot;
            floatPanelPlacement(wp, ws, 1.0f, oneShot, a2);
            ImVec2 got(0, 0), gotSize(0, 0), moved(0, 0);
            auto placeFrame = [&](bool dragItAway) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                floatPlaceApply(oneShot);          // what the panel site does
                ImGui::Begin("T15Float", nullptr, ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoDocking);
                got = ImGui::GetWindowPos();
                gotSize = ImGui::GetWindowSize();
                ImGui::End();
                // the user's own drag, after the placement: by name, which works
                // NOW - the window exists, which is the whole of the defect
                if (dragItAway) ImGui::SetWindowPos("T15Float", moved);
                ImGui::EndFrame();
            };
            placeFrame(false);
            fprintf(stderr, "tileselftest: T15 first frame put it at (%.0f,%.0f) "
                            "%.0fx%.0f, armed=%d\n", got.x, got.y, gotSize.x,
                    gotSize.y, oneShot.armed ? 1 : 0);
            check(got.x == oneShot.pos.x && got.y == oneShot.pos.y,
                  "T15 the first Begin puts the window where the reset asked");
            check(gotSize.x == oneShot.size.x && gotSize.y == oneShot.size.y,
                  "T15 ...at the size it asked for too");
            check(!oneShot.armed, "T15 ...and the one-shot is spent");
            moved = ImVec2(oneShot.pos.x + 200, oneShot.pos.y + 150);
            placeFrame(true);                      // move it out from under
            placeFrame(false);                     // ...and draw again
            fprintf(stderr, "tileselftest: T15 after a drag to (%.0f,%.0f) it is at "
                            "(%.0f,%.0f)\n", moved.x, moved.y, got.x, got.y);
            check(got.x == moved.x && got.y == moved.y,
                  "T15 a spent one-shot never drags the window back");
        }

        fprintf(stderr, "tileselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
