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
            // ...and the two PURE claims, which used to be written so that they
            // could only ever fail and never report (review P3-17): a loop of
            // `if (bad) check(false)` prints nothing at all when it is happy,
            // so a reader could not tell it had run. Counted and asserted.
            int selfMerged = 0;
            std::string selfName;
            std::vector<std::string> everything = jpFontCandidates();
            for (const std::string& m : cjkMergeCandidates()) everything.push_back(m);
            for (const std::string& cand : everything)
                if (cjkMergeFontPath(cand) == cand) { selfMerged++; selfName = cand; }
            check(selfMerged == 0,
                  "C3 no font is ever merged onto itself (" +
                  std::to_string((int)everything.size()) + " path(s) checked" +
                  (selfName.empty() ? "" : ", first offender " + selfName) + ")");
            // A Chinese face IS in the primary search now (review P2-8), so on
            // a machine with one face it is either because the platform needs
            // none or because none is installed - never because the list forgot
            // to look. That is the thing worth pinning here.
            check(cjkMergeFontPath(g_fontChoice.path).empty(),
                  "C3 ...and the one-face state really is 'no second file "
                  "available', not 'never asked'");
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
        check(note.find("appearance.fontPath") != std::string::npos,
              "C4 ...and it names the way out");
        // ---- the Messages door (板306 D), ONE LINE PER OPEN ----------------
        // Driven off app.imagesRev rather than off any one of the six places
        // that push a document, and grouped by BATCH because a batch is the
        // program's own name for one open action. Borrowed state is given back
        // at the end: this suite shares one home with every other test.
        //
        // A sweep does not report a batch that is still GROWING - one more
        // sweep at the same size is what says the load has landed - so every
        // case below sweeps twice, which is also what the frame loop does.
        const size_t logWas = app.msgLog.size();
        const uint64_t revWas = app.imagesRev;
        auto sweep = [&]() { app.imagesRev++; undrawableReportNewDocs(); };
        auto mkDoc = [&](const std::string& nm, int batchId, int seqId) {
            auto d = std::make_unique<ImageDoc>();
            d->name = nm;
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
            sweep();                       // records the size, says nothing yet
            sweep();                       // settled: now it reports
            const std::vector<std::string> got = linesSince(logWas);
            for (const std::string& l : got) say("msg: " + l);
            check(got.size() == 1 && got[0].find("draw as '?'") != std::string::npos,
                  "C4 one open of one such file leaves exactly ONE line");
            // ...and not again. THE TRAP THIS AVOIDS (review P2-11): logMsg
            // suppresses a line identical to the one before it, so an assertion
            // that only swept again would pass even with the de-duplication
            // removed. A DIFFERENT line goes in between, so the suppression
            // under test is this function's and not logMsg's.
            logMsg("cjkfont selftest marker", false);
            const size_t after = app.msgLog.size();
            sweep(); sweep();
            check(app.msgLog.size() == after,
                  "C4 ...and never again for that open (asserted past logMsg's "
                  "own duplicate suppression)");
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
            for (int k = 0; k < 40; k++)
                mkDoc("frame_" + std::to_string(k) + ".npy", 102, si.id);
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
            for (int k = 0; k < 6; k++)
                mkDoc(k < 2 ? thai : ("ok_" + std::to_string(k) + ".npy"), 103, si2.id);
            sweep(); sweep();
            const std::vector<std::string> got2 = linesSince(before2);
            for (const std::string& l : got2) say("msg: " + l);
            check(got2.size() == 1 && got2[0].find("2 of 6 frame(s)") != std::string::npos,
                  "C4 ...and a stack whose FRAMES carry it says 2 of 6");
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
                    t.find("is not a font") != std::string::npos) named = true;
            check(named,
                  "C5 ...and the record says WHY, not just that it was tried");
            // ...and the search still ends somewhere usable rather than being
            // poisoned by the bad override.
            check(c.path == wouldPick,
                  "C5 ...and the search carries on to what it would have picked");
            // A file with a CORRECT signature that stb_truetype cannot parse is
            // a DIFFERENT failure and the signature check cannot see it: this
            // is a well-formed sfnt header declaring zero tables, so
            // stbtt_InitFont finds no cmap/head/hhea/hmtx and Build() returns
            // false. That is the case CFF2 variable fonts hit for real, and it
            // is why looksLikeSfnt() alone does not close the hole.
            const std::string stub = (root0 / "signed-but-empty.ttf").u8string();
            {
                std::ofstream f(pathFromUtf8(stub), std::ios::binary);
                const unsigned char hdr[12] = { 0x00,0x01,0x00,0x00, 0,0, 0,0, 0,0, 0,0 };
                f.write((const char*)hdr, 12);
                const std::string pad(400, '\0');
                f.write(pad.data(), (std::streamsize)pad.size());
            }
            check(looksLikeSfnt(stub),
                  "C5 a correct sfnt signature IS accepted by the signature check");
            ImFontAtlas probe;
            ImVector<ImWchar> r2;
            cjkFontRanges(&probe, r2);
            const bool added = probe.AddFontFromFileTTF(stub.c_str(), 17.0f,
                                                        nullptr, r2.Data) != nullptr;
            check(added,
                  "C5 ...and ImGui ACCEPTS it, which is why the size check is "
                  "not the guard either");
            check(added && !probe.Build(),
                  "C5 ...while Build() returns false - the signal start-up now "
                  "acts on instead of discarding");
            probe.Clear();
            // The sentence that failure produces names the file and says what
            // the user is looking at instead.
            FontChoice bad;
            bad.path = stub; bad.how = "appearance.fontPath";
            const std::string unread = cjkFontUnreadableText(bad);
            say("unreadable: " + unread);
            check(unread.find(stub) != std::string::npos &&
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
            // ...and it is a DIFFERENT sentence from the other three, or the
            // four failures would be one message wearing four hats.
            FontChoice any; any.path = "/x.ttc"; any.how = "built-in candidate";
            check(cjkFontMissingText(any) != cjkFontNoGlyphsText(any) &&
                  cjkFontNoGlyphsText(any) != cjkFontUnreadableText(any) &&
                  cjkFontUnreadableText(any) != cjkFontChineseFallbackText(any),
                  "C5 ...and the four font failures are four different sentences");
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
        check(jpFontCandidates().size() >= 3,
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

    fprintf(stderr, "cjkfontselftest: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
