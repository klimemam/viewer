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
            // OpenEXR is the row that can be in EITHER state in a legitimate
            // build, so which one it is here is asserted rather than assumed -
            // a suite that quietly passed with the format switched off would be
            // reporting on a viewer nobody ships.
#ifdef VIEWER_WITH_EXR
            check(imagefile::decodableFormats() == "PNG, JPEG, TIFF, OpenEXR",
                  "M1 this build decodes PNG, JPEG, TIFF and OpenEXR");
#else
            check(imagefile::decodableFormats() == "PNG, JPEG, TIFF",
                  "M1 this build decodes PNG, JPEG and TIFF");
            check(imagefile::forPath("x.exr") && !imagefile::forPath("x.exr")->decode &&
                  imagefile::forPath("x.exr")->absent,
                  "M1 ...and .exr is still LISTED, with the reason there is no decoder");
#endif
            // Every extension the table dispatches on reaches the file dialog,
            // including a format that is listed but absent: its refusal is more
            // use than the raw dialog a filtered-out file would get.
            check(has(imagefile::dialogPattern(), "*.exr"),
                  "M1 .exr is offered by the file dialog whether or not it decodes");
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

        // ---- M7: TIFF, the baseline round trip ------------------------------
        // The SAME file that this test used to assert a refusal on: gray8.tif is
        // still the baseline, uncompressed, 8-bit greyscale, one-strip TIFF it
        // was written as. Nothing about it was chosen to suit the reader.
        {
            ImageDoc* d = open1("gray8.tif");
            bool shape = d && d->w == 64 && d->h == 48 && d->ch == 1 && d->dtype == "u8";
            bool vals = shape;
            if (shape)
                for (int y = 0; y < 48 && vals; y++)
                    for (int x = 0; x < 64 && vals; x++)
                        if (d->px()[(size_t)y * 64 + x] != (float)((x * 4 + y) % 256))
                            vals = false;
            fprintf(stderr, "mediaselftest: M7 note = \"%s\"\n", d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M7 a baseline 8-bit greyscale TIFF is one 1-channel u8 frame");
            check(vals, "M7 every sample survives: (x*4+y)%256, unscaled");
            check(d && d->white == 255, "M7 an 8-bit file's default range is 0..255 DN");
            check(d && has(d->note, "TIFF") && has(d->note, "8-bit") &&
                  has(d->note, "no scaling"),
                  "M7 the note says what was read and what was NOT done to it");
            check(d && d->src->fsize > 0, "M7 the Watch baseline is stamped (file, size)");
        }

        // ---- M11: 16-bit TIFF, sample for sample, however it is packed ------
        // THE case this format exists for here. gray16.tif is multi-strip with a
        // short last strip (48 rows in 7s), and gray16_lzw.tif holds the SAME
        // samples through LZW and horizontal differencing - the shape a 16-bit
        // TIFF off a real acquisition tool actually has. Asserted against the
        // written values AND against each other: a compression path that changed
        // one DN would pass "it opened" and fail here.
        {
            ImageDoc* d = open1("gray16.tif");
            bool shape = d && d->w == 64 && d->h == 48 && d->ch == 1 && d->dtype == "u16";
            bool exact = shape;
            if (shape)
                for (int i = 0; i < 64 * 48 && exact; i++)
                    if (d->px()[i] != (float)((i * 37) % 65536)) exact = false;
            std::vector<float> plain;
            if (d) plain = d->px();
            fprintf(stderr, "mediaselftest: M11 %s dtype=%s max=%g note=\"%s\"\n",
                    d ? "opened" : "NOT OPENED", d ? d->dtype.c_str() : "",
                    d ? d->vmax : 0.0f, d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M11 a 16-bit greyscale TIFF is one 1-channel u16 frame");
            check(exact, "M11 every sample survives across 7 strips: (y*W+x)*37, unscaled");
            check(d && d->white == 65535, "M11 the display range defaults to the u16 full scale");
            check(d && has(d->note, "7 strips"), "M11 the note says how the file was laid out");

            ImageDoc* z = open1("gray16_lzw.tif");
            bool same = z && !plain.empty() && z->px() == plain;
            fprintf(stderr, "mediaselftest: M11 lzw note = \"%s\"\n",
                    z ? z->note.c_str() : app.toast.c_str());
            check(same, "M11 LZW + Predictor 2 gives the identical samples, DN for DN");
            check(z && has(z->note, "LZW") && has(z->note, "Predictor 2"),
                  "M11 ...and the note names the compression and the predictor");
        }

        // ---- M12: a multi-page TIFF IS a stack ------------------------------
        // core/imagefile.h says so in as many words, and this is what makes it
        // true: three pages become three frames of ONE stack (App::SeqInfo), in
        // page order, each frame knowing which page of the file it came from -
        // the same identity a multi-frame .npy's frames carry (§6.2).
        {
            closeAll();
            app.toast.clear();
            openPath(root + "/stack3.tif");
            const size_t n = app.images.size();
            int sid = n ? app.images[0]->seqId : 0;
            bool oneStack = n == 3 && sid != 0;
            if (oneStack)
                for (size_t i = 0; i < n; i++)
                    if (app.images[i]->seqId != sid || app.images[i]->seqIndex != (int)i)
                        oneStack = false;
            bool levels = n == 3 && app.images[0]->px()[0] == 1000 &&
                          app.images[1]->px()[0] == 2000 && app.images[2]->px()[0] == 3000;
            bool pages = n == 3;
            if (pages)
                for (size_t i = 0; i < n; i++)
                    if (app.images[i]->src->fileFrame != (int)i) pages = false;
            std::string sname;
            for (const auto& s : app.seqs) if (s.id == sid) sname = s.name;
            fprintf(stderr, "mediaselftest: M12 %zu doc(s), seq \"%s\", note[1]=\"%s\"\n",
                    n, sname.c_str(), n > 1 ? app.images[1]->note.c_str() : app.toast.c_str());
            check(oneStack, "M12 a 3-page TIFF opens as ONE stack of 3 frames, in page order");
            check(levels, "M12 ...and page N's pixels are frame N's pixels");
            check(pages, "M12 ...and each frame records which page of the FILE it is");
            check(has(sname, "3 frames"), "M12 the stack says how many frames it has");
            check(n > 1 && has(app.images[1]->note, "page 2 of 3"),
                  "M12 ...and each frame's note names its page");
            check(n == 3 && app.images[1]->black == app.images[0]->black &&
                  app.images[1]->white == app.images[0]->white,
                  "M12 the frames share one display range, so they are comparable");

            // A page that DECLARES it is a thumbnail (NewSubfileType bit 0) is
            // not a frame of the measurement. Dropping it silently would be a
            // stack of 1 that used to be 2; saying so is the whole difference.
            ImageDoc* t = open1("thumb.tif");
            fprintf(stderr, "mediaselftest: M12 thumb %zu doc(s) note=\"%s\"\n",
                    app.images.size(), t ? t->note.c_str() : app.toast.c_str());
            check(app.images.size() == 1 && t && t->seqId == 0 && t->w == 64,
                  "M12 a reduced-resolution page is not a frame: one frame, full size");
            check(t && has(t->note, "reduced-resolution") && has(t->note, "NewSubfileType"),
                  "M12 ...and the note names the page that was left out, and why");

            // A session writes ONE line per stack and rebuilds the frames from
            // the file. That is exactly right for an in-file frame axis and
            // exactly wrong if the writer had emitted one line per FRAME:
            // three lines, three whole-file re-reads, nine frames. Asserted
            // rather than reasoned about, because the count is the only thing
            // that tells the two apart and it is silent when it is wrong.
            closeAll();
            openPath(root + "/stack3.tif");
            const std::string sess =
                (std::filesystem::temp_directory_path() / "viewer_mediatiffstack.vsession").u8string();
            bool saved = app.images.size() == 3;
            if (saved) saveSession(sess, true);
            closeAll();
            std::string lerr = loadSession(sess);
            int sid2 = app.images.empty() ? 0 : app.images[0]->seqId;
            bool back = lerr.empty() && app.images.size() == 3 && sid2 != 0;
            if (back)
                for (size_t i = 0; i < 3; i++)
                    if (app.images[i]->seqId != sid2 ||
                        app.images[i]->px()[0] != (float)(1000 * (i + 1))) back = false;
            fprintf(stderr, "mediaselftest: M12 session restored %zu doc(s) err=\"%s\"\n",
                    app.images.size(), lerr.c_str());
            check(saved && back,
                  "M12 a session that saved a 3-page TIFF reopens 3 frames of ONE stack");
            std::error_code sec;
            std::filesystem::remove(pathFromUtf8(sess), sec);
        }

        // ---- M13: byte order, and a float sample ----------------------------
        // Byte order is a per-file fact in TIFF ("II" / "MM") and a reader that
        // assumed one returns byte-swapped numbers - which still draw a picture.
        // And 32-bit float is the reason this format is here at all: it is the
        // only one of the three that carries a measurement straight.
        {
            ImageDoc* d = open1("rgb16_be.tif");
            bool shape = d && d->w == 64 && d->h == 48 && d->ch == 3 && d->dtype == "u16";
            bool corners = shape &&
                           d->sample(0, 0, 0) == 0 && d->sample(63, 0, 0) == 65535 &&
                           d->sample(0, 47, 1) == 65535 && d->sample(0, 0, 2) == 4096;
            fprintf(stderr, "mediaselftest: M13 be %dx%d %dch corners %g/%g/%g note=\"%s\"\n",
                    d ? d->w : 0, d ? d->h : 0, d ? d->ch : 0,
                    d ? d->sample(0, 0, 0) : -1.f, d ? d->sample(63, 0, 0) : -1.f,
                    d ? d->sample(0, 0, 2) : -1.f, d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M13 a big-endian 16-bit RGB TIFF is 3 channels of u16");
            check(corners, "M13 ...and the samples are the file's, not byte-swapped");
            check(d && has(d->note, "big-endian"), "M13 the note says which byte order the file used");
            check(d && has(d->note, "Deflate"), "M13 ...and that Deflate was what unpacked it");

            ImageDoc* f = open1("float32.tif");
            bool fshape = f && f->ch == 1 && f->dtype == "f32";
            // (x-32)^2+(y-24)^2 over 100, minus 3.5: negative in the middle,
            // which no integer path can represent and no rescaling would keep.
            bool fvals = fshape && f->sample(32, 24, 0) == -3.5f &&
                         f->sample(0, 0, 0) > 12.4f && f->sample(0, 0, 0) < 12.6f;
            fprintf(stderr, "mediaselftest: M13 f32 dtype=%s centre=%g corner=%g\n",
                    f ? f->dtype.c_str() : "-", f ? f->sample(32, 24, 0) : 0.f,
                    f ? f->sample(0, 0, 0) : 0.f);
            check(fshape, "M13 a 32-bit float TIFF keeps its dtype (f32), not an integer one");
            check(fvals, "M13 ...and its values, negatives included, are as stored");
        }

        // ---- M14: RGB, PackBits, and a photometric that is READ not applied --
        {
            ImageDoc* c = open1("rgb8.tif");
            bool rgb = c && c->ch == 3 && c->dtype == "u8" &&
                       c->sample(0, 0, 0) == 0 && c->sample(0, 0, 2) == 96 &&
                       c->sample(63, 0, 0) == 255 && c->sample(0, 47, 1) == 255;
            check(rgb, "M14 an 8-bit RGB TIFF is 3 channels, in file order");
            check(c && has(c->note, "PackBits"), "M14 ...and the note names PackBits");

            // Photometric 0 says 0 is WHITE. Inverting would make the picture
            // look right and every measurement wrong, so the samples are left
            // exactly as stored and the note carries the fact instead.
            ImageDoc* w = open1("whitezero.tif");
            bool asStored = w && w->ch == 1 && w->px()[0] == 0 && w->px()[1] == 4;
            fprintf(stderr, "mediaselftest: M14 whitezero note = \"%s\"\n",
                    w ? w->note.c_str() : app.toast.c_str());
            check(asStored, "M14 a WhiteIsZero TIFF is NOT inverted: values as stored");
            check(w && has(w->note, "WhiteIsZero") && has(w->note, "NOT inverted"),
                  "M14 ...and the note says that, because the picture looks wrong on purpose");
        }

        // ---- M15: what this build does NOT read, named one by one -----------
        // The point of the whole seam. Every one of these is a VALID TIFF that
        // this reader will not read, and each refusal names the exact feature -
        // never "unsupported", and never a plausible wrong picture.
        {
            struct Case { const char* file; const char* needle; const char* what; };
            const Case cases[] = {
                { "tiled.tif",      "tiled layout",
                  "M15 a tiled TIFF is refused by naming the tiling" },
                { "jpeginside.tif", "compression 7 (JPEG)",
                  "M15 JPEG-in-TIFF is refused by naming the compression scheme" },
                { "cfa.tif",        "photometric 32803",
                  "M15 a CFA TIFF is refused rather than opened on a guessed pattern" },
                { "palette.tif",    "photometric 3 (palette",
                  "M15 a palette TIFF is refused by naming the photometric" },
                { "mixedpages.tif", "a stack's frames are one shape",
                  "M15 pages that are not one shape are not a stack, and it says so" },
                { "big.tif",        "BigTIFF",
                  "M15 BigTIFF is refused as BigTIFF, not as a corrupt classic TIFF" },
            };
            for (const Case& c : cases) {
                ImageDoc* d = open1(c.file);
                const std::string msg = app.toast;
                fprintf(stderr, "mediaselftest: M15 %-16s -> \"%s\"\n", c.file, msg.c_str());
                check(!d && app.images.empty() && has(msg, c.file) && has(msg, "TIFF:") &&
                      has(msg, c.needle) && has(msg, "choose a reader"), c.what);
            }
            // ...and the refusal is still the affordance: the Reader panel is
            // raised on the file that could not be opened, exactly as it was
            // when TIFF as a whole was the thing this build could not read.
            open1("tiled.tif");
            check(app.readerPanelOpen && app.readerPickPath.find("tiled.tif") != std::string::npos,
                  "M15 a refused TIFF raises the Reader panel on itself");
            // The decoder that said so is named, the way stb_image is for a PNG.
            check(has(app.toast, "tiffread"), "M15 ...and the refusal names the reader that refused");
        }

        // ---- M16-M23: OpenEXR ------------------------------------------------
        // The format this seam exists for at the far end: .exr is the only one
        // behind it whose samples are FLOAT and SCENE LINEAR, so there is no
        // full scale, negatives are ordinary, and values above 1 are the point.
        // Every assert below is therefore about the NUMBER and not about "it
        // opened" - if anyone ever adds a tone curve, a gamma or a clamp to
        // this load path, M16 is what stops it.
        //
        // The fixtures are written by `mkexr` at build time (CMakeLists.txt),
        // not by tools/gen_testdata.py: PIZ is a wavelet and DWAB is a DCT, and
        // those two are the entire reason the official library is linked rather
        // than a hand-rolled reader. What that costs in honesty is stated:
        // these files come from the same project as the reader, so this is a
        // test of the viewer's INTERPRETATION - channels, layers, dtype,
        // ranges, refusals - and not of byte-level parsing of a stranger's file.
#ifndef VIEWER_WITH_EXR
        // A LOUD skip, never a silent one. -DVIEWER_WITH_EXR=OFF is a supported
        // configuration (no network, or a packager who does not want the
        // dependency) and failing the suite for choosing it would punish the
        // documented escape hatch - but the run must say that the coverage is
        // absent, and why, every single time. Same rule as the quarantine block
        // in CMakeLists.txt.
        fprintf(stderr, "mediaselftest: M16-M23 SKIPPED - built without OpenEXR, so the "
                        ".exr reader is NOT covered by this run "
                        "(configure with -DVIEWER_WITH_EXR=ON to cover it)\n");
#else
        // ---- M16: the values arrive untouched --------------------------------
        {
            ImageDoc* d = open1("rgb_half.exr");
            bool shape = d && d->w == 8 && d->h == 8 && d->ch == 3 && d->dtype == "f16";
            // 12.5 survives only if nothing tone maps; -3.25 only if nothing
            // clamps; 256 only if nothing normalises to 0..1.
            bool exact = shape && d->sample(0, 0, 0) == 12.5f && d->sample(0, 0, 1) == 12.5f &&
                         d->sample(3, 0, 0) == -3.25f && d->sample(2, 0, 0) == 256.0f;
            // half(0.1) is NOT 0.1f. Widening a half to a float is lossless, so
            // the assert is that we get half's number exactly - which is what
            // "no loss on widening" actually means, and is a stronger statement
            // than any tolerance would be.
            // half(0.1) written out: 0.1 x 2^14 = 1638.4, and half keeps 11
            // significant bits, so the stored number is 1638/16384 exactly.
            // Spelled as a literal because main.cpp must not include an
            // OpenEXR header - the seam's rule is that nothing outside
            // core/imagefile.cpp and its backends does.
            const float want01 = 0.0999755859375f;         // = 1638 / 16384
            fprintf(stderr, "mediaselftest: M16 %dx%d %dch dtype=%s px=%.9g/%.9g/%.9g "
                            "half(0.1)=%.9g note=\"%s\"\n",
                    d ? d->w : 0, d ? d->h : 0, d ? d->ch : 0, d ? d->dtype.c_str() : "-",
                    d ? (double)d->sample(0, 0, 0) : 0.0, d ? (double)d->sample(3, 0, 0) : 0.0,
                    d ? (double)d->sample(2, 0, 0) : 0.0, d ? (double)d->sample(4, 0, 0) : 0.0,
                    d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M16 an RGB half .exr is one 3-channel f16 frame");
            check(exact, "M16 12.5 / -3.25 / 256 arrive EXACTLY: no tone map, no clamp, no scaling");
            check(d && d->sample(4, 0, 0) == want01,
                  "M16 a half that is not exact widens to exactly that half");
            check(d && d->vmin == -3.25f && d->vmax == 256.0f,
                  "M16 vmin/vmax are the data's own - a float format has no full scale");
            check(d && d->black == d->vmin && d->white == d->vmax,
                  "M16 the display range initialises from the data, not from 0..1");
            check(d && has(d->note, "OpenEXR") && has(d->note, "half") &&
                  has(d->note, "no scaling"),
                  "M16 the note says what was read and what was NOT done to it");
            check(d && d->src->fsize > 0, "M16 the Watch baseline is stamped (file, size)");

            ImageDoc* f = open1("rgba_f32.exr");
            fprintf(stderr, "mediaselftest: M16 f32 %dch dtype=%s px(5,0)=%.9g\n",
                    f ? f->ch : 0, f ? f->dtype.c_str() : "-",
                    f ? (double)f->sample(5, 0, 0) : 0.0);
            check(f && f->ch == 4 && f->dtype == "f32",
                  "M16 R,G,B,A float is 4 channels and records dtype f32");
            check(f && f->sample(5, 0, 0) == 12345.6789f,
                  "M16 ...and a float half could not have held round-trips exactly");
        }

        // ---- M17: a layer is a DOCUMENT, a page is a FRAME --------------------
        // The distinction core/imagefile.h draws, asserted from both sides: a
        // 3-page TIFF is ONE stack (M12) and a 2-layer .exr is TWO documents,
        // because layers are named and need not share a shape while pages are
        // ordered and do.
        {
            closeAll();
            app.toast.clear();
            openPath(root + "/layers.exr");
            const size_t n = app.images.size();
            std::string names, members;
            for (const auto& d : app.images) {
                names += (names.empty() ? "" : ", ") + d->name;
                members += (members.empty() ? "" : ", ") + d->src->npzMember;
            }
            fprintf(stderr, "mediaselftest: M17 %zu doc(s): %s  members: %s  toast=\"%s\"\n",
                    n, names.c_str(), members.c_str(), app.toast.c_str());
            bool two = n == 2 && app.images[0]->src->npzMember == "diffuse" &&
                       app.images[1]->src->npzMember == "specular" &&
                       app.images[0]->name == "layers.exr:diffuse" &&
                       app.images[1]->name == "layers.exr:specular" &&
                       app.images[0]->ch == 3 && app.images[1]->ch == 3;
            check(two, "M17 each layer becomes its own 3-channel document, named for it");
            check(n == 2 && app.images[0]->seqId == 0 && app.images[1]->seqId == 0,
                  "M17 ...and they are NOT a stack: layers are not a frame axis");
            check(has(app.toast, "2 layers") && has(app.toast, "diffuse") &&
                  has(app.toast, "specular"),
                  "M17 two documents out of one file is said out loud, by name");

            // A named layer beside loose channels: "diffuse" is RGB, and Z and
            // mask are one plane each, because no order over them means
            // anything and packing them as colour would be a guess.
            closeAll();
            openPath(root + "/mixed.exr");
            std::string mm;
            for (const auto& d : app.images) mm += (mm.empty() ? "" : ",") + d->src->npzMember;
            fprintf(stderr, "mediaselftest: M17 mixed %zu doc(s) members: %s\n",
                    app.images.size(), mm.c_str());
            // The ORDER is the file's own: an EXR ChannelList is sorted, so
            // "Z" and "mask" (which sit in the base layer) come out around
            // "diffuse.*" by plain byte order, and Z leads because 'Z' < 'd'.
            // Asserted as it is rather than re-sorted into something prettier -
            // a loader that reorders parts is a loader whose output no longer
            // matches what the file says it holds.
            check(app.images.size() == 3 && mm == "Z,mask,diffuse" &&
                  app.images[0]->ch == 1 && app.images[1]->ch == 1 && app.images[2]->ch == 3,
                  "M17 an RGB layer and two loose channels are 3 documents, 1+1+3 channels");

            ImageDoc* y = open1("lum.exr");
            check(y && app.images.size() == 1 && y->ch == 1 && y->src->npzMember == "Y",
                  "M17 a lone Y is one 1-channel document naming the channel it is");
        }

        // ---- M18: a session brings back the layers it saved, and only those ---
        // The count is the whole assert. The session writes one line per
        // DOCUMENT with its member name, and each line re-reads the WHOLE file:
        // if the member filter were ignored, restoring a 2-layer file would
        // give 2 x 2 = 4 documents, and nothing on screen would say so.
        {
            closeAll();
            openPath(root + "/layers.exr");
            const std::string sess =
                (std::filesystem::temp_directory_path() / "viewer_mediaexrlayers.vsession").u8string();
            bool saved = app.images.size() == 2;
            if (saved) saveSession(sess, true);
            closeAll();
            std::string lerr = loadSession(sess);
            std::string mm;
            for (const auto& d : app.images) mm += (mm.empty() ? "" : ",") + d->src->npzMember;
            fprintf(stderr, "mediaselftest: M18 restored %zu doc(s) members: %s err=\"%s\"\n",
                    app.images.size(), mm.c_str(), lerr.c_str());
            check(saved && lerr.empty() && app.images.size() == 2 && mm == "diffuse,specular",
                  "M18 a session that saved 2 .exr layers reopens exactly those 2");
            std::error_code sec;
            std::filesystem::remove(pathFromUtf8(sess), sec);
        }

        // ---- M19: the compressed paths, which are the whole argument ---------
        // docs/media-support.md §1 chose the official library over a hand-rolled
        // reader because real .exr files are PIZ or DWA, and "NONE and ZIP only"
        // would produce a stream of files that will not open. That reasoning is
        // worth nothing unless the compressed paths are actually exercised.
        // ZIP/ZIPS/RLE/PIZ are lossless, so they are asserted DN for DN against
        // the uncompressed file; DWAB is lossy and is asserted close, never
        // equal - claiming exactness for a lossy codec would be the same kind of
        // lie this loader exists to avoid.
        {
            ImageDoc* p0 = open1("rgb_half.exr");
            std::vector<float> plain;
            if (p0) plain = p0->px();
            for (const char* nm : { "comp_zip.exr", "comp_zips.exr",
                                    "comp_rle.exr", "comp_piz.exr" }) {
                ImageDoc* d = open1(nm);
                bool same = d && !plain.empty() && d->px() == plain;
                fprintf(stderr, "mediaselftest: M19 %-14s %zu doc(s) same=%d note=\"%s\"\n",
                        nm, app.images.size(), (int)same,
                        d ? d->note.c_str() : app.toast.c_str());
                check(same, (std::string("M19 ") + nm +
                             " decompresses to the identical samples").c_str());
            }
            ImageDoc* d = open1("comp_dwab.exr");
            float got = d ? d->sample(0, 0, 0) : 0.0f;
            fprintf(stderr, "mediaselftest: M19 DWAB px(0,0)=%.9g (lossy: 12.5 +/- 0.5)\n",
                    (double)got);
            check(d && d->ch == 3 && std::fabs(got - 12.5f) < 0.5f,
                  "M19 DWAB (lossy) opens and is close - and is NOT asserted exact");
            check(d && has(d->note, "lossy"),
                  "M19 ...and the note says the compression was lossy, because the numbers moved");
        }

        // ---- M20: the data window is the picture -----------------------------
        // A renderer that crops writes a data window smaller than the display
        // window and not at the origin. OpenEXR addresses pixels in IMAGE
        // coordinates, so the slice base is where pixel (0,0) WOULD sit - an
        // address outside the buffer. Getting that arithmetic wrong shifts every
        // pixel and still draws a picture, which is why it is asserted on values
        // and not on dimensions alone.
        {
            ImageDoc* d = open1("window.exr");
            bool shape = d && d->w == 8 && d->h == 8 && d->ch == 1;
            bool vals = shape && d->sample(0, 0, 0) == 12.5f && d->sample(3, 0, 0) == -3.25f &&
                        d->sample(2, 0, 0) == 256.0f && d->sample(7, 7, 0) == 31.5f;
            fprintf(stderr, "mediaselftest: M20 %dx%d px(0,0)=%.9g px(7,7)=%.9g note=\"%s\"\n",
                    d ? d->w : 0, d ? d->h : 0, d ? (double)d->sample(0, 0, 0) : 0.0,
                    d ? (double)d->sample(7, 7, 0) : 0.0,
                    d ? d->note.c_str() : app.toast.c_str());
            check(shape, "M20 a data window at (5,3) gives an 8x8 document, not a 16x16 one");
            check(vals, "M20 ...with the samples where they were written, not offset by (5,3)");
            check(d && has(d->note, "display window"),
                  "M20 ...and the note says these pixels are the data window inside a larger frame");
        }

        // ---- M21: tiled is READ; multi-resolution is refused ------------------
        // Issue #53 left this open: the branch refused every tiled .exr although
        // the library reads one transparently. Resolved here by splitting the
        // question. Tiling is a STORAGE layout, undone losslessly by the code
        // already linked, so refusing it would cost a user a file this build can
        // open exactly right. A mip pyramid is not a layout: it is the same
        // picture at several sizes, and choosing one silently would answer a
        // question the file asked.
        {
            ImageDoc* t = open1("tiled.exr");
            bool vals = t && t->ch == 1 && t->w == 8 &&
                        t->sample(0, 0, 0) == 12.5f && t->sample(3, 0, 0) == -3.25f;
            fprintf(stderr, "mediaselftest: M21 tiled %zu doc(s) note=\"%s\"\n",
                    app.images.size(), t ? t->note.c_str() : app.toast.c_str());
            check(vals, "M21 a one-level tiled .exr READS, to the same samples as the scanline one");
            check(t && has(t->note, "tiled"),
                  "M21 ...and the note says it was tiled, because that is a fact about the file");

            ImageDoc* m = open1("mipmap.exr");
            fprintf(stderr, "mediaselftest: M21 mipmap -> \"%s\"\n", app.toast.c_str());
            check(!m && app.images.empty() && has(app.toast, "multi-resolution") &&
                  has(app.toast, "mipmap"),
                  "M21 a mipmapped .exr is refused: which level is the picture is not ours to pick");
        }

        // ---- M22: what is NOT read, named one by one -------------------------
        // Every one of these is a VALID .exr that OpenEXR itself wrote and
        // reads. A refusal that only passed because the file was malformed
        // would prove nothing at all.
        {
            struct Case { const char* file; const char* needle; const char* what; };
            const Case cases[] = {
                { "chroma.exr",   "chroma-subsampled",
                  "M22 chroma subsampling is refused, naming the channel" },
                { "deep.exr",     "deep EXR",
                  "M22 a deep .exr is refused: a deep pixel is a list, not a value" },
                { "multipart.exr", "multi-part",
                  "M22 a multi-part .exr is refused rather than one part chosen for the user" },
                { "uintchan.exr", "UINT",
                  "M22 a UINT channel is refused rather than widened into float" },
            };
            for (const Case& c : cases) {
                ImageDoc* d = open1(c.file);
                const std::string msg = app.toast;
                fprintf(stderr, "mediaselftest: M22 %-15s -> \"%s\"\n", c.file, msg.c_str());
                check(!d && app.images.empty() && has(msg, c.file) && has(msg, "OpenEXR:") &&
                      has(msg, c.needle) && has(msg, "choose a reader"), c.what);
            }
            // The refusal is still the affordance: the Reader panel is raised on
            // the file that could not be opened.
            open1("deep.exr");
            check(app.readerPanelOpen && app.readerPickPath.find("deep.exr") != std::string::npos,
                  "M22 a refused .exr raises the Reader panel on itself");
            check(has(app.toast, "OpenEXR "),
                  "M22 ...and the refusal names the library that refused, with its version");

            // Correct magic, then nothing. A truncated .exr is a different event
            // from a file that was never one, and the sniff cannot tell them
            // apart - only the decoder can, and it must say so rather than fall
            // through to the raw dialog.
            ImageDoc* b = open1("broken.exr");
            fprintf(stderr, "mediaselftest: M22 broken -> \"%s\"\n", app.toast.c_str());
            check(!b && app.images.empty() && has(app.toast, "broken.exr") &&
                  has(app.toast, "OpenEXR:"),
                  "M22 a truncated .exr is refused as a broken OpenEXR, not as an unknown file");
        }

        // ---- M23: a FOLDER of .exr is one stack -------------------------------
        // One .exr is one frame; the frame axis comes from the FILES. The
        // original branch asserted only that the folder SCANNED as one non-raw
        // group - this drives the load as well, because "it scanned" and "the
        // frames are the files' pixels, in order" are different claims and the
        // second is the one a user depends on.
        {
            closeAll();
            const std::string sdir = root + "/exrseq";
            auto grp = scanFolderGroups(sdir);
            fprintf(stderr, "mediaselftest: M23 %zu group(s) files=%zu isRaw=%d\n",
                    grp.size(), grp.empty() ? (size_t)0 : grp[0].files.size(),
                    grp.empty() ? -1 : (int)grp[0].isRaw);
            check(grp.size() == 1 && grp[0].files.size() == 3 && !grp[0].isRaw,
                  "M23 three .exr files scan as ONE group, and NOT as raw: they carry their shape");

            // openFolder always raises the picker (it is where the file filter
            // lives); a headless caller answers it with pickerAccept, which is
            // the same drain loop --derive-selftest and --close-selftest use.
            auto loadAll = [&]() {
                double t0 = nowSec();
                while (nowSec() - t0 < 120.0) {
                    if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                    pumpSequenceAndQueue();
                    if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                        !app.folderPickOpen) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            };
            const int mode = app.seqLoadMode;
            app.seqLoadMode = 1;                       // always load numbered runs
            openFolder(sdir);
            loadAll();
            app.seqLoadMode = mode;
            const size_t n = app.images.size();
            int sid = n ? app.images[0]->seqId : 0;
            bool oneStack = n == 3 && sid != 0;
            if (oneStack)
                for (size_t i = 0; i < n; i++)
                    if (app.images[i]->seqId != sid) oneStack = false;
            // f_000/f_001/f_002 were written 100 apart at pixel 0, so which
            // file became which frame is readable from the pixels alone. (100
            // and not 1000: 2012.5 is not representable in half and would come
            // back 2012, which would make this assert about rounding instead of
            // about ordering.)
            bool order = n == 3;
            if (order)
                for (size_t i = 0; i < n; i++)
                    if (app.images[i]->px()[0] != 12.5f + 100.0f * (float)i) order = false;
            fprintf(stderr, "mediaselftest: M23 %zu doc(s) seq=%d px0 = %.9g / %.9g / %.9g\n",
                    n, sid, n > 0 ? (double)app.images[0]->px()[0] : 0.0,
                    n > 1 ? (double)app.images[1]->px()[0] : 0.0,
                    n > 2 ? (double)app.images[2]->px()[0] : 0.0);
            check(oneStack, "M23 ...and opening the folder gives ONE stack of 3 frames");
            check(order, "M23 ...whose frames are the files' own pixels, in file order");
        }
#endif  // VIEWER_WITH_EXR

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
            // The list of formats in the sentence is COMPUTED from the table
            // (imagefile.cpp listFormats), so it grows when a row is added -
            // which is the point: this assert is what notices.
            check(!d && has(msg, "not a PNG, JPEG, TIFF or OpenEXR file") &&
                  has(msg, "68 65 6c 6c"),
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
