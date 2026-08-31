// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
    // scanFolderGroups' ANSWER, pinned. Issue #236 rewrote how that answer is
    // computed - the directory listing is handed in instead of re-read once per
    // stack, members are found by name instead of by comparing every file
    // against every member, and the 256 cap now STOPS the walk instead of
    // silently discarding what it keeps finding - and the whole claim of that
    // rewrite is that it changes the answer in no way at all. A faster scan
    // that groups a folder differently is not a fix, it is a new defect with a
    // good benchmark, and the numbers in the PR cannot see the difference.
    //
    // So every case below states the group list by hand, derived from the rule
    // as it is written in siblingNamesAmong and in the leftover fold: names,
    // member counts, and the member names in order. A change in the frame axis,
    // in a pattern's extent, in which files fell together, or in WHICH 256
    // groups survive the cap fails here and names the case.
    //
    // Windowless and self-contained: the fixtures are built in TMP (per-test,
    // see viewer_selftest in CMakeLists.txt) and removed at the end, so nothing
    // here depends on tools/testdata and nothing survives the run.
    if (g_folderScanSelftest) {
        bool ok = true;
        std::error_code ec;
        std::filesystem::path root =
            std::filesystem::temp_directory_path(ec) / "viewer_folderscan";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);

        auto put = [&](const std::filesystem::path& d, const std::string& nm) {
            std::filesystem::create_directories(d, ec);
            std::ofstream f(pathFromUtf8((d / pathFromUtf8(nm)).u8string()),
                            std::ios::binary);
            f << "x";                 // the grouping rule reads NAMES, never bytes
        };
        // "name{a,b,c} name2{d}" - one line per group, groups in the order the
        // scan produced them. Everything a case claims is in this one string,
        // so a failure prints the whole answer rather than the first difference.
        auto shape = [&](const std::vector<App::PendingGroup>& gs) {
            std::string s;
            for (const auto& g : gs) {
                if (!s.empty()) s += " ";
                s += g.name + "{";
                for (size_t i = 0; i < g.files.size(); i++)
                    s += (i ? "," : "") + baseName(g.files[i]);
                s += "}";
            }
            return s;
        };
        auto expect = [&](const char* what, const std::string& dir,
                          const std::string& want) {
            const std::string got = shape(scanFolderGroups(dir));
            const bool pass = got == want;
            fprintf(stderr, "folderscanselftest: %-46s %s\n", what, pass ? "PASS" : "FAIL");
            if (!pass) {
                fprintf(stderr, "  want: %s\n  got : %s\n", want.c_str(), got.c_str());
                ok = false;
            }
        };
        const std::string DD = "\xE2\x80\xA5";        // U+2025 ‥, patternWithExtent's

        // ---- S1: one numbered run, with unreadable-to-us files beside it ------
        // The rewrite hands scanFolderGroups' OWN listing to the rule, and that
        // listing holds only loadable files - findSequenceSiblings used to read
        // the directory again and see everything in it. The two cannot disagree,
        // and this says why in a fixture: a candidate must carry the head's
        // exact extension, the head's extension is loadable because the head is,
        // so notes.txt and frame_001.log were never candidates for frame_00?.npy
        // under either version. They are here so that "the scan ignores them"
        // is asserted rather than assumed.
        {
            std::filesystem::path d = root / "one";
            for (const char* n : { "frame_000.npy", "frame_001.npy", "frame_002.npy" })
                put(d, n);
            put(d, "notes.txt");
            put(d, "frame_001.log");
            expect("S1 one run, non-loadable neighbours ignored", d.u8string(),
                   "frame_000" + DD + "002.npy{frame_000.npy,frame_001.npy,frame_002.npy}");
        }

        // ---- S2: TWO varying digit runs - the frame axis is the LAST one ------
        // The axis is chosen by counting how many distinct values a run takes
        // among the candidates, and that count is what the rewrite replaced
        // (a list re-scanned per candidate -> a set the walk leaves at two).
        // "Two or more" is all the rule ever asked, so a set that stops at two
        // must decide identically - here, on names where BOTH runs vary and the
        // wrong choice is a different number of groups with different members.
        //
        //   a01_b1 a01_b2 a02_b1 a02_b2, stem runs ["a"]["01"]["_b"]["1"]
        //   run 1 varies (01/02) and run 3 varies (1/2); the LAST wins, so each
        //   a-value is its own stack and b is the frame axis. Picking run 1
        //   instead would give two stacks split the other way.
        {
            std::filesystem::path d = root / "twoaxis";
            for (const char* n : { "a01_b1.npy", "a01_b2.npy", "a02_b1.npy", "a02_b2.npy" })
                put(d, n);
            expect("S2 two varying runs, last one is the axis", d.u8string(),
                   "a01_b1" + DD + "2.npy{a01_b1.npy,a01_b2.npy} "
                   "a02_b1" + DD + "2.npy{a02_b1.npy,a02_b2.npy}");
        }

        // ---- S3: the leftover fold, per extension -----------------------------
        // Nothing numbered here, so every file falls through the sibling rule
        // and the per-extension fold answers: two or more of one extension are
        // ONE stack in natural order, a lone one keeps its own name.
        {
            std::filesystem::path d = root / "loose";
            for (const char* n : { "alpha.npy", "beta.npy", "gamma.raw", "delta.raw",
                                   "solo.png" })
                put(d, n);
            expect("S3 leftovers fold per extension", d.u8string(),
                   "*.npy{alpha.npy,beta.npy} *.png{solo.png} *.raw{delta.raw,gamma.raw}");
        }

        // ---- S4: subdirectories -----------------------------------------------
        // Files are filed under the directory they were found in - a lookup now
        // rather than a walk of the directories already seen - and the result is
        // sorted by directory, with each group named by its folder relative to
        // the scanned root. Two subdirectories holding the SAME file names is
        // the case a broken filing would fold into one stack of four.
        {
            std::filesystem::path d = root / "tree";
            for (const char* sub : { "b_second", "a_first" })
                for (const char* n : { "shot_0.npy", "shot_1.npy" })
                    put(d / sub, n);
            expect("S4 per-directory filing and order", d.u8string(),
                   "a_first/shot_0" + DD + "1.npy{shot_0.npy,shot_1.npy} "
                   "b_second/shot_0" + DD + "1.npy{shot_0.npy,shot_1.npy}");
        }

        // ---- S5: the 256 cap, which is now a STOP -----------------------------
        // push() has always dropped groups past 256; the walk used to keep
        // finding them anyway, at the full per-stack cost, and #236 measured
        // that as most of a 17.9-minute wait. Stopping the walk instead must
        // leave the SAME 256 groups: the files are sorted, heads are taken in
        // that order, so the survivors are the first 256 stacks by name - and
        // that is asserted here by name, not merely by count. A stop placed one
        // step early or late shows up as k255/k256 at the end of the list.
        {
            std::filesystem::path d = root / "cap";
            for (int i = 0; i < 300; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "k%03d_f0.npy", i); put(d, nm);
                snprintf(nm, sizeof nm, "k%03d_f1.npy", i); put(d, nm);
            }
            std::string want;
            for (int i = 0; i < 256; i++) {
                char pat[64], f0[32], f1[32];
                snprintf(pat, sizeof pat, "k%03d_f0", i);
                snprintf(f0, sizeof f0, "k%03d_f0.npy", i);
                snprintf(f1, sizeof f1, "k%03d_f1.npy", i);
                if (i) want += " ";
                want += std::string(pat) + DD + "1.npy{" + f0 + "," + f1 + "}";
            }
            expect("S5 the cap stops the walk at the same 256", d.u8string(), want);
        }

        // ---- S6: the same rule, asked the way Watch asks it --------------------
        // siblingNamesAmong now has two mouths: names, and names already split.
        // Watch and the reload rule use the first, the folder scan uses the
        // second, and they are one rule or the tool answers one folder two ways
        // (the reason the split exists at all is written above the function).
        // So the string mouth is asked S2's question directly and must give
        // S2's answer.
        {
            std::vector<std::string> names = { "a01_b1.npy", "a01_b2.npy",
                                               "a02_b1.npy", "a02_b2.npy" };
            std::string pat;
            std::vector<std::string> mem = siblingNamesAmong(names[0], names, pat);
            std::string joined;
            for (size_t i = 0; i < mem.size(); i++) joined += (i ? "," : "") + mem[i];
            const std::string got = pat + "{" + joined + "}";
            const std::string want = "a01_b1" + DD + "2.npy{a01_b1.npy,a01_b2.npy}";
            const bool pass = got == want;
            fprintf(stderr, "folderscanselftest: %-46s %s\n",
                    "S6 the name-only mouth answers alike", pass ? "PASS" : "FAIL");
            if (!pass) {
                fprintf(stderr, "  want: %s\n  got : %s\n", want.c_str(), got.c_str());
                ok = false;
            }
        }

        std::filesystem::remove_all(root, ec);        // ours: ours to remove
        fprintf(stderr, "folderscanselftest: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
