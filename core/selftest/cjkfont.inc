// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---- --cjkfont-selftest: CJK filenames draw as themselves (板306) ----------
// The user report was 「日本語とか中国語のファイル名が文字化けする」 and the
// diagnosis was a DOUBLE absence: this build did not ASK for the code points
// (GetGlyphRangesJapanese plus four hand-picked characters, which leaves 45.8%
// of the common simplified-Chinese set and the whole of General Punctuation and
// Letterlike Symbols outside), and the Windows font it asked does not HAVE
// them (Meiryo holds 1819 of the 2500 simplified characters; 测 and 试 are in
// none of the four Japanese faces on a stock Windows).
//
// Those are two different failures and this test keeps them apart, because the
// fix for each is different and so is what it can promise:
//
//   C1 - the RANGES. Pure, and therefore the same on every machine in the
//        matrix: cjkFontRanges() is this build's own decision about which code
//        points to ask for, and a code point it does not ask for can never be
//        drawn however good the font is. This is the group that was red.
//   C2 - the ATLAS. Whether a glyph actually got baked is a fact about the font
//        file on THIS machine, so the assertions run when a CJK font was found
//        and say so by name when one was not. A pass here on the Ubuntu runner
//        (no CJK font installed) is not the same claim as a pass on a Windows
//        desktop, and the printed lines are what tell the two apart.
//   C3 - the ORDER of the two faces. The one thing a merge can get wrong and
//        still look like it worked.
//   C4 - undrawableCount: the names that are STILL '?' after all of the above.
//   C5 - the search, the override and the refusal that names the paths.
//   C6 - pathFromUtf8 does not throw on bytes that are not UTF-8.
//
// NOGL. Every assertion here is a glyph metric, a code point or a string, and
// the atlas is CPU: ImFontAtlas::Build() rasterises with stb_truetype and needs
// no context, which is why fontAtlasBake() calls it at start-up instead of
// leaving it to the renderer backend. It writes no fixture except one
// settings.jsonc, in its own temp directory, for --settings-selftest's reason.
static int cjkFontSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const std::string& what) {
        fprintf(stderr, "cjkfontselftest: %-66s %s\n", what.c_str(), cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto say = [&](const std::string& s) {
        fprintf(stderr, "cjkfontselftest:   %s\n", s.c_str());
    };

    // The code points the report is about, each with the reason it is here. The
    // name is what the failure will be printed as, so it says what the user
    // would have seen rather than a number.
    //
    // WHAT EACH CLASS CAN BE ASSERTED, and why they are not one list. "Is this
    // code point in the ranges" is this build's decision and is the same
    // everywhere; "did a glyph get baked" is a fact about the font file on the
    // machine, and a test that demanded a glyph no font on the runner has would
    // be asserting that GitHub install a font.
    //   Must - in JIS X 0208, so EVERY Japanese font has them. Required
    //          wherever the atlas has any CJK at all.
    //   Cjk  - simplified-only characters. Required where a second face was
    //          merged (that is what the merge is FOR); reported elsewhere,
    //          because a Japanese-only face legitimately lacks them.
    //   Soft - reported, never required. Outside JIS X 0208, so a strict
    //          Japanese font may not carry them, and U+29C9 has a designed
    //          fallback of its own (shareGlyph prints the word "shared").
    // Nothing in Cjk or Soft escapes unmeasured: whatever fails to bake has to
    // be COUNTED by undrawableCount, which is asserted below and is the whole
    // point of 板306 D.
    enum Cls { Must, Cjk, Soft };
    struct CP { unsigned int cp; Cls cls; const char* what; };
    static const CP kProbes[] = {
        // The two characters in 测试 ("test"), which is how the report reached
        // us at all. Both outside GetGlyphRangesJapanese and both absent from
        // every Japanese face on a stock Windows - so they need the wider range
        // AND the merged face, and they are the probes that fail if either half
        // of the fix is missing.
        { 0x6D4B, Cjk,  "U+6D4B 测  (simplified 'measure')" },
        { 0x8BD5, Cjk,  "U+8BD5 试  (simplified 'test')" },
        { 0x6C49, Cjk,  "U+6C49 汉  (simplified 'Han')" },
        { 0x7EDF, Cjk,  "U+7EDF 统  (simplified 'system')" },
        // THE ONES THAT MADE JAPANESE NAMES BREAK TOO. Letterlike Symbols and
        // General Punctuation were in no range at all, although every candidate
        // font has them: 25℃.npy and 60℃_dark.npy are ordinary names in camera
        // IQ work, and a name that reads 25?.npy is the report's own words.
        { 0x2103, Must, "U+2103 degC (Letterlike Symbols)" },
        { 0x2116, Soft, "U+2116 No.  (Letterlike Symbols)" },
        { 0x2014, Soft, "U+2014 em dash (General Punctuation)" },
        { 0x2013, Soft, "U+2013 en dash (General Punctuation)" },
        { 0x2026, Must, "U+2026 ellipsis (General Punctuation)" },
        { 0x201C, Must, "U+201C left double quote (General Punctuation)" },
        { 0x201D, Must, "U+201D right double quote (General Punctuation)" },
        { 0x2018, Must, "U+2018 left single quote (General Punctuation)" },
        // The four this build already asked for, one at a time. They are in the
        // table so that widening the ranges cannot silently drop one of them -
        // U+2025 in particular is now covered BY the punctuation block rather
        // than by its own AddChar, which is exactly the kind of change that
        // loses a character without anybody noticing.
        { 0x2025, Must, "U+2025 two dot leader (stack extents: frame_000..023.npy)" },
        { 0x29C9, Soft, "U+29C9 two joined squares (the Files panel's share mark)" },
        { 0x03C3, Must, "U+03C3 sigma (the noise tables)" },
        { 0x03BC, Must, "U+03BC mu (the noise tables)" },
        // ...and the Japanese the report led with, which must not regress.
        { 0x3042, Must, "U+3042 hiragana A" },
        { 0x30AB, Must, "U+30AB katakana KA" },
        { 0x6E2C, Must, "U+6E2C 測 (traditional/Japanese 'measure')" },
        { 0x5B9A, Must, "U+5B9A 定" },
        { 0x753B, Must, "U+753B 画" },
        { 0x50CF, Must, "U+50CF 像" },
    };
    const int kProbeN = (int)(sizeof kProbes / sizeof kProbes[0]);
    // A one-character UTF-8 name from a code point, so that what did not bake
    // can be handed straight to undrawableCount.
    auto nameOf = [](unsigned int cp) {
        char b[8] = { 0 };
        ImTextCharToUtf8(b, cp);
        return std::string(b) + ".npy";
    };

    // ======================================================================
    //  C1 - THE RANGES THIS BUILD ASKS FOR
    //
    //  Pure: cjkFontRanges() takes an atlas only to reach ImGui's two static
    //  tables. So this group runs identically on every runner, and it is the
    //  group that was red before 板306 A - measured, with the ranges as they
    //  were: 测 试 汉 统 degC No. and every one of the six punctuation marks
    //  came back "NOT in the ranges".
    // ======================================================================
    ImVector<ImWchar> ranges;
    cjkFontRanges(ImGui::GetIO().Fonts, ranges);
    {
        int wanted = 0;
        for (int k = 0; k + 1 < ranges.Size && ranges[k]; k += 2)
            wanted += (int)ranges[k + 1] - (int)ranges[k] + 1;
        say("ranges: " + std::to_string(ranges.Size / 2) + " interval(s), " +
            std::to_string(wanted) + " code point(s) asked for");
        for (const CP& p : kProbes)
            check(cjkRangeHas(ranges, p.cp),
                  std::string("C1 asked for: ") + p.what);
        // ANTI-VACUITY, both ways. A range builder that answered "yes" to
        // everything would pass every line above and mean nothing, and the
        // Hangul exclusion is a DECISION (板306 A) rather than an oversight -
        // so it is asserted, and a later change that quietly adds Korean has to
        // come here and say so.
        check(!cjkRangeHas(ranges, 0xAC00),
              "C1 ...and NOT Hangul (U+AC00): that is a separate board row");
        check(!cjkRangeHas(ranges, 0x0590),
              "C1 ...and not Hebrew, which nothing has asked for");
        // The price has an upper bound, and it is the acceptance condition of
        // the widening itself: every code point asked for is a glyph
        // rasterised while the window is being made. 6000 is roughly 20% of
        // headroom over the 5075 measured here - enough for the next ordinary
        // addition, not enough for a whole script.
        check(wanted > 4500 && wanted < 6000,
              "C1 ...and the whole set is " + std::to_string(wanted) +
              " code points, inside the 4500..6000 the atlas is budgeted for");
    }

    // ======================================================================
    //  C2 - WHAT ACTUALLY GOT BAKED
    //
    //  The other half of the diagnosis, and the half that is a fact about this
    //  machine rather than about this build. FindGlyphNoFallback, one code
    //  point at a time, against the atlas start-up baked - not a fresh one, so
    //  that what is asserted is what the user is looking at.
    // ======================================================================
    {
        const FontAtlasCost& c = g_fontAtlasCost;
        say("font: " + (g_fontChoice.path.empty() ? std::string("(none found)")
                                                  : g_fontChoice.path) +
            " (chosen by " + g_fontChoice.how + ")");
        say("merged: " + (g_fontMergePath.empty() ? std::string("(no second face)")
                                                  : g_fontMergePath));
        // THE RECORD start-up actually built, one line per path. This is what
        // the refusal prints for a user, so it is worth seeing in a test log
        // too: on a machine where the override was refused it says which path
        // and WHY, which is the only place that reason exists.
        for (const std::string& t : g_fontChoice.tried) say("tried: " + t);
        char b[256];
        snprintf(b, sizeof b, "atlas: %d glyph(s) of %d asked for, %dx%d px "
                              "(%.1f MB RGBA), %d face(s), %.0f ms, font scale %.2f",
                 c.bakedGlyphs, c.wantedCodepoints, c.texW, c.texH,
                 c.texW * (double)c.texH * 4.0 / 1048576.0, c.faces, c.buildMs,
                 c.fontScale);
        say(b);
        // The instrument itself. A cost of zero would mean fontAtlasBake never
        // ran, and every number printed above would be a comfortable fiction.
        check(c.texW > 0 && c.texH > 0 && c.bakedGlyphs > 0 && c.wantedCodepoints > 0,
              "C2 the atlas was built at start-up and its cost was measured");
        ImFont* f = bakedFont();
        check(f != nullptr, "C2 ...and a font is reachable without a frame");
        if (f && fontAtlasHasCjk()) {
            int baked = 0, cjkBaked = 0, cjkN = 0, declared = 0, undeclared = 0;
            for (const CP& p : kProbes) {
                const bool have = p.cp <= 0xFFFF &&
                                  f->FindGlyphNoFallback((ImWchar)p.cp) != nullptr;
                if (have) baked++;
                if (p.cls == Cjk) { cjkN++; if (have) cjkBaked++; }
                if (p.cls == Must)
                    check(have, std::string("C2 baked: ") + p.what);
                else
                    fprintf(stderr, "cjkfontselftest: %-66s %s\n",
                            (std::string("C2 baked: ") + p.what).c_str(),
                            have ? "yes" : "no (reported, not required here)");
                // THE INVARIANT THAT HOLDS ON EVERY MACHINE, and the reason
                // 板306 D exists at all: whatever did not bake is COUNTED, so
                // the program never draws a '?' it has not owned up to. This is
                // what makes "reported, not required" honest rather than a
                // hole - a font's coverage is the machine's business, but
                // saying so is this program's.
                if (!have) {
                    if (undrawableCount(nameOf(p.cp)) >= 1) declared++;
                    else {
                        undeclared++;
                        say(std::string("NOT BAKED AND NOT DECLARED: ") + p.what);
                    }
                }
            }
            say("baked " + std::to_string(baked) + " of " + std::to_string(kProbeN) +
                " probes; simplified-Chinese " + std::to_string(cjkBaked) + " of " +
                std::to_string(cjkN) + "; " + std::to_string(declared) +
                " absence(s) declared by undrawableCount");
            check(undeclared == 0,
                  "C2 every probe that did NOT bake is counted as undrawable");
            // ...and where a second face was merged, the simplified characters
            // are not optional: that is the whole purpose of the merge, and a
            // merge that landed but delivered nothing would pass every other
            // line in C3.
            if (!g_fontMergePath.empty())
                check(cjkBaked == cjkN,
                      "C2 a second face was merged, so all " + std::to_string(cjkN) +
                      " simplified-Chinese probes must bake");
            else
                say("no second face here, so the simplified probes are reported "
                    "rather than required - and " + std::to_string(cjkN - cjkBaked) +
                    " of them would draw as '?', which is what the Messages line "
                    "and the row tooltip say (板306 D).");
            // ...and the atlas does NOT hold a code point outside the ranges,
            // or "it has everything" would explain the passes above.
            check(!f->FindGlyphNoFallback((ImWchar)0x0E01),
                  "C2 ...and Thai, which is in no range, was not baked");
        } else {
            // NOT a silent skip: it says which machine this is and why the
            // group could not run, so a pass here is never read as a claim
            // about glyphs. The range half above ran on this machine too.
            say("NO CJK GLYPHS ON THIS MACHINE - C2's per-glyph half cannot run "
                "here. That is the state the start-up toast reports, and it is "
                "what the Ubuntu runner looks like: no Japanese font installed.");
            check(!g_fontChoice.tried.empty(),
                  "C2 ...and the paths that were looked at were recorded anyway");
        }
    }

    // ======================================================================
    //  C3 - THE ORDER OF THE TWO FACES
    //
    //  THE FAILURE THIS GROUP EXISTS FOR looks exactly like a success: a merge
    //  in the wrong order draws every Japanese kana and kanji in a Chinese
    //  face's shapes. Nothing is missing, nothing is '?', and the typeface of
    //  ordinary Japanese text has silently changed.
    //
    //  ImFontAtlas merges FIRST COME FIRST SERVED (imgui_draw.cpp: "if
    //  (dst_tmp.GlyphsSet.TestBit(codepoint)) continue;"), so the claim under
    //  test is that the Japanese face is added first and the Chinese one
    //  second. It is pinned twice over:
    //
    //   1. From the atlas's OWN RECORD - ConfigData in order, the second entry
    //      carrying MergeMode - which is machine-independent and says exactly
    //      what was asked of ImGui.
    //   2. From the GLYPH METRICS, which is the only thing in the BUILT atlas
    //      that remembers which file a glyph came from. Two probe atlases are
    //      baked, one per face, at the same pixel size, and every code point in
    //      the range set is compared: wherever the two faces DISAGREE the merged
    //      atlas has to have the first one's shape, and wherever only the second
    //      face has the glyph the merged atlas has to have the second one's.
    //
    //  The sweep is the WHOLE range set rather than the 22 named probes,
    //  because 22 shared code points could in principle be drawn identically by
    //  two faces from one foundry and the assertion would pass while measuring
    //  nothing. Several thousand cannot.
    //
    //  Advance and the horizontal box only. Y0/Y1 are offset by the destination
    //  font's ascent - which for a merged face is the FIRST face's ascent - so
    //  a face-2 glyph is legitimately at a different height in the merged atlas
    //  than in an atlas of its own, and comparing it would fail for a reason
    //  that is not the subject.
    // ======================================================================
    {
        auto shapeOf = [](ImFont* f, unsigned int cp, float out[3]) -> bool {
            if (!f || cp > 0xFFFF) return false;
            const ImFontGlyph* g = f->FindGlyphNoFallback((ImWchar)cp);
            if (!g) return false;
            out[0] = g->AdvanceX; out[1] = g->X0; out[2] = g->X1 - g->X0;
            return true;
        };
        auto same = [](const float a[3], const float b[3]) {
            return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
        };
        // ---- WHAT CAN AND CANNOT BE PINNED HERE (review P3) ----------
        // Two assertions used to live here that could not fail on a machine
        // reaching this branch. `selfMerged == 0` walks cjkMergeFontPath,
        // which on Linux returns {} for every input because the merge list
        // is empty there - so it asserted nothing at all. And
        // `cjkMergeFontPath(path).empty()` re-evaluated the same pure
        // function whose emptiness had already put this branch on the
        // screen. Both are replaced by claims about the TABLE, which is
        // the thing that can actually regress, and which is the same on
        // every platform this builds for.
        const std::vector<FontCandidate> tbl = cjkFontCandidates();
        int ja = 0, zh = 0, dupes = 0;
        std::vector<std::string> seenPath;
        for (const FontCandidate& fc : tbl) {
            if (fc.lang == LangJa) ja++; else zh++;
            if (std::find(seenPath.begin(), seenPath.end(), fc.path) != seenPath.end())
                dupes++;
            seenPath.push_back(fc.path);
        }
        say("candidate table: " + std::to_string(ja) + " ja, " +
            std::to_string(zh) + " zh, " + std::to_string(dupes) + " duplicate(s)");
        // THE DEFECT THIS PINS: Hiragino Sans GB was in the primary list
        // AND the merge list, so on macOS the "a Chinese face is being used
        // for everything" message could never fire and a refusal could
        // print one path twice. One tagged table makes that impossible, and
        // a duplicate row here is the regression to catch.
        check(dupes == 0,
              "C3 no path appears twice in the candidate table (it used to: "
              "Hiragino Sans GB was in both lists)");
        check(ja >= 1,
              "C3 ...and this platform lists at least one Japanese face");
        // The two derived views are exactly the two halves of the table -
        // asserted as a partition, so a font cannot be silently dropped
        // from both or counted in both.
        check((int)jpFontCandidates().size() == ja &&
              (int)cjkMergeCandidates().size() == zh &&
              ja + zh == (int)tbl.size(),
              "C3 ...and the ja/zh views partition it exactly");
        // ...and a Chinese face is never the merge partner of itself, which
        // is the one thing the old loop was trying to say. Asserted only
        // where there IS one, and said by name where there is not.
        if (zh == 0) {
            say("this platform lists no Chinese face at all, so 'never merged "
                "onto itself' has no input here - it is asserted on the "
                "platforms that have one.");
        } else {
            int selfMerged = 0;
            for (const std::string& m : cjkMergeCandidates())
                if (cjkMergeFontPath(m) == m) selfMerged++;
            check(selfMerged == 0,
                  "C3 ...and no Chinese face is ever merged onto itself (" +
                  std::to_string(zh) + " checked)");
        }
        if (g_fontMergePath.empty()) {
            // One face is the ordinary state off Windows: Noto Sans CJK and
            // Hiragino Sans GB are pan-CJK in one file, so there is no order to
            // get wrong. Said out loud, and what CAN be asserted is asserted -
            // that the build really is running one face and not two it forgot
            // to count.
            say("ONE FACE on this machine (" +
                (g_fontChoice.path.empty() ? std::string("no font") : g_fontChoice.path) +
                "): there is no merge order to check. cjkMergeFontPath() returns "
                "nothing off Windows by design - the platform's own font is "
                "pan-CJK - and C3's per-glyph half needs two files.");
            check(g_fontAtlasCost.faces <= 1 || g_fontChoice.path.empty(),
                  "C3 with no second face the atlas carries one face");

        } else {
            // AT THE SIZE START-UP USED (review P2-10). These were 17.0f
            // flat, so a run with --ui-scale 2 baked the probes at 17 px while
            // the real atlas was at 34, every metric differed for a reason that
            // is not the subject, and the order assertions failed on a
            // perfectly correct merge.
            const float px = 17.0f * (g_fontAtlasCost.fontScale > 0.0f
                                          ? g_fontAtlasCost.fontScale : 1.0f);
            say("probe atlases baked at " + fmtExact(px) + " px, which is what "
                "start-up used");
            ImFontAtlas one, two;
            ImFont* f1 = one.AddFontFromFileTTF(g_fontChoice.path.c_str(), px,
                                                nullptr, ranges.Data);
            ImFont* f2 = two.AddFontFromFileTTF(g_fontMergePath.c_str(), px,
                                                nullptr, ranges.Data);
            if (f1) one.Build();
            if (f2) two.Build();
            ImFont* merged = bakedFont();
            check(f1 && f2 && merged,
                  "C3 both faces bake on their own, so the merge can be compared");
            // (1) WHAT WAS ASKED OF ImGui, from its own record. Two configs, in
            // order, the second one merging - so "the order" is not inferred
            // from pixels alone. ConfigData[k].Name is "<file>, 17px", which is
            // why the file's leaf name is what is looked for.
            ImFontAtlas* live = ImGui::GetIO().Fonts;
            auto leaf = [](const std::string& p) {
                const size_t k = p.find_last_of("/\\");
                return k == std::string::npos ? p : p.substr(k + 1);
            };
            check(live->ConfigData.Size == 2,
                  "C3 the atlas carries exactly two faces");
            if (live->ConfigData.Size == 2) {
                const std::string n0 = live->ConfigData[0].Name;
                const std::string n1 = live->ConfigData[1].Name;
                say("config: [0] " + n0 + (live->ConfigData[0].MergeMode ? " (merge)" : "") +
                    " | [1] " + n1 + (live->ConfigData[1].MergeMode ? " (merge)" : ""));
                check(n0.find(leaf(g_fontChoice.path)) != std::string::npos &&
                      !live->ConfigData[0].MergeMode,
                      "C3 ...the FIRST is the Japanese face, and it is not a merge");
                check(n1.find(leaf(g_fontMergePath)) != std::string::npos &&
                      live->ConfigData[1].MergeMode,
                      "C3 ...the SECOND is the Chinese face, merged on top");
            }
            // (2) ...and what the BUILT atlas shows, over the whole range set.
            if (f1 && f2 && merged) {
                int both = 0, disagree = 0, wrongWay = 0, onlyTwo = 0, twoWrong = 0;
                std::string firstWrong;
                for (int k = 0; k + 1 < ranges.Size && ranges[k]; k += 2) {
                    for (unsigned int cp = ranges[k]; cp <= (unsigned)ranges[k + 1]; cp++) {
                        float a[3], b[3], m[3];
                        const bool inOne = shapeOf(f1, cp, a);
                        const bool inTwo = shapeOf(f2, cp, b);
                        const bool inM   = shapeOf(merged, cp, m);
                        if (inOne && inTwo) {
                            both++;
                            if (!same(a, b)) {
                                disagree++;
                                if (!(inM && same(m, a))) {
                                    wrongWay++;
                                    if (firstWrong.empty())
                                        firstWrong = "U+" + std::to_string(cp);
                                }
                            }
                        } else if (!inOne && inTwo) {
                            onlyTwo++;
                            if (!(inM && same(m, b))) twoWrong++;
                        }
                    }
                }
                say("faces over the whole range set: " + std::to_string(both) +
                    " code point(s) in both, " + std::to_string(disagree) +
                    " drawn differently by the two, " + std::to_string(onlyTwo) +
                    " only in the second" +
                    (firstWrong.empty() ? "" : "; first wrong: " + firstWrong));
                // The order, stated as the structure rather than as a picture:
                // where the two faces draw the same code point differently, the
                // merged atlas has the FIRST one's shape.
                check(disagree > 0,
                      "C3 the two faces really do draw " + std::to_string(disagree) +
                      " of " + std::to_string(both) +
                      " shared code points differently, so the order is observable");
                check(wrongWay == 0,
                      "C3 ...and on every one of those the merged atlas has the "
                      "FIRST face's shape - Japanese stays in the Japanese font");
                // ...and the second face is doing its job, or the order would
                // be trivially right and the simplified characters absent.
                check(onlyTwo > 0,
                      "C3 ...while " + std::to_string(onlyTwo) +
                      " code point(s) exist only in the second face");
                check(twoWrong == 0,
                      "C3 ...and those come through with the SECOND face's shape");
            }
        }
    }

    // ======================================================================
    //  C4 - THE NAMES THAT ARE STILL '?'
    //
    //  Widening the ranges cannot finish this job: a font is entitled not to
    //  have a glyph, and a code point above U+FFFF does not fit in ImWchar at
    //  all. What is left has to be DECLARED, because 测试1.npy and 试测1.npy
    //  both read "??1.npy" and no panel in the program can tell them apart.
    // ======================================================================
    {
        // A BMP code point in no range on any machine (Thai), so the count is
        // the same on every runner. U+1F600 is above the BMP, which is the
        // other half - and the half no range can ever reach.
        const std::string thai   = "\xE0\xB8\x81" "1.npy";              // U+0E01
        const std::string emoji  = "run\xF0\x9F\x98\x80" ".npy";         // U+1F600
        const std::string plain  = "frame_000.npy";
        const std::string broken = "a\xFF\xFE" "b.npy";                  // not UTF-8
        check(undrawableCount(thai) == 1,
              "C4 one character with no glyph counts 1");
        check(undrawableCount(emoji) == 1,
              "C4 ...and so does a code point above the BMP, which no range reaches");
        check(undrawableCount(plain) == 0,
              "C4 ...and a name the atlas can draw counts 0");
        check(undrawableCount(broken) == 2,
              "C4 ...and each byte that is not UTF-8 counts (two here)");
        // ---- WHICH CAUSE, not just how many (review P3) --------------------
        // The classification used to read `used == 4` as "a code point above
        // U+FFFF". ImGui's ImTextCharFromUtf8 returns, on its ERROR path,
        // `ImMin(wanted, !!s[0] + !!s[1] + !!s[2] + !!s[3])` - the number of
        // non-zero bytes it looked at - so an ILL-FORMED sequence starting
        // with a 4-byte lead byte returns 4 exactly as a well-formed one does.
        // The old code therefore told the user about "emoji and the rarer CJK
        // extensions" when the truth was that their filename is not UTF-8, and
        // offered a fix that cannot work. `*out_char` cannot separate them
        // either: on every error path it is U+FFFD.
        {
            // F0 followed by ASCII: a 4-byte lead byte with no continuation
            // bytes. This is the case that was misfiled.
            const std::string lead4 = "x\xF0" "ABC.npy";
            const UndrawableTally t4 = undrawableTally(lead4);
            say("F0+ASCII -> noGlyph=" + std::to_string(t4.noGlyph) +
                " tooHigh=" + std::to_string(t4.tooHigh) +
                " badBytes=" + std::to_string(t4.badBytes));
            check(t4.badBytes >= 1 && t4.tooHigh == 0,
                  "C4 an ill-formed 4-byte lead is BAD BYTES, not 'above the BMP'");
            // ...and a real emoji, which IS above the BMP, still lands there -
            // or the fix above would have been "call everything bad bytes".
            const UndrawableTally te = undrawableTally(emoji);
            say("emoji    -> noGlyph=" + std::to_string(te.noGlyph) +
                " tooHigh=" + std::to_string(te.tooHigh) +
                " badBytes=" + std::to_string(te.badBytes));
            check(te.total() == 1,
                  "C4 ...while a real emoji is still counted exactly once");
            // ...AND IN THE RIGHT BUCKET. This was measured and not asserted,
            // which meant the simpler implementation - "every failed decode is
            // badBytes" - passed every line here. It is not good enough: with a
            // 16-bit ImWchar ImGui reports a VALID emoji as a decode error, so
            // that implementation tells a user with a 4-byte character in a
            // filename that their bytes are broken, and offers a fix that
            // cannot apply. Only reading the bytes again separates the two.
            check(te.tooHigh == 1 && te.badBytes == 0,
                  "C4 ...as tooHigh, NOT as bad bytes (the bytes are valid)");
            // ...and the sentence that comes out says the build, not the file.
            const std::string ne = undrawableNoteOf(te);
            check(ne.find("this build can draw at all") != std::string::npos &&
                  ne.find("not valid UTF-8") == std::string::npos,
                  "C4 ...and its sentence blames the build, not the bytes");
            // The SENTENCE must not offer a font as the cure for either of
            // those two, because no font can be.
            const std::string n4 = undrawableNoteOf(t4);
            check(n4.find("not valid UTF-8") != std::string::npos &&
                  n4.find("appearance.fontPath") == std::string::npos,
                  "C4 ...and the sentence for bad bytes does NOT suggest a font");
        }
        // THE CASE THE UBUNTU RUNNER CAUGHT, and it is the reason the two
        // assertions above are worth their own lines. ImGui decodes all three
        // of "not UTF-8", "truncated" and "above U+FFFF" to U+FFFD, and
        // GetGlyphRangesJapanese itself ASKS for U+FFFD - so on a machine whose
        // font HAS that glyph (DejaVu Sans does; Meiryo and YaHei do not) an
        // atlas lookup alone answers "nothing wrong with this name" for an
        // emoji. undrawableCount therefore looks at the BYTES: a genuine U+FFFD
        // in a name is exactly EF BF BD and is drawn faithfully; anything else
        // that came out as U+FFFD was substituted and the character is lost.
        {
            ImFont* ff = bakedFont();
            const bool atlasHasFffd =
                ff && ff->FindGlyphNoFallback((ImWchar)0xFFFD) != nullptr;
            say(std::string("the atlas ") + (atlasHasFffd ? "HAS" : "does not have") +
                " a glyph for U+FFFD, which is what decides whether an atlas "
                "lookup alone could tell a substitution from a real one");
            // A name that really contains U+FFFD draws as itself where the font
            // has it - so it is NOT counted, and that is the same call that
            // must still count the emoji. One pair, both directions.
            check(undrawableCount("\xEF\xBF\xBD" ".npy") == (atlasHasFffd ? 0 : 1),
                  "C4 a REAL U+FFFD is judged by the glyph, not by the byte pattern");
            check(undrawableCount(emoji) == 1 && undrawableCount(broken) == 2,
                  "C4 ...while a SUBSTITUTED one is counted either way");
        }
        check(!undrawableNote(thai).empty() && undrawableNote(plain).empty(),
              "C4 the sentence is there for the first and absent for the second");
        // It asks the ATLAS, like shareGlyph(), and not the range table: a name
        // whose characters are all IN the ranges and all baked counts 0, and
        // that is only answerable from the built atlas.
        if (fontAtlasHasCjk()) {
            check(undrawableCount("\xE6\xB8\xAC\xE5\xAE\x9A" "_25\xE2\x84\x83.npy") == 0,
                  "C4 測定_25degC.npy - the report's own shape - counts 0 now");
            check(undrawableCount("\xE6\xB5\x8B\xE8\xAF\x95" "1.npy") == 0,
                  "C4 ...and so does 测试1.npy, which was the '??1.npy' row");
        } else {
            say("no CJK glyphs here, so C4's two report-shaped names cannot be "
                "asserted to count 0 on this machine - the four above are "
                "machine-independent and did run.");
        }
        // THE NAME IS NEVER REWRITTEN. The sentence is a count and a way out;
        // it does not contain an escaped copy of the name, because a row
        // reading "\\u6d4b\\u8bd51.npy" is a row nobody can match against their
        // own folder. Asserted rather than left to the reader of the function.
        const std::string note = undrawableNote(thai);
        check(note.find("\\u") == std::string::npos &&
              note.find("0x") == std::string::npos &&
              note.find(thai) == std::string::npos,
              "C4 ...and the sentence never re-prints the name, escaped or not");
        // THE WAY OUT IS OFFERED ONLY WHERE IT WORKS (review P3). Thai is in
        // NO range this build asks for, so no font on the machine can draw it -
        // the atlas was never going to hold it. Naming appearance.fontPath
        // there is a false instruction, the same type as the rest of this
        // round, so the sentence must NOT name it.
        check(note.find("in a script this build does not load glyphs for") !=
                  std::string::npos &&
              note.find("appearance.fontPath") == std::string::npos,
              "C4 ...and for a script this build never asks for, it does NOT "
              "offer a font (no font could help)");
        // ...and the opposite case, which is the one a font CAN fix: a code
        // point this build DOES ask for and the font happens not to have.
        // U+29C9 is exactly that on a stock Windows - cjkFontRanges adds it by
        // hand for the Files panel's share mark, and neither Meiryo nor YaHei
        // carries it, which is why shareGlyph() has a word fallback. Gated on
        // the observation, because a machine whose font HAS it would measure 0.
        {
            const std::string shareName = "\xE2\xA7\x89" ".npy";   // U+29C9
            const UndrawableTally st = undrawableTally(shareName);
            say("U+29C9 -> noGlyph=" + std::to_string(st.noGlyph) +
                " notAsked=" + std::to_string(st.notAsked));
            check(st.notAsked == 0,
                  "C4 U+29C9 is a code point this build DOES ask for");
            if (st.noGlyph > 0) {
                const std::string sn = undrawableNoteOf(st);
                check(sn.find("no glyph in this font") != std::string::npos &&
                      sn.find("appearance.fontPath") != std::string::npos,
                      "C4 ...and for a glyph the FONT is missing, the way out "
                      "IS offered");
            } else {
                say("this machine's font has U+29C9, so the 'a font can fix it' "
                    "half cannot be shown here - the 'no font can fix it' half "
                    "above did run.");
            }
        }
        // ---- A NEW CAUSE NEEDS EVERY PLACE THAT WALKS THE FIELDS ----------
        // The comment on `operator+=` used to put a NUMBER on this ("one more
        // cause means editing one function") and the number was wrong: a cause
        // is a field, plus `total()`, plus `operator+=`, plus a line in
        // undrawableNoteOf's `add` column - and the defect that comment
        // describes is exactly what forgetting the last one reproduces.
        //
        // A comment cannot keep a count of the code honest. This can. The table
        // is the set of causes; `sizeof` is what fails when a field is added
        // without being added here, and each cause is then asked for all three
        // of the things a cause needs.
        {
            struct Cause { const char* phrase; int UndrawableTally::* field; };
            const Cause causes[] = {
                { "no glyph in this font",            &UndrawableTally::noGlyph  },
                { "script this build does not load",  &UndrawableTally::notAsked },
                { "range this build can draw",        &UndrawableTally::tooHigh  },
                { "not valid UTF-8",                  &UndrawableTally::badBytes },
            };
            const size_t n = sizeof causes / sizeof causes[0];
            check(sizeof(UndrawableTally) == n * sizeof(int),
                  "C4 every field of UndrawableTally is a cause in this table "
                  "(a new one fails here first, which is the point)");
            for (size_t i = 0; i < n; i++) {
                UndrawableTally one{};
                one.*(causes[i].field) = 1;
                check(one.total() == 1,
                      std::string("C4 ...and total() counts it: ") + causes[i].phrase);
                UndrawableTally sum{};
                sum += one;
                check(sum.total() == 1,
                      std::string("C4 ...and operator+= carries it: ") + causes[i].phrase);
                const std::string note = undrawableNoteOf(one);
                check(note.find(causes[i].phrase) != std::string::npos,
                      std::string("C4 ...and the sentence names it: ") + causes[i].phrase);
                check(note.find("1 character(s) here") != std::string::npos,
                      std::string("C4 ...and the total it prints is 1: ") + causes[i].phrase);
            }
        }
        // ---- the Messages door (板306 D), ONE LINE PER OPEN ----------------
        // Driven off app.imagesRev rather than off any one of the places that
        // push a document, and grouped by BATCH because a batch is the
        // program's own name for one open action. Borrowed state is given back
        // at the end: this suite shares one home with every other test.
        //
        // A sweep does not report a batch that is still GROWING - one more
        // sweep at the same size is what says the load has landed - so every
        // case below sweeps twice.
        //
        // THE SWEEP DOES NOT TOUCH app.imagesRev, and that is the whole point
        // of this group now. It used to read
        //
        //     auto sweep = [&]() { app.imagesRev++; undrawableReportNewDocs(); };
        //
        // with a comment claiming "which is also what the frame loop does".
        // The frame loop does NOT do that: every `imagesRev++` in the program
        // happens with a push, and nothing increments it periodically. So the
        // test was supplying the one thing the real program never supplies,
        // and it passed while the feature did not work at all - the comment
        // pinned the defect as the specification. Calling the function twice
        // with the counter held still is what a real second frame looks like,
        // and it fails three ways:
        //   - against the old implementation (rev-gated): 0 lines, because the
        //     second call returns at the gate;
        //   - against a naive fix that just deletes the gate: the line appears
        //     on the FIRST call, with a count taken mid-load;
        //   - against one that never settles: more than one line.
        const size_t logWas = app.msgLog.size();
        const uint64_t revWas = app.imagesRev;
        auto sweep = [&]() { undrawableReportNewDocs(); };
        // EVERY FIXTURE HAS A PATH, and until round 6 none did. `src->path`
        // was left empty, `add()` drops an empty piece, and so every assertion
        // about the Messages sweep was made in a world where documents have no
        // paths - which production has none of. That is how the member clause
        // came to count the path and stay green: the number it printed could
        // only be wrong when a path existed, and no test gave it one.
        auto mkDoc = [&](const std::string& nm, int batchId, int seqId,
                         const std::string& dir = "C:/plain/") {
            auto d = std::make_unique<ImageDoc>();
            d->name = nm;
            d->src->path = dir + nm;
            d->src->w = 4; d->src->h = 4; d->src->ch = 1;
            d->src->dtype = "u16";
            d->uid = app.nextUid++;
            d->batchId = batchId;
            d->seqId = seqId;
            app.images.push_back(std::move(d));
        };
        auto linesSince = [&](size_t from) {
            std::vector<std::string> v;
            for (size_t k = from; k < app.msgLog.size(); k++) v.push_back(app.msgLog[k].text);
            return v;
        };
        {
            mkDoc(thai, 101, 0);
            app.imagesRev++;               // the push, as a real loader does it
            sweep();                       // records the size, says nothing yet
            const std::vector<std::string> mid = linesSince(logWas);
            check(mid.empty(),
                  "C4 the first sweep of a growing batch says nothing (the count "
                  "would be mid-load)");
            sweep();                       // ...and this one, with the counter
                                           // held still, is the settled frame
            const std::vector<std::string> got = linesSince(logWas);
            for (const std::string& l : got) say("msg: " + l);
            check(got.size() == 1 && got[0].find("draw as '?'") != std::string::npos,
                  "C4 one open of one such file leaves exactly ONE line, WITHOUT "
                  "app.imagesRev moving again");
            // ...and not again. THE TRAP THIS AVOIDS (review P2-11): logMsg
            // suppresses a line identical to the one before it, so an assertion
            // that only swept again would pass even with the de-duplication
            // removed. A DIFFERENT line goes in between, so the suppression
            // under test is this function's and not logMsg's.
            logMsg("cjkfont selftest marker", false);
            const size_t after = app.msgLog.size();
            sweep(); sweep(); sweep();
            check(app.msgLog.size() == after,
                  "C4 ...and never again for that open (asserted past logMsg's "
                  "own duplicate suppression, and past three more sweeps)");
        }
        // ---- A STACK IS ONE LINE, NOT ONE PER FRAME (review P2-5) ----------
        // THE DEFECT THIS REPLACES: a folder of 400 numbered frames is one
        // gesture, and the per-document version wrote 400 lines into an
        // app.msgLog capped at 300 - so the log that exists to diagnose this
        // pushed out everything else in it, the start-up font messages
        // included. Measured here with 40 members, which is already more than
        // a tenth of the cap.
        {
            const size_t before = app.msgLog.size();
            App::SeqInfo si;
            si.id = app.nextSeqId++;
            si.name = thai;                // the STACK's name is undrawable
            app.seqs.push_back(si);
            for (int k = 0; k < 40; k++) {
                mkDoc("frame_" + std::to_string(k) + ".npy", 102, si.id);
                app.imagesRev++;           // one bump per push, as a stack load does
            }
            sweep(); sweep();
            const std::vector<std::string> got = linesSince(before);
            for (const std::string& l : got) say("msg: " + l);
            check(got.size() == 1,
                  "C4 a 40-frame stack leaves ONE line, not 40");
            check(!got.empty() && got[0].find(thai) != std::string::npos,
                  "C4 ...and it is the STACK's name, which is what the row draws");
            // ...and when the FRAMES are the undrawable ones, the line says how
            // many - that is the number that tells the user whether two ROWS or
            // two FRAMES are the ambiguous pair.
            const size_t before2 = app.msgLog.size();
            App::SeqInfo si2;
            si2.id = app.nextSeqId++;
            si2.name = "plain_stack";      // the stack's own name is fine
            app.seqs.push_back(si2);
            for (int k = 0; k < 6; k++) {
                mkDoc(k < 2 ? thai : ("ok_" + std::to_string(k) + ".npy"), 103, si2.id);
                app.imagesRev++;
            }
            sweep(); sweep();
            const std::vector<std::string> got2 = linesSince(before2);
            for (const std::string& l : got2) say("msg: " + l);
            check(got2.size() == 1 && got2[0].find("2 of 6 frame(s)") != std::string::npos,
                  "C4 ...and a stack whose FRAMES carry it says 2 of 6");
            // ---- AND A FOLDER MUST NOT MAKE IT "N of N" (review round 6 P2)
            // The clause says the characters are "in their own names". When the
            // tally behind it counted each frame's whole PATH, one '?' in the
            // shared folder made every frame affected, so the clause printed
            // "6 of 6 frame(s) ... in their own names" about six frames whose
            // names are plain ASCII - and said it one sentence after the row's
            // own clause had already reported that same single character.
            //
            // sequence.inc's sibling rule means this is the ONLY shape a folder
            // stack can produce: non-numeric segments of the leaves must match,
            // so counting paths can give N of N and nothing else, and the
            // "some frames but not others" case the clause exists for cannot
            // arise from a path at all.
            const size_t before2b = app.msgLog.size();
            App::SeqInfo si3;
            si3.id = app.nextSeqId++;
            si3.name = "folder_stack";               // the stack's name is fine
            app.seqs.push_back(si3);
            const std::string badDir = "C:/" + thai.substr(0, 3) + "/";
            for (int k = 0; k < 6; k++) {
                mkDoc("ok_" + std::to_string(k) + ".npy", 113, si3.id, badDir);
                app.imagesRev++;
            }
            sweep(); sweep();
            const std::vector<std::string> got3 = linesSince(before2b);
            for (const std::string& l : got3) say("msg: " + l);
            check(got3.size() == 1,
                  "C4 a stack in a folder whose NAME carries it is reported");
            check(!got3.empty() &&
                      got3[0].find("character(s) here") != std::string::npos,
                  "C4 ...by the row's own clause, which is what sees the folder");
            check(!got3.empty() && got3[0].find("frame(s) in this stack") ==
                                       std::string::npos,
                  "C4 ...and NOT by the member clause: no frame's own NAME has "
                  "one, so there is no 'N of N frame(s)' sentence");
        }
        // ---- WHAT IS COUNTED IS A SET OF CHARACTERS (review round 5) ------
        // THE CONTRACT, in the manual's words (§8c): the same character is
        // never counted twice. Two rounds tried to deliver that by making the
        // STRINGS disjoint - splitting the path at its last separator, then
        // deduplicating whole strings - and the arms kept coming: `d.name` is a
        // strict SUPERSTRING of the path's leaf for an npz member, a remote
        // frame, a decorated row and a stack name, and whole-string dedup sees
        // none of them. The npz shape said "8 character(s)" about 4.
        //
        // These are the shapes, each with an EXACT N. Before the fix only (1)
        // passed: the npz case counted 5 where the answer is 3, and no fixture
        // for it existed because nothing in this file ever set `src->member`.
        {
            const std::string kA = thai.substr(0, 3);     // U+0E01, asked nowhere
            const std::string kB = "\xE0\xB8\x82";         // U+0E02
            const std::string kC = "\xE0\xB8\x83";         // U+0E03
            const std::string bad1 = std::string(1, (char)0xFF);   // not UTF-8
            const std::string bad2 = std::string(1, (char)0xFE);
            auto mk = [&](const std::string& path, const std::string& name,
                          const std::string& member) {
                auto d = std::make_unique<ImageDoc>();
                d->src->w = 4; d->src->h = 4; d->src->ch = 1;
                d->src->dtype = "u16";
                d->uid = app.nextUid++;
                d->src->path = path;
                d->name = name;
                d->src->member = member;      // the field no fixture ever set
                return d;
            };
            // ---- THE EQUATION, held by an oracle (review round 7 P2) -----
            // The sentence promises "N character(s) here draw as '?'", so N has
            // to be what the reader sees. The unit has moved three times, and
            // the tests for those moves DECLARED the unit instead of checking
            // that equation - so round 6 could count 4 where the screen shows 1
            // and stay green.
            //
            // This walks the pieces THE WAY THE RENDERER WALKS TEXT: ImGui
            // decodes one step at a time and draws exactly one glyph per step,
            // advancing by whatever the decoder consumed. It counts GLYPH
            // POSITIONS the intended character cannot appear at, and it merges
            // repeats of the same CHARACTER because the sentence counts
            // characters; an ill-formed step is not a character, so it is never
            // merged. Nothing here calls into the tally's own code - no
            // undrawableUnitKey, no undrawableTally - which is what makes a
            // disagreement possible to see.
            auto questionMarks = [&](const std::vector<std::string>& pieces) {
                ImFont* ff = bakedFont();
                std::vector<uint32_t> chars;     // distinct characters shown as '?'
                int steps = 0;                   // decode steps that are no character
                for (const std::string& s : pieces) {
                    const char* p = s.c_str();
                    const char* e = p + s.size();
                    while (p < e) {
                        unsigned int cp = 0;
                        const int used = ImTextCharFromUtf8(&cp, p, e);
                        if (used <= 0) break;
                        const bool realFffd =
                            used == 3 && (unsigned char)p[0] == 0xEF &&
                            (unsigned char)p[1] == 0xBF && (unsigned char)p[2] == 0xBD;
                        const bool err =
                            cp == (unsigned)IM_UNICODE_CODEPOINT_INVALID && !realFffd;
                        auto want = [&chars](uint32_t v) {
                            if (std::find(chars.begin(), chars.end(), v) == chars.end())
                                chars.push_back(v);
                        };
                        if (err) {
                            if (utf8SequenceAbove16Bit(p, e)) {
                                const unsigned char* u = (const unsigned char*)p;
                                want(((uint32_t)(u[0] & 0x07u) << 18) |
                                     ((uint32_t)(u[1] & 0x3Fu) << 12) |
                                     ((uint32_t)(u[2] & 0x3Fu) << 6) |
                                     (uint32_t)(u[3] & 0x3Fu));
                                p += 4;
                            } else {
                                steps++;          // one step, one glyph, one '?'
                                p += used;
                            }
                            continue;
                        }
                        p += used;
                        if (cp < 0x20 || cp == 0x7F) continue;   // no glyph is asked for
                        if (cp <= 0xFFFF && ff &&
                            ff->FindGlyphNoFallback((ImWchar)cp)) continue;   // it drew
                        want((uint32_t)cp);
                    }
                }
                return (int)chars.size() + steps;
            };
            auto countOf = [&](const ImageDoc& d, const std::string& label) {
                const std::vector<std::string> pieces = undrawablePieces(label, d);
                for (const std::string& q : pieces) say("piece: " + q);
                const UndrawableTally t = undrawableTallyOf(pieces);
                const int shown = questionMarks(pieces);
                say("  -> notAsked=" + std::to_string(t.notAsked) +
                    " badBytes=" + std::to_string(t.badBytes) +
                    " total=" + std::to_string(t.total()) +
                    "  (the row shows " + std::to_string(shown) + ")");
                // EVERY fixture in this group checks the equation, not just the
                // ones written for it.
                check(t.total() == shown,
                      "C4 the number equals the '?' this row shows (" +
                      std::to_string(t.total()) + " vs " + std::to_string(shown) + ")");
                return t.total();
            };
            // (1) ONE bad character in the folder and a DIFFERENT one in the
            // leaf: 2. A list that counts the leaf twice gives 3; one that
            // drops the directory gives 1.
            const auto d1 = mk("C:/data" + kA + "/frame" + kB + ".npy",
                               "frame" + kB + ".npy", std::string());
            check(countOf(*d1, d1->name) == 2,
                  "C4 one bad character in the folder and a different one in "
                  "the file name counts 2");
            // (2) THE SAME character in both halves: 1, and this is the number
            // the contract decides. It is not "one per place on screen" - the
            // sentence says how many CHARACTERS of this row cannot be drawn,
            // and there is one such character here however often it appears.
            // THIS NUMBER CHANGED WITH THIS ROUND: it used to be 2.
            const auto d2 = mk("C:/data" + kA + "/frame" + kA + ".npy",
                               "frame" + kA + ".npy", std::string());
            check(countOf(*d2, d2->name) == 1,
                  "C4 ...while the SAME character in both halves counts 1 "
                  "(the same character is never counted twice)");
            // (3) THE npz SHAPE, which is the one that said 8 about 4.
            // loader_npz builds `d.name` as baseName(path) + ":" + member, so
            // the leaf is a strict PREFIX of the name and the member a strict
            // SUFFIX of it - three distinct characters, each of them in two
            // pieces at once.
            const auto d3 = mk("C:/" + kA + "/" + kB + ".npz",
                               kB + ".npz:" + kC, kC);
            check(countOf(*d3, d3->name) == 3,
                  "C4 ...and an npz member row counts its three characters "
                  "once each, though every one of them is in two pieces");
            // (4) A DECORATED LABEL, which is what a preview or a stale row
            // draws: the label is no longer EQUAL to d.name, so whole-string
            // dedup lets it in - and the answer must not move.
            check(countOf(*d3, "  " + d3->name + "  [preview]") == 3,
                  "C4 ...and a decorated label does not change the count");
            // (5) A STACK ROW's label, which is the Messages sweep's shape: it
            // passes the STACK's name, and the row draws exactly that. The
            // answer is 2, not 3 - the member character kC is in the FRAME's
            // name, which a stack row does not draw anywhere, and the sentence
            // this feeds says "N character(s) HERE". The frames' own names are
            // the member clause's subject and are counted there (review round
            // 6 P3). Before the fix `d.name` was a piece whatever the label
            // was, so this read 3 and claimed a character the row cannot show.
            check(countOf(*d3, "stack of " + kB + ".npz") == 2,
                  "C4 ...while a STACK row counts only what the stack row "
                  "shows: its own label and its tooltip's first line");
            // (6) ...AND THE THING THAT IS NOT A CHARACTER. An ill-formed run
            // is one decode step, one glyph, one count - never merged with
            // another, because there is no "the same character" to merge it
            // with (the ruling; manual.md §8c says it in the canon). The
            // numbers here are what the renderer draws, which `countOf` now
            // checks independently for every fixture.
            const auto d4 = mk("C:/a" + bad1 + "/b" + bad2 + ".npy",
                               "b" + bad2 + ".npy", std::string());
            check(countOf(*d4, d4->name) == 3,
                  "C4 ...three stray bytes across the label and the path count 3");
            const auto d5 = mk("C:/a" + bad1 + "/b" + bad1 + ".npy",
                               "b" + bad1 + ".npy", std::string());
            check(countOf(*d5, d5->name) == 3,
                  "C4 ...and the same byte value in three places is still 3: "
                  "bytes that are not a character are not deduplicated");
            // ...AND ONE STEP IS ONE '?' HOWEVER MANY BYTES IT EATS. This is
            // the case round 6 got wrong: counting one byte at a time, an
            // ill-formed four-byte sequence came out as 4 while the renderer
            // draws a single glyph for it and swallows the other three bytes.
            // `\xF0\x80\x80\x80` is a valid-looking 4-byte lead with valid
            // continuation bytes and an overlong value, so the decoder consumes
            // all four and reports an error.
            const auto d6 = mk("C:/x/a\xF0\x80\x80\x80" "b.npy",
                               "a\xF0\x80\x80\x80" "b.npy", std::string());
            check(countOf(*d6, d6->name) == 2,
                  "C4 ...so one ill-formed FOUR-byte step is ONE '?' in each "
                  "piece that carries it, not four");
            // ...and the bytes a bad lead byte DOES swallow are not counted
            // again as drawable characters, which is the other direction of the
            // same equation.
            const UndrawableTally tl = undrawableTally("x\xF0" "ABC.npy");
            say("F0+ASCII -> badBytes=" + std::to_string(tl.badBytes) +
                " total=" + std::to_string(tl.total()));
            check(tl.badBytes == 1 && tl.total() == 1,
                  "C4 ...and an ill-formed lead byte is one count, with the "
                  "bytes it consumed inside it");
            // ...and the old lower bound, kept and labelled with what it is
            // now worth, which is very little.
            //
            // IT DOES NOT PROVE THE PIECES ARE DISJOINT - that is how round 4's
            // critical finding stayed green - and since round 5 made the whole
            // path one piece it does not prove coverage either: the path is a
            // substring of itself, so `covered >= path.size()` is true for ANY
            // piece list that contains the path, whatever else is in it or
            // missing from it (review round 6 P3). It is kept as a guard on one
            // thing only: that the tooltip's first line is still IN the list,
            // which is the piece a caller could silently stop adding. The
            // exact-N lines above are what says everything else.
            const std::vector<std::string> pieces =
                undrawablePieces(d3->name, *d3);
            size_t covered = 0;
            for (const std::string& q : pieces)
                if (d3->src->path.find(q) != std::string::npos) covered += q.size();
            check(covered >= d3->src->path.size(),
                  "C4 ...and the tooltip's first line is still one of the "
                  "pieces (which is ALL this says - see the comment)");
            app.images.clear();
        }

        // ---- A SECOND OPEN OF THE SAME BATCH STILL SPEAKS (review P3) ----
        // batchReuse() returns an EXISTING batch id when the same folder is
        // opened again, so a per-BATCH "already said" flag silenced every later
        // open from a folder that had been reported once. Keyed per ROW instead.
        // Nothing asserted that, so putting the flag back on the batch stayed
        // green - this is the line that notices.
        {
            const size_t before4 = app.msgLog.size();
            mkDoc(thai, 109, 0);                 // first row of batch 109
            app.imagesRev++;
            sweep(); sweep();
            const size_t afterFirst = app.msgLog.size();
            check(afterFirst > before4, "C4 the first row of an open is reported");
            // A SECOND row arriving in the SAME batch, with a different name.
            const std::string other = "\xE0\xB8\x82" "2.npy";   // U+0E02, also unasked
            mkDoc(other, 109, 0);
            app.imagesRev++;
            sweep(); sweep();
            const std::vector<std::string> got4 = linesSince(afterFirst);
            for (const std::string& l : got4) say("msg: " + l);
            check(got4.size() == 1 && got4[0].find(other) != std::string::npos,
                  "C4 ...and so is a NEW row that arrives in the same batch "
                  "(a per-batch flag would have silenced it)");
        }

        // ---- A STACK IS NOT REPORTED WHILE IT IS STILL LOADING -----------
        // The settle rule used to be "the size is the same as it was last
        // sweep", which rode on a universal that is false: that every
        // `imagesRev++` comes with a push. Plenty of sites bump the counter
        // with no push - a grouping invalidation, a series edit, an export, a
        // modal closing - so one of those landing mid-load made two sweeps see
        // the same size and
        // the batch was reported with the frames that had arrived so far. A
        // 400-frame stack said "N of 4" and was never corrected, which is the
        // exact failure the comment above the rule says it exists to prevent
        // (review round 7 P2).
        //
        // The fixture is the mechanism: push two of six frames, then move the
        // revision counter WITHOUT pushing, which is what those sites do.
        {
            const size_t before7 = app.msgLog.size();
            App::SeqInfo si4;
            si4.id = app.nextSeqId++;
            si4.name = thai;                  // the stack's own name is undrawable
            si4.expectedFrames = 6;           // what the loader wrote down
            app.seqs.push_back(si4);
            for (int k = 0; k < 2; k++) {
                mkDoc("part_" + std::to_string(k) + ".npy", 130, si4.id);
                app.imagesRev++;
            }
            sweep();
            app.imagesRev++;                  // a bump with no push, twice
            sweep();
            app.imagesRev++;
            sweep();
            check(linesSince(before7).empty(),
                  "C4 a stack with 2 of its 6 frames says NOTHING yet, however "
                  "often the revision counter moves without a push");
            for (int k = 2; k < 6; k++) {
                mkDoc("part_" + std::to_string(k) + ".npy", 130, si4.id);
                app.imagesRev++;
            }
            sweep(); sweep();
            const std::vector<std::string> got7 = linesSince(before7);
            for (const std::string& l : got7) say("msg: " + l);
            check(got7.size() == 1,
                  "C4 ...and exactly ONE line once all six have arrived");
            check(!got7.empty() && got7[0].find(thai) != std::string::npos,
                  "C4 ...naming the stack");
        }
        // ---- THE PROGRAM MUST NOT MAKE A NAME THAT IS NOT UTF-8 ----------
        // §8c says, flatly, that bytes which are not UTF-8 are a property of
        // the name ON DISK, and offers no remedy because none exists for a
        // file somebody else named. mergedStackName() cut the common prefix of
        // the merged group names at a BYTE, so two folders whose names share
        // the first byte of a three-byte character produced a stack name that
        // ended mid-sequence - a '?' on a row, reported with the one sentence
        // that blames the file system, about a name this program
        // had just built (review round 6 P3).
        //
        // Asserted as an exact string rather than through the tally: a tally
        // answers 0 for everything when no atlas is baked, which would make
        // this pass on a machine with no font for the wrong reason.
        {
            std::vector<App::PendingGroup> sel(2);
            sel[0].name = "abcd\xE6\xB8\xAC";          // abcd + U+6E2C
            sel[1].name = "abcd\xE6\xB9\xAF";          // abcd + U+6E6F
            const std::string nm = mergedStackName(sel);
            say("merged stack name: " + nm);
            check(nm == "abcd (merged)",
                  "C4 a merged stack name is cut at a CHARACTER, not at a byte "
                  "(the shared lead byte of two different characters is not a "
                  "prefix of either)");
            // ...and when nothing whole survives, the root folder's name takes
            // over - the branch that was already there, reached now by a prefix
            // that was trimmed rather than by one that was short to begin with.
            const std::string rootWas = app.folderPickRoot;
            app.folderPickRoot = "C:/data/run7";
            sel[0].name = "\xE6\xB8\xAC";
            sel[1].name = "\xE6\xB9\xAF";
            const std::string nm2 = mergedStackName(sel);
            say("merged stack name (no whole prefix): " + nm2);
            check(nm2 == "run7 (merged)",
                  "C4 ...and a prefix with no whole character in it falls back "
                  "to the root folder's name");
            app.folderPickRoot = rootWas;
        }
        // ---- THE SUMMARY LINE, which nothing had ever produced ------------
        // `hits.size() > 5` was unreachable from every selftest, so the
        // sentence "N of the M item(s) just opened" was never built - including
        // by the round-5 change that fixed its denominator. A fix with no test
        // is a decision nobody can see (review round 6).
        //
        // SIX loose rows in one batch, which is the shape the cap exists for
        // (a stack is one line however many frames it has).
        {
            const size_t before5 = app.msgLog.size();
            for (int k = 0; k < 6; k++) { mkDoc(thai, 120, 0); app.imagesRev++; }
            sweep(); sweep();
            const std::vector<std::string> got5 = linesSince(before5);
            for (const std::string& l : got5) say("msg: " + l);
            check(got5.size() == 1,
                  "C4 more than five affected rows in one open collapse to ONE "
                  "summary line");
            check(!got5.empty() &&
                      got5[0].find("6 of the 6 item(s) just opened") !=
                          std::string::npos,
                  "C4 ...which says 6 of the 6 item(s) just opened");
            // ...AND THE DENOMINATOR IS THIS OPEN, not the batch. batchReuse()
            // hands the same batch id back when a folder is opened again, so a
            // second gesture into batch 120 must say "of the 7", not "of the
            // 13". This is the sentence round 5's P2 fix is about.
            const size_t before6 = app.msgLog.size();
            for (int k = 0; k < 6; k++) { mkDoc(thai, 120, 0); app.imagesRev++; }
            mkDoc("plain_again.npy", 120, 0); app.imagesRev++;
            sweep(); sweep();
            const std::vector<std::string> got6 = linesSince(before6);
            for (const std::string& l : got6) say("msg: " + l);
            check(got6.size() == 1 &&
                      got6[0].find("6 of the 7 item(s) just opened") !=
                          std::string::npos,
                  "C4 ...and a second gesture into the same batch counts only "
                  "the rows IT added (6 of the 7, not 6 of the 13)");
        }
        // ---- A CLEAN FOLDER SAYS NOTHING (review, new critical 1) --------
        // THE DEFECT THIS FIXES: the tooltip's tally was computed over the
        // whole concatenated tooltip, which is multi-line - and the atlas has
        // no glyph for U+000A because the ranges start at U+0020. So EVERY row
        // in the panel, plain ASCII ones included, grew a "1 character here
        // draws as '?'" that was false. A warning on everything is the same as
        // no warning at all, and worse than silence because it is wrong.
        {
            const size_t before3 = app.msgLog.size();
            for (int k = 0; k < 5; k++) {
                mkDoc("plain_" + std::to_string(k) + ".npy", 104, 0);
                app.imagesRev++;
            }
            sweep(); sweep();
            check(linesSince(before3).empty(),
                  "C4 an all-ASCII open leaves NO line at all");
            // ...and the tooltip's own shape: the note for a multi-line block
            // whose visible characters are all drawable must be empty, which is
            // what the newline rule buys.
            check(undrawableNote("C:/data/frame_000.npy\n256x256 1ch u16").empty(),
                  "C4 ...and a multi-line tooltip body full of drawable "
                  "characters is not reported either");
            check(undrawableNote("a\tb\r\nc").empty(),
                  "C4 ...nor are tabs and carriage returns");
        }
        app.images.clear();
        app.seqs.clear();
        app.imagesRev = revWas;
        app.msgLog.resize(logWas);
    }

    // ======================================================================
    //  C5 - THE SEARCH, THE OVERRIDE, AND A REFUSAL THAT CAN BE ACTED ON
    // ======================================================================
    {
        // Its own directory, for --settings-selftest's recorded reason: the
        // fixtures here are FONT files and a settings.jsonc, and anything
        // written into the shared VIEWER_TEST_HOME is read by every test that
        // runs after it and by every later run of the suite.
        std::error_code ec0;
        const std::filesystem::path root0 =
            std::filesystem::temp_directory_path(ec0) / "viewer_cjkfont_fonts";
        std::filesystem::remove_all(root0, ec0);
        std::filesystem::create_directories(root0, ec0);
        // ---- THE OVERRIDE IS CONSULTED BEFORE THE CANDIDATES ---------------
        // This used to hand jpFontChoice the viewer BINARY and assert that it
        // was accepted, which pinned the P1 defect as the specification: a
        // file that is not a font was "usable", and a start-up that took it
        // died with SIGSEGV inside ImFontAtlas::Build(). The intent was only
        // ever "the override is looked at first", so that is what is asserted,
        // with a REAL font - and the strongest form available: a font that is
        // NOT the one the search would have picked on its own must win.
        std::error_code ec;
        std::vector<std::string> realFonts;
        for (const std::string& cand : jpFontCandidates())
            if (jpFontChoice(cand).path == cand) realFonts.push_back(cand);
        const std::string wouldPick = jpFontChoice(std::string()).path;
        say("usable candidates on this machine: " + std::to_string(realFonts.size()) +
            "; the search on its own picks " +
            (wouldPick.empty() ? std::string("(nothing)") : wouldPick));
        if (realFonts.size() >= 2) {
            // The LAST usable candidate: the search alone would never return
            // it, so taking it proves precedence rather than coincidence.
            const std::string other = realFonts.back();
            const FontChoice c = jpFontChoice(other);
            check(c.path == other && std::string(c.how) == "appearance.fontPath" &&
                  other != wouldPick,
                  "C5 the override beats the candidate the search would pick");
        } else if (realFonts.size() == 1) {
            const FontChoice c = jpFontChoice(realFonts[0]);
            check(c.path == realFonts[0] &&
                  std::string(c.how) == "appearance.fontPath",
                  "C5 the override is consulted first (one usable font here, so "
                  "precedence over a SECOND one cannot be shown)");
        } else {
            say("no usable font in the candidate list on this machine, so C5's "
                "positive half cannot run - the negatives below did.");
        }
        // ...and when it misses it is still NAMED, so the refusal says "you
        // asked for this and it is not there" instead of listing four paths the
        // user never mentioned.
        const std::string nope = "/definitely/not/here/NoSuchFont.ttf";
        const FontChoice miss = jpFontChoice(nope);
        check(!miss.tried.empty() && miss.tried[0] == nope,
              "C5 an override that misses is the first path in the record");
        check(miss.path != nope, "C5 ...and is not used");
        // ---- A FILE THAT IS NOT A FONT (review P1) -------------------------
        // THE ONE THAT CRASHED. AddFontFromFileTTF only reads bytes, so it
        // returns non-null for a text file and the `if (!jp)` fallback never
        // runs; the process then dies inside Build(), before openCrashFile()
        // and the SIGSEGV handler exist, so there is not even a record.
        // Measured before this was fixed: `viewer --no-window` exited 139 with
        // nothing printed after the "ui scale:" line.
        {
            const std::string txt = (root0 / "not-a-font.txt").u8string();
            {
                std::ofstream f(pathFromUtf8(txt), std::ios::binary);
                const std::string pad(400, 'x');   // over the >100 byte assert
                f << "this is not a font\n" << pad;
            }
            check(!looksLikeSfnt(txt),
                  "C5 a text file is not accepted as a font");
            const FontChoice c = jpFontChoice(txt);
            check(c.path != txt, "C5 ...so the override does not take it");
            bool named = false;
            for (const std::string& t : c.tried)
                if (t.find("not-a-font.txt") != std::string::npos &&
                    t.find("unusable: it has no sfnt signature") != std::string::npos)
                    named = true;
            check(named,
                  "C5 ...and the record says WHY, not just that it was tried");
            // ...and the search still ends somewhere usable rather than being
            // poisoned by the bad override.
            check(c.path == wouldPick,
                  "C5 ...and the search carries on to what it would have picked");
            // ---- THE SHAPES THAT CRASHED, EVERY ONE OF THEM ----------------
            // MEASURED on the build before the gate, with these exact files:
            //   4 bytes "00 01 00 00" ............... exit 0 (survived)
            //   8 bytes, numTables = 0xFFFF ......... EXIT 139
            //   first 1 KB of a real arial.ttf ...... EXIT 139
            //   first 64 KB of a real arial.ttf ..... EXIT 139
            //   first 4 KB of a real meiryo.ttc ..... EXIT 139
            // The signature is correct in all five. What kills the process is
            // stbtt__find_table: it reads num_tables at fontstart+4 with no
            // bound at all and then walks 16 bytes per entry, so a TRUNCATED
            // REAL FONT is the worst case - its directory is genuine, every
            // required table is "found", and each one points past the end of
            // the file.
            //
            // THE FIXTURES ARE BUILT HERE, not copied from C:\Windows\Fonts.
            // That decision is deliberate: a test that reads the machine's own
            // fonts asserts something different on each runner (and nothing at
            // all on the Ubuntu one, which has no arial.ttf), while a font this
            // test assembles byte by byte is the same input everywhere. The
            // POSITIVE control matters as much as the negatives - a gate that
            // refused everything would pass every line below - so the same
            // builder makes a complete font that must be ACCEPTED, and the
            // broken ones are that same font, cut.
            auto put16 = [](std::string& b, unsigned v) {
                b += (char)(unsigned char)((v >> 8) & 0xFF);
                b += (char)(unsigned char)(v & 0xFF);
            };
            auto put32 = [](std::string& b, unsigned long v) {
                b += (char)(unsigned char)((v >> 24) & 0xFF);
                b += (char)(unsigned char)((v >> 16) & 0xFF);
                b += (char)(unsigned char)((v >> 8) & 0xFF);
                b += (char)(unsigned char)(v & 0xFF);
            };
            // A minimal sfnt that satisfies every check sfntLooksSane makes:
            // the six tables stb_truetype requires, each wholly inside the
            // file. Their CONTENTS are not fonts - this exercises the gate,
            // which is a bounds check, and nothing here is ever handed to
            // stb_truetype.
            auto buildFont = [&](const std::vector<std::string>& tags) {
                std::string dir, body;
                const size_t bodyAt = 12 + 16 * tags.size();
                for (const std::string& t : tags) {
                    const size_t off = bodyAt + body.size();
                    const std::string content(64, 'T');
                    dir += t;
                    put32(dir, 0);                       // checksum
                    put32(dir, (unsigned long)off);
                    put32(dir, (unsigned long)content.size());
                    body += content;
                }
                std::string out;
                put32(out, 0x00010000ul);
                put16(out, (unsigned)tags.size());
                put16(out, 0); put16(out, 0); put16(out, 0);
                out += dir;
                out += body;
                return out;
            };
            auto writeFile = [&](const char* nm, const std::string& bytes) {
                const std::string fp = (root0 / nm).u8string();
                std::ofstream f(pathFromUtf8(fp), std::ios::binary);
                f.write(bytes.data(), (std::streamsize)bytes.size());
                f.close();
                return fp;
            };
            const std::vector<std::string> kAll =
                { "cmap", "head", "hhea", "hmtx", "glyf", "loca" };
            const std::string whole = buildFont(kAll);
            // (1) THE POSITIVE CONTROL. Without this every "false" below could
            // be a gate that simply says no to everything.
            {
                const std::string fp = writeFile("whole.ttf", whole);
                std::string why;
                check(sfntLooksSane(fp, why),
                      "C5 a complete sfnt with every required table IS accepted" +
                      (why.empty() ? std::string() : " (said: " + why + ")"));
            }
            // (2) ...and the same font, CUT. This is the truncated-real-font
            // shape, which is the one that crashed.
            struct Cut { const char* nm; size_t keep; const char* what; };
            const Cut cuts[] = {
                { "cut_half.ttf",  whole.size() / 2, "cut in half" },
                { "cut_dir.ttf",  12 + 16 * 3,       "cut inside its table directory" },
                { "cut_hdr.ttf",  8,                 "cut inside its header" },
            };
            for (const Cut& cu : cuts) {
                const std::string fp = writeFile(cu.nm, whole.substr(0, cu.keep));
                std::string why;
                const bool sane = sfntLooksSane(fp, why);
                say(std::string(cu.nm) + " -> " + (sane ? "ACCEPTED" : "refused: " + why));
                check(!sane, std::string("C5 a font ") + cu.what + " is refused");
                // ...and the refusal reaches the RECORD, which is the only
                // place the user can read it.
                const FontChoice fc2 = jpFontChoice(fp);
                bool said2 = false;
                for (const std::string& t : fc2.tried)
                    if (t.find(cu.nm) != std::string::npos &&
                        t.find("unusable: ") != std::string::npos) said2 = true;
                check(said2, std::string("C5 ...and the record says why for ") + cu.nm);
                check(fc2.path == wouldPick,
                      std::string("C5 ...and the search carries on past ") + cu.nm);
            }
            // (3) The two hand-written headers from the measurement table.
            {
                std::string four; put32(four, 0x00010000ul);
                const std::string fp = writeFile("four-bytes.ttf", four);
                std::string why;
                check(!sfntLooksSane(fp, why), "C5 a 4-byte file is refused");
                say("four-bytes.ttf -> refused: " + why);
                std::string eight; put32(eight, 0x00010000ul);
                put16(eight, 0xFFFF); put16(eight, 0);
                const std::string fp2 = writeFile("eight-bytes.ttf", eight);
                check(!sfntLooksSane(fp2, why),
                      "C5 ...and so is an 8-byte one (too small for a directory)");
                say("eight-bytes.ttf -> refused: " + why);
                // ...and a file that IS big enough for a header and claims
                // 65535 tables it does not have. This is the one that reaches
                // the directory-size check rather than the size floor, and it
                // is the shape stbtt__find_table walks off the end of.
                std::string liar; put32(liar, 0x00010000ul);
                put16(liar, 0xFFFF); put16(liar, 0); put16(liar, 0); put16(liar, 0);
                liar += std::string(400, 'L');
                const std::string fp3 = writeFile("liar.ttf", liar);
                check(!sfntLooksSane(fp3, why),
                      "C5 ...and one claiming 65535 tables it has no room for");
                say("liar.ttf -> refused: " + why);
                check(why.find("65535") != std::string::npos,
                      "C5 ...with the count it claimed in the reason");
            }
            // (4) numTables = 0, which was the ONLY shape the old test had -
            // and the only one that was already safe.
            {
                std::string z; put32(z, 0x00010000ul);
                put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0);
                z += std::string(400, 'z');
                const std::string fp = writeFile("no-tables.ttf", z);
                std::string why;
                check(!sfntLooksSane(fp, why), "C5 a font declaring no tables is refused");
                say("no-tables.ttf -> refused: " + why);
            }
            // (5) Every required table present EXCEPT the outlines, which is
            // what a CFF2 variable font looks like from here: the gate names it
            // rather than leaving stb_truetype to fail silently in a release
            // build (ImGui's own assert for it is compiled out).
            {
                const std::string fp = writeFile("no-outlines.ttf",
                    buildFont({ "cmap", "head", "hhea", "hmtx" }));
                std::string why;
                check(!sfntLooksSane(fp, why),
                      "C5 a font with no glyf and no 'CFF ' is refused (the CFF2 shape)");
                say("no-outlines.ttf -> refused: " + why);
                check(why.find("CFF2") != std::string::npos,
                      "C5 ...and the reason names CFF2, which is what it will be");
            }
            // (6) glyf without loca - stbtt requires the pair.
            {
                const std::string fp = writeFile("glyf-no-loca.ttf",
                    buildFont({ "cmap", "head", "hhea", "hmtx", "glyf" }));
                std::string why;
                check(!sfntLooksSane(fp, why), "C5 glyf without loca is refused");
                say("glyf-no-loca.ttf -> refused: " + why);
            }
            // (7) A COLLECTION, whole and cut. `ttcf` is what meiryo.ttc is -
            // the file whose first 4 KB crashed the process - and the collection
            // header is a SECOND place a truncated file can point outside
            // itself, before any font's directory is even reached.
            //
            // (In a real .ttc the table offsets are absolute from the start of
            // the FILE, while `whole` was built with offsets from the start of
            // ITSELF. That does not matter to what is under test: the gate is a
            // bounds check, and both readings land inside this file. It is
            // written down so the next reader does not take this fixture for a
            // model of the format.)
            {
                std::string ttc;
                put32(ttc, 0x74746366ul);        // 'ttcf'
                put32(ttc, 0x00010000ul);        // version
                put32(ttc, 1);                   // numFonts
                put32(ttc, 16);                  // offset of font 0
                ttc += whole;
                const std::string fp = writeFile("coll.ttc", ttc);
                std::string why;
                check(sfntLooksSane(fp, why),
                      "C5 a well-formed collection is accepted" +
                      (why.empty() ? std::string() : " (said: " + why + ")"));
                // Cut to HALF, so the cut is real: the whole thing is only a
                // few hundred bytes, and a fixed 2048 would have cut nothing
                // at all and asserted the opposite of what it said.
                const std::string fp2 = writeFile("coll-cut.ttc",
                                                  ttc.substr(0, ttc.size() / 2));
                check(!sfntLooksSane(fp2, why), "C5 ...and a cut one is refused");
                say("coll-cut.ttc -> refused: " + why);
                // ...and a collection whose own header points past the end,
                // which is the check that happens before any directory.
                std::string bad2;
                put32(bad2, 0x74746366ul);
                put32(bad2, 0x00010000ul);
                put32(bad2, 4);                  // claims four fonts
                put32(bad2, 0x7FFFFFFFul);       // ...the first of them out there
                bad2 += std::string(64, 'c');
                const std::string fp3 = writeFile("coll-liar.ttc", bad2);
                check(!sfntLooksSane(fp3, why),
                      "C5 ...and one whose font offsets point outside the file");
                say("coll-liar.ttc -> refused: " + why);
            }
            // The sentence a failure produces names the file and says what the
            // user is looking at instead.
            FontChoice bad;
            bad.path = (root0 / "cut_half.ttf").u8string();
            bad.how = "appearance.fontPath";
            const std::string unread = cjkFontUnreadableText(bad);
            say("unreadable: " + unread);
            check(unread.find("cut_half.ttf") != std::string::npos &&
                  unread.find("built-in font") != std::string::npos &&
                  unread.find("appearance.fontPath") != std::string::npos,
                  "C5 ...and the message names the file, the fallback and the way out");
        }
        // ---- the Chinese face taken as PRIMARY (review P2-8) ---------------
        // The merge candidates were only ever reachable as the SECOND face, so
        // an English Windows with no Japanese pack had YaHei installed, unused,
        // and not even listed in the refusal. It is taken now, and it is said.
        {
            bool inPrimary = false;
            for (const std::string& m : cjkMergeCandidates()) {
                const FontChoice c = jpFontChoice(std::string());
                for (const std::string& t : c.tried) if (t == m) inPrimary = true;
                // Only reachable when no Japanese face exists, which is not
                // this machine - so what is asserted is the LIST, not the pick.
                (void)c;
            }
            const std::vector<std::string> mc = cjkMergeCandidates();
            say("Chinese faces this platform knows: " + std::to_string(mc.size()) +
                (inPrimary ? " (reached as primary here)" : " (not reached here: a "
                 "Japanese face was found first, which is the ordinary case)"));
            FontChoice cn;
            cn.path = mc.empty() ? "/some/Chinese.ttc" : mc[0];
            cn.how = "Chinese face (no Japanese face on this machine)";
            const std::string fb = cjkFontChineseFallbackText(cn);
            say("chinese-fallback: " + fb);
            check(fb.find(cn.path) != std::string::npos &&
                  fb.find("Chinese glyph shapes") != std::string::npos &&
                  fb.find("appearance.fontPath") != std::string::npos,
                  "C5 the Chinese-primary sentence names the font and what it costs");
            // ...and it is a DIFFERENT sentence from the three compared here,
            // or they would be one message wearing several hats. Three of the
            // pairs, which is why C7 compares ALL of them pairwise - that is
            // the assertion that keeps this honest as messages are added.
            FontChoice any; any.path = "/x.ttc"; any.how = "built-in candidate";
            check(cjkFontMissingText(any) != cjkFontNoGlyphsText(any) &&
                  cjkFontNoGlyphsText(any) != cjkFontUnreadableText(any) &&
                  cjkFontUnreadableText(any) != cjkFontChineseFallbackText(any),
                  "C5 ...and these four font failures are four different sentences");
        }
        // The refusal carries every path that was looked at. Built from a
        // synthetic choice so the assertion does not depend on which paths
        // happen to exist here.
        FontChoice fake;
        fake.tried = { "/a/one.ttf", "/b/two.ttc", "/c/three.otf" };
        const std::string refusal = cjkFontMissingText(fake);
        say("refusal: " + refusal);
        bool allThere = true;
        for (const std::string& t : fake.tried)
            if (refusal.find(t) == std::string::npos) allThere = false;
        check(allThere,
              "C5 the refusal lists every path it looked at, not just 'not found'");
        check(refusal.find("appearance.fontPath") != std::string::npos,
              "C5 ...and names the key that is the way out");
        // The OTHER failure, which used to be silent: a font was found and has
        // no CJK in it. fc-match answers sans:lang=ja with DejaVu Sans on a
        // machine with no Japanese font, so this sentence is reachable.
        FontChoice found;
        found.path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        found.how = "fc-match";
        const std::string noGlyphs = cjkFontNoGlyphsText(found);
        check(noGlyphs.find(found.path) != std::string::npos &&
              noGlyphs.find("fc-match") != std::string::npos &&
              noGlyphs.find("appearance.fontPath") != std::string::npos,
              "C5 ...and 'the font has no CJK in it' names the font and the way out");
        // The candidate list is not empty on any platform this builds for, or
        // the refusal above would have nothing to print.
        //
        // NOT-EMPTY, never a count. This used to read `>= 3` and that number was
        // the length of the OLD macOS list: giving every candidate an explicit
        // ja/zh tag moved Hiragino Sans GB to the Chinese side - which is the
        // point of the tag, and the reason the Chinese-primary sentence can be
        // raised at all - leaving two Japanese rows there and this assert red on
        // macOS only. A threshold is the wrong shape for what the comment above
        // actually claims, and re-pinning it at 2 would fail again the next time
        // the table is re-sorted for a good reason.
        check(!jpFontCandidates().empty(),
              "C5 this platform has a candidate list to look through");
        std::filesystem::remove_all(root0, ec0);    // the fixtures are this test's
    }

    // ---- the settings key ---------------------------------------------------
    // Its own temp directory, for --settings-selftest's recorded reason: a
    // settings.jsonc written into the shared VIEWER_TEST_HOME is read by every
    // test that runs after it, and by every later run of the suite.
    {
        const std::string keptPath = g_settingsFontPath;
        const PrefsBase keptBase = g_prefsBase;
        std::error_code ec;
        std::filesystem::path root =
            std::filesystem::temp_directory_path(ec) / "viewer_cjkfont_selftest";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        auto writeFix = [&](const std::string& body, const char* nm) {
            std::string p = (root / nm).u8string();
            std::ofstream f(pathFromUtf8(p), std::ios::binary);
            f.write(body.data(), (std::streamsize)body.size());
            return p;
        };
        g_settingsFontPath.clear();
        g_prefsBase = PrefsBase{};
        {
            const std::string p = writeFix(
                "{\n  \"appearance\": {\n    \"fontPath\": \"/opt/fonts/Mine.ttc\",\n  },\n}\n",
                "ok.jsonc");
            SettingsReport r = loadSettingsFrom(p);
            check(r.parsed && r.rejected == 0 &&
                  g_settingsFontPath == "/opt/fonts/Mine.ttc",
                  "C5 appearance.fontPath in settings.jsonc lands");
        }
        {
            // "" is REFUSED BY NAME rather than taken as "search the usual
            // places": "" is how a key is spelled when somebody meant to type a
            // path and did not, and 判断9's way back is to delete the key. The
            // refusal has to carry the LINE AND COLUMN and the key's own path,
            // or it points at nothing. The key is on line 3 at column 5, which
            // is where this file's other refusals point too (R13 in
            // --settings-selftest asserts the same shape).
            g_prefsBase.fontPath = "/from/prefs.ttc";
            const std::string p = writeFix(
                "{\n  \"appearance\": {\n    \"fontPath\": \"\",\n  },\n}\n",
                "empty.jsonc");
            SettingsReport r = loadSettingsFrom(p);
            bool named = false;
            for (const std::string& n : r.notes) {
                say("note: " + n);
                if (n.find("appearance.fontPath") != std::string::npos &&
                    n.find(":3:5:") != std::string::npos)
                    named = true;
            }
            check(r.rejected == 1 && named,
                  "C5 an empty path is refused, naming line 3 column 5 and the key");
            // ...and the refusal is a SKIP: the layer under the key stands, so
            // a bad line in settings.jsonc does not also throw away the machine's
            // own prefs.txt answer.
            check(g_settingsFontPath == "/from/prefs.ttc",
                  "C5 ...and the prefs.txt layer under the refused key stands");
            g_prefsBase.fontPath.clear();
        }
        {
            // A number is the other refusal shape, and it is the type error
            // rather than the range one.
            g_settingsFontPath.clear();
            const std::string p = writeFix(
                "{\n  \"appearance\": {\n    \"fontPath\": 7,\n  },\n}\n", "num.jsonc");
            SettingsReport r = loadSettingsFrom(p);
            bool named = false;
            for (const std::string& n : r.notes)
                if (n.find("appearance.fontPath") != std::string::npos &&
                    n.find("a string") != std::string::npos)
                    named = true;
            check(r.rejected == 1 && named && g_settingsFontPath.empty(),
                  "C5 ...and a number is refused as 'wants a string'");
        }
        // The prefs.txt layer under it: a value there is what settings.jsonc
        // falls back to when the file stops naming the key (phase④'s shape).
        {
            g_prefsBase.fontPath = "/from/prefs.ttc";
            g_settingsFontPath = "/stale";
            SettingsReport r = loadSettingsFrom((root / "nothing-here.jsonc").u8string());
            (void)r;
            check(g_settingsFontPath == "/from/prefs.ttc",
                  "C5 with no settings.jsonc the prefs.txt layer is what is in force");
        }
        // ...and the sentence the Preferences row owes. It is said ONCE, when
        // the box is left, not once per character: ImGui::InputText returns
        // true on every keystroke, so a path typed by hand used to be worth one
        // toast and one Messages line per letter - thirty lines about one
        // change, which is the log made useless by the very thing meant to make
        // the change visible. The panel sets g_prefTextMidEdit around its
        // commit and says the sentence on IsItemDeactivatedAfterEdit; what is
        // asserted here is the half that is not ImGui - that the flag silences
        // it and that clearing the flag does not.
        {
            const std::string toastWas = app.toast;
            const size_t logWas2 = app.msgLog.size();
            g_settingsFontPath = "/some/font.ttc";
            app.toast.clear();
            g_prefTextMidEdit = true;
            prefAfterChange(SK_fontPath);
            g_prefTextMidEdit = false;
            check(app.toast.empty(),
                  "C5 mid-edit the 'next start' sentence is not said at all");
            prefAfterChange(SK_fontPath);
            check(app.toast.find("next start") != std::string::npos &&
                  app.toast.find("/some/font.ttc") != std::string::npos,
                  "C5 ...and once the edit is over it is said, naming the font");
            // Empty means "back to the built-in search", which is a different
            // sentence and not the absence of one.
            g_settingsFontPath.clear();
            app.toast.clear();
            prefAfterChange(SK_fontPath);
            check(app.toast.find("built-in search") != std::string::npos &&
                  app.toast.find("next start") != std::string::npos,
                  "C5 ...and clearing it says so, also at the next start");
            app.toast = toastWas;
            app.msgLog.resize(logWas2);
        }
        // Given back at the ONE exit of this group, because every selftest
        // shares one home and a borrowed setting outlives the process.
        g_settingsFontPath = keptPath;
        g_prefsBase = keptBase;
        std::filesystem::remove_all(root, ec);
    }

    // ======================================================================
    //  C6 - A NAME THAT IS NOT UTF-8 IS A NAME, NOT A CRASH (板306 E)
    //
    //  std::filesystem::u8path THROWS on an illegal byte sequence, measured
    //  with this toolchain, and pathFromUtf8 is called from ~40 places none of
    //  which is inside a try. Where such bytes COME from is four separate
    //  defects and its own change; that the process survives them is this one.
    // ======================================================================
    {
        const int badWas = pathFromUtf8BadCount();
        int replaced = 0;
        check(utf8Sanitized("plain.npy", &replaced) == "plain.npy" && replaced == 0,
              "C6 well-formed UTF-8 is returned untouched");
        // 測 is three well-formed bytes; keeping them proves the sanitiser is
        // not simply throwing away everything above ASCII.
        check(utf8Sanitized("\xE6\xB8\xAC.npy", &replaced) == "\xE6\xB8\xAC.npy" &&
              replaced == 0,
              "C6 ...and so is a three-byte kanji");
        utf8Sanitized("a\xFF\xFE" "b", &replaced);
        check(replaced == 2, "C6 two stray bytes become two U+FFFD");
        utf8Sanitized("\xE6\xB8", &replaced);
        check(replaced == 2,
              "C6 ...and a truncated sequence is replaced byte by byte");
        utf8Sanitized("\xC0\xAF", &replaced);
        check(replaced == 2, "C6 ...and an overlong form is not a character");
        utf8Sanitized("\xED\xA0\x80", &replaced);
        check(replaced == 3, "C6 ...and neither is an encoded surrogate");
        // THE ONE THAT USED TO TERMINATE THE PROCESS. The claim that holds on
        // EVERY platform is "it does not throw"; whether it had anything to
        // recover from is a property of the platform, and both halves are said
        // out loud rather than assumed:
        //   - Windows converts to UTF-16, so illegal bytes have no code unit to
        //     become and u8path throws. That is the crash, and here the count
        //     has to move and the bytes have to be kept for the report.
        //   - POSIX paths ARE bytes. The conversion cannot fail, and it must
        //     not be made to: a Linux file whose name is not UTF-8 is a file
        //     you can still open, and sanitising it here would make it
        //     unopenable. Nothing to count, which is the correct outcome.
        bool threw = false;
        std::filesystem::path got;
        try { got = pathFromUtf8("a\xFF\xFE" "b.npy"); }
        catch (...) { threw = true; }
        check(!threw, "C6 pathFromUtf8 does not throw on bytes that are not UTF-8");
        const int moved = pathFromUtf8BadCount() - badWas;
        check(moved == 0 || moved == 1,
              "C6 ...and it either converted them or counted them, exactly once");
        if (moved == 1) {
            say("this platform cannot convert those bytes (Windows: UTF-16), so "
                "the substitution happened and was recorded.");
            check(pathFromUtf8LastBad() == "a\xFF\xFE" "b.npy",
                  "C6 ...and the bytes it could not read are kept, for the report");
            // A path built from replacement characters names no file, which is
            // the right answer: every caller goes on to exists() / ifstream and
            // gets a clean "no" it can report instead of a std::terminate.
            std::error_code ec;
            check(!std::filesystem::exists(got, ec),
                  "C6 ...and the path it hands back matches no file");
        } else {
            say("this platform's paths are bytes (POSIX), so the conversion "
                "cannot fail and nothing was replaced - which is right: such a "
                "file can still be opened by its real name.");
            check(!got.empty(), "C6 ...and the name came through as it was given");
        }
        // The ordinary path is not disturbed - the try/catch must not have
        // become the normal route.
        check(pathFromUtf8("plain.npy").u8string() == "plain.npy" &&
              pathFromUtf8BadCount() == badWas + moved,
              "C6 a well-formed name still converts, and counts nothing");
    }

    // ======================================================================
    //  C7 - THE BREADCRUMB, AND THE RECOVERY IT BACKS UP
    //
    //  WHY THIS EXISTS AT ALL. The gate above refuses every shape of broken
    //  font that has been measured, and it still cannot make stb_truetype
    //  safe: past the table directory it follows offsets inside cmap subtables
    //  and inside loca/glyf that it has no bound for. Each check narrows the
    //  hole; none closes it.
    //
    //  What closes the CONSEQUENCE is this: the path about to be baked is
    //  written down first and erased after, so a start that did not come back
    //  leaves a note, and the next start does not repeat it. Without it the
    //  measured failure was permanent - appearance.fontPath persists, there is
    //  no command-line flag that overrides it, so the only way back was to edit
    //  a settings file with an application that would not open.
    //
    //  The same discipline is already in this repo, a few hundred lines from
    //  the breadcrumb: the autosave slot lock, whose comment says "A CRASH
    //  skips it on purpose: the stale lock left behind is what marks that
    //  autosave reclaimable."
    // ======================================================================
    {
        // The crumb lives in the CONFIG directory, which for a selftest is the
        // shared VIEWER_TEST_HOME - so whatever this group writes there has to
        // be gone by the time it returns, or every later test in the suite
        // starts skipping a font.
        const std::vector<std::string> crumbWas = fontBakeCrumbRead();
        fontBakeCrumbClear();
        const std::string real = jpFontChoice(std::string()).path;
        if (real.empty()) {
            // AND NOTHING ELSE IN THIS GROUP RUNS, which is what this used to
            // deny: it said "the file half below still ran", and every line of
            // C7 - the breadcrumb format, the pid rules, the accounting, the
            // convergence loop - is inside the `else`. The gate above it (C6)
            // is the part that needs no font. Said accurately because a skip
            // message that overstates what ran is a skip message that hides a
            // hole (review round 6 P3).
            say("no usable font on this machine, so the whole of C7 is skipped: "
                "every check in it needs a real font file to name. The font "
                "GATE's own cases (C6) do not and did run.");
        } else {
            // (1) WITH NO CRUMB the override is used. Asserted first and in the
            // same group on purpose: an implementation that skipped the
            // override forever would satisfy every line in (2).
            const FontChoice ok1 = jpFontChoice(real, fontBakeCrumbRead());
            check(ok1.path == real && !ok1.skippedAfterCrash,
                  "C7 with no breadcrumb the override is used");
            // (2) WITH A CRUMB naming it, it is passed over, the record says
            // why, and the search carries on to something usable.
            //
            // THE LINE HAS TO COME FROM A DEAD PROCESS. A breadcrumb line whose
            // writer is still running means "that process is baking right now",
            // which says nothing about whether the font is safe - so this
            // process's own line is deliberately not evidence, and writing one
            // with fontBakeCrumbWrite() here would prove nothing. The test
            // writes the line itself, in the format the reader parses, under a
            // pid it has CHECKED is gone.
            unsigned long deadPid = 0;
            for (unsigned long cand = 0x7FFFFFF0ul; cand > 0x7FFF0000ul; cand -= 7ul)
                if (!pidAlive(cand)) { deadPid = cand; break; }
            check(deadPid != 0, "C7 a pid that is not running could be found");
            // After the check, not before it: the scan's sentinel is 0, and
            // "using pid 0 as a process that is gone" is a sentence about a
            // scan that found nothing (review round 6, same shape as the live
            // pid below).
            if (deadPid)
                say("using pid " + std::to_string(deadPid) + " as a process that is gone");
            auto writeRawCrumb = [&](const std::string& body) {
                std::ofstream f(pathFromUtf8(fontBakeCrumbPath()),
                                std::ios::binary | std::ios::trunc);
                f << body;
            };
            auto readRawCrumb = [&]() {
                std::ifstream f(pathFromUtf8(fontBakeCrumbPath()), std::ios::binary);
                std::ostringstream os; os << f.rdbuf();
                return os.str();
            };
            // ...AND A LIVE PID THAT IS NOT OURS. The two properties that make
            // this design different from "truncate the file and write your own
            // line" are both about ANOTHER process's line, and until this round
            // every check here used currentPid() for "alive" - which takes the
            // `who == me` arm of `who == me || pidAlive(who)` and never reaches
            // pidAlive() at all. Deleting pidAlive() from that condition passed
            // the whole group.
            //
            // Found by asking, not assumed: pid 4 on Windows (System, which
            // answers ERROR_ACCESS_DENIED and so counts as alive) and pid 1 on
            // Linux and macOS (init / launchd, EPERM) are hit on the first few
            // candidates, and the parent is tried first where it can be asked
            // for. If none can be found the two lines below say so and are
            // skipped rather than asserting something about this machine.
            unsigned long livePid = 0;
#if !defined(_WIN32)
            if (const unsigned long pp = (unsigned long)getppid();
                pp && pp != currentPid() && pidAlive(pp)) livePid = pp;
#endif
            for (unsigned long cand = 1; !livePid && cand < 70000ul; cand++)
                if (cand != currentPid() && pidAlive(cand)) livePid = cand;
            // ...and SAY WHAT HAPPENED, not what was hoped for. This used to
            // print "using pid 0 as a process that is NOT us and is running"
            // when the scan found nothing, contradicting the skip message 20
            // lines below - a scan's empty answer has to read as an empty
            // answer or the log argues with itself (review round 6).
            if (livePid) say("using pid " + std::to_string(livePid) +
                             " as a process that is NOT us and is running");
            else         say("no live pid other than our own could be found by "
                             "the scan, so the two ANOTHER-PROCESS checks are "
                             "skipped below rather than asserted");
            writeRawCrumb(std::to_string(deadPid) + " " + real + "\n");
            const std::vector<std::string> back = fontBakeCrumbRead();
            check(back.size() == 1 && back[0] == real,
                  "C7 a line from a process that is GONE is read as evidence");
            // ...and the other direction, which is the half two windows need:
            // a line from a process that is STILL RUNNING is not evidence. Our
            // own pid is the one live pid a test can name for certain.
            writeRawCrumb(std::to_string(currentPid()) + " " + real + "\n");
            check(fontBakeCrumbRead().empty(),
                  "C7 ...while a line from a LIVE process is not (a second "
                  "window mid-bake must not disable this one's font)");
            // ...and clearing takes OUR lines only. B finishing its bake used
            // to delete the whole file, which threw away A's in-flight record.
            writeRawCrumb(std::to_string(currentPid()) + " " + real + "\n" +
                          std::to_string(deadPid) + " /someone/elses.ttc\n");
            fontBakeCrumbClear();
            const std::vector<std::string> left = fontBakeCrumbRead();
            check(left.size() == 1 && left[0] == "/someone/elses.ttc",
                  "C7 ...and clearing removes only this process's lines");
            if (!livePid) {
                say("no second live pid could be named on this machine, so the "
                    "two ANOTHER-PROCESS properties below cannot be shown here "
                    "- everything else in C7 did run.");
            } else {
                // ---- (2a) A LIVE LINE IS NOT EVIDENCE, and the line that
                // makes it so is `pidAlive(who)` in fontBakeCrumbRead. The
                // check above this one used OUR pid, which short-circuits on
                // `who == me`; this one cannot be satisfied that way. Without
                // it, a second window mid-bake disables this one's font and
                // the suite stays green.
                writeRawCrumb(std::to_string(livePid) + " " + real + "\n");
                check(fontBakeCrumbRead().empty(),
                      "C7 ...and a line from ANOTHER process that is still "
                      "running is not evidence either");
                // ...and the dead/alive pair, on the SAME shape, so the line
                // above cannot pass by reading nothing at all.
                writeRawCrumb(std::to_string(deadPid) + " " + real + "\n");
                check(fontBakeCrumbRead().size() == 1,
                      "C7 ...while the same line under a DEAD pid is");
                // ---- (2b) ANOTHER PROCESS'S LINE SURVIVES OUR WRITE. The
                // write is an append in effect - "A reads its crumb while B is
                // mid-bake" needs B's line to still be there afterwards - and
                // a write that simply truncated the file passed every line in
                // this group before this one existed.
                writeRawCrumb(std::to_string(livePid) + " /someone/elses.ttc\n");
                fontBakeCrumbWrite({ real });
                const std::string raw = readRawCrumb();
                say("crumb after our write: " + raw);
                check(raw.find("/someone/elses.ttc") != std::string::npos,
                      "C7 ...and a live other process's line is STILL THERE "
                      "after this process writes its own (the rewrite is an "
                      "append, not a truncate)");
                check(raw.find(std::to_string(currentPid()) + "#") !=
                          std::string::npos &&
                      raw.find(real) != std::string::npos,
                      "C7 ...alongside this start's own line, which now carries "
                      "the death count it is retrying from");
            }
            // ---- (3) A START THAT CHOSE NO FONT AT ALL still consumes the
            // dead line (review round 5 P2). Consumption used to live inside
            // fontBakeCrumbWrite, which returns early on `paths.empty()`, so a
            // start whose breadcrumb named every candidate it had skipped them
            // all, baked nothing, wrote nothing - and left the line. The next
            // start read it, skipped the same paths, and was empty again: the
            // font skipped FOR EVER that the comment on fontBakeCrumbConsume
            // rejects by name, down the one route where no message can offer a
            // way out either.
            //
            // Run in main()'s order for that start: read, choose (nothing),
            // consume, write (nothing).
            writeRawCrumb(std::to_string(deadPid) + " " + real + "\n");
            const std::vector<std::string> ev0 = fontBakeCrumbRead();
            check(ev0.size() == 1 && ev0[0] == real,
                  "C7 a start reads the dead line before it chooses");
            fontBakeCrumbAccount();
            fontBakeCrumbWrite({});           // nothing was chosen: nothing to bake
            check(fontBakeCrumbRead().empty(),
                  "C7 ...and a start that chose NO font accounts it anyway, so "
                  "the start after this one tries the file again");
            check(fontBakeCrumbForever().empty(),
                  "C7 ...and one death is not the permanent kind");
            const FontChoice again = jpFontChoice(real, fontBakeCrumbRead());
            check(again.path == real && !again.skippedAfterCrash,
                  "C7 ...so the next start tries that file again");
            writeRawCrumb(std::to_string(deadPid) + " " + real + "\n");
            const FontChoice sk = jpFontChoice(real, back);
            check(sk.skippedAfterCrash, "C7 ...and the next choice skips that file");
            // ONE claim, not a disjunction. The right-hand side used to be
            // `real == jpFontChoice("", back).path`, which is TRUE precisely
            // when the breadcrumb was ignored and the search returned the
            // skipped font anyway - so the assertion passed in the one case it
            // existed to catch.
            check(sk.path != real,
                  "C7 ...and does not return it as the answer");
            bool said = false;
            for (const std::string& t : sk.tried)
                if (t.find(real) != std::string::npos &&
                    t.find("the last start did not survive baking it") !=
                        std::string::npos) said = true;
            check(said, "C7 ...and the record says that is why");
            // (3) THE FIFTH MESSAGE. Its whole value is the last clause - the
            // way out - because the user's own setting is being overruled and
            // only they can decide what to do about it.
            const std::string m = cjkFontCrashSkipText(sk);
            say("crash-skip: " + m);
            check(m.find(real) != std::string::npos,
                  "C7 the fifth message names the file that was skipped");
            check(m.find("did not survive") != std::string::npos,
                  "C7 ...and says why it was skipped");
            check(m.find("appearance.fontPath") != std::string::npos &&
                  m.find("delete that line") != std::string::npos,
                  "C7 ...and names BOTH ways out: point it elsewhere, or remove it");
            // ...AND THE SECOND FACE'S OWN SENTENCE, which did not exist: a
            // breadcrumb that took the merge face away produced no message at
            // all, so the simplified-Chinese half went missing in silence
            // (review round 7 P3).
            {
                const std::string ms = cjkFontMergeSkippedText({ "/second/face.ttc" }, sk);
                say("merge-skipped: " + ms);
                check(ms.find("/second/face.ttc") != std::string::npos,
                      "C7 the skipped second face is named");
                check(ms.find("simplified-Chinese") != std::string::npos,
                      "C7 ...and what it costs is said");
                check(ms.find("cannot be blamed") != std::string::npos,
                      "C7 ...and so is the thing that cannot be known: both "
                      "faces are read together, so the death is not attributable");
            }
            // ...AND THE OTHER BRANCH OF THAT SENTENCE (review round 5 P3).
            // What was skipped here is the user's own setting, so "delete that
            // line" can be carried out. When a BUILT-IN candidate is what was
            // skipped there is no such line, and only `skippedWasOverride`
            // knows which case it is - so the false side needs its own
            // assertion or the branch can be deleted with the suite green.
            //
            // NOT asserted on "appearance.fontPath": BOTH sentences name it
            // (the built-in one offers it as a way to choose something else),
            // so that substring cannot tell the two apart. The clause that can
            // is the one about whose decision it was.
            {
                FontChoice bi;
                bi.skippedAfterCrash = true;
                bi.skippedWasOverride = false;
                bi.skippedOnce.push_back("/usr/share/fonts/x.ttc");
                bi.tried.push_back("/usr/share/fonts/x.ttc (skipped: the last "
                                   "start did not survive baking it)");
                const std::string bm = cjkFontCrashSkipText(bi);
                say("crash-skip (built-in): " + bm);
                check(bm.find("not something you set") != std::string::npos,
                      "C7 ...and for a BUILT-IN candidate it says so instead");
                check(bm.find("delete that line") == std::string::npos,
                      "C7 ...and does NOT tell the reader to delete a line "
                      "that does not exist");
                check(bm.find("/usr/share/fonts/x.ttc") != std::string::npos,
                      "C7 ...while still naming the file that was skipped");
            }
            // ...and it is a fifth SENTENCE, not one of the four wearing a hat.
            // All six pairs, because three comparisons out of six is not "they
            // are all different".
            FontChoice any; any.path = "/x.ttc"; any.how = "built-in candidate";
            any.skippedOnce.push_back("/x.ttc");
            FontChoice gone = any;
            gone.skippedOnce.clear();
            gone.skippedForever.push_back("/x.ttc");
            FontAtlasCost dropCost; dropCost.mergeDropped = "/y.ttc";
            // EVERY SENTENCE THIS BUILD CAN RAISE AT START-UP, and the two
            // forms of the crash-skip message are two of them: C5 names this
            // matrix as the thing that keeps the set honest "as messages are
            // added", and the sentence added for a file that is finished with
            // was not in it (review round 7 P3). A form is a sentence here, not
            // a function.
            const std::string msgs[8] = {
                cjkFontMissingText(any), cjkFontNoGlyphsText(any),
                cjkFontUnreadableText(any), cjkFontChineseFallbackText(any),
                cjkFontCrashSkipText(any), cjkFontMergeDroppedText(dropCost, any),
                cjkFontCrashSkipText(gone, "/cfg/font-baking"),
                cjkFontMergeSkippedText({ "/y.ttc" }, any),
            };
            int distinct = 0, pairs = 0;
            for (int i = 0; i < 8; i++)
                for (int k = i + 1; k < 8; k++) { pairs++; if (msgs[i] != msgs[k]) distinct++; }
            check(pairs == 28 && distinct == 28,
                  "C7 ...and all " + std::to_string(pairs) +
                  " pairs of the start-up font sentences differ");
            // (4) SURVIVING CLEARS IT, which is what keeps one crash from
            // disabling the override for good. Written as this process's own
            // line - that is what a real bake leaves behind, and what
            // fontBakeCrumbClear() is responsible for - with the death from
            // the group above accounted first, because that is the order
            // main() runs in (read, choose, account, write). Three headings
            // had stacked up here over three rounds, each describing the same
            // four lines; this is one (review round 6 P3).
            fontBakeCrumbAccount();
            fontBakeCrumbWrite({ real });
            fontBakeCrumbClear();
            check(fontBakeCrumbRead().empty(), "C7 a completed bake clears the breadcrumb");
            const FontChoice ok2 = jpFontChoice(real, fontBakeCrumbRead());
            check(ok2.path == real && !ok2.skippedAfterCrash,
                  "C7 ...and the override works again on the next start");
            // ---- (5) IT CONVERGES, and that is the round-6 fix -------------
            // THE DEFECT: the first shape of this DELETED a dead line once a
            // start had acted on it, so the start after that had no record and
            // baked the same file again. On a machine with one usable font:
            // crash, open, crash, open, ... for ever. The comment defending
            // the delete said "at most ONE more crash", which is true of one
            // cycle and silent about the cycle repeating.
            //
            // Run as a loop, because the defect is a PROPERTY OF THE SEQUENCE
            // and no single start can show it. Each iteration is one start in
            // main()'s order - read, decide, account, and (if it chose a file)
            // leave behind the line a crash leaves. 'B' = this start baked the
            // file, 's' = skipped it this time, 'S' = skipped it for good.
            {
                writeRawCrumb("");
                // What a start that died would have left: its in-flight line,
                // under a pid that is gone, carrying the deaths it knew of -
                // which is what fontBakeCrumbWrite absorbs from the record.
                auto die = [&](const std::string& path) {
                    int d = 0;
                    for (const FontBakeLine& L : fontBakeCrumbLines())
                        if (L.who == 0 && L.path == path) d = L.deaths;
                    writeRawCrumb(std::to_string(deadPid) + "#" +
                                  std::to_string(d) + " " + path + "\n");
                };
                std::string pattern;
                for (int start = 1; start <= 6; start++) {
                    const std::vector<std::string> skip = fontBakeCrumbRead();
                    const std::vector<std::string> forever = fontBakeCrumbForever();
                    const bool baking =
                        std::find(skip.begin(), skip.end(), real) == skip.end();
                    pattern += baking ? 'B' : (forever.empty() ? 's' : 'S');
                    fontBakeCrumbAccount();
                    if (baking) die(real);          // ...and it did not come back
                }
                say("six starts on a machine with one usable font: " + pattern);
                check(pattern == "BsBSSS",
                      "C7 a file that kills start-up is retried ONCE and then "
                      "skipped for good - BsBSSS, not the BsBsBs loop that "
                      "deleting the line produced");
                // THE FOURTH LETTER IS 'S' AND NOT 's', and that is round 7's
                // one-off fix: the start that RECORDS the second death is the
                // one the user is sitting in front of, so it has to say "this
                // is finished with" rather than "it will be tried again" about
                // a file no start will ever try again. Forever() used to read
                // the death count off the line while Account() wrote count + 1,
                // so that start promised a retry and the next start silently
                // broke the promise.
                check(pattern.find('s') == 1 && pattern.find('S') == 3,
                      "C7 ...with exactly one 'try it again' start, and the "
                      "start that records the second death saying so itself");
                check(pattern.find('B') < pattern.find('S'),
                      "C7 ...and no start bakes it again after the first "
                      "permanent skip (the sequence converges)");
                // ...and the record is what the next start reads, so it has to
                // still be there after a start that did nothing with it.
                check(fontBakeCrumbForever().size() == 1 &&
                          fontBakeCrumbForever()[0] == real,
                      "C7 ...and the record of the two deaths survives");
                // THE MESSAGE IS THE OTHER HALF. "Skip for ever" was refused
                // the first time because nothing could name a way out of it.
                // The way out is this file, and the sentence says so.
                const FontChoice pf = jpFontChoice(real, fontBakeCrumbRead(),
                                                   fontBakeCrumbForever());
                check(pf.skippedAfterCrash && pf.skippedForever.size() == 1,
                      "C7 ...and the choice knows this skip is the permanent one");
                const std::string pm = cjkFontCrashSkipText(pf, fontBakeCrumbPath());
                say("crash-skip (permanent): " + pm);
                check(pm.find("no longer tried") != std::string::npos,
                      "C7 ...and the message says it is not tried any more");
                check(pm.find(dispPath(fontBakeCrumbPath())) != std::string::npos,
                      "C7 ...and names the record to delete, which is the way "
                      "out the first version could not name");
                check(pm.find(real) != std::string::npos,
                      "C7 ...and still names the file itself");
                // ...and the two forms are not the same sentence.
                FontChoice one = pf;
                one.skippedForever.clear();
                one.skippedOnce.push_back(real);
                check(cjkFontCrashSkipText(one, fontBakeCrumbPath()) != pm,
                      "C7 ...and 'not this time' and 'not again' are different "
                      "sentences");
                // ---- AND A MIXED SEARCH SAYS BOTH, ABOUT THE RIGHT FILE ----
                // One file finished with and one skipped for a single start is
                // the ordinary shape on a machine with several candidates, and
                // the message used to pick ONE sentence for the whole set from
                // `!skippedForever.empty()` - telling the user that a file due
                // to be retried on the next start was never going to be tried
                // again (review round 7 P2).
                FontChoice mix;
                mix.path = "/in/use.ttc";
                mix.skippedAfterCrash = true;
                mix.skippedForever.push_back("/gone/for/good.ttc");
                mix.skippedOnce.push_back("/one/death.ttc");
                const std::string mm = cjkFontCrashSkipText(mix, fontBakeCrumbPath());
                say("crash-skip (mixed): " + mm);
                const size_t atGone = mm.find("/gone/for/good.ttc");
                const size_t atOnce = mm.find("/one/death.ttc");
                const size_t atAgain = mm.find("no longer tried");
                const size_t atRetry = mm.find("tried once more on the next start");
                check(atGone != std::string::npos && atOnce != std::string::npos,
                      "C7 a mixed search names BOTH files");
                check(atAgain != std::string::npos && atRetry != std::string::npos,
                      "C7 ...and makes BOTH statements");
                check(atOnce < atRetry && atGone < atAgain && atRetry < atGone,
                      "C7 ...with each file next to the statement about IT - "
                      "one sentence per path, not one flag for the set");
                // ...and the way out that is only true of the permanent one
                // carries its precondition, because carrying it out on an
                // unreplaced file spends the two crashes again.
                check(mm.find("If you have REPLACED that file") != std::string::npos,
                      "C7 ...and the record-deleting way out says what it "
                      "costs if the file has not been replaced");
                writeRawCrumb("");          // nothing of this is left behind
                // ---- THE RETRY'S RECORD IS ABSORBED, which is what makes a
                // transient failure recoverable and which no test held (review
                // round 7 P3). Deleting the absorb from fontBakeCrumbWrite left
                // the whole suite green and put production back on the
                // every-other-start loop: the record would survive our own
                // line, so surviving the bake would clear our line and leave
                // the `0#1` behind for the next start to count again.
                writeRawCrumb("0#1 " + real + "\n");
                check(fontBakeCrumbRead().empty(),
                      "C7 a record of one death is the retry, not a skip");
                fontBakeCrumbWrite({ real });
                const std::string absorbed = readRawCrumb();
                say("crumb while retrying: " + absorbed);
                // A LINE that starts with `0#`, not the substring: a pid
                // ending in 0 makes "1200#1 " contain "0#1 ".
                check(absorbed.compare(0, 2, "0#") != 0 &&
                          absorbed.find("\n0#") == std::string::npos,
                      "C7 ...and writing the in-flight line TAKES that record "
                      "(it is this attempt's own count now)");
                check(absorbed.find(std::to_string(currentPid()) + "#1 ") !=
                          std::string::npos,
                      "C7 ...carrying the count, so a crash here is the second "
                      "death and not the first");
                fontBakeCrumbClear();
                check(fontBakeCrumbRead().empty() && fontBakeCrumbForever().empty(),
                      "C7 ...and surviving the bake clears the record with it, "
                      "so a transient failure leaves nothing behind");
                writeRawCrumb("");
            }
        }
        // Given back, at the one exit - and the file is REMOVED, not cleared.
        // fontBakeCrumbClear() takes this process's lines only, by design, and
        // a record (pid 0) is nobody's line: a `0#2 <font>` left here would
        // make every later test in this run, and every later RUN, skip that
        // font for good. The suite shares one home, so this group cleans up by
        // deleting the file rather than by asking a function whose contract is
        // narrower than that.
        {
            std::error_code ec;
            std::filesystem::remove(pathFromUtf8(fontBakeCrumbPath()), ec);
        }
        if (!crumbWas.empty()) fontBakeCrumbWrite(crumbWas);
    }

    // ======================================================================
    //  C8 - THE RECOVERY FROM A FAILED Build(), RUN FOR REAL
    //
    //  This is the branch the P1 fix is made of and nothing was exercising it.
    //  It runs LAST because it replaces the live atlas: every group above reads
    //  the one start-up baked, and this one deliberately destroys it.
    //
    //  ONLY WITH NDEBUG. Every path that makes ImFontAtlas::Build() return
    //  false goes through an IM_ASSERT first - "stbtt_InitFont(): failed to
    //  parse FontData" - so on a build with assertions live this would abort
    //  instead of failing, and a test that cannot choose how it dies is worse
    //  than a test that says it is not running. The CI matrix builds Release,
    //  so this group runs there; a developer's default build says so by name.
    // ======================================================================
#if defined(NDEBUG)
    {
        std::error_code ec8;
        const std::filesystem::path r8 =
            std::filesystem::temp_directory_path(ec8) / "viewer_cjkfont_recover";
        std::filesystem::remove_all(r8, ec8);
        std::filesystem::create_directories(r8, ec8);
        // A font that PASSES the gate - every required table, all in bounds -
        // and whose contents are not a font at all. That is precisely the
        // residue the gate cannot catch, so it is the right input for the
        // machinery that exists because the gate cannot catch it.
        std::string b;
        auto p16 = [&](unsigned v) { b += (char)(unsigned char)(v >> 8); b += (char)(unsigned char)(v & 0xFF); };
        auto p32 = [&](unsigned long v) {
            b += (char)(unsigned char)((v >> 24) & 0xFF); b += (char)(unsigned char)((v >> 16) & 0xFF);
            b += (char)(unsigned char)((v >> 8) & 0xFF);  b += (char)(unsigned char)(v & 0xFF);
        };
        const char* tags8[] = { "cmap", "head", "hhea", "hmtx", "glyf", "loca" };
        const size_t nT = 6, bodyAt = 12 + 16 * nT;
        p32(0x00010000ul); p16((unsigned)nT); p16(0); p16(0); p16(0);
        for (size_t k = 0; k < nT; k++) {
            b += tags8[k];
            p32(0); p32((unsigned long)(bodyAt + 64 * k)); p32(64);
        }
        // ZERO-FILLED, and that is the whole trick of this fixture. With
        // filler bytes stb_truetype reads `numTables` out of the middle of the
        // cmap table, gets 21588, and walks 8 bytes per entry straight off the
        // end of the buffer - measured, SIGSEGV, which is exactly the residue
        // the gate cannot catch and therefore the wrong input for testing the
        // recovery. With zeros the cmap declares NO encoding subtables, so
        // stbtt_InitFont returns 0 at `if (info->index_map == 0) return 0;`
        // before it dereferences anything else, and Build() returns false
        // cleanly. One byte pattern is a crash, the other is the branch.
        b += std::string(64 * nT, '\0');
        const std::string fp8 = (r8 / "passes-the-gate.ttf").u8string();
        { std::ofstream f(pathFromUtf8(fp8), std::ios::binary); f.write(b.data(), (std::streamsize)b.size()); }
        std::string why8;
        check(sfntLooksSane(fp8, why8),
              "C8 the fixture passes the gate (which is what makes it the right one)");

        ImFontAtlas* A = ImGui::GetIO().Fonts;
        A->Clear();
        ImVector<ImWchar> r8r;
        cjkFontRanges(A, r8r);
        const bool added8 = A->AddFontFromFileTTF(fp8.c_str(), 17.0f, nullptr,
                                                  r8r.Data) != nullptr;
        check(added8, "C8 ...and ImGui accepts it, so the failure is Build()'s");
        const FontChoice keptChoice = g_fontChoice;
        g_fontChoice.path = fp8;
        g_fontChoice.how = "appearance.fontPath";
        fontAtlasBake(r8r, 1.0f);
        check(g_fontAtlasCost.buildFailed,
              "C8 a failed Build() is RECORDED, not discarded");
        check(g_fontAtlasCost.bakedGlyphs > 0 && g_fontAtlasCost.faces == 1,
              "C8 ...and the atlas was rebuilt from the built-in font");
        check(bakedFont() != nullptr && bakedFont()->FindGlyphNoFallback((ImWchar)'A'),
              "C8 ...so text still draws: the built-in font has ASCII");
        check(!fontAtlasHasCjk(),
              "C8 ...and it says the CJK glyphs are gone, which they are");
        // The summary line must not read as though that file is in use.
        const std::string from8 = fontAtlasFromText(g_fontAtlasCost, g_fontChoice,
                                                    std::string());
        say("summary: from " + from8);
        check(from8.find("built-in font") != std::string::npos,
              "C8 the summary line names the built-in font, which is what was baked");
        check(from8.find(fp8) != std::string::npos,
              "C8 ...and still names the file that failed, as the file that failed");
        // ...and the same mouth reports a font that would not even LOAD.
        FontAtlasCost loadFail;
        loadFail.buildFailed = true;
        const std::string from9 = fontAtlasFromText(loadFail, g_fontChoice, std::string());
        check(from9 == from8,
              "C8 ...and a font that could not be read at all says the same thing");
        // ---- AND THE RETRY THAT DROPS THE SECOND FACE (review P3) --------
        // `mergeDropped` was only ever set by hand in a message test, so the
        // branch that produces it - "two faces would not bake, the primary
        // alone did" - ran nowhere. Driven for real here: a GOOD primary plus
        // the gate-passing-but-unbakeable file as the second face. The atlas
        // must come back with the primary, one face, and mergeDropped naming
        // the one that was left out.
        if (!keptChoice.path.empty()) {
            ImFontAtlas* B = ImGui::GetIO().Fonts;
            B->Clear();
            ImVector<ImWchar> rr;
            cjkFontRanges(B, rr);
            g_fontChoice = keptChoice;             // a font that really bakes
            g_fontMergePath = fp8;                 // ...and one that cannot
            const bool p1 = B->AddFontFromFileTTF(keptChoice.path.c_str(),
                                                  CJK_FONT_PX, nullptr, rr.Data) != nullptr;
            ImFontConfig mc; mc.MergeMode = true;
            const bool p2 = B->AddFontFromFileTTF(fp8.c_str(), CJK_FONT_PX,
                                                  &mc, rr.Data) != nullptr;
            check(p1 && p2, "C8 both faces were accepted, so Build() is the judge");
            fontAtlasBake(rr, 1.0f);
            say("after the two-face bake: faces=" + std::to_string(g_fontAtlasCost.faces) +
                " glyphs=" + std::to_string(g_fontAtlasCost.bakedGlyphs) +
                " dropped=" + g_fontAtlasCost.mergeDropped);
            check(g_fontAtlasCost.mergeDropped == fp8,
                  "C8 the second face is named as the one that was dropped");
            check(!g_fontAtlasCost.buildFailed,
                  "C8 ...and the bake SUCCEEDED, because the primary alone works");
            // ONE FACE, AND IT IS THE PRIMARY'S GLYPHS - asserted against a
            // probe of the primary ALONE rather than against "does the atlas
            // have kana".
            //
            // This line read `faces == 1 && fontAtlasHasCjk()` and failed on
            // the Ubuntu runner, correctly: there the primary is DejaVu Sans
            // (fc-match's answer on a machine with no Japanese font) and it has
            // no CJK of its own, so the retry succeeded and the kana were still
            // absent. The assertion had mixed a STRUCTURAL claim - the merge is
            // gone and the primary survived - with a FACT ABOUT THE MACHINE's
            // font, which is the same mistake this round has been removing
            // everywhere else. Comparing glyph counts with a probe states only
            // the structure, and states it more precisely than kana ever did.
            ImFontAtlas probe;
            ImVector<ImWchar> pr;
            cjkFontRanges(&probe, pr);
            int aloneGlyphs = -1;
            if (probe.AddFontFromFileTTF(keptChoice.path.c_str(), CJK_FONT_PX,
                                         nullptr, pr.Data) && probe.Build()) {
                aloneGlyphs = 0;
                for (ImFont* pf : probe.Fonts) aloneGlyphs += pf->Glyphs.Size;
            }
            probe.Clear();
            say("the primary alone bakes " + std::to_string(aloneGlyphs) +
                " glyph(s); the atlas has " + std::to_string(g_fontAtlasCost.bakedGlyphs));
            check(g_fontAtlasCost.faces == 1,
                  "C8 ...with ONE face, the second having been dropped");
            check(aloneGlyphs > 0 && g_fontAtlasCost.bakedGlyphs == aloneGlyphs,
                  "C8 ...and exactly the glyphs the primary bakes on its own "
                  "(so the primary survived and the merge is gone)");
            // ...and what that costs is REPORTED, not asserted: whether the
            // surviving face has CJK is a property of this machine's font.
            say(std::string("after dropping the second face the atlas ") +
                (fontAtlasHasCjk() ? "still has" : "does NOT have") +
                " CJK - which is a fact about the primary font here, not about "
                "the retry");
            check(g_fontMergePath.empty(),
                  "C8 ...and the record stops claiming a merge it does not have");
            const std::string dm = cjkFontMergeDroppedText(g_fontAtlasCost, keptChoice);
            say("merge-dropped: " + dm);
            check(dm.find(fp8) != std::string::npos &&
                  dm.find(keptChoice.path) != std::string::npos,
                  "C8 ...and the message names both files");
        }
        g_fontChoice = keptChoice;
        std::filesystem::remove_all(r8, ec8);
    }
#else
    say("C8 SKIPPED: this build has assertions live, and every route to a false "
        "ImFontAtlas::Build() passes an IM_ASSERT first - so the recovery would "
        "abort rather than fail here. It runs on the Release builds CI makes.");
#endif

    fprintf(stderr, "cjkfontselftest: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
