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
//   ...in the view frame n is on   V7  A / B / slot C as three stacks, every
//   screen in (#304)                   A/B range mode x fit x scope: frame k of
//                                      the video == renderDocRGBA(cur()) after
//                                      selectImage(frame k), the screen itself
//                                      == main's rule derived by hand, and the
//                                      range named in the toast. Then follow
//                                      off, a single-image B, Link range, B's
//                                      and C's stacks in their roles, a stack
//                                      nobody compares, the PNG's view and
//                                      note, a union over a preview refused,
//                                      and sides HELD (not refused) where A's
//                                      stack is the longer.
//   the COMPARE VIEW (#308)        V8  composeCompareRGBA: each tile ==
//                                      renderDocRGBA(side, &v) where the layout
//                                      puts it (split / tiled / wipe / diff x
//                                      follow x range mode), video frame k ==
//                                      compose(exportFrameViewFor(A_k)) == the
//                                      compare PNG; the panes are tileSlots'
//                                      (a slot on B's document is a pane), the
//                                      letters in the badge inks, blink refused.
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
    const int   saveScope = app.rangeRule;
    const bool  saveLink  = app.linkRange;
    const int   saveCmpR  = app.compareRangeMode;
    const float saveFps   = app.videoFps;
    const int   saveFmt   = app.videoFormat;
    app.compareRangeMode = 0;      // no A/B union over the export's shoulder
    app.linkRange = false;

    {   // ---- V1: the container, read back --------------------------------
        // Per-stack mode with a range set by hand, so every expected byte below
        // comes from renderDocRGBA on numbers this test chose.
        app.rangeRule = 1;
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
        app.rangeRule = 1;
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

    {   // ---- V2c: the stack closing under a running export ----------------
        // The frames are held by UID, not by pointer and not by index, exactly
        // so that this is NOTICEABLE rather than undefined: a uid that no
        // longer resolves is a frame that went away, and the job stops and
        // deletes instead of writing whatever moved into that slot.
        app.rangeRule = 1;
        const int seq = mkSeq("v253_closed");
        const int i0 = mk("v2c_0.npy", seq, 0, 1.0f, 0.0f);
        mk("v2c_1.npy", seq, 1, 1.0f, 8.0f);
        mk("v2c_2.npy", seq, 2, 1.0f, 16.0f);
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v2c.avi");
        check(startStackVideoExport(seq, path, 30.0).empty(), "V2c the export starts");
        check(pumpVideoExport(), "V2c one frame is written");
        closeAll();                                   // ...and the stack is gone
        check(!pumpVideoExport(), "V2c the next pump stops the job");
        check(!app.videoExport.active, "V2c ...and nothing is left running");
        fprintf(stderr, "videoselftest: V2c said: %s\n", app.toast.c_str());
        check(has(app.toast, "closed") && has(app.toast, "deleted"),
              "V2c ...and it says a frame was closed, and that the file is gone");
        check(!std::filesystem::exists(pathFromUtf8(path)),
              "V2c ...which it is");
    }

    {   // ---- V3: a decimated preview is not an export --------------------
        // The stack-level statement of export.inc's E5: `remoteStep > 1` means
        // "still the preview", and a video whose frame 2 is a sixteenth of the
        // picture is a file that can never afterwards explain itself. The
        // refusal is exportRefusalFor's own words, so the two surfaces cannot
        // drift apart.
        app.rangeRule = 1;
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
        // `fits`, not `small`: <rpcndr.h> - which arrives with windows.h -
        // carries `#define small char`, so a local of that name compiles here
        // and is a syntax error on MSVC. (It cost this branch one CI round.)
        const std::string big = videowrite::aviSizeRefusal(4096, 4096, 100);
        const std::string fits = videowrite::aviSizeRefusal(4096, 4096, 10);
        fprintf(stderr, "videoselftest: V4 4096x4096x100 -> \"%s\"\n", big.c_str());
        check(!big.empty(), "V4 a 5 GB stack is refused as AVI");
        check(has(big, "4 GB") && has(big, "ffmpeg"),
              "V4 ...naming the limit and the way out");
        check(fits.empty(), "V4 a 0.5 GB stack is not (anti-vacuity)");
    }

    {   // ---- V5: the ffmpeg branch, on BOTH sides --------------------------
        // Not a skip when ffmpeg is absent (#253): the absent case is FORCED by
        // pointing PATH at a directory that holds no ffmpeg, so the refusal is
        // asserted on every runner. PATH is put back immediately after.
        app.rangeRule = 1;
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
        // "Each frame fits itself" is chosen because it is the rule with WORK
        // to do: every frame is drawn at its own min..max, exactly as the
        // screen draws it after a step - and, since #321, without the stored
        // range being written (I3). The fixture makes that visible - with the
        // frames' loaded 0..1 range, frames 2 and 3 are saturated white, so a
        // missing fit is a uniform picture rather than a subtle shift, and the
        // anti-vacuity check below is precisely that.
        app.rangeRule = RR_EachFrame;
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
            if (effBlack(d) != d.vmin || effWhite(d) != d.vmax) ranges = false;
            if (d.black == d.vmin && d.white == d.vmax) ranges = false;   // drawn, not written
        }
        check(ranges, "V6 every frame is drawn at its OWN min..max, and no stored range was written");
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

    {   // ---- V6b: ...and the re-fit is the FLAVOUR's, not min..max ---------
        // "Auto per frame" has three answers since #252 (min..max, the
        // 0.1-99.9% cut, median->target) and selectImage applies whichever
        // app.autoFlavor names. The export must apply the SAME one, or a video
        // of a stack browsed in another flavour shows frames the user never
        // saw. Asserted on the MEDIAN flavour, black = min, target 0.25: for a
        // ramp the median sits at the middle, so white = min + (med - min)/0.25
        // lands about a full span ABOVE max - unmistakably not min..max, on a
        // fixture of any size. (The pct flavour would not do here: on 48
        // pixels the 0.1% cut is 0.048 px and falls in bin 0, so its answer IS
        // min..max and the anti-vacuity half could not be told from a no-op.)
        const int saveFlav = app.autoFlavor, saveBM = app.medBlackMode;
        const float saveTgt = app.medTarget;
        app.rangeRule = RR_EachFrame;
        app.linkRange = false;
        app.autoFlavor = AF_Median;
        app.medBlackMode = MB_Min;
        app.medTarget = 0.25f;
        const int seq = mkSeq("v253_flavour");
        const int i0 = mk("v6b_0.npy", seq, 0, 1.0f, 0.0f);
        mk("v6b_1.npy", seq, 1, 2.0f, 10.0f);
        mk("v6b_2.npy", seq, 2, 3.0f, 40.0f);
        app.current = i0;
        const std::string path = tmpFile("viewer_video_v6b.avi");
        check(startStackVideoExport(seq, path, 30.0).empty(), "V6b the median-flavour export starts");
        check(runToEnd(), "V6b ...and finishes");
        std::vector<int> fr = framesOfSeq(seq);
        bool same = fr.size() == 3, notMinMax = fr.size() == 3;
        for (size_t k = 0; k < fr.size(); k++) {
            ImageDoc& d = *app.images[fr[k]];
            float lo = 0, hi = 0;
            medianFit(d, lo, hi);
            const float eb = effBlack(d), ew = effWhite(d);
            fprintf(stderr, "videoselftest: V6b frame %zu drawn at %.6g..%.6g | median fit %.6g..%.6g | "
                            "min..max %.6g..%.6g\n", k + 1, eb, ew, lo, hi, d.vmin, d.vmax);
            if (eb != lo || ew != hi) same = false;
            if (eb == d.vmin && ew == d.vmax) notMinMax = false;
        }
        check(same, "V6b every frame is drawn at the flavour in force (median->target)");
        check(notMinMax, "V6b ...which is not its min..max (anti-vacuity)");
        std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
        closeAll();
        app.autoFlavor = saveFlav;
        app.medBlackMode = saveBM;
        app.medTarget = saveTgt;
    }

    {   // ---- V7: every exported frame IS the screen in its view (#304) -------
        // The display range is a property of the VIEW, not of the document.
        // Frame k of A's stack is drawn in viewSidesFor(frame k) - the screen
        // once A is stepped onto frame k, B and slot C followed to THEIR frame
        // k - and a stack held by B or C is drawn in that role. Asserted at the
        // strongest point: the bytes the export wrote, read back out of the
        // container, against what the screen draws for that frame.
        //
        // Three stacks of three frames as A / B / slot C, 64x40 each, whose
        // content ranges move frame by frame and differ side by side, each
        // with one low and one high OUTLIER pixel. 2560 pixels make the
        // 0.1-99.9% cut drop 2.5 of them a side, so it is a different range
        // from min-max (asserted below), which 48 pixels could not give.
        //   A_k: ramp 0..64   + 10k     outliers -50 / +200
        //   B_k: ramp 0..128  - 20+5k   outliers -100 / +400
        //   C_k: ramp 0..32   + 30k     outliers -25 / +100
        const int W7 = 64, H7 = 40;                 // W7*3 = 192: DIB stride = W7*3
        const int saveFlav = app.autoFlavor;
        const auto saveCmpMode = app.compareMode;
        const bool saveFollow = app.compareFollowFrame;
        const std::vector<uint64_t> saveExtra = app.cmpExtra;
        const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
        const std::string saveBName = app.compareB;
        const int saveBSeq = app.compareBSeq;
        const float saveLB = app.linkBlack, saveLW = app.linkWhite;
        auto mk7 = [&](const char* nm, int seq, int idx, float gain, float bias) {
            auto d = std::make_unique<ImageDoc>();
            d->name = nm;
            d->src->w = W7; d->src->h = H7; d->src->ch = 1;
            d->src->dtype = "f32";
            d->src->data.assign((size_t)W7 * H7, 0.0f);
            for (int p = 0; p < W7 * H7; p++)
                d->src->data[(size_t)p] = (float)p / 40.0f * gain + bias;
            d->src->data[0] = bias - 50.0f * gain;
            d->src->data[(size_t)W7 * H7 - 1] = bias + 200.0f * gain;
            d->syncMirrors();
            computeMinMax(*d);
            d->black = 0.0f; d->white = 1.0f;
            d->uid = app.nextUid++;
            d->seqId = seq;
            d->seqIndex = idx;
            app.images.push_back(std::move(d));
            app.imagesRev++;
            return (int)app.images.size() - 1;
        };
        app.linkRange = false;
        app.compareFollowFrame = true;
        const int sa = mkSeq("v304_A"), sb = mkSeq("v304_B"), sc = mkSeq("v304_C"),
                  sd = mkSeq("v304_D");
        for (int k = 0; k < 3; k++) {
            char n[32];
            snprintf(n, sizeof n, "v7a_%d.npy", k); mk7(n, sa, k, 1.0f, 10.0f * k);
            snprintf(n, sizeof n, "v7b_%d.npy", k); mk7(n, sb, k, 2.0f, -20.0f + 5.0f * k);
            snprintf(n, sizeof n, "v7c_%d.npy", k); mk7(n, sc, k, 0.5f, 30.0f * k);
            snprintf(n, sizeof n, "v7d_%d.npy", k); mk7(n, sd, k, 3.0f, 7.0f * k);
        }
        const int iL = mk7("v7_lone.npy", 0, 0, 4.0f, -60.0f);     // a single image
        ImageDoc* L = app.images[iL].get();
        const std::vector<int> fa = framesOfSeq(sa), fb = framesOfSeq(sb), fc = framesOfSeq(sc),
                               fd = framesOfSeq(sd);
        auto A_ = [&](int k) { return app.images[fa[k]].get(); };
        auto B_ = [&](int k) { return app.images[fb[k]].get(); };
        auto C_ = [&](int k) { return app.images[fc[k]].get(); };
        auto D_ = [&](int k) { return app.images[fd[k]].get(); };
        selectImage(fa[0]);
        app.cmpExtra.clear();
        setCompareB(B_(0));
        app.cmpExtra.push_back(C_(0)->uid);
        app.compareMode = App::CmpSplit;
        {
            ViewSides v0 = viewSidesFor(cur());
            check(v0.n == 3 && v0.side[1] == B_(0) && v0.side[2] == C_(0) &&
                  v0.role[1] == 1 && v0.role[2] == 2,
                  "V7 fixture: A, B and slot C are the screen's three sides");
            ViewSides v2 = viewSidesFor(A_(2));
            check(v2.n == 3 && v2.side[1] == B_(2) && v2.side[2] == C_(2),
                  "V7 viewSidesFor(A's frame 3) follows B and C to THEIR frame 3");
            ViewSides vs = comparedSides();
            bool alias = vs.n == v0.n;
            for (int i = 0; i < vs.n && alias; i++)
                alias = vs.side[i] == v0.side[i] && vs.role[i] == v0.role[i];
            check(alias, "V7 comparedSides() is viewSidesFor(cur())");
            float pl = 0, ph = 0, ml = 0, mh = 0;
            app.autoFlavor = AF_Pct;   fitByFlavor(*A_(0), pl, ph);
            app.autoFlavor = AF_MinMax; fitByFlavor(*A_(0), ml, mh);
            fprintf(stderr, "videoselftest: V7 A_1 0.1-99.9%% %.6g..%.6g | min-max %.6g..%.6g\n",
                    pl, ph, ml, mh);
            check(pl != ml && ph != mh, "V7 fixture: 0.1-99.9% is not min-max on these frames");
        }
        // Each case starts from the same stored ranges: none a fit can land
        // on, so a side drawn at its doc's own numbers is visible.
        auto resetRanges = [&]() {
            for (const auto* g : { &fa, &fb, &fc, &fd })
                for (int i : *g) {
                    ImageDoc* d = app.images[i].get();
                    d->black = d->vmin - 3.0f; d->white = d->vmax + 7.0f;
                    d->fitValid = false;
                }
            L->black = L->vmin - 3.0f; L->white = L->vmax + 7.0f; L->fitValid = false;
        };
        // Export one stack; its frames back as RGB, and what the export said.
        auto exportSeq = [&](int seq, std::vector<std::vector<uint8_t>>& frames,
                             std::string& said, const char* tag) {
            frames.clear();
            const std::string path = tmpFile("viewer_video_v7.avi");
            std::string why = startStackVideoExport(seq, path, 30.0);
            if (!why.empty()) fprintf(stderr, "videoselftest: V7 %s refused: %s\n", tag, why.c_str());
            const bool ran = why.empty() && runToEnd();
            said = app.toast;
            std::vector<uint8_t> bytes;
            readFileBytes(path, bytes);
            Avi Av = parseAvi(bytes);
            if (ran && Av.ok)
                for (auto& f : Av.frames) frames.push_back(aviFrameToRGB(bytes, f.first, W7, H7));
            std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
            return ran && Av.ok;
        };
        auto sameRGB = [&](const std::vector<uint8_t>& rgb, const std::vector<uint8_t>& rgba) {
            if (rgb.size() != (size_t)W7 * H7 * 3 || rgba.size() != (size_t)W7 * H7 * 4) return false;
            for (size_t p = 0; p < (size_t)W7 * H7; p++)
                for (int c = 0; c < 3; c++) if (rgb[p * 3 + c] != rgba[p * 4 + c]) return false;
            return true;
        };
        auto screenOf = [&](ImageDoc* d) { std::vector<uint8_t> px; renderDocRGBA(*d, px); return px; };
        // main's rule for one side of the screen, derived by hand from the
        // mode and drawn with compare OFF at exactly that range, so nothing
        // of the view code decides it
        // the range a doc wears alone (#321): its fit under "each frame fits
        // itself", else its stored numbers
        auto handOwn = [&](ImageDoc* d, float& lo, float& hi) {
            lo = d->black; hi = d->white;
            if (!d->rangeOwn && app.rangeRule == RR_EachFrame) fitByFlavor(*d, lo, hi);
        };
        auto handBytes = [&](ImageDoc* S, int k) {
            float lo = 0, hi = 0;
            switch (app.compareRangeMode) {
                case 1: handOwn(A_(k), lo, hi); break;
                case 2: lo = std::min({ A_(k)->vmin, B_(k)->vmin, C_(k)->vmin });
                        hi = std::max({ A_(k)->vmax, B_(k)->vmax, C_(k)->vmax }); break;
                case 3: fitByFlavor(*S, lo, hi); break;
                default: handOwn(S, lo, hi); break;
            }
            const float sb0 = S->black, sw0 = S->white;
            const auto cm = app.compareMode;
            const int rr = app.rangeRule;
            app.compareMode = App::CmpOff;
            app.rangeRule = RR_OneRange;       // S alone, drawn at exactly lo..hi
            S->black = lo; S->white = hi;
            std::vector<uint8_t> px = screenOf(S);
            S->black = sb0; S->white = sw0;
            app.compareMode = cm;
            app.rangeRule = rr;
            return px;
        };
        auto fnv = [](uint64_t h, const std::vector<uint8_t>& b) {
            for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
            return h;
        };
        static const char* MODE[4] = { "own", "A's", "union", "each-fits" };
        static const char* FLV[3] = { "min-max", "pct", "median" };
        static const char* SCOPE_FLV[3] = { "min-max", "0.1-99.9%", "median->target" };
        // the range note a video announces, in rangeWords' words (#321 I4)
        auto wantNote = [&](int scope, int mode, int fl) {
            const std::string sc = scope == 0 ? std::string("each frame fits itself (") + SCOPE_FLV[fl] + ")"
                                              : std::string("one range for the stack");
            switch (mode) {
                case 1: return std::string("range: A's range for every side") +
                               (scope == 0 ? ", per frame" : "");
                case 2: return std::string("range: union over A, B, C (min-max), per frame");
                case 3: return std::string("range: each side fits itself (") + SCOPE_FLV[fl] +
                               "), per frame";
                default: return "range: " + sc;
            }
        };

        // ---- the grid: A's stack, every mode x fit x scope ------------------
        bool allMatch = true, allScreen = true, anyDiffers = false, notes = true;
        int combos = 0;
        std::vector<uint8_t> frame1Union;          // for the anti-vacuity below
        for (int scope = 0; scope <= 1; scope++)
        for (int mode = 0; mode <= 3; mode++)
        for (int fl = AF_MinMax; fl <= AF_Median; fl++) {
            app.rangeRule = scope; app.compareRangeMode = mode; app.autoFlavor = fl;
            resetRanges();
            selectImage(fa[0]);
            std::vector<std::vector<uint8_t>> fr; std::string said;
            const bool readable = exportSeq(sa, fr, said, "grid") && fr.size() == 3;
            bool match = readable, screen = true;
            uint64_t h = 1469598103934665603ull;
            // every frame is visited even after a mismatch, so the screen hash
            // covers the same bytes whatever the export did
            for (int k = 0; k < 3 && readable; k++) {
                selectImage(fa[k]);
                const std::vector<uint8_t> scr = screenOf(cur());
                if (!sameRGB(fr[k], scr)) match = false;
                for (ImageDoc* S : { A_(k), B_(k), C_(k) }) {
                    std::vector<uint8_t> s = screenOf(S);
                    if (s != handBytes(S, k)) screen = false;
                    h = fnv(h, s);
                }
                if (k > 0) {   // the bug: frames drawn in frame 0's view
                    const ViewSides v0 = viewSidesFor(A_(0));
                    std::vector<uint8_t> wrong; renderDocRGBA(*cur(), wrong, &v0);
                    if (wrong != scr) anyDiffers = true;
                }
                if (scope == 0 && mode == 2 && fl == AF_MinMax && k == 1) frame1Union = fr[k];
            }
            const std::string want = wantNote(scope, mode, fl);
            fprintf(stderr, "videoselftest: V7 scope=%s mode=%-9s fit=%-7s video==screen %d, "
                            "screen==main's rule %d, screen hash %016llx | %s\n",
                    scope ? "stack" : "frame", MODE[mode], FLV[fl], match ? 1 : 0,
                    screen ? 1 : 0, (unsigned long long)h, said.c_str());
            if (!match) allMatch = false;
            if (!screen) allScreen = false;
            if (!has(said, want.c_str())) {
                notes = false;
                fprintf(stderr, "videoselftest: V7   wanted \"%s\"\n", want.c_str());
            }
            combos++;
        }
        check(combos == 24 && allMatch,
              "V7 video frame k == the screen after selectImage(frame k), 24 combinations");
        check(allScreen, "V7 the screen's bytes, every side, are the rule's (hand range)");
        check(anyDiffers, "V7 ...and frame 0's view would have drawn some frame differently (anti-vacuity)");
        check(notes, "V7 the video toast names the range in the controls' words, per frame");

        // A video of A's stack against the screen with A stepped onto each
        // frame, over modes x scopes, under whatever the case set up
        auto aStackMatches = [&](const char* tag, const char* noteHas, int nFrames) {
            bool all = true, said = true;
            for (int scope = 0; scope <= 1; scope++)
            for (int mode = 0; mode <= 3; mode++) {
                app.rangeRule = scope; app.compareRangeMode = mode;
                resetRanges();
                selectImage(fa[0]);
                std::vector<std::vector<uint8_t>> fr; std::string s;
                bool m = exportSeq(sa, fr, s, tag) && (int)fr.size() == nFrames;
                for (int k = 0; k < nFrames && m; k++) {
                    selectImage(fa[k]);
                    if (!sameRGB(fr[k], screenOf(cur()))) m = false;
                }
                if (!m) { all = false; fprintf(stderr, "videoselftest: V7 %s scope=%d mode=%d MISMATCH | %s\n",
                                               tag, scope, mode, s.c_str()); }
                if (noteHas && !has(s, noteHas)) said = false;
            }
            return all && said;
        };
        app.autoFlavor = AF_Pct;

        // ---- follow-frame off: B and C stay on their pinned frame ----------
        app.compareFollowFrame = false;
        check(aStackMatches("follow-off", nullptr, 3),
              "V7 follow-frame off: video frame k == the screen, every mode x scope");
        app.compareFollowFrame = true;

        // ---- a single-image B -------------------------------------------
        setCompareB(L);
        check(aStackMatches("lone-B", nullptr, 3),
              "V7 a single-image B: video frame k == the screen, every mode x scope");
        setCompareB(B_(0));

        // ---- Link range -----------------------------------------------------
        app.linkRange = true; app.linkBlack = -7.0f; app.linkWhite = 95.0f;
        check(aStackMatches("link", "range: one range over all open images", 3),
              "V7 Link range: video frame k == the screen, and it says \"one range over all open images\"");
        app.linkRange = false;

        // ---- B's stack, exported: each B_k in its on-screen role ------------
        // With follow-frame on, B_k is on screen as B when A stands on A_k;
        // that texture is what frame k has to be.
        auto roleStackMatches = [&](int seq, const std::vector<int>& f, const char* letter) {
            bool all = true, said = true;
            const std::string tag = std::string("drawn as ") + letter;
            for (int scope = 0; scope <= 1; scope++)
            for (int mode = 0; mode <= 3; mode++) {
                app.rangeRule = scope; app.compareRangeMode = mode;
                resetRanges();
                selectImage(fa[0]);
                std::vector<std::vector<uint8_t>> fr; std::string s;
                bool m = exportSeq(seq, fr, s, tag.c_str()) && fr.size() == 3;
                for (int k = 0; k < 3 && m; k++) {
                    selectImage(fa[k]);
                    if (!sameRGB(fr[k], screenOf(app.images[f[k]].get()))) m = false;
                }
                if (!m) { all = false; fprintf(stderr, "videoselftest: V7 %s stack scope=%d mode=%d "
                                                       "MISMATCH | %s\n", letter, scope, mode, s.c_str()); }
                if (mode != 0 && !has(s, tag.c_str())) said = false;
            }
            return all && said;
        };
        check(roleStackMatches(sb, fb, "B"),
              "V7 B's stack exported: frame k == B's texture with A on frame k, \"drawn as B\"");
        check(roleStackMatches(sc, fc, "C"),
              "V7 slot C's stack exported: frame k == C's texture with A on frame k, \"drawn as C\"");
        {   // follow-frame off: A stays where it is and B_k is shown as B
            app.compareFollowFrame = false;
            bool all = true;
            for (int mode = 0; mode <= 3; mode++) {
                app.rangeRule = 1; app.compareRangeMode = mode;
                resetRanges();
                selectImage(fa[1]);
                std::vector<std::vector<uint8_t>> fr; std::string s;
                bool m = exportSeq(sb, fr, s, "B follow-off") && fr.size() == 3;
                for (int k = 0; k < 3 && m; k++) {
                    setCompareB(B_(k));                  // B pinned to B_k: the screen
                    if (!sameRGB(fr[k], screenOf(B_(k)))) m = false;
                }
                setCompareB(B_(0));
                if (!m) all = false;
            }
            check(all, "V7 ...and with follow-frame off, frame k == the screen with B pinned to B_k");
            app.compareFollowFrame = true;
        }
        {   // a stack nobody compares: its own range, as before
            bool all = true;
            for (int scope = 0; scope <= 1; scope++)
            for (int mode = 0; mode <= 3; mode++) {
                app.rangeRule = scope; app.compareRangeMode = mode;
                resetRanges();
                selectImage(fa[0]);
                std::vector<std::vector<uint8_t>> fr; std::string s;
                bool m = exportSeq(sd, fr, s, "uncompared") && fr.size() == 3;
                const auto cm = app.compareMode;
                app.compareMode = App::CmpOff;
                for (int k = 0; k < 3 && m; k++)
                    if (!sameRGB(fr[k], screenOf(D_(k)))) m = false;
                app.compareMode = cm;
                if (!m) all = false;
            }
            check(all, "V7 a stack nobody compares is exported at its own range");
        }

        {   // ---- union: B's frame 2 changes -> the video's frame 2 changes ----
            app.rangeRule = 0; app.compareRangeMode = 2; app.autoFlavor = AF_MinMax;
            ImageDoc* B1 = B_(1);
            const std::vector<float> keep = B1->src->data;
            for (float& x : B1->src->data) x *= 3.0f;
            B1->syncMirrors(); computeMinMax(*B1);
            resetRanges();
            selectImage(fa[0]);
            std::vector<std::vector<uint8_t>> fr; std::string s;
            const bool okv = exportSeq(sa, fr, s, "B widened") && fr.size() == 3 && !frame1Union.empty();
            check(okv && fr[1] != frame1Union,
                  "V7 union: B's frame 2 widened -> the video's frame 2 changed");
            B1->src->data = keep; B1->syncMirrors(); computeMinMax(*B1);
        }

        {   // ---- the PNG: the view the dialog was opened under, and its note ---
            app.rangeRule = 1; app.compareRangeMode = 2; app.autoFlavor = AF_MinMax;
            resetRanges();
            selectImage(fa[0]);
            const uint64_t viewA = cur()->uid;           // what the dialog captures
            const ViewSides v0 = viewSidesFor(A_(0));
            std::vector<uint8_t> want; renderDocRGBA(*B_(0), want, &v0);
            selectImage(fa[1]);                          // a step while the dialog is up
            const std::vector<uint8_t> nowScreen = screenOf(B_(0));
            const std::string png = tmpFile("viewer_video_v7.png");
            saveViewPng(png, B_(0)->uid, viewA);
            const std::string said = app.toast;
            std::vector<uint8_t> got;
            readFileBytes(png, got);
            std::error_code ec; std::filesystem::remove(pathFromUtf8(png), ec);
            fprintf(stderr, "videoselftest: V7 PNG of B_1 said: %s\n", said.c_str());
            check(!got.empty() && got == encodePng(want, W7, H7),
                  "V7 PNG: drawn in the view the dialog was opened under, not the one at save time");
            check(want != nowScreen, "V7 ...which differs from the view at save time (anti-vacuity)");
            char num[96];
            snprintf(num, sizeof num, " = %.6g..%.6g",
                     std::min({ A_(0)->vmin, B_(0)->vmin, C_(0)->vmin }),
                     std::max({ A_(0)->vmax, B_(0)->vmax, C_(0)->vmax }));
            check(has(said, (std::string("range: union over A, B, C (min-max) - drawn as B") + num).c_str()),
                  "V7 ...and its toast names the union, the role and the numbers");
            // dot by dot announces exactly exportRangeNote(cur(), comparedSides())
            const std::string dot = exportRangeNote(*cur(), comparedSides(), false);
            snprintf(num, sizeof num, " = %.6g..%.6g",
                     std::min({ A_(1)->vmin, B_(1)->vmin, C_(1)->vmin }),
                     std::max({ A_(1)->vmax, B_(1)->vmax, C_(1)->vmax }));
            fprintf(stderr, "videoselftest: V7 dot by dot of A_2 says: %s\n", dot.c_str());
            check(dot == std::string("range: union over A, B, C (min-max)") + num,
                  "V7 dot by dot: \"range: union over A, B, C (min-max) = lo..hi\"");
            app.compareRangeMode = 0;
            check(exportRangeNote(*cur(), comparedSides(), false).rfind(
                      "range: one range for the stack · each side keeps its own", 0) == 0,
                  "V7 ...\"one range for the stack · each side keeps its own\" in mode 0");
            app.linkRange = true;
            check(exportRangeNote(*cur(), comparedSides(), false).rfind(
                      "range: one range over all open images = ", 0) == 0,
                  "V7 ...\"one range over all open images\" under Link range");
            app.linkRange = false;
        }

        {   // ---- a side the union is taken over is a preview: refused, named --
            app.rangeRule = 1;
            const std::string path = tmpFile("viewer_video_v7c.avi");
            ImageDoc* B2 = B_(2);
            ImageDoc* C1 = C_(1);
            const int wasB = B2->src->remoteStep, wasC = C1->src->remoteStep, wasL = L->src->remoteStep;
            selectImage(fa[0]);
            app.compareRangeMode = 2;
            B2->src->remoteStep = 4;
            const std::string bPrev = stackVideoRefusal(sa, path, 30.0);
            app.compareRangeMode = 1;
            const std::string bPrevAs = stackVideoRefusal(sa, path, 30.0);
            B2->src->remoteStep = wasB;
            app.compareRangeMode = 2;
            C1->src->remoteStep = 4;
            const std::string cPrev = stackVideoRefusal(sa, path, 30.0);
            C1->src->remoteStep = wasC;
            setCompareB(L);
            L->src->remoteStep = 4;
            const std::string lPrev = stackVideoRefusal(sa, path, 30.0);
            L->src->remoteStep = wasL;
            setCompareB(B_(0));
            // B's stack exported while A's paired frame is a preview: the
            // union starts from A's min/max, so A is checked too - and with
            // follow-frame off, the A on screen is
            ImageDoc* A1 = A_(1);
            const int wasA = A1->src->remoteStep;
            A1->src->remoteStep = 4;
            const std::string aPrev = stackVideoRefusal(sb, path, 30.0);
            A1->src->remoteStep = wasA;
            app.compareFollowFrame = false;
            selectImage(fa[1]);
            A1->src->remoteStep = 4;
            const std::string aPrevOff = stackVideoRefusal(sb, path, 30.0);
            A1->src->remoteStep = wasA;
            app.compareFollowFrame = true;
            selectImage(fa[0]);
            fprintf(stderr, "videoselftest: V7 B's stack, A_2 preview -> \"%s\"\n", aPrev.c_str());
            fprintf(stderr, "videoselftest: V7 ...follow off, A on screen preview -> \"%s\"\n", aPrevOff.c_str());
            check(has(aPrev, "frame 2 of 3") && has(aPrev, "A's frame 2") && has(aPrev, "preview"),
                  "V7 B's stack exported over a preview A frame refuses, naming A and the frame");
            check(has(aPrevOff, "frame 1 of 3") && has(aPrevOff, "A's frame 2"),
                  "V7 ...and with follow-frame off, the preview A on screen is named too");
            const std::string none = stackVideoRefusal(sa, path, 30.0);
            fprintf(stderr, "videoselftest: V7 B preview -> \"%s\"\n", bPrev.c_str());
            fprintf(stderr, "videoselftest: V7 C preview -> \"%s\"\n", cPrev.c_str());
            fprintf(stderr, "videoselftest: V7 single-image B preview -> \"%s\"\n", lPrev.c_str());
            check(has(bPrev, "frame 3 of 3") && has(bPrev, "B's frame 3") && has(bPrev, "preview"),
                  "V7 union over a preview B frame refuses, naming the side and the frame");
            check(has(cPrev, "frame 2 of 3") && has(cPrev, "C's frame 2"),
                  "V7 ...and a preview slot C frame, by its letter");
            check(has(lPrev, "B (a single image) is still the decimated preview") &&
                  !has(lPrev, "B's frame"),
                  "V7 ...and a single-image B by name, with no frame number");
            check(bPrevAs.empty(), "V7 ...while A's range, which never reads B, does not refuse");
            check(none.empty() && stackVideoRefusal(sb, path, 30.0).empty(),
                  "V7 ...and with every frame full the union exports (anti-vacuity)");
        }

        {   // ---- A longer than B and C: they are HELD, as on screen ----------
            // A has a frame 4, B and C do not: the screen draws B and C on
            // their PINNED frame (frame 1 here) marked [stale] and unions over
            // them; the export does the same and says so - it does not refuse,
            // which for a loaded stack would be a "wait" that never ends.
            mk7("v7a_3.npy", sa, 3, 1.0f, 30.0f);
            const std::vector<int> fa4 = framesOfSeq(sa);
            check(fa4.size() == 4 && stackVideoRefusal(sa, tmpFile("viewer_video_v7d.avi"), 30.0).empty(),
                  "V7 A one frame longer than B and C: the union export is not refused");
            bool m = true, said = true;
            for (int scope = 0; scope <= 1; scope++)
            for (int mode = 0; mode <= 3; mode++) {
                app.rangeRule = scope; app.compareRangeMode = mode;
                resetRanges();
                selectImage(fa4[0]);
                std::vector<std::vector<uint8_t>> fr; std::string s;
                bool ok1 = exportSeq(sa, fr, s, "held") && fr.size() == 4;
                for (int k = 0; k < 4 && ok1; k++) {
                    selectImage(fa4[k]);
                    if (!sameRGB(fr[k], screenOf(cur()))) ok1 = false;
                }
                if (!ok1) m = false;
                if (mode == 2) {
                    fprintf(stderr, "videoselftest: V7 held, scope=%d: %s\n", scope, s.c_str());
                    if (!has(s, "; B held at frame 1") || !has(s, "; C held at frame 1")) said = false;
                } else if (has(s, "held at")) said = false;
            }
            check(m, "V7 ...and frame 4 == the screen, B and C held, every mode x scope");
            check(said, "V7 ...and the union's toast says \"B held at frame 1\" (only the union's)");
            app.rangeRule = 1; app.compareRangeMode = 2;
            selectImage(fa4[3]);
            check(has(exportRangeNote(*cur(), comparedSides(), false), "; B held at frame 1"),
                  "V7 ...and so does a picture of frame 4");
        }
        closeAll();
        app.cmpExtra = saveExtra;
        app.compareBUid = saveBUid;
        app.lastCompareBUid = saveLastB;
        app.compareB = saveBName;
        app.compareBSeq = saveBSeq;
        app.compareMode = saveCmpMode;
        app.compareFollowFrame = saveFollow;
        app.autoFlavor = saveFlav;
        app.linkBlack = saveLB; app.linkWhite = saveLW;
    }

    {   // ---- V8: the compare view, composed at 1:1 (#308) ---------------------
        // composeCompareRGBA is the one place the compare picture is built.
        // Asserted: every tile IS renderDocRGBA(side, &v) bit for bit, at the
        // place the layout puts it, with the separators, the padding, the
        // wipe line and the difference where the spec says; frame k of the
        // compare video IS composeCompareRGBA of the view A's frame k is on
        // screen in, and IS the PNG "Save compare view as PNG..." writes with
        // A on frame k. Four modes (side by side, tiled with three sides,
        // wipe, difference) x follow-frame on/off x two range modes; blink is
        // refused. Every expected layout number is derived here by hand.
        //   pixel p of a side = (7p mod 97) * gain + bias
        //   A_k: 64x40, gain 1, bias 10k     B_k: 64x40, gain 2, bias -20 + 5k
        //   C_k: 40x24, gain 0.5, bias 30k   (smaller: padded in its cell)
        const auto saveCmpMode = app.compareMode;
        const bool saveFollow = app.compareFollowFrame;
        const std::vector<uint64_t> saveExtra = app.cmpExtra;
        const uint64_t saveBUid = app.compareBUid, saveLastB = app.lastCompareBUid;
        const std::string saveBName = app.compareB;
        const int saveBSeq = app.compareBSeq;
        const float saveWipe = app.wipeFrac, saveDiffGain = app.diffGain;
        const bool saveDiffAbs = app.diffAbs, saveLetters = app.cmpExportLetters;
        float saveRect[4]; for (int i = 0; i < 4; i++) saveRect[i] = g_canvasRect[i];
        const int saveRectFrame = g_canvasRectFrame;
        const ImVec2 saveCenter = app.view.center;
        const float saveZoom = app.view.zoom, saveUi = app.uiScale;
        // A canvas as drawCanvas records it, and a view that is NOT a fit: the
        // image sits right of the canvas centre at 3x, so the wipe divider's
        // image column is nothing like wipeFrac x width.
        g_canvasRect[0] = 100; g_canvasRect[1] = 50; g_canvasRect[2] = 400; g_canvasRect[3] = 300;
        g_canvasRectFrame = 1;
        app.view.center = ImVec2(20.0f, 20.0f);
        app.view.zoom = 3.0f;
        app.uiScale = 1.0f;
        // the column the SCREEN's divider passes through: drawCanvas's wipeX,
        // through the canvas's own inverse transform
        auto screenWipeCol = [&](int w) {
            const ImVec2 P0(g_canvasRect[0], g_canvasRect[1]), SZ(g_canvasRect[2], g_canvasRect[3]);
            const float ix = tileUnmap(P0, SZ, app.view.center, app.view.zoom,
                                       ImVec2(P0.x + SZ.x * app.wipeFrac, P0.y)).x;
            return std::clamp((int)lroundf(ix), 0, w);
        };
        auto mk8 = [&](const char* nm, int seq, int idx, int w, int h, float gain, float bias) {
            auto d = std::make_unique<ImageDoc>();
            d->name = nm;
            d->src->w = w; d->src->h = h; d->src->ch = 1;
            d->src->dtype = "f32";
            d->src->data.assign((size_t)w * h, 0.0f);
            for (int p = 0; p < w * h; p++)
                d->src->data[(size_t)p] = (float)((p * 7) % 97) * gain + bias;
            d->syncMirrors();
            computeMinMax(*d);
            d->uid = app.nextUid++;
            d->seqId = seq;
            d->seqIndex = idx;
            app.images.push_back(std::move(d));
            app.imagesRev++;
            return (int)app.images.size() - 1;
        };
        const int sa = mkSeq("v308_A"), sb = mkSeq("v308_B"), sc = mkSeq("v308_C");
        for (int k = 0; k < 3; k++) {
            char n[32];
            snprintf(n, sizeof n, "v8a_%d.npy", k); mk8(n, sa, k, 64, 40, 1.0f, 10.0f * k);
            snprintf(n, sizeof n, "v8b_%d.npy", k); mk8(n, sb, k, 64, 40, 2.0f, -20.0f + 5.0f * k);
            snprintf(n, sizeof n, "v8c_%d.npy", k); mk8(n, sc, k, 40, 24, 0.5f, 30.0f * k);
        }
        const std::vector<int> fa = framesOfSeq(sa), fb = framesOfSeq(sb), fc = framesOfSeq(sc);
        auto A_ = [&](int k) { return app.images[fa[k]].get(); };
        auto B_ = [&](int k) { return app.images[fb[k]].get(); };
        auto C_ = [&](int k) { return app.images[fc[k]].get(); };
        auto resetRanges = [&]() {
            for (const auto* g : { &fa, &fb, &fc })
                for (int i : *g) {
                    ImageDoc* d = app.images[i].get();
                    d->black = d->vmin - 3.0f; d->white = d->vmax + 7.0f;
                    d->fitValid = false;
                }
        };
        app.linkRange = false;
        app.rangeRule = 1;
        app.cmpExportLetters = false;
        app.diffGain = 0; app.diffAbs = false;
        selectImage(fa[0]);
        app.cmpExtra.clear();
        setCompareB(B_(0));

        auto px4 = [](const std::vector<uint8_t>& b, int W, int x, int y) {
            return &b[((size_t)y * W + x) * 4];
        };
        auto isRGB = [](const uint8_t* p, uint8_t r, uint8_t g, uint8_t b) {
            return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
        };
        // The cell at column x0 holds `side` (sw x sh) top-left, black beyond it
        auto cellIs = [&](const std::vector<uint8_t>& got, int W, int x0, int cw, int chh,
                          const std::vector<uint8_t>& side, int sw, int sh, int c0, int c1) {
            for (int y = 0; y < chh; y++)
                for (int x = std::max(x0, c0); x < std::min(x0 + cw, c1); x++) {
                    const uint8_t* p = px4(got, W, x, y);
                    const int sx = x - x0;
                    if (sx < sw && y < sh) {
                        const uint8_t* q = &side[((size_t)y * sw + sx) * 4];
                        if (memcmp(p, q, 4) != 0) return false;
                    } else if (!isRGB(p, 0, 0, 0)) return false;
                }
            return true;
        };
        auto colIs = [&](const std::vector<uint8_t>& got, int W, int H, int x, uint8_t v) {
            for (int y = 0; y < H; y++) if (!isRGB(px4(got, W, x, y), v, v, v)) return false;
            return true;
        };
        // The spec, derived by hand for view v in the current mode: the
        // composite's size, and every pixel of it
        auto composeIsSpec = [&](const ViewSides& v, const char* tag) {
            std::vector<uint8_t> got; int w = 0, h = 0; std::string note;
            if (!composeCompareRGBA(v, got, w, h, note)) {
                fprintf(stderr, "videoselftest: V8 %s refused: %s\n", tag, note.c_str());
                return false;
            }
            std::vector<ImageDoc*> drawn;
            // the canvas's panes: one per letter (ViewSides::pane), A and B
            // only in the two-image modes
            if (app.compareMode == App::CmpSplit) for (int i = 0; i < v.nPane; i++) drawn.push_back(v.pane[i]);
            else { drawn.push_back(v.pane[0]); drawn.push_back(v.pane[1]); }
            int cw = 0, chh = 0;
            for (ImageDoc* d : drawn) { cw = std::max(cw, d->w); chh = std::max(chh, d->h); }
            bool okc = true;
            std::vector<uint8_t> s;
            if (app.compareMode == App::CmpSplit) {
                const int n = (int)drawn.size();
                okc = w == n * cw + (n - 1) * 2 && h == chh;
                for (int k = 0; k < n && okc; k++) {
                    renderDocRGBA(*drawn[k], s, &v);
                    const int x0 = k * (cw + 2);
                    okc = cellIs(got, w, x0, cw, chh, s, drawn[k]->w, drawn[k]->h, 0, w);
                    if (okc && k) okc = colIs(got, w, h, x0 - 2, 128) && colIs(got, w, h, x0 - 1, 128);
                }
            } else if (app.compareMode == App::CmpWipe) {
                const int div = screenWipeCol(cw);
                const bool line = div > 0 && div < cw;      // off the picture: no line
                okc = w == cw && h == chh;
                if (okc) { renderDocRGBA(*drawn[0], s, &v); okc = cellIs(got, w, 0, cw, chh, s, drawn[0]->w, drawn[0]->h, 0, div); }
                if (okc) { renderDocRGBA(*drawn[1], s, &v); okc = cellIs(got, w, 0, cw, chh, s, drawn[1]->w, drawn[1]->h, div + (line ? 1 : 0), w); }
                if (okc && line) okc = colIs(got, w, h, div, 255);
            } else {
                const float g = app.diffGain > 0 ? app.diffGain : diffAutoGainOf(*drawn[0], *drawn[1]);
                renderDiffRGBA(*drawn[0], *drawn[1], drawn[0]->w, drawn[0]->h, 1, g, s);
                okc = w == drawn[0]->w && h == drawn[0]->h && got == s;
            }
            if (!okc) fprintf(stderr, "videoselftest: V8 %s: composite is not the spec (%dx%d) | %s\n",
                              tag, w, h, note.c_str());
            return okc;
        };
        auto exportCompare = [&](std::vector<std::vector<uint8_t>>& frames, int& w, int& h,
                                 std::string& said, const char* tag) {
            frames.clear();
            const std::string path = tmpFile("viewer_video_v8.avi");
            std::string why = startCompareVideoExport(path, 30.0);
            if (!why.empty()) fprintf(stderr, "videoselftest: V8 %s refused: %s\n", tag, why.c_str());
            w = app.videoExport.w; h = app.videoExport.h;
            const bool ran = why.empty() && runToEnd();
            said = app.toast;
            std::vector<uint8_t> bytes;
            readFileBytes(path, bytes);
            Avi Av = parseAvi(bytes);
            if (ran && Av.ok)
                for (auto& f : Av.frames) {
                    std::vector<uint8_t> rgb = aviFrameToRGB(bytes, f.first, w, h), rgba((size_t)w * h * 4, 255);
                    for (size_t p = 0; p < (size_t)w * h; p++)
                        for (int c = 0; c < 3; c++) rgba[p * 4 + c] = rgb[p * 3 + c];
                    frames.push_back(rgba);
                }
            std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
            return ran && Av.ok;
        };
        struct Mode8 { const char* name; int mode; bool slotC; float wipe; const char* says; };
        const Mode8 MODES[] = {
            { "split",  App::CmpSplit, false, 0.5f,  "compare: side by side A | B, 1:1 in 64x40 cells" },
            { "tiled",  App::CmpSplit, true,  0.5f,  "compare: tiled A | B | C, 1:1 in 64x40 cells" },
            // divider at canvas x 200 / 380: image x 20 + (200 - 300) / 3 = -13.3
            // -> 0 (the screen shows only B), and 20 + (380 - 300) / 3 = 46.7 -> 47
            { "wipe25", App::CmpWipe,  true,  0.25f, "compare: wipe: B from column 0 of 64, where the screen's divider is (all B)" },
            { "wipe70", App::CmpWipe,  false, 0.7f,  "compare: wipe: B from column 47 of 64, where the screen's divider is (1 px line)" },
            { "diff",   App::CmpDiff,  true,  0.5f,  "compare: difference A-B, full scale " },
        };
        bool specScreen = true, specOff = true, videoIsCompose = true, videoIsPng = true,
             notes = true, sizes = true, canvasPanes = true;
        int cases = 0;
        for (int follow = 1; follow >= 0; follow--)
        for (int rmode : { 0, 2 })
        for (const Mode8& M : MODES) {
            app.compareFollowFrame = follow != 0;
            app.compareRangeMode = rmode;
            app.compareMode = M.mode;
            app.wipeFrac = M.wipe;
            app.cmpExtra.clear();
            if (M.slotC) app.cmpExtra.push_back(C_(0)->uid);
            resetRanges();
            selectImage(fa[0]);
            char tag[64];
            snprintf(tag, sizeof tag, "%s follow=%d range=%d", M.name, follow, rmode);
            if (!composeIsSpec(comparedSides(), tag)) specScreen = false;
            // the panes laid out ARE the canvas's: tileSlots' documents and
            // letters in tileSlots' order, and as many as tileLayout lays out
            // in its one row (TilePane is an x and a width - no rows)
            if (M.mode == App::CmpSplit) {
                const std::vector<TileSlot> ts = tileSlots();
                const ViewSides vs = comparedSides();
                std::vector<TilePane> tp;
                const bool laid = tileLayout(0.0f, 4000.0f, (int)ts.size(), 1.0f, 1.0f, tp);
                bool same = laid && (int)ts.size() == vs.nPane && tp.size() == ts.size();
                for (size_t i = 0; same && i < ts.size(); i++) {
                    same = ts[i].doc == vs.pane[i] && ts[i].tag == paneRoleLetter(vs.paneRole[i]);
                    if (same && i) same = tp[i].x0 > tp[i - 1].x0;
                }
                if (!same) { canvasPanes = false; fprintf(stderr, "videoselftest: V8 %s panes != canvas\n", tag); }
            }
            // a view NOT on screen: A's frame 3, B and C following (or not)
            if (!composeIsSpec(viewSidesFor(A_(2)), tag)) specOff = false;
            // the composite's note
            {
                std::vector<uint8_t> g; int w = 0, h = 0; std::string note;
                composeCompareRGBA(comparedSides(), g, w, h, note);
                if (!has(note, M.says)) { notes = false; fprintf(stderr, "videoselftest: V8 %s note: %s\n", tag, note.c_str()); }
                if (M.slotC && M.mode != App::CmpSplit && !has(note, "C not shown")) notes = false;
                if (M.slotC && M.mode == App::CmpSplit && !has(note, "padded with black to 64x40 (C 40x24)")) notes = false;
                if (M.mode != App::CmpDiff && rmode == 2 && !has(note, "range: union over A, B")) notes = false;
                if (M.mode == App::CmpDiff && has(note, "letters")) notes = false;   // claims none
            }
            // the video: frame k == composeCompareRGBA(exportFrameViewFor(A_k))
            std::vector<std::vector<uint8_t>> fr; int vw = 0, vh = 0; std::string said;
            const bool ran = exportCompare(fr, vw, vh, said, tag) && fr.size() == 3;
            const int wantW = M.mode != App::CmpSplit ? 64 : M.slotC ? 3 * 64 + 2 * 2 : 2 * 64 + 2;
            if (!(vw == wantW && vh == 40)) sizes = false;
            // the union is worked out again for every frame, and the video says so
            if (!has(said, M.says) ||
                (M.mode != App::CmpDiff && rmode == 2 && !has(said, "(min-max), per frame"))) {
                notes = false;
                fprintf(stderr, "videoselftest: V8 %s video said: %s\n", tag, said.c_str());
            }
            for (int k = 0; k < 3; k++) {
                ImageDoc* stepTo = nullptr;
                std::vector<uint8_t> want; int w = 0, h = 0; std::string note;
                const bool c = composeCompareRGBA(exportFrameViewFor(A_(k), stepTo), want, w, h, note);
                if (!ran || !c || fr[k] != want) {
                    videoIsCompose = false;
                    fprintf(stderr, "videoselftest: V8 %s frame %d != composeCompareRGBA(exportFrameViewFor)\n", tag, k + 1);
                }
                // ...and the PNG of that view, with A stepped onto frame k
                selectImage(fa[k]);
                const std::string png = tmpFile("viewer_video_v8.png");
                saveCompareViewPng(png);
                std::vector<uint8_t> got;
                readFileBytes(png, got);
                std::error_code ec; std::filesystem::remove(pathFromUtf8(png), ec);
                if (!ran || got.empty() || got != encodePng(fr[k], vw, vh)) {
                    videoIsPng = false;
                    fprintf(stderr, "videoselftest: V8 %s frame %d != the PNG with A on frame %d\n", tag, k + 1, k + 1);
                }
                selectImage(fa[0]);
            }
            cases++;
        }
        check(cases == 20 && specScreen,
              "V8 every tile == renderDocRGBA(side, &v); separators, padding, wipe line, diff as specified");
        check(specOff, "V8 ...and for a view not on screen (A's frame 3), drawn in THAT view");
        check(canvasPanes, "V8 the panes are tileSlots' (same docs, letters, order), one row as tileLayout lays them");
        check(sizes, "V8 the video is 130x40 / 196x40 / 64x40 as the layout says");
        check(videoIsCompose, "V8 video frame k == composeCompareRGBA(exportFrameViewFor(A_k)), 20 cases");
        check(videoIsPng, "V8 ...== the compare PNG with A on frame k, byte for byte");
        check(notes, "V8 the composite and the video say mode, layout, padding, unshown slots and range");
        {   // anti-vacuity: the view matters, and B is not A
            app.compareMode = App::CmpSplit; app.compareRangeMode = 2; app.cmpExtra.clear();
            app.compareFollowFrame = true;
            resetRanges();
            selectImage(fa[0]);
            const ViewSides v2 = viewSidesFor(A_(2));
            std::vector<uint8_t> inV2, inScreen;
            renderDocRGBA(*B_(2), inV2, &v2);
            renderDocRGBA(*B_(2), inScreen);
            std::vector<uint8_t> a0, b0;
            const ViewSides v0 = comparedSides();
            renderDocRGBA(*A_(0), a0, &v0); renderDocRGBA(*B_(0), b0, &v0);
            check(inV2 != inScreen && a0 != b0,
                  "V8 fixture: a side's bytes depend on the view, and A's are not B's (anti-vacuity)");
        }
        {   // the letters: only inside each plate, in the canvas badge's ink
            app.compareMode = App::CmpSplit; app.compareRangeMode = 0;
            app.cmpExtra.assign(1, C_(0)->uid);
            selectImage(fa[0]);
            std::vector<uint8_t> off, on; int w = 0, h = 0; std::string note;
            composeCompareRGBA(comparedSides(), off, w, h, note);
            app.cmpExportLetters = true;
            composeCompareRGBA(comparedSides(), on, w, h, note);
            app.cmpExportLetters = false;
            // 64x40 cells: scale 1, so each plate is 7x9 at (x0 + 1, 1)
            const int GLYPH_PX[3] = { 18, 20, 13 };      // A, B, C counted by hand
            // the badge inks, by hand from drawCanvas: A blue, B orange, and the
            // first slot pane past A and B the first tile hue
            const uint8_t INK[3][3] = { { 120, 200, 255 }, { 255, 190, 120 }, { 105, 220, 130 } };
            bool outside = true, inks = true;
            int inkN[3] = {};
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int cell = -1;
                    for (int k = 0; k < 3; k++) {
                        const int x0 = k * 66 + 1;
                        if (x >= x0 && x < x0 + 7 && y >= 1 && y < 10) cell = k;
                    }
                    const uint8_t* p = px4(on, w, x, y);
                    if (cell < 0) { if (memcmp(p, px4(off, w, x, y), 4) != 0) outside = false; continue; }
                    if (isRGB(p, INK[cell][0], INK[cell][1], INK[cell][2])) inkN[cell]++;
                    else if (!isRGB(p, 0, 0, 0)) inks = false;
                }
            check(outside, "V8 letters on: every pixel outside the three plates is unchanged");
            check(inks && inkN[0] == GLYPH_PX[0] && inkN[1] == GLYPH_PX[1] && inkN[2] == GLYPH_PX[2],
                  "V8 ...and inside, A / B / C in the canvas badge inks on black, 18 / 20 / 13 glyph pixels");
            bool badge = true;
            for (int k = 0; k < 3; k++) {
                const ImU32 c = paneBadgeInk(k, k, 2);
                if ((uint8_t)(c >> IM_COL32_R_SHIFT) != INK[k][0] || (uint8_t)(c >> IM_COL32_G_SHIFT) != INK[k][1] ||
                    (uint8_t)(c >> IM_COL32_B_SHIFT) != INK[k][2]) badge = false;
            }
            check(badge, "V8 ...which are paneBadgeInk's, the one definition the canvas badges draw with");
            check(has(note, "letters on"), "V8 ...and the note says the letters are on");
        }
        {   // a slot on B's own stack: the canvas draws it as a pane of its
            // own, so the export does too - while the range union counts the
            // document once
            app.compareMode = App::CmpSplit; app.compareRangeMode = 2; app.compareFollowFrame = true;
            app.cmpExtra.assign(1, B_(1)->uid);     // C pinned to B's frame 2, following
            resetRanges();
            selectImage(fa[0]);
            const ViewSides v = comparedSides();
            check(v.n == 2 && v.nPane == 3 && v.pane[2] == B_(0) && v.paneRole[2] == 2 &&
                      tileSlots().size() == 3,
                  "V8 fixture: C on B's document - two sides for the range, three panes on the canvas");
            check(composeIsSpec(v, "dup"), "V8 ...and the composite has three panes, C's == B's pixels");
            std::vector<uint8_t> g; int w = 0, h = 0; std::string note;
            composeCompareRGBA(v, g, w, h, note);
            check(w == 3 * 64 + 2 * 2 && has(note, "tiled A | B | C"),
                  "V8 ...196 px wide, said as \"tiled A | B | C\"");
            std::vector<std::vector<uint8_t>> fr; int vw = 0, vh = 0; std::string said;
            bool vid = exportCompare(fr, vw, vh, said, "dup") && fr.size() == 3 && vw == 196;
            for (int k = 0; k < 3 && vid; k++) {
                ImageDoc* stepTo = nullptr;
                std::vector<uint8_t> want;
                composeCompareRGBA(exportFrameViewFor(A_(k), stepTo), want, w, h, note);
                vid = fr[k] == want;
            }
            check(vid, "V8 ...and the video's frame k has C following B to B_k as a third pane");
            app.cmpExtra.clear();
        }
        {   // blink: refused, by name, and no file is written
            app.compareMode = App::CmpFlip;
            std::vector<uint8_t> g; int w = 0, h = 0; std::string note;
            const bool c = composeCompareRGBA(comparedSides(), g, w, h, note);
            const std::string path = tmpFile("viewer_video_v8b.avi");
            const std::string why = startCompareVideoExport(path, 30.0);
            fprintf(stderr, "videoselftest: V8 blink -> \"%s\"\n", why.c_str());
            check(!c && has(note, "blink") && has(note, "side by side or wipe"),
                  "V8 blink is refused, pointing at side by side or wipe");
            check(has(why, "blink") && !app.videoExport.active &&
                      !std::filesystem::exists(pathFromUtf8(path)),
                  "V8 ...and so is a blink video, before a byte is written");
            app.compareMode = App::CmpDiff;
            app.cmpExtra.clear();
            setCompareB(C_(0));                       // 40x24 against 64x40
            check(!composeCompareRGBA(comparedSides(), g, w, h, note) &&
                      has(note, "same size (A 64x40, B 40x24)"),
                  "V8 a difference of two sizes is refused, naming both");
            setCompareB(B_(0));
            app.compareMode = App::CmpSplit;
            check(composeCompareRGBA(comparedSides(), g, w, h, note), "V8 ...and side by side composes (anti-vacuity)");
            // compare on with nobody to compare with is not "compare is off"
            setCompareB(nullptr);
            app.cmpExtra.clear();
            check(!composeCompareRGBA(comparedSides(), g, w, h, note) &&
                      has(note, "nothing to compare with (no B, no slots)"),
                  "V8 compare on with no B and no slot: \"nothing to compare with (no B, no slots)\"");
            setCompareB(B_(0));
        }
        {   // a canvas too narrow for the tiles: the canvas declines them, so the export does
            app.compareMode = App::CmpSplit; app.compareRangeMode = 0;
            app.cmpExtra.assign(1, C_(0)->uid);
            selectImage(fa[0]);
            g_canvasRect[2] = 300;                   // 3 x 110 px + two 6 px bands > 300
            std::vector<uint8_t> g; int w = 0, h = 0; std::string note;
            const bool c = composeCompareRGBA(comparedSides(), g, w, h, note);
            const std::string path = tmpFile("viewer_video_v8n.avi");
            const std::string why = startCompareVideoExport(path, 30.0);
            fprintf(stderr, "videoselftest: V8 narrow -> \"%s\"\n", note.c_str());
            check(!c && note == tileNarrowSaid(3, 110.0f, 300.0f) &&
                      has(note, "3 compare panes need at least 110 px each - this canvas is 300 px"),
                  "V8 a canvas too narrow for 3 tiles: refused in the canvas's own words");
            check(has(why, "3 compare panes need") && !std::filesystem::exists(pathFromUtf8(path)),
                  "V8 ...and so is the video, before a byte is written");
            app.cmpExtra.clear();
            check(composeCompareRGBA(comparedSides(), g, w, h, note),
                  "V8 ...while A | B, which the canvas never tiles, composes there (anti-vacuity)");
            g_canvasRect[2] = 400;
        }
        {   // the spec is taken when the export is ASKED for (#308 review)
            app.compareMode = App::CmpWipe; app.wipeFrac = 0.7f; app.compareRangeMode = 0;
            app.cmpExtra.clear();
            selectImage(fa[0]);
            // video: the modal opens on A's stack with the divider at column 47 ...
            askCompareVideoExport();
            app.videoAskOpen = false; app.videoAskCompare = false;
            const App::CompareSpec asked = app.videoDlgCmpSpec;
            // ... then, while the OS dialog is up, A moves to another stack and
            // the view is panned
            app.view.center = ImVec2(40.0f, 20.0f);
            selectImage(fc[1]);
            std::vector<std::vector<uint8_t>> fr; int vw = 0, vh = 0; std::string said;
            const std::string path = tmpFile("viewer_video_v8r.avi");
            const std::string why = startCompareVideoExport(path, 30.0, &app.videoDlgCmpSpec);
            const int jobSeq = app.videoExport.seqId;
            vw = app.videoExport.w; vh = app.videoExport.h;
            bool ran = why.empty() && runToEnd();
            std::vector<uint8_t> bytes; readFileBytes(path, bytes);
            Avi Av = parseAvi(bytes);
            bool same = ran && Av.ok && Av.frames.size() == 3 && jobSeq == sa && asked.wipeCol == 47;
            for (int k = 0; k < 3 && same; k++) {
                std::vector<uint8_t> rgb = aviFrameToRGB(bytes, Av.frames[k].first, vw, vh), want;
                int w = 0, h = 0; std::string note;
                composeCompareRGBA(viewSidesFor(A_(k)), want, w, h, note, &asked);
                for (size_t q = 0; q < (size_t)w * h && same; q++)
                    for (int c = 0; c < 3; c++) if (rgb[q * 3 + c] != want[q * 4 + c]) same = false;
                if (same && !isRGB(&want[(size_t)47 * 4], 255, 255, 255)) same = false;   // the line
            }
            std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
            check(same, "V8 video: A's stack and the divider column as asked, though A moved and the view panned");
            // PNG: the spec of the dialog's opening, not of the save
            selectImage(fa[0]);
            app.view.center = ImVec2(20.0f, 20.0f);
            const App::CompareSpec atOpen = compareSpecNow();
            app.view.center = ImVec2(40.0f, 20.0f);
            app.cmpExportLetters = true;
            const std::string png = tmpFile("viewer_video_v8r.png");
            saveCompareViewPng(png, A_(0)->uid, &atOpen);
            std::vector<uint8_t> got; readFileBytes(png, got);
            std::filesystem::remove(pathFromUtf8(png), ec);
            std::vector<uint8_t> want, now; int w = 0, h = 0; std::string note;
            composeCompareRGBA(viewSidesFor(A_(0)), want, w, h, note, &atOpen);
            composeCompareRGBA(viewSidesFor(A_(0)), now, w, h, note);
            check(!got.empty() && got == encodePng(want, w, h) && want != now,
                  "V8 PNG: drawn by the spec the dialog opened under, not the one at save time");
            app.cmpExportLetters = false;
            app.view.center = ImVec2(20.0f, 20.0f);
        }
        {   // ---- a change while the video is being written --------------------
            // Drawn by the spec (letters, gain): carries on, and every frame and
            // the toast are the spec's. Resolved from the app (B, a slot, |A-B|):
            // stops, and the partial file goes. A's stack: the frames are the
            // ones snapshotted, wherever A goes.
            const std::string path = tmpFile("viewer_video_v8m.avi");
            auto midWrite = [&](const std::function<void()>& change, std::vector<std::vector<uint8_t>>& fr,
                                std::string& said, App::CompareSpec& S) {
                fr.clear();
                selectImage(fa[0]);
                S = compareSpecNow();
                if (!startCompareVideoExport(path, 30.0).empty()) return false;
                const int w = app.videoExport.w, h = app.videoExport.h;
                pumpVideoExport();                    // frame 1 is written
                change();
                runToEnd();
                said = app.toast;
                std::vector<uint8_t> bytes;
                if (!readFileBytes(path, bytes)) return false;   // deleted
                Avi Av = parseAvi(bytes);
                for (auto& f : Av.frames) {
                    std::vector<uint8_t> rgb = aviFrameToRGB(bytes, f.first, w, h), rgba((size_t)w * h * 4, 255);
                    for (size_t q = 0; q < (size_t)w * h; q++)
                        for (int c = 0; c < 3; c++) rgba[q * 4 + c] = rgb[q * 3 + c];
                    fr.push_back(rgba);
                }
                std::error_code ec; std::filesystem::remove(pathFromUtf8(path), ec);
                return Av.ok;
            };
            auto framesAreSpec = [&](const std::vector<std::vector<uint8_t>>& fr, const App::CompareSpec& S) {
                if (fr.size() != 3) return false;
                for (int k = 0; k < 3; k++) {
                    std::vector<uint8_t> want; int w = 0, h = 0; std::string note;
                    composeCompareRGBA(viewSidesFor(A_(k)), want, w, h, note, &S);
                    if (fr[k] != want) return false;
                }
                return true;
            };
            std::vector<std::vector<uint8_t>> fr; std::string said; App::CompareSpec S;
            app.compareRangeMode = 0; app.cmpExtra.clear(); setCompareB(B_(0));
            // letters, snapshotted
            app.compareMode = App::CmpSplit; app.cmpExportLetters = false;
            bool okL = midWrite([&] { app.cmpExportLetters = true; }, fr, said, S) && framesAreSpec(fr, S) &&
                       has(said, "no letters");
            app.cmpExportLetters = false;
            check(okL, "V8 mid-write: letters toggled -> every frame and the toast are the spec's (no letters)");
            // the difference's gain, snapshotted
            app.compareMode = App::CmpDiff; app.diffGain = 50.0f;
            bool okG = midWrite([&] { app.diffGain = 5.0f; }, fr, said, S) && framesAreSpec(fr, S) &&
                       has(said, "full scale +-50 (set)");
            check(okG, "V8 mid-write: diff gain 50 -> 5 -> the video stays at +-50, and says so");
            // |A-B|: renderDiffRGBA reads it, so it stops the job
            bool gone = !midWrite([&] { app.diffAbs = true; }, fr, said, S) && !app.videoExport.active &&
                        has(said, "changed mid-write") && has(said, "deleted");
            app.diffAbs = false; app.diffGain = 0;
            check(gone, "V8 mid-write: |A-B| toggled -> stopped, the partial file deleted");
            // B
            app.compareMode = App::CmpSplit;
            gone = !midWrite([&] { setCompareB(C_(0)); }, fr, said, S) && has(said, "changed mid-write");
            setCompareB(B_(0));
            check(gone, "V8 mid-write: B re-pinned -> stopped, the partial file deleted");
            // a slot
            gone = !midWrite([&] { app.cmpExtra.push_back(C_(0)->uid); }, fr, said, S) &&
                   has(said, "changed mid-write");
            app.cmpExtra.clear();
            check(gone, "V8 mid-write: a slot added -> stopped, the partial file deleted");
            // A's stack: a frame lands in it and A walks off to another stack
            bool okA = midWrite([&] {
                mk8("v8a_3.npy", sa, 3, 64, 40, 1.0f, 30.0f);
                selectImage(fc[2]);
            }, fr, said, S) && framesAreSpec(fr, S);
            check(okA, "V8 mid-write: A's stack grows and A moves away -> the 3 frames asked for, unchanged");
        }
        {   // a hand-edited session with more cmpslot lines than letters
            const size_t cap = strlen(SLOT_LETTERS);
            app.cmpExtra.clear();
            app.cmpSlotRestore.clear();
            for (int i = 0; i < (int)cap + 2; i++) {
                char n[32]; snprintf(n, sizeof n, "v8slot_%02d.npy", i);
                const int at = mk8(n, 0, 0, 8, 8, 1.0f, (float)i);
                app.images[at]->seqId = 0;
                app.images[at]->src->path = n;
                app.cmpSlotRestore.push_back({ -1, n, std::string() });
            }
            pumpCompareSlotRestore();
            const std::string said = app.msgLog.empty() ? std::string() : app.msgLog.back().text;
            fprintf(stderr, "videoselftest: V8 restore cap -> \"%s\"\n", said.c_str());
            char last2[96];
            snprintf(last2, sizeof last2, "not restored: v8slot_%02d.npy, v8slot_%02d.npy", (int)cap, (int)cap + 1);
            check(app.cmpExtra.size() == cap && app.cmpSlotRestore.empty() && has(said, last2),
                  "V8 session restore: 16 cmpslot wants -> 14 slots (C to P), the 2 dropped named in Messages");
            app.cmpExtra.clear();
        }
        closeAll();
        for (int i = 0; i < 4; i++) g_canvasRect[i] = saveRect[i];
        g_canvasRectFrame = saveRectFrame;
        app.view.center = saveCenter;
        app.view.zoom = saveZoom;
        app.uiScale = saveUi;
        app.cmpExtra = saveExtra;
        app.compareBUid = saveBUid;
        app.lastCompareBUid = saveLastB;
        app.compareB = saveBName;
        app.compareBSeq = saveBSeq;
        app.compareMode = saveCmpMode;
        app.compareFollowFrame = saveFollow;
        app.wipeFrac = saveWipe;
        app.diffGain = saveDiffGain;
        app.diffAbs = saveDiffAbs;
        app.cmpExportLetters = saveLetters;
    }

    // The home is shared: give back every mode this test borrowed.
    app.rangeRule       = saveScope;
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
