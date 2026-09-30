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
            say("no usable font on this machine, so C7 cannot show an override "
                "being taken or skipped - the file half below still ran.");
        } else {
            // (1) WITH NO CRUMB the override is used. Asserted first and in the
            // same group on purpose: an implementation that skipped the
            // override forever would satisfy every line in (2).
            const FontChoice ok1 = jpFontChoice(real, fontBakeCrumbRead());
            check(ok1.path == real && !ok1.skippedAfterCrash,
                  "C7 with no breadcrumb the override is used");
            // (2) WITH A CRUMB naming it, it is passed over, the record says
            // why, and the search carries on to something usable.
            fontBakeCrumbWrite({ real });
            const std::vector<std::string> back = fontBakeCrumbRead();
            check(back.size() == 1 && back[0] == real,
                  "C7 the breadcrumb survives a write and a read");
            const FontChoice sk = jpFontChoice(real, back);
            check(sk.skippedAfterCrash, "C7 ...and the next choice skips that file");
            check(sk.path != real || real == jpFontChoice(std::string(), back).path,
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
            // ...and it is a fifth SENTENCE, not one of the four wearing a hat.
            // All six pairs, because three comparisons out of six is not "they
            // are all different".
            FontChoice any; any.path = "/x.ttc"; any.how = "built-in candidate";
            FontAtlasCost dropCost; dropCost.mergeDropped = "/y.ttc";
            const std::string msgs[6] = {
                cjkFontMissingText(any), cjkFontNoGlyphsText(any),
                cjkFontUnreadableText(any), cjkFontChineseFallbackText(any),
                cjkFontCrashSkipText(any), cjkFontMergeDroppedText(dropCost, any),
            };
            int distinct = 0, pairs = 0;
            for (int i = 0; i < 6; i++)
                for (int k = i + 1; k < 6; k++) { pairs++; if (msgs[i] != msgs[k]) distinct++; }
            check(pairs == 15 && distinct == 15,
                  "C7 ...and all " + std::to_string(pairs) +
                  " pairs of the six font messages differ");
            // (4) SURVIVING CLEARS IT. Otherwise the first crash would disable
            // the override for good.
            fontBakeCrumbClear();
            check(fontBakeCrumbRead().empty(), "C7 a completed bake clears the breadcrumb");
            const FontChoice ok2 = jpFontChoice(real, fontBakeCrumbRead());
            check(ok2.path == real && !ok2.skippedAfterCrash,
                  "C7 ...and the override works again on the next start");
        }
        // Given back, at the one exit.
        fontBakeCrumbClear();
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
