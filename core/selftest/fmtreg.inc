// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // ---- the format seam inside the source registry (§6.2 / §3.2) ------------
    // The registry answers three questions about a document, and until now it
    // answered all three differently depending on which DOOR the pixels came
    // through rather than on what they are:
    //
    //   * does this source REGISTER - i.e. does the file it came from have an
    //     identity tuple (path + member + frame + mtime + fsize) at all? A
    //     format with no tuple has §6.2's hole open: an overwrite followed by a
    //     re-open cannot be told from a re-open of the same bytes, which is the
    //     defect the local:// sources hit when they registered 0,0;
    //   * does RELOAD reach it - can "Reload from disk" give a .tif that was
    //     just re-exported its new pixels, or is re-opening the file the only
    //     way;
    //   * does a SECOND OPEN share those pixels, or does one file on disk
    //     become two copies in memory (and two stacks that a reload of one
    //     leaves disagreeing).
    //
    // .npy, .npz members and the headerless raw dialog answered yes to all
    // three. Everything behind core/imagefile.h - PNG, JPEG, vendor RAW, TIFF,
    // OpenEXR, y4m - answered no to all three, and the members of a viewer
    // container answered no as well. This test is that census, turned into
    // assertions, one row per format.
    //
    // WHY THE FIXTURES ARE WRITTEN HERE. The point of every case is "the file
    // on disk changed and the document followed", so each format needs TWO
    // versions of one file whose pixels differ and whose difference is known on
    // this side. Three of them (PNG, TIFF, y4m) are cheap enough to write from
    // their samples, which is what tools/gen_testdata.py does and for the same
    // reason. The other three are not: a JPEG needs an encoder, an .exr needs
    // the library's writer (tools/mkexr.cpp exists precisely because it cannot
    // be hand-rolled at a sane price) and a .dng needs a plausible camera IFD.
    // For those the two versions are two of the fixtures gen_testdata.py and
    // mkexr already wrote, copied into the scratch area under ONE name - which
    // is exactly what "someone re-exported the file" looks like on disk.
    if (!g_fmtRegSelftest.empty()) {
        bool ok = true;
        auto check = [&](bool cond, const std::string& what) {
            fprintf(stderr, "fmtregselftest: %-64s %s\n", what.c_str(),
                    cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        const std::string media = g_fmtRegSelftest + "/media";
        std::string root = g_fmtRegSelftest;
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/') root.pop_back();
        root += "/fmtreg-scratch";
        std::error_code ec;
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        std::filesystem::create_directories(pathFromUtf8(root), ec);

        auto putBytes = [](const std::string& path, const std::vector<uint8_t>& b) {
            std::ofstream f(pathFromUtf8(path), std::ios::binary);
            f.write((const char*)b.data(), (std::streamsize)b.size());
        };
        // The two scalar members a viewer container is made of, laid down the
        // way numpy lays them down (a str_ scalar is <U<n> in UCS-4).
        auto i32 = [](int32_t v) { return selftestNpyBytes("<i4", {}, &v, 4); };
        auto str1 = [](const std::string& s) {
            size_t item = s.empty() ? 1 : s.size();
            std::vector<uint32_t> cps(item, 0);
            for (size_t i = 0; i < s.size(); i++) cps[i] = (uint32_t)(unsigned char)s[i];
            return selftestNpyBytes(("<U" + std::to_string(item)).c_str(), {},
                                    cps.data(), cps.size() * 4);
        };
        // Copy, not rename: the destination must be OVERWRITTEN in place, which
        // is what a re-export does and what the identity tuple has to notice.
        auto copyOver = [&](const std::string& from, const std::string& to) {
            std::vector<uint8_t> b;
            if (!readFileBytes(from, b)) return false;
            putBytes(to, b);
            return true;
        };

        // ---- the three fixtures written from their samples ------------------
        // 16-bit greyscale PNG: sample (x,y) = base + y*w + x, so which version
        // of the file a document is holding is readable from any one pixel.
        auto writePng16 = [&](const std::string& path, int w, int h, int base) {
            std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
            auto chunk = [&](const char* tag, const std::vector<uint8_t>& data) {
                uint32_t n = (uint32_t)data.size();
                for (int i = 3; i >= 0; i--) out.push_back((uint8_t)(n >> (8 * i)));
                std::vector<uint8_t> body(tag, tag + 4);
                body.insert(body.end(), data.begin(), data.end());
                out.insert(out.end(), body.begin(), body.end());
                uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, body.data(), body.size());
                for (int i = 3; i >= 0; i--) out.push_back((uint8_t)(crc >> (8 * i)));
            };
            std::vector<uint8_t> ihdr;
            auto be32 = [&](std::vector<uint8_t>& v, uint32_t x) {
                for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i)));
            };
            be32(ihdr, (uint32_t)w); be32(ihdr, (uint32_t)h);
            ihdr.push_back(16); ihdr.push_back(0);      // 16-bit, greyscale
            ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
            chunk("IHDR", ihdr);
            std::vector<uint8_t> raw;
            for (int y = 0; y < h; y++) {
                raw.push_back(0);                        // filter 0: none
                for (int x = 0; x < w; x++) {
                    int v = (base + y * w + x) & 0xffff;
                    raw.push_back((uint8_t)(v >> 8));    // PNG samples are big-endian
                    raw.push_back((uint8_t)(v & 0xff));
                }
            }
            mz_ulong cn = mz_compressBound((mz_ulong)raw.size());
            std::vector<uint8_t> comp((size_t)cn);
            mz_compress(comp.data(), &cn, raw.data(), (mz_ulong)raw.size());
            comp.resize((size_t)cn);
            chunk("IDAT", comp);
            chunk("IEND", {});
            putBytes(path, out);
        };
        // Uncompressed 16-bit greyscale TIFF, one strip per page, N pages -
        // little-endian classic TIFF, the layout core/tiffread.cpp reads. Page
        // p, sample (x,y) = base + 1000*p + y*w + x: which PAGE a frame came
        // from is in the pixels, which is what fileFrame provenance needs.
        auto writeTiff16 = [&](const std::string& path, int w, int h, int base, int pages) {
            const size_t pxBytes = (size_t)w * h * 2;
            const uint32_t ifdSize = 2 + 12 * 10 + 4;    // 10 entries, all inline
            std::vector<uint8_t> out;
            auto p16 = [&](uint32_t v) {
                out.push_back((uint8_t)(v & 0xff)); out.push_back((uint8_t)((v >> 8) & 0xff));
            };
            auto p32 = [&](uint32_t v) {
                for (int i = 0; i < 4; i++) out.push_back((uint8_t)((v >> (8 * i)) & 0xff));
            };
            out.push_back('I'); out.push_back('I');
            p16(42);
            const uint32_t dataAt = 8;
            const uint32_t ifd0 = dataAt + (uint32_t)(pxBytes * (size_t)pages);
            p32(ifd0);
            for (int p = 0; p < pages; p++)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        int v = (base + 1000 * p + y * w + x) & 0xffff;
                        out.push_back((uint8_t)(v & 0xff));
                        out.push_back((uint8_t)((v >> 8) & 0xff));
                    }
            for (int p = 0; p < pages; p++) {
                // tags in ASCENDING order, as TIFF requires; SHORT values are
                // left-justified in the four-byte value field
                struct E { uint16_t tag, type; uint32_t val; };
                const E ents[10] = {
                    { 256, 4, (uint32_t)w },                       // ImageWidth
                    { 257, 4, (uint32_t)h },                       // ImageLength
                    { 258, 3, 16 },                                // BitsPerSample
                    { 259, 3, 1 },                                 // Compression: none
                    { 262, 3, 1 },                                 // Photometric: BlackIsZero
                    { 273, 4, dataAt + (uint32_t)(pxBytes * (size_t)p) },   // StripOffsets
                    { 277, 3, 1 },                                 // SamplesPerPixel
                    { 278, 4, (uint32_t)h },                       // RowsPerStrip
                    { 279, 4, (uint32_t)pxBytes },                 // StripByteCounts
                    { 339, 3, 1 },                                 // SampleFormat: uint
                };
                p16(10);
                for (const E& e : ents) {
                    p16(e.tag); p16(e.type); p32(1);
                    if (e.type == 3) { p16(e.val); p16(0); } else p32(e.val);
                }
                p32(p + 1 < pages ? ifd0 + ifdSize * (uint32_t)(p + 1) : 0);
            }
            putBytes(path, out);
        };
        // A y4m of N Cmono16 frames - a text header plus raw little-endian
        // planes, which is the whole of the format (core/y4mread.h). Frame f,
        // sample (x,y) = base + 1000*f + y*w + x, for the same reason the TIFF
        // pages carry their page number.
        auto writeY4m16 = [&](const std::string& path, int w, int h, int base, int frames) {
            char hdr[128];
            snprintf(hdr, sizeof hdr, "YUV4MPEG2 W%d H%d F30:1 Ip A1:1 Cmono16\n", w, h);
            std::vector<uint8_t> out(hdr, hdr + strlen(hdr));
            for (int f = 0; f < frames; f++) {
                const char* fm = "FRAME\n";
                out.insert(out.end(), fm, fm + 6);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        int v = (base + 1000 * f + y * w + x) & 0xffff;
                        out.push_back((uint8_t)(v & 0xff));
                        out.push_back((uint8_t)((v >> 8) & 0xff));
                    }
            }
            putBytes(path, out);
        };
        // A JPEG cannot be written from its samples here (there is no encoder in
        // this binary and embedding a second literal would only prove that two
        // literals differ), and it cannot be copied from a second fixture either
        // because gen_testdata.py has exactly one. So version B is version A
        // with its QUANTIZATION TABLES scaled: a legal baseline JPEG of the same
        // dimensions whose dequantized coefficients - and therefore whose
        // pixels - are different. Nothing here depends on WHAT they become; the
        // reference open below reads that off the file itself.
        auto writeJpegRequantized = [&](const std::string& from, const std::string& to,
                                        int mul) {
            std::vector<uint8_t> b;
            if (!readFileBytes(from, b)) return false;
            bool touched = false;
            for (size_t i = 0; i + 3 < b.size();) {
                if (b[i] != 0xff) { i++; continue; }
                const uint8_t m = b[i + 1];
                if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { i += 2; continue; }
                if (m == 0xda) break;                    // scan data: stop parsing
                const size_t len = ((size_t)b[i + 2] << 8) | b[i + 3];
                if (m == 0xdb) {                         // DQT: 8-bit tables only here
                    size_t q = i + 4, end = i + 2 + len;
                    while (q < end && q < b.size()) {
                        const int prec = b[q] >> 4;
                        q++;
                        const size_t n = prec ? 128 : 64;
                        for (size_t k = 0; k < n && q + k < b.size(); k++)
                            if (!prec) {
                                int v = b[q + k] * mul;
                                b[q + k] = (uint8_t)(v > 255 ? 255 : (v < 1 ? 1 : v));
                                touched = true;
                            }
                        q += n;
                    }
                }
                i += 2 + len;
            }
            if (!touched) return false;
            putBytes(to, b);
            return true;
        };

        // ---- the cases ------------------------------------------------------
        // `a` and `b` are the two versions of one file: `a` is written to the
        // live path, `b` to a reference path that is opened once so that what
        // the reload has to produce is read off the FILE rather than predicted.
        struct Case {
            const char* fmt;        // the row of core/imagefile.h's table
            std::string live;       // the path the document is opened from
            std::string ref;        // the same picture, version B, under its own name
            std::string bSrc;       // where version B's bytes come from (copied cases)
            int docs;               // how many documents one open of it produces
        };
        std::vector<Case> cases;
        auto add = [&](const char* fmt, const char* ext, int docs) -> Case& {
            Case c;
            c.fmt = fmt;
            c.live = root + "/live_" + fmt + ext;
            c.ref  = root + "/ref_" + fmt + ext;
            c.docs = docs;
            for (auto& s : { &c.live, &c.ref }) {
                std::string& p = *s;
                for (char& ch : p) if (ch == ' ') ch = '_';
            }
            cases.push_back(c);
            return cases.back();
        };
        // PNG / TIFF / y4m: written from samples. The TIFF and the y4m hold
        // more than one picture on purpose - unnamed pictures are FRAMES, so
        // these are the cases where fileFrame is the provenance a reload has to
        // replay (the .npy frame-axis case, in another format).
        {
            Case& c = add("PNG", ".png", 1);
            writePng16(c.live, 8, 6, 100);
            writePng16(c.ref, 8, 6, 40000);
        }
        {
            Case& c = add("TIFF", ".tif", 3);
            writeTiff16(c.live, 8, 6, 200, 3);
            writeTiff16(c.ref, 8, 6, 41000, 3);
        }
        {
            Case& c = add("y4m", ".y4m", 4);
            writeY4m16(c.live, 8, 6, 300, 4);
            writeY4m16(c.ref, 8, 6, 42000, 4);
        }
        {   // JPEG: version B is version A requantized (see above)
            Case& c = add("JPEG", ".jpg", 1);
            c.bSrc = c.ref;
            bool made = copyOver(media + "/gradient.jpg", c.live) &&
                        writeJpegRequantized(media + "/gradient.jpg", c.ref, 3);
            check(made, "F0 JPEG fixture: one picture and its requantized twin");
        }
        {   // OpenEXR: two frames of the .exr sequence mkexr wrote, 100 apart
            Case& c = add("OpenEXR", ".exr", 1);
            c.bSrc = c.ref;
            bool made = copyOver(media + "/exrseq/f_000.exr", c.live) &&
                        copyOver(media + "/exrseq/f_001.exr", c.ref);
            check(made, "F0 OpenEXR fixture: two frames of the .exr sequence");
        }
        {   // vendor RAW: two .dng of one shape with different sensor samples
            Case& c = add("vendor RAW", ".dng", 1);
            c.bSrc = c.ref;
            bool made = copyOver(media + "/sensor_rggb.dng", c.live) &&
                        copyOver(media + "/sensor_gbrg.dng", c.ref);
            check(made, "F0 vendor RAW fixture: two .dng of one shape");
        }

        // ---- helpers over the open list --------------------------------------
        auto openRange = [&](const std::string& path, size_t& first) {
            first = app.images.size();
            app.toast.clear();
            openPath(path);
            return app.images.size() - first;
        };

        closeAll();

        // ---- F0: every case's two versions really are different pictures ------
        // Read version B out of the FILE, once, and keep the pixels: everything
        // below compares against these rather than against a predicted number,
        // so a fixture that failed to differ fails HERE and is never mistaken
        // for a reload that did not happen.
        std::vector<std::vector<std::vector<float>>> want(cases.size());
        for (size_t ci = 0; ci < cases.size(); ci++) {
            size_t first = 0;
            size_t n = openRange(cases[ci].ref, first);
            for (size_t k = 0; k < n; k++) want[ci].push_back(app.images[first + k]->px());
            std::vector<int> idx;
            for (size_t k = 0; k < n; k++) idx.push_back((int)(first + k));
            closeImages(idx);
            check(n == (size_t)cases[ci].docs && !want[ci].empty() &&
                  !want[ci][0].empty(),
                  std::string("F0 ") + cases[ci].fmt + " reference version opens (" +
                      std::to_string(n) + " doc(s))");
        }

        // ---- F1: a second open SHARES the pixels ------------------------------
        // One file on disk is one set of pixels. Two opens of it that decode
        // twice are two copies that a reload of one leaves disagreeing, and the
        // memory is the smaller half of that.
        std::vector<size_t> firstOpen(cases.size(), 0);
        for (size_t ci = 0; ci < cases.size(); ci++) {
            size_t f1 = 0, f2 = 0;
            size_t n1 = openRange(cases[ci].live, f1);
            size_t n2 = openRange(cases[ci].live, f2);
            firstOpen[ci] = f1;
            bool shared = n1 == (size_t)cases[ci].docs && n1 == n2;
            for (size_t k = 0; k < n1 && shared; k++)
                if (app.images[f1 + k]->src != app.images[f2 + k]->src) shared = false;
            // ...and the fixture pair really is a pair: version A's pixels are
            // NOT version B's. Asserted here, where both are in hand, so that a
            // "reload produced version B" further down cannot be satisfied by
            // two files that were the same picture all along.
            bool differ = n1 == want[ci].size();
            for (size_t k = 0; k < n1 && differ; k++)
                if (app.images[f1 + k]->px() == want[ci][k]) differ = false;
            check(differ, std::string("F0 ") + cases[ci].fmt +
                              ": the two versions of the file are different pictures");
            fprintf(stderr, "fmtregselftest: F1 %s: %zu + %zu doc(s), sources %s\n",
                    cases[ci].fmt, n1, n2, shared ? "SHARED" : "separate");
            check(shared, std::string("F1 ") + cases[ci].fmt +
                              ": a second open shares the pixels, never copies them");
            // and the source really is IN the registry, under its own tuple
            bool keyed = n1 == (size_t)cases[ci].docs;
            for (size_t k = 0; k < n1 && keyed; k++)
                if (app.images[f1 + k]->src->regKey.empty()) keyed = false;
            check(keyed, std::string("F1 ") + cases[ci].fmt +
                             ": every document registered an identity tuple");
            // drop the second open again: the rest of the test walks the first
            std::vector<int> idx;
            for (size_t k = 0; k < n2; k++) idx.push_back((int)(f2 + k));
            closeImages(idx);
        }

        // ---- the event: every file is re-exported ------------------------------
        // One sleep for the whole set. Long enough to cross a write-time bucket
        // on ANY filesystem this can run on (reload.inc's 2-second lesson): the
        // TIFF, the y4m, the .exr and the JPEG all keep their SIZE across the
        // rewrite, so mtime is the only half of the tuple that can move.
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        for (size_t ci = 0; ci < cases.size(); ci++) {
            const Case& c = cases[ci];
            if (!c.bSrc.empty()) { copyOver(c.bSrc, c.live); continue; }
            if (strcmp(c.fmt, "PNG") == 0) writePng16(c.live, 8, 6, 40000);
            else if (strcmp(c.fmt, "TIFF") == 0) writeTiff16(c.live, 8, 6, 41000, 3);
            else writeY4m16(c.live, 8, 6, 42000, 4);
        }

        // ---- F2: Reload from disk shows the new pixels -------------------------
        for (size_t ci = 0; ci < cases.size(); ci++) {
            const size_t f = firstOpen[ci];
            bool allOk = true, allNew = true, revd = true;
            std::string firstErr;
            for (size_t k = 0; k < (size_t)cases[ci].docs; k++) {
                ImageDoc* d = app.images[f + k].get();
                const int dr = d->dataRev, rv = d->src->rev;
                std::string err, note;
                if (!reloadSource(d->src, err, &note)) {
                    // ...and the two claims below are then FALSE, not skipped:
                    // "every document holds the new pixels" must never pass
                    // because no document was reloaded.
                    allOk = allNew = revd = false;
                    if (firstErr.empty()) firstErr = err;
                    continue;
                }
                if (d->px() != want[ci][k]) allNew = false;
                if (d->dataRev != dr + 1 || d->src->rev != rv + 1) revd = false;
            }
            fprintf(stderr, "fmtregselftest: F2 %s: reload %s%s\n", cases[ci].fmt,
                    allOk ? "succeeded" : "REFUSED: ", firstErr.c_str());
            check(allOk, std::string("F2 ") + cases[ci].fmt +
                             ": Reload from disk reaches this format at all");
            check(allNew, std::string("F2 ") + cases[ci].fmt +
                              ": ...and every document now holds the FILE's new pixels");
            check(revd, std::string("F2 ") + cases[ci].fmt +
                            ": ...with rev and dataRev moved, so keyed caches unlatch");
        }

        // ---- F3: the reloaded source RE-KEYS, and a stale tuple is never adopted
        // Two halves of one rule. After a reload the source belongs to the NEW
        // tuple, so re-opening the changed file finds it (never a second decode
        // of identical bytes); and a file overwritten WITHOUT a reload is a
        // different tuple, so the next open reads the disk instead of adopting
        // pixels that no longer describe it - the hole the local:// sources had
        // open while they registered mtime/fsize 0,0.
        for (size_t ci = 0; ci < cases.size(); ci++) {
            const size_t f = firstOpen[ci];
            size_t f3 = 0;
            size_t n3 = openRange(cases[ci].live, f3);
            bool adopted = n3 == (size_t)cases[ci].docs;
            for (size_t k = 0; k < n3 && adopted; k++)
                if (app.images[f3 + k]->src != app.images[f + k]->src) adopted = false;
            check(adopted, std::string("F3 ") + cases[ci].fmt +
                               ": a re-open after the reload adopts the re-keyed source");
            std::vector<int> idx;
            for (size_t k = 0; k < n3; k++) idx.push_back((int)(f3 + k));
            closeImages(idx);
        }
        {   // ...and the other half, on the PNG (the case whose bytes this test
            // writes, so a third version is free)
            const size_t ci = 0;
            const size_t f = firstOpen[ci];
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            writePng16(cases[ci].live, 8, 6, 7000);
            size_t f4 = 0;
            size_t n4 = openRange(cases[ci].live, f4);
            ImageDoc* fresh = n4 ? app.images[f4].get() : nullptr;
            fprintf(stderr, "fmtregselftest: F3 overwrite without reload: %s\n",
                    (fresh && fresh->src == app.images[f]->src) ? "ADOPTED the stale resident"
                                                                : "own tuple, fresh decode");
            check(fresh && fresh->src != app.images[f]->src && fresh->px()[0] == 7000.0f,
                  "F3 PNG: an overwritten file is a NEW tuple, never adopted stale");
            std::vector<int> idx;
            for (size_t k = 0; k < n4; k++) idx.push_back((int)(f4 + k));
            closeImages(idx);
        }

        // ---- F4: unnamed pictures are FRAMES, and reload replays which one -----
        // A multi-page TIFF and a y4m are the .npy frame axis in another
        // format: fileFrame says which picture of the file these pixels are,
        // and a file that no longer has that picture is refused rather than
        // silently given picture 0's.
        for (const char* fmt : { "TIFF", "y4m" }) {
            size_t ci = 0;
            while (ci < cases.size() && strcmp(cases[ci].fmt, fmt) != 0) ci++;
            const size_t f = firstOpen[ci];
            const int docs = cases[ci].docs;
            bool provenance = true;
            for (int k = 0; k < docs; k++)
                if (app.images[f + (size_t)k]->src->fileFrame != k) provenance = false;
            check(provenance, std::string("F4 ") + fmt +
                                  ": every frame recorded WHICH picture of the file it is");
            // the file loses all but its first picture: the frames past it have
            // nothing to be reloaded to
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            if (strcmp(fmt, "TIFF") == 0) writeTiff16(cases[ci].live, 8, 6, 60000, 1);
            else writeY4m16(cases[ci].live, 8, 6, 60000, 1);
            ImageDoc* last = app.images[f + (size_t)docs - 1].get();
            const std::vector<float> kept = last->px();
            std::string e1;
            bool r1 = reloadSource(last->src, e1);
            fprintf(stderr, "fmtregselftest: F4 %s vanished picture: %s\n", fmt, e1.c_str());
            check(!r1 && e1.find("no longer exists") != std::string::npos &&
                      last->px() == kept,
                  std::string("F4 ") + fmt +
                      ": a picture the file no longer holds is refused, pixels stay");
            std::string e2;
            bool r2 = reloadSource(app.images[f]->src, e2);
            check(r2 && app.images[f]->px()[0] == 60000.0f,
                  std::string("F4 ") + fmt + ": the picture it still holds keeps reloading");
        }

        // ---- F5: NAMED parts are documents, and reload replays which one -------
        // The .exr layer is what an .npz member is: the name is the provenance,
        // two layers of one file are two tuples, and a layer the file no longer
        // names is refused BY NAME rather than silently replaced by another.
        {
            const std::string lay = root + "/live_layers.exr";
            copyOver(media + "/layers.exr", lay);
            size_t f = 0;
            size_t n = openRange(lay, f);
            bool two = n == 2 && app.images[f]->src->member == "diffuse" &&
                       app.images[f + 1]->src->member == "specular";
            size_t f2 = 0;
            size_t n2 = openRange(lay, f2);
            bool shared = n2 == n;
            for (size_t k = 0; k < n2 && shared; k++)
                if (app.images[f2 + k]->src != app.images[f + k]->src) shared = false;
            bool distinct = two && app.images[f]->src != app.images[f + 1]->src &&
                            !app.images[f]->src->regKey.empty() &&
                            app.images[f]->src->regKey != app.images[f + 1]->src->regKey;
            check(two && distinct,
                  "F5 OpenEXR: two layers are two documents under two tuples");
            check(shared, "F5 OpenEXR: a second open shares both layers' pixels");
            std::vector<int> idx;
            for (size_t k = 0; k < n2; k++) idx.push_back((int)(f2 + k));
            closeImages(idx);
            // the file is re-exported WITHOUT the layers: a single-layer .exr
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            const std::vector<float> kept = app.images[f + 1]->px();
            copyOver(media + "/exrseq/f_002.exr", lay);
            std::string e;
            bool r = reloadSource(app.images[f + 1]->src, e);
            fprintf(stderr, "fmtregselftest: F5 vanished layer: %s\n", e.c_str());
            check(!r && e.find("specular") != std::string::npos &&
                      e.find("no longer in the file") != std::string::npos &&
                      app.images[f + 1]->px() == kept,
                  "F5 OpenEXR: a layer the file no longer names is refused by name");
        }

        // ---- F5b: what the registry was FOR - #82's staleness ------------------
        // A frame average is a computation over pixels that can move on without
        // it, and it ends up in reports as if it had been captured, so it has to
        // be able to say whether it still describes its stack. The key is
        // (avgSeqId, avgStackRev), and the only thing that moves stackRev is the
        // reload walk - so an average over PNG or TIFF frames could not go
        // (stale) at all while those frames were outside the walk. It can now,
        // and this is the one assertion that says so end to end.
        {
            const std::string sp = root + "/avg.tif";
            writeTiff16(sp, 8, 6, 100, 4);
            size_t f = 0;
            const size_t n = openRange(sp, f);
            const int seq = n ? app.images[f]->seqId : 0;
            const size_t before = app.images.size();
            if (seq) requestStackAverage(seq);
            ImageDoc* avg = app.images.size() == before + 1 ? app.images.back().get() : nullptr;
            const bool made = n == 4 && seq && avg && !avg->avgOfPath.empty() &&
                              avg->avgSeqId == seq && !avg->avgStale;
            check(made, "F5b fixture: a frame average over a four-page TIFF stack");
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            writeTiff16(sp, 8, 6, 50000, 4);
            std::string e;
            const bool r = made && reloadSource(app.images[f]->src, e);
            fprintf(stderr, "fmtregselftest: F5b after reloading page 0: avgStale=%d\n",
                    (made && avg->avgStale) ? 1 : 0);
            check(r && avg->avgStale,
                  "F5b reloading one page marks the average (stale) - the stackRev key moved");
            // ...and it is LATCHED, not recomputed behind the user: the picture
            // is still the one that was computed, which is what makes the mark
            // worth having.
            // page p, pixel i = 100 + 1000p + i, so the mean over four pages is
            // 100 + 1500 + i - the PRE-rewrite numbers, still there
            bool held = made && avg->px().size() == (size_t)8 * 6;
            for (size_t i = 0; held && i < avg->px().size(); i++)
                if (fabsf(avg->px()[i] - (1600.0f + (float)i)) > 0.01f) held = false;
            check(held, "F5b ...and the average's own pixels are untouched until Recompute");
            std::vector<int> idx;
            for (size_t k = f; k < app.images.size(); k++) idx.push_back((int)k);
            closeImages(idx);
        }

        // ---- F6: the members of a viewer container ----------------------------
        // A .vnz is a FILE on this disk holding pixels, so its members have an
        // identity tuple exactly as an .npz member does, and two opens of one
        // container must not decode it twice. Reload stays a refusal, and the
        // refusal is the honest one: a container rebuilds as a UNIT (one
        // __viewer line restores the whole tree), so there is no per-member
        // re-decode to offer.
        {
            std::vector<uint16_t> px((size_t)2 * 6 * 8);
            for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(500 + i);
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32(1) },
                { "__n",        i32(2) },
                { "__layer_0",  str1("batch") },
                { "__parent_0", i32(-1) },
                { "__name_0",   str1("fmtreg") },
                { "__layer_1",  str1("stack") },
                { "__parent_1", i32(0) },
                { "__name_1",   str1("s1") },
                { "__pixels_1", selftestNpyBytes("<u2", { 2, 6, 8 }, px.data(), px.size() * 2) },
            };
            const std::string vz = root + "/tree.npz";
            selftestWriteNpz(vz, cm);
            size_t f = 0, f2 = 0;
            size_t n = openRange(vz, f);
            size_t n2 = openRange(vz, f2);
            bool shared = n == 2 && n2 == 2;
            for (size_t k = 0; k < n2 && shared; k++)
                if (app.images[f2 + k]->src != app.images[f + k]->src) shared = false;
            fprintf(stderr, "fmtregselftest: F6 container: %zu + %zu doc(s), sources %s\n",
                    n, n2, shared ? "SHARED" : "separate");
            check(shared, "F6 container: a second open of a .vnz shares its members' pixels");
            std::string e;
            bool r = n ? reloadSource(app.images[f]->src, e) : true;
            check(!r && e.find("container") != std::string::npos &&
                      e.find("re-opening the file") != std::string::npos,
                  "F6 container: a member still reloads as a UNIT, and says so");
            std::vector<int> idx;
            for (size_t k = 0; k < n2; k++) idx.push_back((int)(f2 + k));
            for (size_t k = 0; k < n; k++) idx.push_back((int)(f + k));
            std::sort(idx.begin(), idx.end());
            closeImages(idx);
        }

        // ---- F7: what a reader produced stays OUT, and why ---------------------
        // The same tree, reached through a READER over some other file. The
        // tuple names the file (path + member + frame + mtime + fsize) and has
        // no field for WHICH reader was run, so two readers over one .dat would
        // be one tuple and the second would silently adopt the first's pixels -
        // §3.3's "two readings are two sets of pixels", one layer up. Until the
        // tuple can say which reading this is, reader output is not registered.
        {
            std::vector<uint16_t> px((size_t)6 * 8);
            for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(900 + i);
            std::vector<std::pair<std::string, std::vector<uint8_t>>> cm = {
                { "__viewer",   i32(1) },
                { "__n",        i32(2) },
                { "__layer_0",  str1("batch") },
                { "__parent_0", i32(-1) },
                { "__name_0",   str1("read") },
                { "__layer_1",  str1("frame") },
                { "__parent_1", i32(0) },
                { "__name_1",   str1("f1") },
                { "__pixels_1", selftestNpyBytes("<u2", { 6, 8 }, px.data(), px.size() * 2) },
            };
            const std::string cache = root + "/reader_out.npz";
            const std::string origin = root + "/instrument.dat";
            selftestWriteNpz(cache, cm);
            putBytes(origin, std::vector<uint8_t>(64, 0x7f));
            size_t before = app.images.size();
            std::string e1 = loadNpz(cache, "", 0, "probe.py:load", origin);
            std::string e2 = loadNpz(cache, "", 0, "probe.py:load", origin);
            const size_t got = app.images.size() - before;
            bool two = e1.empty() && e2.empty() && got == 2;
            bool apart = two && app.images[before]->src != app.images[before + 1]->src &&
                         app.images[before]->src->regKey.empty();
            fprintf(stderr, "fmtregselftest: F7 reader output: %zu doc(s), sources %s\n",
                    got, apart ? "separate (unregistered)" : "SHARED");
            check(two && apart,
                  "F7 reader output is NOT registered: the tuple cannot say which reader");
            std::vector<int> idx;
            for (size_t k = 0; k < got; k++) idx.push_back((int)(before + k));
            closeImages(idx);
        }

        // ---- F8: the menu item's availability tracks reloadSource --------------
        // The Files right-click greys "Reload from disk" out, and it used to do
        // that from its own copy of reloadSource's refusals - which is how the
        // item stayed greyed for a format the walk had learned to reload. One
        // predicate, asserted here against the function it now guards.
        for (size_t ci = 0; ci < cases.size(); ci++) {
            const FrameSource& S = *app.images[firstOpen[ci]]->src;
            const std::string why = reloadUnavailable(S);
            check(why.empty(), std::string("F8 ") + cases[ci].fmt +
                                   ": the Reload menu item is offered, not greyed");
        }
        {
            FrameSource unit;                 // a container member
            unit.path = root + "/tree.npz";
            unit.member = "__pixels_1";
            FrameSource peer;                 // a peer's pixels
            peer.remoteUrl = "ssh://host/x.npy";
            FrameSource none;                 // a montage / plugin result
            check(!reloadUnavailable(unit).empty() && !reloadUnavailable(peer).empty() &&
                      !reloadUnavailable(none).empty(),
                  "F8 the three sources reload cannot serve are still greyed, with a reason");
        }

        closeAll();
        std::filesystem::remove_all(pathFromUtf8(root), ec);
        fprintf(stderr, "fmtregselftest: %s\n", ok ? "ok" : "FAILED");
        stopRbWorker();
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        return ok ? 0 : 1;
    }
