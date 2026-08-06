// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- PNG / JPEG / TIFF, through the format seam (core/imagefile.h) ------
    // What this test is FOR, in one line: a picture format must arrive as the
    // same kind of thing a .npy arrives as, with its own numbers intact.
    //
    // So the assertions are about meaning, not about "it opened":
    //   * the 16-bit PNG comes back sample for sample, 0..65535 - not divided
    //     by anything, not read through an 8-bit door;
    //   * a greyscale PNG is ONE channel and RGBA is FOUR (the last-axis rule),
    //     rather than everything being padded to 4 the way a decoder's
    //     convenience API would have delivered it;
    //   * where the decoder DID change the scale (a 4-bit PNG, expanded x17)
    //     the note says so, because the Inspector is where a user finds out
    //     what happened to their numbers;
    //   * a refusal names the file and the reason (docs/input-adapters.md
    //     §3.2) - "unsupported" is not a message, and TIFF being absent from
    //     this build is a different sentence from a file being corrupt.
    //
    // Every case goes through openPath, the door a drop or a command line uses,
    // so what is asserted is the sentence the user actually gets.
    if (!g_mediaSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "mediaselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        const std::string root = g_mediaSelftest;
        auto open1 = [&](const char* name) -> ImageDoc* {
            closeAll();
            app.toast.clear();
            openPath(root + "/" + name);
            return app.images.empty() ? nullptr : app.images[0].get();
        };
        auto has = [](const std::string& hay, const char* needle) {
            return hay.find(needle) != std::string::npos;
        };

        // ---- M1: the seam's own contract ------------------------------------
        // Every row either decodes or explains itself. A row with neither is a
        // format that would refuse with an empty sentence, which is the exact
        // failure this table exists to prevent.
        {
            bool sound = !imagefile::backends().empty();
            for (const imagefile::Backend& b : imagefile::backends()) {
                if (!b.format || !b.exts || !b.sniff) sound = false;
                if (b.decode && (b.absent || !b.library || !*b.library)) sound = false;
                if (!b.decode && (!b.absent || !*b.absent)) sound = false;
            }
            fprintf(stderr, "mediaselftest: table = %zu format(s), decodable: %s\n",
                    imagefile::backends().size(), imagefile::decodableFormats().c_str());
            check(sound, "M1 every format either decodes or says why it does not");
            check(imagefile::decodableFormats() == "PNG, JPEG",
                  "M1 this build decodes PNG and JPEG");
        }

        // ---- M2: 16-bit PNG, sample for sample ------------------------------
        // The round trip: tools/gen_testdata.py wrote (y*W+x)*21, and that is
        // what has to come back. A decoder that normalised to 0..1, or that
        // took the 8-bit path, passes "it opened" and fails here.
        {
            ImageDoc* d = open1("gray16.png");
            bool shape = d && d->w == 64 && d->h == 48 && d->ch == 1;
            bool exact = shape;
            if (shape)
                for (int i = 0; i < 64 * 48 && exact; i++)
                    if (d->px()[i] != (float)((i * 21) % 65536)) exact = false;
            fprintf(stderr, "mediaselftest: M2 %s dtype=%s min=%g max=%g white=%g note=\"%s\"\n",
                    d ? "opened" : "NOT OPENED", d ? d->dtype.c_str() : "",
                    d ? d->vmin : 0.0f, d ? d->vmax : 0.0f, d ? d->white : 0.0f,
                    d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M2 a 16-bit greyscale PNG is one 1-channel frame");
            check(exact, "M2 every sample survives: (y*W+x)*21, unscaled");
            check(d && d->dtype == "u16" && d->vmax == 64491,
                  "M2 the dtype is the FILE's (u16) and the range is 0..65535 DN");
            check(d && d->white == 65535, "M2 the display range defaults to the u16 full scale");
            check(d && has(d->note, "16-bit") && has(d->note, "no scaling"),
                  "M2 the note says what was read and what was NOT done to it");
            check(d && d->src->fsize > 0, "M2 the Watch baseline is stamped (file, size)");
        }

        // ---- M3: the channel rule, all four ways ----------------------------
        // docs/input-adapters.md §3.1: the last axis, four or fewer, is
        // channels. Greyscale is 1 - asking the decoder for RGBA always (the
        // usual way to call one) would have made every mono capture 4-channel
        // and every per-channel statistic meaningless.
        {
            ImageDoc* g = open1("gray8.png");
            bool grey = g && g->ch == 1 && g->dtype == "u8" && g->w == 64 && g->h == 48;
            bool greyVals = grey;
            if (grey)
                for (int y = 0; y < 48 && greyVals; y++)
                    for (int x = 0; x < 64 && greyVals; x++)
                        if (g->px()[(size_t)y * 64 + x] != (float)((x * 4 + y) % 256))
                            greyVals = false;
            check(grey && greyVals, "M3 8-bit greyscale is 1 channel, values as stored");
            check(g && g->white == 255, "M3 an 8-bit file's default range is 0..255 DN");

            ImageDoc* c = open1("rgb8.png");
            bool rgb = c && c->ch == 3 &&
                       c->sample(0, 0, 0) == 0 && c->sample(0, 0, 2) == 96 &&
                       c->sample(63, 0, 0) == 255 && c->sample(0, 47, 1) == 255;
            check(rgb, "M3 RGB is 3 channels, in file order");

            ImageDoc* a = open1("rgba8.png");
            check(a && a->ch == 4 && a->sample(0, 0, 3) == 200,
                  "M3 RGBA is 4 channels and alpha is a channel, not applied");

            ImageDoc* p = open1("pal8.png");
            bool pal = p && p->ch == 3 && p->sample(0, 0, 0) == 0 &&
                       p->sample(0, 0, 1) == 1 && p->sample(0, 0, 2) == 2;
            check(pal, "M3 a palette PNG becomes colour samples");
            check(p && has(p->note, "palette"),
                  "M3 ...and says so: the numbers are palette entries now");
        }

        // ---- M4: where the decoder changes the scale, it is declared --------
        // A 4-bit PNG holds 0..15 and stb hands back 0..255 (x17). That is the
        // ONE place these formats are rescaled, and an undeclared rescale is
        // precisely the failure this project refuses to ship.
        {
            ImageDoc* d = open1("gray4.png");
            bool vals = d && d->ch == 1 && d->px()[0] == 0 && d->px()[1] == 17 &&
                        d->px()[2] == 34 && d->px()[3] == 51;
            fprintf(stderr, "mediaselftest: M4 note = \"%s\"\n", d ? d->note.c_str() : "");
            check(vals, "M4 a 4-bit PNG arrives expanded to 0..255");
            check(d && has(d->note, "4-bit") && has(d->note, "x17"),
                  "M4 ...and the note names the depth AND the factor");
        }

        // ---- M5: JPEG - lossy, and honest about the colour transform --------
        // Structural, never pixel-exact: a different decoder behind this seam
        // is allowed to differ by a few DN, and a test that forbade that would
        // be a test of stb_image rather than of the viewer.
        {
            ImageDoc* d = open1("gradient.jpg");
            bool shape = d && d->w == 32 && d->h == 24 && d->ch == 3 && d->dtype == "u8";
            // (not "near": <windows.h> defines that one away)
            auto about = [](float v, float want) { return v > want - 12 && v < want + 12; };
            bool corners = shape &&
                           about(d->sample(0, 0, 0), 0) && about(d->sample(31, 0, 0), 255) &&
                           about(d->sample(0, 23, 1), 255) && about(d->sample(0, 0, 2), 96);
            fprintf(stderr, "mediaselftest: M5 %dx%d %dch corners %g/%g/%g note=\"%s\"\n",
                    d ? d->w : 0, d ? d->h : 0, d ? d->ch : 0,
                    d ? d->sample(0, 0, 0) : -1.f, d ? d->sample(31, 0, 0) : -1.f,
                    d ? d->sample(0, 23, 1) : -1.f, d ? d->note.c_str() : "");
            check(shape, "M5 a baseline JPEG opens as one 3-channel 8-bit frame");
            check(corners, "M5 the ramp comes back where it was written");
            check(d && has(d->note, "YCbCr"),
                  "M5 the note names the codec's colour transform - it happened to the numbers");
        }

        // ---- M6: the bytes decide, and the disagreement is said out loud ----
        {
            ImageDoc* d = open1("mislabelled.png");
            fprintf(stderr, "mediaselftest: M6 note = \"%s\"\n", d ? d->note.c_str() : "");
            check(d && d->ch == 3 && d->w == 32,
                  "M6 a JPEG named .png decodes as the JPEG it is");
            check(d && has(d->note, "the name says PNG but the bytes are JPEG"),
                  "M6 ...and the note says the name and the bytes disagreed");
        }

        // ---- M7: TIFF is absent, and that is a SENTENCE ---------------------
        // §3.2's register: the file, the reason, and what to do instead. The
        // fixture is a valid baseline TIFF, so this cannot pass by accident on
        // a malformed-file path.
        {
            ImageDoc* d = open1("gray8.tif");
            const std::string msg = app.toast;
            fprintf(stderr, "mediaselftest: M7 toast = \"%s\"\n", msg.c_str());
            check(!d && app.images.empty(), "M7 a TIFF opens nothing in this build");
            check(has(msg, "gray8.tif"), "M7 the refusal names the file");
            check(has(msg, "TIFF is not read by this build") &&
                  has(msg, "no TIFF decoder is linked"),
                  "M7 ...and names what is missing, not just 'unsupported'");
            check(has(msg, "PNG and JPEG are read natively") && has(msg, "choose a reader"),
                  "M7 ...and says what does work, and where to go next");
            check(app.readerPanelOpen && app.readerPickPath.find("gray8.tif") != std::string::npos,
                  "M7 the refusal IS the affordance: the Reader panel is raised on it");
        }

        // ---- M8: the two other ways to be refused ---------------------------
        {
            ImageDoc* d = open1("broken.png");
            std::string msg = app.toast;
            fprintf(stderr, "mediaselftest: M8 broken = \"%s\"\n", msg.c_str());
            check(!d && has(msg, "broken.png") && has(msg, "PNG:"),
                  "M8 a corrupt PNG is refused, by name, as a PNG");
            check(has(msg, "stb_image"),
                  "M8 ...and names the decoder that said so");

            d = open1("notanimage.png");
            msg = app.toast;
            fprintf(stderr, "mediaselftest: M8 not-an-image = \"%s\"\n", msg.c_str());
            check(!d && has(msg, "not a PNG, JPEG or TIFF file") && has(msg, "68 65 6c 6c"),
                  "M8 a file that is none of them is refused with its first bytes");
        }

        // ---- M9: a session that saved a .png reopens a .png ------------------
        // The session file records a path and lets the loader decide what to do
        // with it, and that loader was loadNpy for everything that was not an
        // .npz. Without this branch a restored PNG comes back as "not a .npy
        // file (bad magic)" - a session that cannot reopen what it saved.
        {
            ImageDoc* d = open1("gray16.png");
            const std::string sess =
                (std::filesystem::temp_directory_path() / "viewer_mediaselftest.vsession").u8string();
            bool saved = d != nullptr;
            if (saved) saveSession(sess, true);
            closeAll();
            std::string lerr = loadSession(sess);
            ImageDoc* r = app.images.empty() ? nullptr : app.images[0].get();
            bool back = lerr.empty() && app.images.size() == 1 && r &&
                        r->w == 64 && r->h == 48 && r->ch == 1 && r->dtype == "u16" &&
                        r->px().size() == 64u * 48u && r->px()[7] == (float)(7 * 21);
            fprintf(stderr, "mediaselftest: M9 restored %zu doc(s) %s err=\"%s\"\n",
                    app.images.size(), r ? r->dtype.c_str() : "-", lerr.c_str());
            check(saved && back, "M9 a session that saved a PNG opens the PNG again");
            std::error_code sec;
            std::filesystem::remove(pathFromUtf8(sess), sec);
        }

        // ---- M10: crop, then Restore full, on a picture file -----------------
        // Restore full re-reads the FILE, and it reached for loadNpy too.
        {
            ImageDoc* d = open1("rgb8.png");
            bool cropped = false, restored = false;
            if (d) {
                cropInPlace(*d, 8, 4, 16, 12);
                cropped = d->w == 16 && d->h == 12;
                restoreFull();
                ImageDoc* r = app.images.empty() ? nullptr : app.images[0].get();
                restored = r && r->w == 64 && r->h == 48 && r->ch == 3 &&
                           r->sample(63, 0, 0) == 255;
            }
            fprintf(stderr, "mediaselftest: M10 cropped=%d restored=%d\n",
                    (int)cropped, (int)restored);
            check(cropped && restored, "M10 crop and Restore full re-read the picture file");
        }

        closeAll();
        fprintf(stderr, "mediaselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
