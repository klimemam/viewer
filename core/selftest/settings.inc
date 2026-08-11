// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 設定ファイル
//
// --settings-selftest: settings.jsonc, issue #50 stage 1.
// The spec is docs/settings-inventory.md; the 判断 numbers below are its §9.
//
// FIXTURES GO IN THIS TEST'S OWN TEMP DIRECTORY, and that is not housekeeping -
// it is the subject. VIEWER_TEST_HOME is ONE folder shared by every selftest
// (only TMP/TEMP/TMPDIR are per-test, see CMakeLists.txt), so a settings.jsonc
// written there would be read by every test that runs afterwards and would
// survive the run: a9b7973 is the recorded case, where one `watchfiles 0` took
// the browse group from 3.1 s PASS to 183 s FAIL and kept it there. A test whose
// fixture IS a settings file is the one that must never write into that home.
// It is also not built inside a shared fixture folder (ad2d4fc): a leftover
// subdirectory of tools/testdata/rb is a ROW of that folder.
//
// ANTI-VACUITY. A settings test that would pass with the file absent is testing
// nothing, so every "the file did X" assertion here is paired: the field is set
// to the OPPOSITE first, and group L4/R9 run the identical code against a path
// that does not exist and assert that nothing moved. If the loader were gutted
// to a no-op, the paired assertions fail and the controls still pass - which is
// the only shape that tells the two apart.
static int settingsSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "settingsselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto say = [&](const std::string& s) {
        fprintf(stderr, "settingsselftest:   %s\n", s.c_str());
    };
    auto dumpNotes = [&](const SettingsReport& r) {
        if (!r.parseError.empty()) say("refused: " + r.parseError);
        for (const std::string& n : r.notes) say("note: " + n);
    };
    auto anyNote = [](const SettingsReport& r, const std::string& needle) {
        for (const std::string& n : r.notes)
            if (n.find(needle) != std::string::npos) return true;
        return false;
    };

    std::error_code ec;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "viewer_settings_selftest";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    fprintf(stderr, "settingsselftest: fixtures in %s\n", root.u8string().c_str());

    int fixN = 0;
    auto writeFix = [&](const std::string& body) {
        std::string p = (root / ("s" + std::to_string(++fixN) + ".jsonc")).u8string();
        std::ofstream f(pathFromUtf8(p), std::ios::binary);
        f.write(body.data(), (std::streamsize)body.size());
        return p;
    };
    const std::string missing = (root / "there-is-no-such-file.jsonc").u8string();

    // The state prefs.txt would have left behind: every field at its BUILT-IN
    // default, so that "the file changed it" and "it was already that" cannot
    // be confused. Restored before each group.
    auto baseline = [&]() {
        app.themeVariant = ui_theme::VariantDark;
        app.themeAccent = 0;
        app.compactUi = true;
        app.frameMode = 1;
        app.showFps = false;
        app.showGrid = false;
        app.dispGamma = 1.0f;
        app.dragPans = false;
        app.wheelZoomPlain = false;
        app.fitOnSwitch = false;
        app.rbFlat = false;
        app.rbTree = false;
        app.rbNatural = true;
        app.seqLoadMode = 0;
        app.remoteExe.clear();
        app.procPolicy = App::PolAuto;
        app.lowBandwidth = false;
        app.pythonExe.clear();
        app.readerPlaces.clear();
        snprintf(app.lin.unit, sizeof app.lin.unit, "%s", "lx");
        g_settingsRepoUrl.clear();
        g_settingsEditorCmd.clear();
        adapter::setSettingsEditor({});
    };

    // ======================================================================
    //  P — THE DIALECT
    //
    //  The decision being pinned: JSONC as VS Code defines it (JSON + // and
    //  /* */ comments + trailing commas), and NOT JSON5. Every JSON5 extra is
    //  a way for the file to mean one thing to this parser and another to the
    //  editor the user is looking at it in, so the extras are asserted to be
    //  REFUSED as loudly as the accepted forms are asserted to work.
    // ======================================================================
    baseline();
    {
        // P1/P2/P3/P4: the three things JSONC adds to JSON, each carrying a
        // value that is the OPPOSITE of the default, so an ignored file fails.
        std::string p = writeFix(
            "{\n"
            "  // a line comment\n"
            "  /* a block comment\n"
            "     over two lines */\n"
            "  \"appearance\": {\n"
            "    \"showFps\": true,   // trailing comment\n"
            "    \"pixelGrid\": true, /* and a block one */\n"
            "  },\n"
            "  \"readers\": {\n"
            "    \"searchPath\": [\"/a\", \"/b\",],\n"
            "  },\n"
            "}\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed, "P1 // and /* */ comments parse");
        check(app.showFps && app.showGrid, "P2 ...and the values after them are read");
        // Counting APPLIED, not merely "nothing was rejected": with the reader
        // gutted to a no-op, rejected == 0 and unknown == 0 are both true and
        // this assertion would pass having read nothing. Three keys is what
        // the fixture above carries.
        check(r.applied == 3 && r.rejected == 0 && r.unknown == 0,
              "P3 trailing commas are accepted (object)");
        check(app.readerPlaces.size() == 2 && app.readerPlaces[0] == "/a" &&
              app.readerPlaces[1] == "/b", "P4 trailing comma in an array, 2 entries");
    }
    {
        // P5: single quotes. JSON5 would take this; JSONC does not, and the
        // refusal has to NAME the dialect or the fix is not obvious.
        //
        // The GOOD section is first, on purpose: it is what makes the third
        // assertion a claim rather than a tautology. A parser that applied as
        // it went would have showFps on by the time it reached line 3.
        std::string p = writeFix(
            "{\n"
            "  \"appearance\": { \"showFps\": true },\n"
            "  'browse': { \"tree\": true }\n"
            "}\n");
        baseline();
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(!r.parsed && r.parseError.find(":3:3:") != std::string::npos,
              "P5 a single-quoted key is refused at 3:3");
        check(r.parseError.find("DOUBLE-quoted") != std::string::npos,
              "P5 ...naming the dialect, not just 'unexpected'");
        check(r.fileExists && !app.showFps && !app.rbTree,
              "P5 ...and nothing in that file was applied, the good key included");
    }
    {
        // P6: an unquoted key - the other JSON5-ism people type from memory.
        std::string p = writeFix("{\n  appearance: { \"showFps\": true }\n}\n");
        baseline();
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(!r.parsed && r.parseError.find("unquoted keys") != std::string::npos,
              "P6 an unquoted key is refused, by name");
    }
    {
        // P7: "//" INSIDE a string is not a comment. A peer executable or a
        // repo URL is the everyday value that contains one, and a comment
        // stripper that runs before the parser eats exactly this.
        baseline();
        std::string p = writeFix(
            "{ \"remote\": { \"repoUrl\": \"https://example.invalid/x.git\" } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && g_settingsRepoUrl == "https://example.invalid/x.git",
              "P7 // inside a string is text, not a comment");
    }
    {
        // P8: a UTF-8 BOM. It is what Notepad writes, the user cannot see it,
        // and refusing a file for it would be the worst possible first
        // experience of hand-editing one.
        baseline();
        std::string p = writeFix("\xEF\xBB\xBF{ \"appearance\": { \"showFps\": true } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && app.showFps, "P8 a UTF-8 BOM is skipped, not refused");
    }
    {
        // P9: escapes. Derived BY HAND: "caf\u00e9" is c,a,f + U+00E9, and
        // U+00E9 is two bytes in UTF-8 (0xC3 0xA9), so the unit is 5 bytes
        // long and its 4th byte is 0xC3. A parser that copied the escape
        // through would give 9 bytes; one that dropped it would give 3.
        baseline();
        std::string p = writeFix("{ \"series\": { \"defaultUnit\": \"caf\\u00e9\" } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        std::string u = app.lin.unit;
        say("defaultUnit is " + std::to_string(u.size()) + " bytes");
        check(r.parsed && u.size() == 5 && (unsigned char)u[3] == 0xC3 &&
              (unsigned char)u[4] == 0xA9, "P9 \\u00e9 decodes to 2 UTF-8 bytes");
    }
    {
        // P10: JSON's number grammar, strictly. Each of these parses in some
        // other dialect and must not parse here, because a file that means
        // one thing to the editor and another to the app is the whole reason
        // a dialect was chosen at all.
        struct { const char* lit; const char* what; } bad[] = {
            { "+1",       "P10 +1 is refused (JSON has no leading +)" },
            { ".5",       "P10 .5 is refused (a number needs a leading digit)" },
            { "01",       "P10 01 is refused (no leading zero)" },
            { "0x10",     "P10 0x10 is refused (JSON has no hex)" },
            { "Infinity", "P10 Infinity is refused" },
            { "NaN",      "P10 NaN is refused" },
        };
        for (const auto& b : bad) {
            baseline();
            std::string p = writeFix(std::string("{ \"appearance\": { \"accent\": ") +
                                     b.lit + " } }\n");
            SettingsReport r = loadSettingsFrom(p);
            // fileExists and a non-empty reason, not a bare !parsed: with the
            // reader gutted, !parsed is true for every file on disk and all
            // six of these would pass having opened nothing.
            check(r.fileExists && !r.parsed && !r.parseError.empty(), b.what);
            if (r.parsed) dumpNotes(r);
        }
        // ...and the forms that ARE JSON must still work, or the six above
        // would pass by the parser being broken rather than by being strict.
        baseline();
        std::string p = writeFix("{ \"appearance\": { \"accent\": 1, "
                                 "\"displayGamma\": 2.2 } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && app.themeAccent == 1 && app.dispGamma == 2.2f,
              "P10 1 and 2.2 still parse (the strictness is not a break)");
    }
    {
        // P11: the recursion guard. 40 open brackets, no crash, a named
        // refusal. MAX_DEPTH is 16, so the refusal lands inside the guard.
        baseline();
        std::string deep = "{ \"appearance\": ";
        for (int i = 0; i < 40; i++) deep += "[";
        deep += "1";
        for (int i = 0; i < 40; i++) deep += "]";
        deep += " }\n";
        std::string p = writeFix(deep);
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(!r.parsed && r.parseError.find("nested too deeply") != std::string::npos,
              "P11 40 nested arrays refuse by name instead of recursing");
    }
    {
        // P12: the top level must be the object. An array or a bare number is
        // a file somebody wrote for a different program.
        baseline();
        std::string p = writeFix("[1, 2, 3]\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(!r.parsed && r.parseError.find("inside { }") != std::string::npos,
              "P12 a top-level array is refused, with the shape named");
    }
    {
        // P13: the same key twice. JSON says last-wins and says it silently,
        // and silence here is exactly "I set it and nothing happened" - the
        // FIRST spelling is the one the user is looking at.
        baseline();
        std::string p = writeFix(
            "{\n  \"loading\": {\n"
            "    \"stackPrompt\": \"always\",\n"
            "    \"stackPrompt\": \"never\"\n"
            "  }\n}\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(app.seqLoadMode == 2, "P13 a duplicated key takes the LAST value");
        check(anyNote(r, "written twice") && anyNote(r, "line 3"),
              "P13 ...and both places are named, so it is not silent");
    }
    {
        // P14: an unterminated /* comment is a refusal at the comment's OWN
        // position, not at end-of-file. Where it was opened is the line to fix.
        baseline();
        std::string p = writeFix("{\n  /* never closed\n  \"appearance\": {}\n}\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(!r.parsed && r.parseError.find(":2:3:") != std::string::npos,
              "P14 an unclosed /* is refused AT THE /*, at 2:3");
    }

    // ======================================================================
    //  L — THE LOAD ORDER
    //
    //      built-in defaults  <  prefs.txt  <  settings.jsonc  <  flags
    //
    //  判断13 (a flag is a temporary override; there must not be two truths)
    //  and 判断18 as rewritten on 2026-08-11 (precedence is SPECIFIC BEATS
    //  GENERAL, not "env vs file").
    // ======================================================================
    baseline();
    {
        // L1/L2: prefs.txt gave false; the file says true; the file wins.
        // L3: and the app SAYS it did - without that line a user toggles the
        // menu, restarts, finds it back and has no way to learn why.
        app.wheelZoomPlain = false;     // as if loadPrefs() had just read it
        app.rbTree = false;
        std::string p = writeFix(
            "{ \"input\": { \"wheelZoomPlain\": true }, "
            "  \"browse\": { \"tree\": true } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(app.wheelZoomPlain && app.rbTree,
              "L1 settings.jsonc beats the value prefs.txt gave");
        check(r.applied == 2, "L2 ...for exactly the two keys it names");
        bool named = false;
        for (const std::string& s : r.overrode) if (s == "input.wheelZoomPlain") named = true;
        check(named && r.overrode.size() == 2,
              "L3 ...and every overridden key is listed by name");
    }
    {
        // L4: THE CONTROL. The identical field, the identical starting value,
        // the identical call - against a path that does not exist. If this
        // passed while L1 also passed with the loader gutted, the pair would
        // be indistinguishable; that is the whole reason it is here.
        app.wheelZoomPlain = false;
        app.rbTree = false;
        SettingsReport r = loadSettingsFrom(missing);
        check(!r.fileExists && !app.wheelZoomPlain && !app.rbTree,
              "L4 CONTROL: with no file, the prefs value is untouched");
        check(r.applied == 0 && r.notes.empty() && r.parseError.empty(),
              "L4 ...and an absent file is silent, not an error");
    }
    {
        // L5: THE FLAG WINS (判断13), asserted end to end through a REAL
        // start of this binary rather than by calling things in an order the
        // test chose. The child is given a config dir of its own, holding a
        // settings.jsonc that asks for the system title bar; --settings-template
        // then prints the resolved state.
        //
        //   run A, no flag       -> settings won   -> "titleBar": "system"
        //   run B, --frame integrated
        //                        -> the flag won   -> frameMode is 1, the
        //                           DEFAULT, so the template comments it out
        //
        // Both directions are asserted, because "the string is absent" alone
        // would also be true of a run that printed nothing at all.
        auto getEnvCopy = [](const char* k) {
            const char* v = getenv(k);
            return v ? std::string(v) : std::string();
        };
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };
        const std::string oldAppdata = getEnvCopy("APPDATA");
        const std::string oldHome = getEnvCopy("HOME");
        std::filesystem::path childHome = root / "childhome";
        std::filesystem::create_directories(childHome, ec);
        setEnv("APPDATA", childHome.u8string());
        setEnv("HOME", childHome.u8string());
        std::string cfg = viewerConfigDir();          // the same resolver the app uses
        std::string sp = cfg + "/settings.jsonc";
        {
            std::ofstream f(pathFromUtf8(sp), std::ios::binary);
            f << "{ \"appearance\": { \"titleBar\": \"system\" } }\n";
        }
        std::string exe = app.exePath;
        adapter::Run a = adapter::run({ exe, "--no-window", "--settings-template" }, 120000);
        adapter::Run b = adapter::run({ exe, "--no-window", "--settings-template",
                                       "--frame", "integrated" }, 120000);
        setEnv("APPDATA", oldAppdata);                 // before ANY assertion can
        setEnv("HOME", oldHome);                       // return early
        say("child A started=" + std::to_string((int)a.started) +
            " exit=" + std::to_string(a.exit) + " bytes=" + std::to_string(a.out.size()));
        check(a.started && a.exit == 0 && b.started && b.exit == 0,
              "L5 both child runs started and exited 0");
        bool aHas = a.out.find("\"titleBar\": \"system\"") != std::string::npos;
        bool bHas = b.out.find("\"titleBar\": \"system\"") != std::string::npos;
        bool bDef = b.out.find("// \"titleBar\": <default>") != std::string::npos;
        say(std::string("A carries titleBar=system: ") + (aHas ? "yes" : "no") +
            ", B carries it: " + (bHas ? "yes" : "no"));
        check(aHas, "L5 with no flag, settings.jsonc decides the title bar");
        check(!bHas && bDef, "L5 --frame beats settings.jsonc (judgment 13)");
    }
    {
        // L6: 判断18. readers.editor is a setting THIS application owns;
        // EDITOR is a variable every program on the machine shares, so the
        // specific one wins - git's GIT_EDITOR > core.editor > VISUAL > EDITOR
        // is the model. Asserted on the RESOLVER, because asserting it by
        // launching an editor is not something a selftest can do.
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };
        const char* was = getenv("EDITOR");
        const std::string oldEditor = was ? was : "";
        setEnv("EDITOR", "generic-editor");
        baseline();

        std::string why;
        std::string cmd = adapter::editorCommand(why);
        check(cmd == "generic-editor" && why == "$EDITOR",
              "L6 with no setting, $EDITOR is what would run");

        std::string p = writeFix("{ \"readers\": { \"editor\": \"specific-editor\" } }\n");
        SettingsReport r = loadSettingsFrom(p);
        cmd = adapter::editorCommand(why);
        dumpNotes(r);
        const bool hadSpecific = (cmd == "specific-editor");
        check(hadSpecific && why.find("readers.editor") != std::string::npos,
              "L6 readers.editor beats $EDITOR (specific over general)");

        // ...and deleting the key hands $EDITOR back. "not set" has to be a
        // state you can return to, or 判断9's "delete the key" is a lie.
        // hadSpecific is carried into the assertion because "it is $EDITOR
        // now" is only news if it was NOT $EDITOR a moment ago - without it
        // this passes on a reader that never set anything.
        std::string p2 = writeFix("{ \"readers\": { \"pythonExecutable\": \"py\" } }\n");
        loadSettingsFrom(p2);
        cmd = adapter::editorCommand(why);
        check(hadSpecific && cmd == "generic-editor" && why == "$EDITOR",
              "L6 deleting the key gives $EDITOR back (no third state)");
        setEnv("EDITOR", oldEditor);
    }
    {
        // L7: 判断17. The build-time VIEWER_REPO_URL is the DEFAULT, and the
        // peer bootstrap script is where it is actually spent - so the
        // assertion is on the SCRIPT, not on the variable holding it.
        baseline();
        const std::string built = bootstrapScript();
        check(built.find(VIEWER_REPO_URL) != std::string::npos,
              "L7 with no setting, the built-in repo URL is what is cloned");
        std::string p = writeFix(
            "{ \"remote\": { \"repoUrl\": \"https://fork.invalid/v.git\" } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        const std::string over = bootstrapScript();
        check(over.find("https://fork.invalid/v.git") != std::string::npos,
              "L7 remote.repoUrl reaches the peer bootstrap script");
        check(over.find(VIEWER_REPO_URL) == std::string::npos,
              "L7 ...and the built-in URL is no longer in it");
    }

    // ======================================================================
    //  R — THE REFUSALS
    //
    //  Two granularities, 判断7 and 判断8. A whole file that cannot be parsed
    //  refuses whole; one bad value refuses alone. 100 lines thrown away for
    //  one typo is not a refusal, it is a punishment.
    // ======================================================================
    {
        // R1: a file with a GOOD key before a syntax error applies NOTHING.
        // The good key is first on purpose: a parser that applied as it went
        // would pass every other assertion in this file and fail this one.
        baseline();
        std::string p = writeFix(
            "{\n"
            "  \"appearance\": { \"compact\": false },\n"
            "  \"input\": { \"dragPans\": true \n"       // no closing brace
            "}\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.fileExists && !r.parsed && !r.parseError.empty(),
              "R1 an unparseable file is FOUND and then refused");
        check(r.fileExists && app.compactUi && !app.dragPans,
              "R1 ...and NOTHING from it was applied, not even the good key");
        // The position, derived by hand from the fixture above. The inner
        // object's members end after `true`; the `}` on line 4 closes the
        // INNER object (it is the first one still open), leaving the outer one
        // open at end of file. Bumping that `}` puts the cursor at 4:2, the
        // newline moves it to 5:1, and that is where the outer object's member
        // loop asks for ',' or '}' and finds nothing.
        say("R1 refusal: " + r.parseError);
        check(r.parseError.find(".jsonc:5:1:") != std::string::npos &&
              r.parseError.find("',' or '}'") != std::string::npos,
              "R1 ...naming file, line 5, column 1 and the reason");
    }
    {
        // R2: ...and the file is not written back. Overwriting it destroys the
        // very text the user has to fix, and this program has no writer for
        // this file at all - which is the claim, so it is measured.
        baseline();
        std::string p = writeFix("{ \"appearance\": { oops } }\n");
        std::vector<uint8_t> before, after;
        readFileBytes(p, before);
        SettingsReport r = loadSettingsFrom(p);
        readFileBytes(p, after);
        check(r.fileExists && !r.parsed && !r.parseError.empty(),
              "R2 the broken file is read and refused");
        // r.fileExists rides along on purpose: "the bytes did not change" is
        // trivially true of a file nobody opened, and that is precisely the
        // shape of a test that proves nothing.
        check(r.fileExists && before == after && !before.empty(),
              "R2 ...and its bytes are byte-for-byte unchanged");
    }
    {
        // R3: 判断8. One bad value, and the rest of the file still lands.
        // Both halves are asserted: that the bad key did NOT move (a coerced
        // 5 would have silently become "light"), and that the good key DID.
        baseline();
        std::string p = writeFix(
            "{\n"
            "  \"appearance\": {\n"
            "    \"theme\": 5,\n"
            "    \"compact\": false\n"
            "  }\n}\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && r.rejected == 1, "R3 one bad value refuses exactly one key");
        check(r.parsed && app.themeVariant == ui_theme::VariantDark,
              "R3 ...the bad key keeps its previous value, uncoerced");
        check(!app.compactUi, "R3 ...and the rest of the file is still applied");
        check(anyNote(r, "\"appearance.theme\"") && anyNote(r, ":3:5:"),
              "R3 ...named with its own line and column");
    }
    {
        // R4: 判断9. null is a TYPE ERROR, never "explicitly the default".
        // Making it mean the default would need a three-valued bucket, and
        // terminology.md:105-108 already turned that down.
        baseline();
        app.showFps = true;                  // as if prefs.txt had it on
        std::string p = writeFix("{ \"appearance\": { \"showFps\": null } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && r.rejected == 1 && app.showFps,
              "R4 null is refused and does NOT reset the key to its default");
        check(anyNote(r, "null is not a value") && anyNote(r, "delete the key"),
              "R4 ...saying so, and saying how to get the default");
    }
    {
        // R5: 判断6. An unknown key is skipped, NAMED, and given the nearest
        // key it does know. A settings file's unknown key is a typo as often
        // as not, and skipping it in silence reproduces #50's own symptom at
        // the level of the format.
        baseline();
        std::string p = writeFix(
            "{ \"browse\": { \"naturlSort\": false, \"grouped\": false } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.unknown == 1, "R5 an unknown key is counted, not swallowed");
        check(anyNote(r, "\"browse.naturlSort\"") && anyNote(r, "\"browse.naturalSort\"?"),
              "R5 ...named, with the nearest known key suggested");
        check(app.rbNatural && app.rbFlat,
              "R5 ...the typo does nothing, the real key beside it works");
    }
    {
        // R6: a mistyped SECTION is one message, not seven. Saying "unknown
        // key" once per key under it buries the one fact that matters.
        baseline();
        std::string p = writeFix(
            "{ \"apperance\": { \"showFps\": true, \"compact\": false, "
            "\"pixelGrid\": true } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.unknown == 1 && r.notes.size() == 1,
              "R6 a mistyped section is named ONCE, not once per key");
        check(anyNote(r, "\"appearance\"?") && anyNote(r, "3 key(s)"),
              "R6 ...suggesting the section and counting what was lost");
        check(r.parsed && !app.showFps && app.compactUi,
              "R6 ...and none of it was applied");
    }
    {
        // R7: 判断18's second half. These two are not "an env var that wins",
        // they are NOT ON THIS MACHINE: core/serve.cpp reads them inside the
        // peer process, on the far side of an ssh connection. The reason a
        // setting is refused is worth more than the refusal, so it is asserted.
        baseline();
        std::string p = writeFix(
            "{ \"remote\": { \"pluginPath\": [\"/x\"], \"pinBudgetMB\": 2048 } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.notHere == 2 && r.unknown == 0,
              "R7 remote.pluginPath/pinBudgetMB are refused as not-a-setting");
        check(anyNote(r, "INSIDE THE PEER") && anyNote(r, "other machine"),
              "R7 ...giving the REASON (the peer, on the other machine)");
        // notes.size() is the anchor: "no note says 'did you mean'" is true
        // of an empty list, which is what a reader that read nothing produces.
        check(r.notes.size() == 2 && !anyNote(r, "did you mean"),
              "R7 ...and never offered as a spelling mistake");
    }
    {
        // R8: the measuring class is DEFERRED, and deferral is a fact the file
        // has to be able to state. "not a key" and "not yet" are different
        // answers, and only one of them tells the user to wait.
        baseline();
        app.memBudgetGB = 0;
        std::string p = writeFix(
            "{ \"measuring\": { \"memoryBudgetGB\": 8 },\n"
            "  \"loading\": { \"raw\": { \"dtype\": \"u16\" } },\n"
            "  \"input\": { \"keyBindings\": {} },\n"
            "  \"panels\": { \"messages\": true } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.later == 4 && r.unknown == 0,
              "R8 measuring/raw/keyBindings/panels are named as 'not yet'");
        check(r.later == 4 && app.memBudgetGB == 0,
              "R8 ...and memoryBudgetGB really is not read (judgment 10/11)");
        check(anyNote(r, "judgment 14") && anyNote(r, "judgment 10/11"),
              "R8 ...each pointing at the ruling that deferred it");
    }
    {
        // R9: THE SECOND CONTROL. An absent file produces no note of any kind,
        // so none of R5-R8's counters can be passing on leftovers.
        baseline();
        SettingsReport r = loadSettingsFrom(missing);
        check(!r.fileExists && r.notes.empty() && r.unknown == 0 &&
              r.rejected == 0 && r.later == 0 && r.notHere == 0,
              "R9 CONTROL: an absent file reports nothing at all");
    }
    {
        // R10: a section given a scalar. It is the shape a user reaches for
        // when they half-remember a flat file, and it must not be a crash.
        baseline();
        std::string p = writeFix("{ \"appearance\": true }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && r.rejected == 1 && anyNote(r, "wants an object"),
              "R10 a section given a scalar is refused, by name");
    }
    {
        // R11: a key written at the TOP level instead of inside its section.
        // The suggestion points at the full path, because "there is no such
        // key" is true and unhelpful when the key exists one line further in.
        baseline();
        std::string p = writeFix("{ \"showFps\": true }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.unknown == 1 && anyNote(r, "\"appearance.showFps\"?"),
              "R11 a top-level key is told which section it belongs in");
        check(r.unknown == 1 && !app.showFps, "R11 ...and is not applied from there");
    }
    {
        // R12: an out-of-range enum prints its whole alphabet. A refusal the
        // user cannot act on costs the same as no refusal.
        baseline();
        std::string p = writeFix("{ \"loading\": { \"stackPrompt\": \"sometimes\" } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.rejected == 1 && app.seqLoadMode == 0,
              "R12 an unknown enum word is refused, value unchanged");
        check(anyNote(r, "\"ask\"") && anyNote(r, "\"always\"") && anyNote(r, "\"never\""),
              "R12 ...listing every word it would have taken");
        // ...and displayGamma, where a plausible wrong number is the hazard:
        // 1.8 silently rounded to 2.2 is "I set it and nothing happened".
        baseline();
        std::string p2 = writeFix("{ \"appearance\": { \"displayGamma\": 1.8 } }\n");
        SettingsReport r2 = loadSettingsFrom(p2);
        dumpNotes(r2);
        check(r2.rejected == 1 && app.dispGamma == 1.0f,
              "R12 displayGamma 1.8 is refused, not rounded to 2.2");
    }

    {
        // R13: the refusal has to reach THE SCREEN (§5.3), and the startup
        // path makes that harder than it looks: loadSettings() runs before
        // ImGui::CreateContext(), because the settings decide the title bar
        // and the title bar is chosen while the window is being made. toast()
        // guards itself against a null context - it has to, or every windowless
        // selftest would segfault on one - and that guard silently turns an
        // early toast into a message with a deadline already in the past. So
        // the sentence is HELD and raised later, and this is where that is
        // asserted, through the real loadSettings() and the real settingsPath().
        auto setEnv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
            _putenv_s(k, v.c_str());
#else
            if (v.empty()) unsetenv(k); else setenv(k, v.c_str(), 1);
#endif
        };
        auto getEnvCopy = [](const char* k) {
            const char* v = getenv(k);
            return v ? std::string(v) : std::string();
        };
        const std::string oldAppdata = getEnvCopy("APPDATA");
        const std::string oldHome = getEnvCopy("HOME");
        std::filesystem::path h = root / "toasthome";
        std::filesystem::create_directories(h, ec);
        setEnv("APPDATA", h.u8string());
        setEnv("HOME", h.u8string());
        baseline();
        g_settingsToast.clear();
        app.toast.clear();
        const size_t logWas = app.msgLog.size();
        {
            std::ofstream f(pathFromUtf8(viewerConfigDir() + "/settings.jsonc"),
                            std::ios::binary);
            f << "{ \"appearance\": { \"compact\": }\n";
        }
        loadSettings();                       // the REAL startup entry point
        setEnv("APPDATA", oldAppdata);        // before any assertion can return
        setEnv("HOME", oldHome);
        // Column derived by hand from `{ "appearance": { "compact": }`:
        //   1 {   2 sp   3 "   4-13 appearance   14 "   15 :   16 sp   17 {
        //   18 sp   19 "   20-26 compact   27 "   28 :   29 sp   30 }
        // so the value that never arrived is asked for at column 30, and the
        // '}' sitting there is what cannot start one.
        say("held toast: " + g_settingsToast);
        check(g_settingsToast.find("settings.jsonc:1:30") != std::string::npos,
              "R13 loadSettings holds the refusal for the screen, with its position");
        check(app.toast.empty(),
              "R13 ...and does not spend it before there is a context to draw it");
        // Both lines land in the log: the position, and the promise that the
        // file was not overwritten. The second is the one that stops a user
        // deleting their own file to make the message go away.
        bool saidUntouched = false;
        for (size_t k = logWas; k < app.msgLog.size(); k++)
            if (app.msgLog[k].text.find("nothing was written") != std::string::npos)
                saidUntouched = true;
        check(app.msgLog.size() >= logWas + 2 && saidUntouched,
              "R13 ...and Messages carries both the reason and 'not written back'");
        // Cleared FIRST: without this, a build that spent the toast at load
        // time leaves its message sitting in app.toast, and the assertion
        // below reads that leftover as though settingsToastIfAny had raised it.
        app.toast.clear();
        settingsToastIfAny();
        const std::string raised = app.toast;
        app.toast.clear();
        settingsToastIfAny();                 // ...and only once
        check(raised.find("settings.jsonc:1:30") != std::string::npos && app.toast.empty(),
              "R13 ...raised exactly once when a context exists, then consumed");
        g_settingsToast.clear();
    }

    // ======================================================================
    //  S — THE SCHEMA TABLE
    //
    //  One table feeds the applier, the spelling suggester and the template.
    //  This walks it: every readable row is fed its own sample and has to be
    //  accepted. A row added to the table and to nothing else fails here.
    // ======================================================================
    {
        baseline();
        std::string doc = "{\n";
        std::string lastSec;
        int readable = 0;
        for (size_t k = 0; k < SETTING_KEY_COUNT; k++) {
            const SKeyDef& d = SETTING_KEYS[k];
            if (d.status != SS_Read) continue;
            readable++;
            std::string path = d.path;
            std::string sec = path.substr(0, path.find('.'));
            std::string leaf = path.substr(path.find('.') + 1);
            if (sec != lastSec) {
                if (!lastSec.empty()) doc += "  },\n";
                doc += "  \"" + sec + "\": {\n";
                lastSec = sec;
            }
            doc += "    \"" + leaf + "\": " + d.sample + ",\n";
        }
        if (!lastSec.empty()) doc += "  },\n";
        doc += "}\n";
        std::string p = writeFix(doc);
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        say("schema walk: " + std::to_string(readable) + " readable keys, applied " +
            std::to_string(r.applied));
        check(r.parsed, "S1 the schema walk document parses");
        check(r.applied == readable && r.unknown == 0 && r.rejected == 0,
              "S1 every readable key in the table is accepted by the applier");
        // ...and the samples really were applied, spot-checked across the
        // sections so that "applied" is not merely a counter being bumped.
        check(app.themeVariant == ui_theme::VariantLight && !app.compactUi &&
              app.frameMode == 0 && app.dragPans && app.rbFlat &&
              app.seqLoadMode == 2 && app.procPolicy == App::PolLocalFetch &&
              std::string(app.lin.unit) == "cd/m2",
              "S1 ...and the values really landed in the fields they name");
    }

    // ======================================================================
    //  T — THE TEMPLATE, i.e. THE MIGRATION
    //
    //  `viewer --settings-template` prints a settings.jsonc holding THIS
    //  user's current values - which is what prefs.txt gave - for exactly the
    //  keys that differ from a default. It writes nothing: 判断3 is that the
    //  app never writes this file, and a file the app created behind the
    //  user's back is not a file the user wrote.
    // ======================================================================
    {
        baseline();
        // A user's prefs.txt, in the fields it lands in. Five non-defaults and
        // everything else left alone, so the template has both kinds in it.
        app.showFps = true;
        app.wheelZoomPlain = true;
        app.seqLoadMode = 2;
        app.pythonExe = "C:/py/python.exe";
        app.readerPlaces = { "/one", "/two" };
        snprintf(app.lin.unit, sizeof app.lin.unit, "%s", "cd/m2");

        std::string txt = settingsTemplateText();
        auto active = [&](const char* leaf) {
            return txt.find(std::string("\n    \"") + leaf + "\":") != std::string::npos;
        };
        auto commented = [&](const char* leaf) {
            return txt.find(std::string("\n    // \"") + leaf + "\":") != std::string::npos;
        };
        check(active("showFps") && active("wheelZoomPlain") && active("stackPrompt") &&
              active("pythonExecutable") && active("searchPath") && active("defaultUnit"),
              "T1 the template carries every value that differs from a default");
        // 判断9 made executable: a key AT its default is not written out, or
        // "chose the default" and "never chose" stop being different the day
        // a default moves (board row 157 is the recorded case).
        check(commented("compact") && !active("compact") &&
              commented("naturalSort") && !active("naturalSort"),
              "T2 ...and a key at its default is commented out, never written");

        // T3: the round trip. Save it, wipe every field back to the default,
        // read it back, and the five values have to return - through the real
        // parser, which is also what proves the template is valid in its own
        // dialect (comments, trailing commas and all).
        std::string p = writeFix(txt);
        baseline();
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed, "T3 the template this program prints parses with its own reader");
        check(r.applied > 0 && r.unknown == 0 && r.rejected == 0 && r.notHere == 0,
              "T3 ...with no key its own reader refuses");
        check(app.showFps && app.wheelZoomPlain && app.seqLoadMode == 2 &&
              app.pythonExe == "C:/py/python.exe" &&
              app.readerPlaces.size() == 2 && app.readerPlaces[1] == "/two" &&
              std::string(app.lin.unit) == "cd/m2",
              "T3 ...and every value comes back exactly as it went in");
        // The migration promise in one line: what the template carried is what
        // prefs.txt held, so nobody loses a setting by moving to the new file.
        check(r.applied == 6, "T3 ...six keys carried over, which is what was set");
    }
    {
        // T4: prefs.txt is NOT touched by any of this. The whole change is a
        // reader, and the honest version of "we split the files" is that stage
        // 1 splits only the AUTHORITY: prefs.txt is still written by the app
        // and still read first. A writer appearing here would be found by this.
        std::string pp = prefsPath();
        std::vector<uint8_t> before, after;
        bool had = !pp.empty() && readFileBytes(pp, before);
        baseline();
        std::string p = writeFix("{ \"appearance\": { \"showFps\": true } }\n");
        loadSettingsFrom(p);
        bool has = !pp.empty() && readFileBytes(pp, after);
        // app.showFps anchors it: a reader that opened nothing also wrote
        // nothing, and this assertion has to be able to tell those apart.
        check(app.showFps && had == has && before == after,
              "T4 reading settings.jsonc writes nothing to prefs.txt");
    }

    baseline();
    std::filesystem::remove_all(root, ec);
    fprintf(stderr, "settingsselftest: %s\n", ok ? "ok" : "FAILED");
    // the orderly shutdown every early-returning selftest does: an unjoined
    // worker at exit is std::terminate() printed under the last line above
    stopRbWorker();
    stopSequenceLoader();
    stopRemoteFetcher();
    stopMeasureWorker();
    return ok ? 0 : 1;
}
