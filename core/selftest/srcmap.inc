// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 全テーマ共有
    // ---- who holds which pixels (docs/verify-functional.md D-1/D-2/D-7) ------
    // The probe this test exists to exercise is srcMapDump(); the asserts below
    // are what it makes sayable. Every one of them is about a fact that had no
    // printing path at all before: the number of SOURCES behind N documents,
    // how many documents hold each one, and the Watch baseline statSourceFile
    // wrote (A-12 could only be settled by reading the code - the three decode
    // paths were traced by eye and the values never seen).
    //
    // M1 is the stage-1 baseline: nothing shares. It is not a law - stage 2
    // exists to break it - and it is written so that breaking it is LOUD.
    // M4/M5 do the sharing by hand (white-box, as S6 sets app.selectedAnn) for
    // two reasons: a probe that can only ever print refs=1 proves nothing about
    // the day something shares, and the crop CoW is unreachable in stage 1
    // (cropInPlace's own comment says so) yet is the whole of C2-3.
    if (!g_srcmapSelftest.empty()) {
        auto loadAll = [&]() {
            double t0 = glfwGetTime();
            while (glfwGetTime() - t0 < 300.0) {
                if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
                pumpSequenceAndQueue();
                if (!app.seqRunning && app.seqQueue.empty() && !seqReadyPending() &&
                    !app.folderPickOpen) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        };
        bool ok = true;
        auto check = [&](bool cond, const char* what) {
            fprintf(stderr, "srcmapselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
            if (!cond) ok = false;
        };
        openFolder(g_srcmapSelftest);
        loadAll();
        if (app.images.size() < 2) {
            fprintf(stderr, "srcmapselftest: need 2 images under %s\n",
                    g_srcmapSelftest.c_str());
            stopSequenceLoader(); stopRemoteFetcher(); stopMeasureWorker(); stopRbWorker();
            return 1;
        }
        srcMapDump("M1 after open");

        // ---- M1: one source per membership, and the mirrors agree with it ---
        {
            std::vector<SrcMapRow> rows = srcMapRows();
            bool one = !rows.empty();
            int shared = 0;
            for (const SrcMapRow& r : rows) if (r.refs != 1) { one = false; shared++; }
            check(one && (int)rows.size() == srcMapSources(),
                  "M1 stage 1: N documents, N sources, one holder each");
            fprintf(stderr, "srcmapselftest: M1 %zu doc(s) over %d source(s), "
                            "%d with more than one holder\n",
                    rows.size(), srcMapSources(), shared);
            bool mirrors = true;
            for (const auto& d : app.images)
                if (d->w != d->src->w || d->h != d->src->h || d->ch != d->src->ch ||
                    d->dtype != d->src->dtype)
                    mirrors = false;
            // §2.1: w/h/ch/dtype are per-doc MIRRORS. Nothing printed them
            // beside the source they mirror, so a stale one was invisible.
            check(mirrors, "M1 every mirror field agrees with the source it mirrors");
        }

        // ---- M2: the Watch baseline is really in there (D-2 / A-12) ---------
        // A-12 established BY READING that statSourceFile covers npy, npz and
        // raw. This is the npy path, executed: the values are compared against
        // the file on disk, so "the baseline is set" cannot pass on a zero.
        {
            int withPath = 0, good = 0;
            std::string firstBad;
            for (const auto& d : app.images) {
                const FrameSource& S = *d->src;
                if (S.path.empty()) continue;
                std::error_code sec;
                auto p = pathFromUtf8(S.path);
                if (!std::filesystem::exists(p, sec)) continue;
                withPath++;
                auto t = std::filesystem::last_write_time(p, sec);
                int64_t wantM = sec ? 0 : (int64_t)t.time_since_epoch().count();
                auto sz = std::filesystem::file_size(p, sec);
                uint64_t wantF = sec ? 0 : (uint64_t)sz;
                if (S.mtime == wantM && S.fsize == wantF && S.mtime != 0 && S.fsize != 0)
                    good++;
                else if (firstBad.empty())
                    firstBad = d->name + " mtime=" + std::to_string(S.mtime) + "/" +
                               std::to_string(wantM) + " fsize=" +
                               std::to_string(S.fsize) + "/" + std::to_string(wantF);
            }
            fprintf(stderr, "srcmapselftest: M2 npy: %d of %d file-backed frame(s) "
                            "carry the disk's own mtime+size%s%s\n",
                    good, withPath, firstBad.empty() ? "" : "; first miss: ",
                    firstBad.c_str());
            check(withPath > 0 && good == withPath,
                  "M2 the npy decode path leaves the Watch baseline on disk's values");
        }

        // ---- M3: ...and so does the raw decode path -------------------------
        {
            std::filesystem::path raw = std::filesystem::path(g_srcmapSelftest).parent_path() /
                                        "noise_640x480_gray16.raw";
            std::error_code rec;
            bool have = std::filesystem::exists(raw, rec);
            check(have, "M3 fixture: the raw frame is where the generator puts it");
            if (have) {
                size_t before = app.images.size();
                RawDialog rd;
                rd.path = raw.u8string();
                rd.w = 640; rd.h = 480;
                rd.dtype = RD_U16; rd.interp = RI_GRAY;
                std::string rerr = loadRaw(rd);
                check(rerr.empty() && app.images.size() == before + 1,
                      "M3 the raw frame opens");
                if (app.images.size() == before + 1) {
                    const FrameSource& S = *app.images[before]->src;
                    auto sz = std::filesystem::file_size(pathFromUtf8(S.path), rec);
                    fprintf(stderr, "srcmapselftest: M3 raw: mtime=%lld fsize=%llu "
                                    "(file %llu)\n", (long long)S.mtime,
                            (unsigned long long)S.fsize,
                            (unsigned long long)(rec ? 0 : sz));
                    check(S.mtime != 0 && !rec && S.fsize == (uint64_t)sz,
                          "M3 the raw decode path leaves the Watch baseline too");
                }
            }
        }

        // ---- M4: the probe can SEE sharing (its own calibration) ------------
        // Nothing in stage 1 shares, so M1 would read the same if the probe
        // printed a constant. Alias two memberships onto one source - which is
        // what stage 2 will do for real - and the map has to say so, in the
        // ordinal, in refs, and in the byte accounting (C1-2's 1x).
        std::vector<int> f0;
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->seqId != 0 && app.images[i]->seqId == app.images[0]->seqId)
                f0.push_back(i);
        if (f0.size() < 2) { f0.clear(); f0.push_back(0); f0.push_back(1); }
        {
            ImageDoc* a = app.images[f0[0]].get();
            ImageDoc* b = app.images[f0[1]].get();
            size_t bytesBefore = residentImageBytes();
            size_t oneFrame = b->px().size() * sizeof(float);
            b->src = a->src;                    // stage 2, by hand
            b->syncMirrors();
            srcMapDump("M4 two documents, one source");
            std::vector<SrcMapRow> rows = srcMapRows();
            int ordA = -1, ordB = -2;
            long refsA = 0, refsB = 0;
            for (const SrcMapRow& r : rows) {
                if (r.uid == a->uid) { ordA = r.src; refsA = r.refs; }
                if (r.uid == b->uid) { ordB = r.src; refsB = r.refs; }
            }
            check(ordA >= 0 && ordB == ordA,
                  "M4 both memberships print the SAME source ordinal");
            check(refsA == 2 && refsB == 2,
                  "M4 ...and both say two holders (use_count is readable at last)");
            check((int)rows.size() == srcMapSources() + 1,
                  "M4 one fewer source than documents");
            fprintf(stderr, "srcmapselftest: M4 resident %zu -> %zu B (one frame is "
                            "%zu B)\n", bytesBefore, residentImageBytes(), oneFrame);
            check(residentImageBytes() == bytesBefore - oneFrame,
                  "M4 shared pixels are counted once, not twice");

            // ---- M5: the crop CoW (C2-3), which stage 1 cannot reach --------
            // cropInPlace's use_count()>1 branch is dead code until something
            // shares; with M4's alias in place it is live, and the assert is
            // the one C2-3 asks for: the OTHER side's pixels do not move.
            int aw = a->w, ah = a->h;
            std::vector<float> aPixels = a->px();
            cropInPlace(*b, 0, 0, std::max(1, b->w / 2), std::max(1, b->h / 2));
            srcMapDump("M5 after cropping one of them");
            rows = srcMapRows();
            long ra = 0, rb = 0; int oa = -1, ob = -1;
            for (const SrcMapRow& r : rows) {
                if (r.uid == a->uid) { ra = r.refs; oa = r.src; }
                if (r.uid == b->uid) { rb = r.refs; ob = r.src; }
            }
            check(ra == 1 && rb == 1 && oa != ob,
                  "M5 the crop CoW split them: one holder each, two sources");
            check(a->w == aw && a->h == ah && a->px() == aPixels,
                  "M5 ...and the other side's pixels never moved (C2-3)");
            check(b->w == std::max(1, aw / 2) && b->dataRev > 0,
                  "M5 ...while the cropped side really was cropped");
        }

        // ---- M6: a clone is a new identity, not a second name for one -------
        {
            ImageDoc* a = app.images[f0[0]].get();
            a->src->rev = 7;                    // any non-zero: it must not travel
            auto fresh = cloneSource(*a->src);
            check(fresh->srcId != a->src->srcId && fresh->rev == 0 &&
                  fresh->data == a->src->data,
                  "M6 cloneSource: same content, own identity, rev back to 0 (D-7)");
            a->src->rev = 0;
        }

        srcMapDump("M7 final");
        fprintf(stderr, "srcmapselftest: %s\n", ok ? "ok" : "FAILED");
        stopSequenceLoader();
        stopRemoteFetcher();
        stopMeasureWorker();
        stopRbWorker();
        return ok ? 0 : 1;
    }
