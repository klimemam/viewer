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
            // docs/verify-ui.md E3. Everything here goes straight to AddText
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

        fprintf(stderr, "tileselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
