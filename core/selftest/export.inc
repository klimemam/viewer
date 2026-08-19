// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
    // The series (系列) layer - the MODEL, with no UI behind it (phase 1 draws
    // nothing at all). Every invariant docs/terminology.md and docs/series-plan.md
    // state, checked exhaustively (seriesAudit) after every mutation, on the real
    // load path: Open Folder -> picker -> stacks.
    // ---- --export-selftest: getting a picture OUT ---------------------------
    // Every claim here is one I cannot see on this machine (the GL client area
    // captures white, see docs/background/project/todo-open.md), so each is asserted on the bytes.
    if (!g_exportSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool c, const char* what) {
            fprintf(stderr, "exportselftest: %-58s %s\n", what, c ? "PASS" : "FAILED");
            if (!c) ok = false;
        };
        auto drain = [&]() {
            double t0 = nowSec();
            while (nowSec() - t0 < 120.0) {
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        openFolder(g_exportSelftest);
        if (app.folderPickOpen) pickerAccept();
        drain();
        if (app.images.empty()) { fprintf(stderr, "exportselftest: nothing loaded\n"); return 1; }
        ImageDoc* im = cur();
        check(im && im->w > 0, "E1 an image is open");

        {   // PNG: a real signature, the right size, and the SAME pixels the
            // screen would get (renderDocRGBA is the one mapping, not a copy)
            std::vector<uint8_t> rgba; int w = 0, h = 0;
            bool got = exportDocRGBA(im, rgba, w, h);
            check(got && w == im->w && h == im->h, "E2 the export is the image's own size");
            check(rgba.size() == (size_t)w * h * 4, "E2 ...and one RGBA quad per pixel");
            std::vector<uint8_t> png = encodePng(rgba, w, h);
            static const uint8_t SIG[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
            bool sig = png.size() > 8 && memcmp(png.data(), SIG, 8) == 0;
            check(sig, "E3 the bytes are a PNG");
            // IHDR carries the dimensions big-endian at offset 16
            uint32_t pw = 0, ph = 0;
            if (png.size() > 24) {
                for (int i = 0; i < 4; i++) pw = (pw << 8) | png[16 + i];
                for (int i = 0; i < 4; i++) ph = (ph << 8) | png[20 + i];
            }
            fprintf(stderr, "exportselftest: PNG %zu bytes, IHDR %ux%u, image %dx%d\n",
                    png.size(), pw, ph, w, h);
            check((int)pw == w && (int)ph == h, "E3 ...and its IHDR matches the image");
            // the range really is applied: a black pixel and a white pixel differ
            std::vector<uint8_t> a2;
            float ob = im->black, ow2 = im->white;
            im->black = im->vmin; im->white = im->vmax; renderDocRGBA(*im, a2);
            im->black = im->vmax; im->white = im->vmax + 1;
            std::vector<uint8_t> b2; renderDocRGBA(*im, b2);
            im->black = ob; im->white = ow2;
            check(a2 != b2, "E4 the display range reaches the export");
        }

        {   // E5: a DECIMATED PREVIEW is not an export.
            //
            // `remoteStep > 1` means "still the preview" (state.h:130), and
            // every analysis path in the tree already refuses one by name:
            // preprocess.inc, setanalysis.inc, panel_projection.inc and
            // panel_temporal.inc all carry the same line with the comment
            // `previews lie`. Export was outside that agreement, and it is the
            // one surface that makes an explicit claim about the mapping: the
            // menu item says "dot by dot", i.e. one output pixel per source
            // pixel, which at step 4 is false by a factor of sixteen. The PNG
            // is worse than the clipboard, because a file outlives the sentence
            // that would have qualified it.
            //
            // Anti-vacuity: the SAME document exports once the step is 1, so
            // this cannot pass by refusing everything.
            const int wasStep = im->src->remoteStep;
            std::vector<uint8_t> r1; int w1 = 0, h1 = 0;
            im->src->remoteStep = 4;
            bool prev = exportDocRGBA(im, r1, w1, h1);
            std::string whyPrev = exportRefusalFor(im);
            im->src->remoteStep = 1;
            std::vector<uint8_t> r2; int w2 = 0, h2 = 0;
            bool full = exportDocRGBA(im, r2, w2, h2);
            std::string whyFull = exportRefusalFor(im);
            im->src->remoteStep = wasStep;
            fprintf(stderr, "exportselftest: E5 step 4 -> exported=%d why=\"%s\"; "
                            "step 1 -> exported=%d why=\"%s\"\n",
                    prev ? 1 : 0, whyPrev.c_str(), full ? 1 : 0, whyFull.c_str());
            check(!prev, "E5 a decimated preview is not exported");
            check(whyPrev.find("preview") != std::string::npos &&
                  whyPrev.find("4") != std::string::npos,
                  "E5 ...and the refusal names it a preview, and says the step");
            check(full && whyFull.empty(),
                  "E5 the same document exports at step 1 (anti-vacuity)");
        }

        {   // E6: the file the OS dialog names and the pixels it receives come
            // from the SAME document.
            //
            // The default file name is taken from the document the menu item
            // was used on; the pixels used to be taken from `cur()` when the
            // dialog closed. A load completing, or a follow-frame landing,
            // while the OS dialog is up therefore wrote a DIFFERENT image under
            // the name the user typed for this one - silently, because a PNG
            // says nothing about which document it came from. The ROI export
            // one panel over already keeps this rule for its text ("built NOW:
            // the numbers may change while the OS dialog is open"); this is the
            // same rule for pixels, kept by identity rather than by holding a
            // copy of the frame.
            if (app.images.size() >= 2) {
                ImageDoc* a = app.images[0].get();
                ImageDoc* b = app.images[1].get();
                // the two must actually differ, or "saved the right one" is not
                // a statement anyone could check
                std::vector<uint8_t> ra, rb;
                renderDocRGBA(*a, ra); renderDocRGBA(*b, rb);
                selectImage(1);                       // ...the world moves under the dialog
                std::string pa = (std::filesystem::temp_directory_path() /
                                  "viewer_e6_dialog.png").u8string();
                saveViewPng(pa, a->uid);              // the dialog was opened on A
                std::vector<uint8_t> onDisk;
                readFileBytes(pa, onDisk);
                std::vector<uint8_t> wantA = encodePng(ra, a->w, a->h);
                std::vector<uint8_t> wantB = encodePng(rb, b->w, b->h);
                std::error_code e6ec;
                std::filesystem::remove(pathFromUtf8(pa), e6ec);
                selectImage(0);
                fprintf(stderr, "exportselftest: E6 wrote %zu bytes; A=%zu B=%zu; "
                                "matches A=%d matches B=%d\n",
                        onDisk.size(), wantA.size(), wantB.size(),
                        onDisk == wantA ? 1 : 0, onDisk == wantB ? 1 : 0);
                check(ra != rb, "E6 fixture: the two documents really differ");
                check(!onDisk.empty() && onDisk == wantA,
                      "E6 the pixels are the document the dialog was opened on");
                check(onDisk != wantB,
                      "E6 ...and not whatever became current while it was open");
            } else {
                check(false, "E6 needs two open documents");
            }
        }

        {   // montage: geometry, count, CFA phase, and n-of-N honesty
            std::string err;
            check(!montageROI(true, err), "E5 no ROI selected -> refused");
            fprintf(stderr, "exportselftest: refusal said: %s\n", err.c_str());
            // a rectangle ROI at an ODD origin, to exercise the CFA snap
            int before = (int)app.images.size();
            App::Ann ann; ann.type = 0; ann.x = 3; ann.y = 5; ann.w = 9; ann.h = 7;
            ann.id = app.nextAnnId++;
            app.anns.push_back(ann);
            app.selectedAnn = ann.id;
            std::vector<int> fr = framesOfSeq(im->seqId);
            check(fr.size() >= 2, "E5 the fixture stack has frames to lay out");
            bool made = montageROI(true, err);
            check(made, "E6 horizontal montage is produced");
            if (made) {
                ImageDoc* m = app.images.back().get();
                int n = (int)fr.size();
                // with a CFA image the ROI snaps to even: 3,5,9x7 -> 2,4,10x8
                int erw = im->cfa ? 10 : 9, erh = im->cfa ? 8 : 7;
                fprintf(stderr, "exportselftest: montage %dx%d from %d frame(s), "
                                "ROI %dx%d (cfa=%d), name '%s'\n",
                        m->w, m->h, n, erw, erh, im->cfa, m->name.c_str());
                check(m->w == erw * n && m->h == erh, "E6 ...W = ROI width x frames, H = ROI height");
                check((int)app.images.size() == before + 1, "E6 ...exactly one image was added");
                check(m->batchId == im->batchId, "E7 the montage joins its source's batch");
                check(m->cfa == im->cfa && m->cfaPattern == im->cfaPattern,
                      "E7 ...and keeps the CFA description");
                if (im->cfa) check((erw & 1) == 0 && (erh & 1) == 0,
                                   "E8 the ROI was snapped even, so every tile shares the phase");
                // the first tile really is frame 0's ROI, pixel for pixel
                const ImageDoc& f0 = *app.images[fr[0]];
                int rx = im->cfa ? 2 : 3, ry = im->cfa ? 4 : 5;
                bool same = true;
                for (int y = 0; y < erh && same; y++)
                    for (int x = 0; x < erw; x++)
                        if (m->px()[((size_t)y * m->w + x) * m->ch] !=
                            f0.px()[((size_t)(ry + y) * f0.w + (rx + x)) * f0.ch]) { same = false; break; }
                check(same, "E9 tile 0 is frame 0's ROI, pixel for pixel");
            }
            int mid = (int)app.images.size();
            // the montage BECOMES the current image (you want to see what you
            // just made), and a montage is not a stack - so going round again
            // means going back to the source first. That is the real gesture,
            // not a test artefact.
            selectImage(fr[0]);
            app.selectedAnn = ann.id;
            bool madeV = montageROI(false, err);
            check(madeV, "E10 vertical montage is produced");
            if (madeV) {
                ImageDoc* v = app.images.back().get();
                int n = (int)fr.size();
                int erw = im->cfa ? 10 : 9, erh = im->cfa ? 8 : 7;
                check(v->w == erw && v->h == erh * n, "E10 ...H = ROI height x frames");
                check((int)app.images.size() == mid + 1, "E10 ...one image added");
            }
            // ---- E11: per-frame auto range - a picture, and it says so ------
            {
                selectImage(fr[0]);
                app.selectedAnn = ann.id;
                std::string err2;
                bool madeN = montageROI(true, err2, true);
                check(madeN, "E11 per-frame montage is produced");
                if (madeN) {
                    ImageDoc* mN = app.images.back().get();
                    int erw2 = im->cfa ? 10 : 9, erh2 = im->cfa ? 8 : 7;
                    // every tile spans exactly 0..1: its own min hit 0, max hit 1
                    bool spans = true;
                    for (int k = 0; k < (int)fr.size() && spans; k++) {
                        float lo = FLT_MAX, hi = -FLT_MAX;
                        for (int y = 0; y < erh2; y++)
                            for (int x = 0; x < erw2; x++) {
                                float vpx = mN->px()[((size_t)y * mN->w + (k * erw2 + x)) * mN->ch];
                                if (!std::isfinite(vpx)) continue;
                                lo = std::min(lo, vpx); hi = std::max(hi, vpx);
                            }
                        if (fabsf(lo) > 1e-6f || fabsf(hi - 1.0f) > 1e-6f) spans = false;
                    }
                    check(spans, "E11 every tile is mapped to exactly 0..1");
                    check(mN->black == 0.0f && mN->white == 1.0f,
                          "E11 the display range is the normalised one");
                    check(mN->name.find("per-frame range") != std::string::npos,
                          "E11 the name says per-frame range");
                    check(mN->note.find("NOT DN") != std::string::npos,
                          "E11 the note says the values are not DN");
                    // ...and the ordinary montage still copies raw pixels
                    // (E9 pinned that; this asserts the flag defaults off)
                    check(mN->dtype == "f32", "E11 dtype is f32, whatever the source was");
                }
            }
        }
        fprintf(stderr, "exportselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
        return ok ? 0 : 1;
    }
