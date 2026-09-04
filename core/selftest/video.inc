// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// ---- --video-selftest: a stack out as a lossless video (issue #253) ---------
// Three claims, and the file is laid out as them:
//
//   the CONTAINER is real          V1  a minimal RIFF parser, written here and
//                                      not shared with the writer, reads the
//                                      file back: avih.dwTotalFrames, strh's
//                                      rate/scale, one 00db per frame of
//                                      exactly w*h*3 bytes, an idx1 whose
//                                      offsets land on those chunks, and frame
//                                      0's pixels once the DIB's bottom-up
//                                      order and BGR channels are undone.
//   the REFUSALS are the repo's    V2-V4  a preview frame, a stack over AVI
//                                      1.0's 4 GB, and a .mkv asked for where
//                                      no ffmpeg exists - each named, reasoned
//                                      and with the way out attached, and each
//                                      asserted with an anti-vacuity case
//                                      beside it so "refuse everything" cannot
//                                      pass.
//   there is ONE mapping           V6  on an "Auto per frame" stack, where the
//                                      export must re-fit each frame exactly as
//                                      selectImage does: frame n of the video,
//                                      recovered from the container, encoded as
//                                      a PNG, is byte-identical to what "select
//                                      frame n, Save image as PNG" writes.
//
// Windowless and fixture-free on the histhl/framesize pattern and for the same
// reasons: every document is analytic and built in memory, and every expected
// number is a byte, a count or a string derived by hand beside the assert.
//
// The ffmpeg half is NOT a skip when ffmpeg is absent (issue #253 says so): the
// absent case is FORCED - PATH is pointed at a directory that holds no ffmpeg -
// so the refusal is asserted on every runner in the matrix, and the present
// case additionally runs ffprobe. Both PATH and every borrowed mode are given
// back at the end: one selftest home is shared by the whole suite.
static int videoSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "videoselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto has = [](const std::string& s, const char* sub) {
        return s.find(sub) != std::string::npos;
    };
    const std::filesystem::path tmp = std::filesystem::temp_directory_path();
    auto tmpFile = [&](const char* name) { return (tmp / name).u8string(); };

    // ---- the fixture ------------------------------------------------------
    // A stack of three frames whose CONTENT ranges differ, which is what makes
    // "Auto per frame" a testable claim: with a shared 0..1 range frames 2 and
    // 3 are saturated white, and only a per-frame re-fit makes them a gradient.
    const int W = 8, H = 6;                     // W*3 = 24, a multiple of 4, so
                                                // the DIB stride IS W*3 and the
                                                // issue's "w*h*3 per chunk"
                                                // holds literally
    auto mkSeq = [](const char* nm) {
        App::SeqInfo si;
        si.id = app.nextSeqId++;
        si.name = nm;
        app.seqs.push_back(si);
        return si.id;
    };
    auto mk = [&](const char* nm, int seq, int idx, float gain, float bias) {
        auto d = std::make_unique<ImageDoc>();
        d->name = nm;
        d->src->w = W; d->src->h = H; d->src->ch = 1;
        d->src->dtype = "f32";
        d->src->data.assign((size_t)W * H, 0.0f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                d->src->data[(size_t)y * W + x] = (float)(y * W + x) * gain + bias;
        d->syncMirrors();
        computeMinMax(*d);
        d->black = 0.0f; d->white = 1.0f;       // deliberately wrong for gain>1
        d->uid = app.nextUid++;
        d->seqId = seq;
        d->seqIndex = idx;
        app.images.push_back(std::move(d));
        app.imagesRev++;
        return (int)app.images.size() - 1;
    };
    // Drive the job to the end. It is sliced one frame per call on purpose, so
    // the loop IS the frame loop, minus the drawing. Bounded, so a job that
    // refuses to finish fails the test instead of hanging the suite.
    auto runToEnd = [&]() {
        int guard = 0;
        while (pumpVideoExport() && ++guard < 10000) {}
        return !app.videoExport.active;
    };

    // ---- a minimal RIFF/AVI reader, written HERE ---------------------------
    // Deliberately not sharing a line with the writer: a parser built from the
    // writer's own constants would agree with it whatever either of them did.
    struct Avi {
        bool ok = false;
        std::string why;
        uint32_t totalFrames = 0, rate = 0, scale = 0, microSec = 0;
        int32_t biWidth = 0, biHeight = 0;
        uint16_t biBitCount = 0;
        uint32_t biCompression = 0xFFFFFFFFu;
        std::string strhType;
        size_t moviFourcc = 0;                      // idx1 offsets are from here
        std::vector<std::pair<size_t, uint32_t>> frames;   // data offset, size
        std::vector<std::pair<uint32_t, uint32_t>> idx;    // dwOffset, dwSize
    };
    auto rd32 = [](const std::vector<uint8_t>& b, size_t o) -> uint32_t {
        if (o + 4 > b.size()) return 0;
        return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
               ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    };
    auto rd16 = [](const std::vector<uint8_t>& b, size_t o) -> uint16_t {
        if (o + 2 > b.size()) return 0;
        return (uint16_t)((uint32_t)b[o] | ((uint32_t)b[o + 1] << 8));
    };
    auto tag = [](const std::vector<uint8_t>& b, size_t o) {
        return o + 4 <= b.size() ? std::string((const char*)&b[o], 4) : std::string();
    };
    auto parseAvi = [&](const std::vector<uint8_t>& b) {
        Avi A;
        if (b.size() < 64 || tag(b, 0) != "RIFF" || tag(b, 8) != "AVI ") {
            A.why = "not a RIFF/AVI"; return A;
        }
        if (rd32(b, 4) != (uint32_t)(b.size() - 8)) { A.why = "RIFF size is wrong"; return A; }
        // walk(at, end) over a chunk list; LIST bodies are walked recursively
        std::function<bool(size_t, size_t)> walk = [&](size_t at, size_t end) {
            while (at + 8 <= end) {
                const std::string t = tag(b, at);
                const uint32_t sz = rd32(b, at + 4);
                const size_t body = at + 8;
                if (body + sz > end) { A.why = "chunk " + t + " runs past its parent"; return false; }
                if (t == "LIST") {
                    const std::string kind = tag(b, body);
                    if (kind == "movi") A.moviFourcc = body;
                    if (!walk(body + 4, body + sz)) return false;
                } else if (t == "avih") {
                    A.microSec    = rd32(b, body + 0);
                    A.totalFrames = rd32(b, body + 16);
                } else if (t == "strh") {
                    A.strhType = tag(b, body + 0);
                    A.scale    = rd32(b, body + 20);
                    A.rate     = rd32(b, body + 24);
                } else if (t == "strf") {
                    A.biWidth       = (int32_t)rd32(b, body + 4);
                    A.biHeight      = (int32_t)rd32(b, body + 8);
                    A.biBitCount    = rd16(b, body + 14);
                    A.biCompression = rd32(b, body + 16);
                } else if (t == "00db") {
                    A.frames.emplace_back(body, sz);
                } else if (t == "idx1") {
                    for (uint32_t i = 0; i + 16 <= sz; i += 16)
                        A.idx.emplace_back(rd32(b, body + i + 8), rd32(b, body + i + 12));
                }
                at = body + sz + (sz & 1);     // RIFF pads chunks to even
            }
            return true;
        };
        A.ok = walk(12, b.size());
        return A;
    };
    // One frame of the container, back as top-down RGB: undo the DIB's
    // bottom-up row order and its BGR channel order. This is the half of the
    // format that a writer gets wrong silently - the picture is merely upside
    // down and blue - so it is asserted rather than eyeballed.
    auto aviFrameToRGB = [&](const std::vector<uint8_t>& b, size_t off, int w, int h) {
        const size_t stride = (size_t)(((w * 3) + 3) & ~3);
        std::vector<uint8_t> rgb((size_t)w * h * 3, 0);
        for (int y = 0; y < h; y++) {
            const uint8_t* src = &b[off + (size_t)(h - 1 - y) * stride];
            for (int x = 0; x < w; x++) {
                rgb[((size_t)y * w + x) * 3 + 0] = src[x * 3 + 2];
                rgb[((size_t)y * w + x) * 3 + 1] = src[x * 3 + 1];
                rgb[((size_t)y * w + x) * 3 + 2] = src[x * 3 + 0];
            }
        }
        return rgb;
    };

    // ---- borrowed, and given back at the end -------------------------------
    const int   saveScope = app.rangeScope;
    const bool  saveLink  = app.linkRange;
    const int   saveCmpR  = app.compareRangeMode;
    const float saveFps   = app.videoFps;
    const int   saveFmt   = app.videoFormat;
    app.compareRangeMode = 0;      // no A/B union over the export's shoulder
    app.linkRange = false;

    {   // ---- V1: the container, read back --------------------------------
        // Per-stack mode with a range set by hand, so every expected byte below
        // comes from renderDocRGBA on numbers this test chose.
        app.rangeScope = 1;
        const int seq = mkSeq("v253_struct");
        const int i0 = mk("v1_0.npy", seq, 0, 1.0f, 0.0f);
        mk("v1_1.npy", seq, 1, 1.0f, 8.0f);
        mk("v1_2.npy", seq, 2, 1.0f, 16.0f);
        for (int i : framesOfSeq(seq)) { app.images[i]->black = 0.0f; app.images[i]->white = 63.0f; }
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v1.avi");
        const double FPS = 12.0;                  // integer, so rate/scale is exact
        std::string why = startStackVideoExport(seq, path, FPS);
        check(why.empty(), "V1 the export starts");
        if (!why.empty()) fprintf(stderr, "videoselftest: V1 refused: %s\n", why.c_str());
        check(runToEnd(), "V1 ...and finishes");
        std::vector<uint8_t> bytes;
        check(readFileBytes(path, bytes) && !bytes.empty(), "V1 a file was written");
        Avi A = parseAvi(bytes);
        fprintf(stderr, "videoselftest: V1 %zu bytes, parsed=%d (%s), frames=%zu, "
                        "avih.total=%u, strh %s rate=%u scale=%u, "
                        "bih %dx%d %u bpp compression=%u, idx=%zu\n",
                bytes.size(), A.ok ? 1 : 0, A.why.c_str(), A.frames.size(),
                A.totalFrames, A.strhType.c_str(), A.rate, A.scale,
                A.biWidth, A.biHeight, (unsigned)A.biBitCount, A.biCompression,
                A.idx.size());
        check(A.ok, "V1 the bytes parse as RIFF/AVI, chunk by chunk");
        check(A.totalFrames == 3, "V1 avih.dwTotalFrames == 3");
        check(A.strhType == "vids", "V1 the stream is a video stream");
        check(A.scale != 0 && (double)A.rate / (double)A.scale == FPS,
              "V1 strh rate/scale == the fps asked for");
        check(A.microSec == 1000000u / 12u, "V1 ...and dwMicroSecPerFrame agrees with it");
        check(A.biWidth == W && A.biHeight == H,
              "V1 the BITMAPINFOHEADER carries the frame size");
        check(A.biBitCount == 24 && A.biCompression == 0,
              "V1 ...24-bit, BI_RGB: uncompressed, which is what lossless means here");
        check(A.biHeight > 0, "V1 ...and a positive biHeight, i.e. bottom-up DIB rows");
        check(A.frames.size() == 3, "V1 three 00db chunks");
        bool sized = A.frames.size() == 3;
        for (auto& f : A.frames)
            if (f.second != (uint32_t)(W * H * 3) ||
                f.second != videowrite::aviFrameBytes(W, H)) sized = false;
        check(sized, "V1 ...each of exactly w*h*3 bytes");
        // idx1 is what a player seeks with: its offsets are measured from the
        // 'movi' fourcc, so an off-by-four here plays as "frame 1 is garbage".
        bool idxOk = A.idx.size() == A.frames.size() && A.moviFourcc != 0;
        for (size_t i = 0; i < A.idx.size() && idxOk; i++) {
            const size_t chunkAt = A.moviFourcc + A.idx[i].first;
            if (tag(bytes, chunkAt) != "00db") idxOk = false;
            else if (A.moviFourcc + A.idx[i].first + 8 != A.frames[i].first) idxOk = false;
            else if (A.idx[i].second != A.frames[i].second) idxOk = false;
        }
        check(idxOk, "V1 every idx1 entry points at its own 00db chunk");
        // ...and the PIXELS: the same mapping the screen uses, once the DIB's
        // two conventions are undone.
        if (A.frames.size() == 3) {
            std::vector<int> fr = framesOfSeq(seq);
            bool same = true;
            for (int k = 0; k < 3 && same; k++) {
                std::vector<uint8_t> rgba;
                renderDocRGBA(*app.images[fr[k]], rgba);
                std::vector<uint8_t> got = aviFrameToRGB(bytes, A.frames[k].first, W, H);
                for (size_t p = 0; p < (size_t)W * H && same; p++)
                    for (int c = 0; c < 3; c++)
                        if (got[p * 3 + c] != rgba[p * 4 + c]) { same = false; break; }
            }
            check(same, "V1 every frame is renderDocRGBA's own bytes (bottom-up, BGR undone)");
            // ANTI-VACUITY: the three frames must actually differ, or "the
            // pixels match" is a statement about one picture written thrice.
            std::vector<uint8_t> f0 = aviFrameToRGB(bytes, A.frames[0].first, W, H);
            std::vector<uint8_t> f2 = aviFrameToRGB(bytes, A.frames[2].first, W, H);
            check(f0 != f2, "V1 ...and frame 1 and frame 3 are different pictures");
        }
        std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
        closeAll();
    }

    {   // ---- V2: Stop leaves NOTHING behind -------------------------------
        // A half-written video that plays for one of its three frames is worse
        // than no file: only the file survives to be looked at later, and it
        // says nothing about having been interrupted.
        app.rangeScope = 1;
        const int seq = mkSeq("v253_stop");
        const int i0 = mk("v2_0.npy", seq, 0, 1.0f, 0.0f);
        mk("v2_1.npy", seq, 1, 1.0f, 8.0f);
        mk("v2_2.npy", seq, 2, 1.0f, 16.0f);
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v2.avi");
        check(startStackVideoExport(seq, path, 30.0).empty(), "V2 the export starts");
        check(pumpVideoExport(), "V2 one frame is written per pump");
        check(app.videoExport.at == 1 && app.videoExport.active,
              "V2 ...exactly one, and the job is still running");
        check(videoExportProgress() == "1 / 3 frames",
              "V2 ...and it reports \"1 / 3 frames\"");
        check(std::filesystem::exists(pathFromUtf8(path)), "V2 the partial file exists");
        app.videoExport.stop = true;
        check(!pumpVideoExport(), "V2 Stop ends the job on the next pump");
        check(!app.videoExport.active, "V2 ...and nothing is left running");
        check(!std::filesystem::exists(pathFromUtf8(path)),
              "V2 ...and the partial file is DELETED");
        closeAll();
    }

    {   // ---- V2b: ...and so does simply GOING AWAY ------------------------
        // Stop is one way out of a running export; the process ending is the
        // other, and no pump gets to run on that path. The invariant is put on
        // the writer itself - a sink destroyed without a finish() deletes what
        // it wrote - so quitting mid-export cannot leave a file with no index
        // whose header still says 0 frames.
        const std::string path = tmpFile("viewer_video_v2b.avi");
        std::string why;
        std::vector<uint8_t> frame((size_t)W * H * 3, 0x40);
        {
            std::unique_ptr<videowrite::VideoSink> s =
                videowrite::openAvi(path, W, H, 30.0, why);
            check(s != nullptr, "V2b a writer opens");
            if (s) {
                check(s->writeFrame(frame.data(), why), "V2b ...and takes a frame");
                check(std::filesystem::exists(pathFromUtf8(path)), "V2b ...leaving a file on disk");
            }
        }                                  // destroyed WITHOUT finish()
        check(!std::filesystem::exists(pathFromUtf8(path)),
              "V2b a writer that never finished leaves nothing behind");
        {   // anti-vacuity: a writer that DID finish keeps its file
            std::unique_ptr<videowrite::VideoSink> s =
                videowrite::openAvi(path, W, H, 30.0, why);
            if (s) { s->writeFrame(frame.data(), why); s->finish(why); }
        }
        check(std::filesystem::exists(pathFromUtf8(path)),
              "V2b ...while a finished one keeps its file (anti-vacuity)");
        std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
    }

    {   // ---- V3: a decimated preview is not an export --------------------
        // The stack-level statement of export.inc's E5: `remoteStep > 1` means
        // "still the preview", and a video whose frame 2 is a sixteenth of the
        // picture is a file that can never afterwards explain itself. The
        // refusal is exportRefusalFor's own words, so the two surfaces cannot
        // drift apart.
        app.rangeScope = 1;
        const int seq = mkSeq("v253_preview");
        const int i0 = mk("v3_0.npy", seq, 0, 1.0f, 0.0f);
        const int i1 = mk("v3_1.npy", seq, 1, 1.0f, 8.0f);
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v3.avi");
        const int wasStep = app.images[i1]->src->remoteStep;
        app.images[i1]->src->remoteStep = 4;
        const std::string prev = stackVideoRefusal(seq, path, 30.0);
        app.images[i1]->src->remoteStep = 1;
        const std::string full = stackVideoRefusal(seq, path, 30.0);
        app.images[i1]->src->remoteStep = wasStep;
        fprintf(stderr, "videoselftest: V3 preview -> \"%s\"\n", prev.c_str());
        check(has(prev, "preview") && has(prev, "4"),
              "V3 a preview frame refuses, names it a preview, and says the step");
        check(has(prev, "frame 2"), "V3 ...and says WHICH frame");
        check(full.empty(), "V3 the same stack exports at step 1 (anti-vacuity)");
        // ...and a file dialog answered while the fps is nonsense refuses too,
        // rather than handing NaN to a writer that can only fail on it.
        check(!stackVideoRefusal(seq, path, 0.0).empty(), "V3 fps 0 refuses");
        check(!std::filesystem::exists(pathFromUtf8(path)),
              "V3 ...and a refused export creates no file at all");
        closeAll();
    }

    {   // ---- V4: AVI 1.0's 4 GB, refused BEFORE the first frame ----------
        // 32-bit chunk offsets: past 4 GB the file is unreachable to any
        // player. Discovering that at frame 900 costs the whole write, so the
        // arithmetic is done on the numbers up front. The way out is the
        // repo's usual shape - the OTHER format, named, with what to install.
        const std::string big = videowrite::aviSizeRefusal(4096, 4096, 100);
        const std::string small = videowrite::aviSizeRefusal(4096, 4096, 10);
        fprintf(stderr, "videoselftest: V4 4096x4096x100 -> \"%s\"\n", big.c_str());
        check(!big.empty(), "V4 a 5 GB stack is refused as AVI");
        check(has(big, "4 GB") && has(big, "ffmpeg"),
              "V4 ...naming the limit and the way out");
        check(small.empty(), "V4 a 0.5 GB stack is not (anti-vacuity)");
    }

    {   // ---- V5: the ffmpeg branch, on BOTH sides --------------------------
        // Not a skip when ffmpeg is absent (#253): the absent case is FORCED by
        // pointing PATH at a directory that holds no ffmpeg, so the refusal is
        // asserted on every runner. PATH is put back immediately after.
        app.rangeScope = 1;
        const int seq = mkSeq("v253_ffmpeg");
        const int i0 = mk("v5_0.npy", seq, 0, 1.0f, 0.0f);
        mk("v5_1.npy", seq, 1, 1.0f, 8.0f);
        mk("v5_2.npy", seq, 2, 1.0f, 16.0f);
        app.current = i0;
        const std::string mkvPath = tmpFile("viewer_video_v5.mkv");
        const char* pathEnv = getenv("PATH");
        const std::string savedPath = pathEnv ? pathEnv : "";
        auto setPath = [](const std::string& v) {
#if defined(_WIN32)
            _putenv_s("PATH", v.c_str());
#else
            setenv("PATH", v.c_str(), 1);
#endif
        };
        setPath(tmp.u8string());            // a directory with no ffmpeg in it
        const bool sawNone = videowrite::ffmpegPath().empty();
        const std::string noFf = stackVideoRefusal(seq, mkvPath, 30.0);
        const std::string aviStillOk = stackVideoRefusal(seq, tmpFile("viewer_video_v5.avi"), 30.0);
        setPath(savedPath);
        fprintf(stderr, "videoselftest: V5 no ffmpeg -> \"%s\"\n", noFf.c_str());
        check(sawNone, "V5 the forced-absent PATH really holds no ffmpeg");
        check(has(noFf, "ffmpeg") && has(noFf, "PATH"),
              "V5 .mkv without ffmpeg refuses, and names what is missing");
        check(has(noFf, ".avi"),
              "V5 ...and the way out is the format this build writes itself");
        check(aviStillOk.empty(),
              "V5 ...while .avi is unaffected by ffmpeg's absence (anti-vacuity)");
        check(!std::filesystem::exists(pathFromUtf8(mkvPath)),
              "V5 ...and a refused .mkv creates no file");

        const std::string ff = videowrite::ffmpegPath();
        fprintf(stderr, "videoselftest: V5 ffmpeg on PATH: %s\n",
                ff.empty() ? "(none)" : ff.c_str());
        if (ff.empty()) {
            // The machine has no ffmpeg. The refusal above IS the assertion for
            // this case; there is nothing further that can be checked honestly.
            fprintf(stderr, "videoselftest: V5 no ffmpeg here - the FFV1 write is "
                            "not exercised on this machine\n");
        } else {
            std::string why = startStackVideoExport(seq, mkvPath, 30.0);
            check(why.empty(), "V5 .mkv starts where ffmpeg exists");
            if (!why.empty()) fprintf(stderr, "videoselftest: V5 refused: %s\n", why.c_str());
            check(runToEnd(), "V5 ...and the pipe closes cleanly");
            std::vector<uint8_t> mk2;
            check(readFileBytes(mkvPath, mk2) && mk2.size() > 32, "V5 ...leaving a file");
            check(mk2.size() > 4 && mk2[0] == 0x1A && mk2[1] == 0x45 &&
                  mk2[2] == 0xDF && mk2[3] == 0xA3,
                  "V5 ...whose first bytes are Matroska's EBML magic");
            // ffprobe is the independent reader: OUR parser cannot be asked
            // what codec ffmpeg chose.
            std::string probe = ff;
            const size_t slash = probe.find_last_of("/\\");
            probe = (slash == std::string::npos ? std::string() : probe.substr(0, slash + 1)) +
                    "ffprobe" + (probe.size() > 4 && probe.substr(probe.size() - 4) == ".exe"
                                 ? ".exe" : "");
            if (!std::filesystem::exists(pathFromUtf8(probe))) {
                fprintf(stderr, "videoselftest: V5 no ffprobe beside ffmpeg - "
                                "codec_name is not asserted on this machine\n");
            } else {
                const std::string out = tmpFile("viewer_video_v5.probe.txt");
                std::string cmd = "\"" + probe + "\" -v error -select_streams v:0 "
                                  "-count_frames -show_entries "
                                  "stream=codec_name,nb_frames,nb_read_frames "
                                  "-of default=nw=1 \"" + mkvPath + "\" > \"" + out + "\" 2>&1";
#if defined(_WIN32)
                cmd = "\"" + cmd + "\"";     // cmd.exe eats the outer pair
#endif
                const int rc = system(cmd.c_str());
                std::string txt;
                readWholeFile(out, txt);
                std::error_code pec; std::filesystem::remove(pathFromUtf8(out), pec);
                fprintf(stderr, "videoselftest: V5 ffprobe rc=%d said:\n%s\n", rc, txt.c_str());
                check(has(txt, "codec_name=ffv1"), "V5 ffprobe reads the codec back as FFV1");
                check(has(txt, "nb_read_frames=3") || has(txt, "nb_frames=3"),
                      "V5 ...and counts three frames");
            }
            std::error_code ec; std::filesystem::remove(pathFromUtf8(mkvPath), ec);
        }
        closeAll();
    }

    {   // ---- V6: ONE mapping, on an "Auto per frame" stack ----------------
        // The claim the whole feature rests on: what the video shows is what
        // the screen would show, frame for frame. Asserted at the strongest
        // point available - the PNG export's OWN BYTES - so a second rendering
        // path could not slip between them unnoticed.
        //
        // "Auto per frame" is chosen because it is the mode with WORK to do:
        // the export must re-fit each frame to its own min..max, exactly as
        // selectImage does on a step. The fixture makes that visible - with the
        // frames' loaded 0..1 range, frames 2 and 3 are saturated white, so a
        // missing re-fit is a uniform picture rather than a subtle shift, and
        // the anti-vacuity check below is precisely that.
        app.rangeScope = 0;                 // Auto per frame
        app.linkRange = false;
        const int seq = mkSeq("v253_perframe");
        const int i0 = mk("v6_0.npy", seq, 0, 1.0f, 0.0f);   // 0..47
        mk("v6_1.npy", seq, 1, 2.0f, 10.0f);                 // 10..104
        mk("v6_2.npy", seq, 2, 3.0f, 40.0f);                 // 40..181
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v6.avi");
        std::string why = startStackVideoExport(seq, path, 30.0);
        check(why.empty(), "V6 the per-frame export starts");
        check(runToEnd(), "V6 ...and finishes");
        std::vector<uint8_t> bytes;
        readFileBytes(path, bytes);
        Avi A = parseAvi(bytes);
        check(A.ok && A.frames.size() == 3, "V6 three frames came back out");
        std::vector<int> fr = framesOfSeq(seq);
        bool ranges = fr.size() == 3;
        for (size_t k = 0; k < fr.size() && ranges; k++) {
            const ImageDoc& d = *app.images[fr[k]];
            if (d.black != d.vmin || d.white != d.vmax) ranges = false;
        }
        check(ranges, "V6 every frame was re-fitted to its OWN min..max, as a step would");
        if (A.ok && A.frames.size() == 3) {
            // ANTI-VACUITY: frame 2 under the loaded 0..1 range is uniformly
            // white. Its being a gradient is the proof that the re-fit reached
            // the video and not merely the documents.
            std::vector<uint8_t> f1 = aviFrameToRGB(bytes, A.frames[1].first, W, H);
            bool varied = false;
            for (size_t p = 3; p < f1.size(); p++) if (f1[p] != f1[0]) { varied = true; break; }
            check(varied, "V6 frame 2 is a gradient, not the saturated white 0..1 would give");
            bool match = true;
            for (int k = 0; k < 3 && match; k++) {
                selectImage(fr[k]);
                const std::string png = tmpFile("viewer_video_v6_frame.png");
                saveViewPng(png, app.images[fr[k]]->uid);
                std::vector<uint8_t> want;
                readFileBytes(png, want);
                std::error_code pec; std::filesystem::remove(pathFromUtf8(png), pec);
                // the video's frame, put back into RGBA and through the SAME
                // encoder the PNG export uses: what is compared is the picture,
                // not two encoders' opinions of it
                std::vector<uint8_t> got = aviFrameToRGB(bytes, A.frames[k].first, W, H);
                std::vector<uint8_t> rgba((size_t)W * H * 4, 255);
                for (size_t p = 0; p < (size_t)W * H; p++)
                    for (int c = 0; c < 3; c++) rgba[p * 4 + c] = got[p * 3 + c];
                std::vector<uint8_t> asPng = encodePng(rgba, W, H);
                fprintf(stderr, "videoselftest: V6 frame %d: PNG %zu bytes, video-as-PNG %zu, "
                                "equal=%d\n", k + 1, want.size(), asPng.size(),
                        (!want.empty() && want == asPng) ? 1 : 0);
                if (want.empty() || want != asPng) match = false;
            }
            check(match, "V6 frame n of the video IS the PNG export of frame n");
        }
        std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
        closeAll();
    }

    // The home is shared: give back every mode this test borrowed.
    app.rangeScope       = saveScope;
    app.linkRange        = saveLink;
    app.compareRangeMode = saveCmpR;
    app.videoFps         = saveFps;
    app.videoFormat      = saveFmt;
    app.current          = -1;
    fprintf(stderr, "videoselftest: %s\n", ok ? "ok" : "FAILED");
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
