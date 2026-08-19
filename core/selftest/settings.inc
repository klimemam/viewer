// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 設定ファイル
//
// --settings-selftest: settings.jsonc, issue #50 stage 1.
// The spec is docs/features/settings/settings-inventory.md; the 判断 numbers below are its §9.
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
        // 段2d/2e: three fields the file can now set, so they belong in the
        // state every block starts from. Without them R8's `memoryBudgetGB: 8`
        // survived into T3 and the template carried a seventh key - the
        // shared-state hazard this suite already knows about, one scope in.
        app.memBudgetGB = 0;
        app.watchEnabled = true;
        app.watchAutoReload = false;
        // #204, and here for the same reason the three above are: group D sets
        // this key, and a value that survived into T3 would put a seventh key
        // in the template and make T3's count measure the previous group.
        app.folderScanDepth = 6;
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
        // The recipe library (#166) is a value this file owns outright, like the
        // two above, so its built-in default is EMPTY. It has to be cleared here
        // as well as in the reader: the schema walk (S1) feeds every readable row
        // its own sample, one of which is a recipe, and the template groups (T)
        // then run afterwards - a leftover recipe would make the template carry a
        // seventh key and T3's count would be measuring the previous group.
        g_rawLibrary.clear();
        adapter::setSettingsEditor({});
        g_prefsBaseActive = false;
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
        // THREE now, not four: measuring.memoryBudgetGB was the fourth and it
        // is READ since 段2d - its M1 declaration ("n of N" on a short stack)
        // was already required by the canon, which is what let the section
        // open. The assertion moves with the fact rather than being relaxed:
        // the key it used to name is now asserted to LAND.
        check(r.later == 3 && r.unknown == 0,
              "R8 raw/keyBindings/panels are named as 'not yet'");
        check(app.memBudgetGB == 8.0f,
              "R8 ...and memoryBudgetGB IS read now (judgment 10/11 discharged)");
        check(anyNote(r, "judgment 14"),
              "R8 ...each deferred key still points at the ruling that deferred it");
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
        // displayGamma is not an enum: 1.0 and 2.2 are presets, while every
        // other positive finite float is Custom (#226). A Custom value must be
        // accepted exactly, never rounded to the nearer preset.
        baseline();
        std::string p2 = writeFix("{ \"appearance\": { \"displayGamma\": 1.8 } }\n");
        SettingsReport r2 = loadSettingsFrom(p2);
        dumpNotes(r2);
        check(r2.rejected == 0 && app.dispGamma == (float)1.8 &&
              displayGammaPreset(app.dispGamma) == -1,
              "R12 displayGamma 1.8 is Custom, not rounded to a preset");

        // Zero, negative, overflow-to-infinity and underflow-to-zero all keep
        // the previous value. The last two catch validation done only before
        // narrowing JSON's double to App's float.
        const char* badGamma[] = { "0", "-1", "1e39", "1e999", "1e-50", "1e-999" };
        bool badKept = true;
        for (const char* bad : badGamma) {
            baseline();
            app.dispGamma = 1.25f;
            const SettingsReport br = loadSettingsFrom(writeFix(
                std::string("{ \"appearance\": { \"displayGamma\": ") + bad + " } }\n"));
            badKept = badKept && br.rejected == 1 && app.dispGamma == 1.25f;
        }
        check(badKept, "R12 invalid gamma values are refused and leave the value unchanged");

        float parsedGamma = 0;
        const float awkward = 1.2345678f;
        const std::string saidGamma = displayGammaText(awkward);
        check(parseDisplayGamma(saidGamma.c_str(), parsedGamma) &&
              displayGammaBits(parsedGamma) == displayGammaBits(awkward),
              "R12 Custom gamma text round-trips the exact float");
        const float maxGamma = std::numeric_limits<float>::max();
        const std::string maxSaid = displayGammaText(maxGamma);
        check(parseDisplayGamma(maxSaid.c_str(), parsedGamma) && parsedGamma == maxGamma,
              "R12 the largest finite Custom gamma also round-trips");
        check(displayGammaPreset(1.0f) == 0 && displayGammaPreset(2.2f) == 1 &&
              displayGammaPreset(1.25f) == -1 &&
              displayGammaNeedsTransform(1.0005f),
              "R12 presets are exact and every nearby Custom value is transformed");
        check(displayGammaBits(1.01f) != displayGammaBits(1.02f),
              "R12 nearby Custom values have distinct change stamps");
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

    // ---- O1..O4: WHO decides this value (docs/features/settings/preferences-panel-design.md §5)
    // #50's symptom is "I set it and it did not take". Four layers can decide a
    // key and until now nothing could say which one did - so the Preferences
    // panel's first requirement is not a widget, it is an answer.
    //
    // Only the two SPEAKING layers are recorded. A file line and a flag NAME a
    // key; "the default" and "this machine" are what is LEFT and are told apart
    // by the value. Asserting all four here is what pins that split.
    {
        baseline();
        // the file names exactly one key, at a line this test controls
        const std::string op = writeFix("// a comment, so the line number is not 1\n"
                                        "{ \"appearance\": { \"showFps\": true } }\n");
        loadSettingsFrom(op);
        // ...and a flag names another. parseCli is not run here (it would eat
        // this process's argv), so the ledger call it makes is made directly -
        // the same one line, from the same helper.
        settingsOriginCli(SK_policy, "--remote-policy");
        // compact: baseline() is the state prefs.txt would leave, and it holds
        // false while the built-in default is true. Derivation: no speaking
        // layer + settingsCurrentValue non-empty = this machine.
        app.compactUi = false;
        const OriginEntry& oFps  = settingsOriginOf(SK_showFps);
        const OriginEntry& oPol  = settingsOriginOf(SK_policy);
        const OriginEntry& oComp = settingsOriginOf(SK_compact);
        const OriginEntry& oDrag = settingsOriginOf(SK_dragPans);
        fprintf(stderr, "settingsselftest: O showFps=%d line=%d | policy=%d flag=%s | "
                        "compact=%d value=\"%s\" | dragPans=%d value=\"%s\"\n",
                (int)oFps.who, oFps.line, (int)oPol.who,
                oPol.cliFlag ? oPol.cliFlag : "-",
                (int)oComp.who, settingsCurrentValue(SK_compact).c_str(),
                (int)oDrag.who, settingsCurrentValue(SK_dragPans).c_str());
        check(oFps.who == SetOrigin::File && oFps.line == 2,
              "O1 a key the file set names the FILE and the line it is on");
        check(oPol.who == SetOrigin::Cli && oPol.cliFlag &&
              std::string(oPol.cliFlag) == "--remote-policy",
              "O2 a key a flag set names the FLAG (a flag beats the file)");
        check(oComp.who == SetOrigin::None && !settingsCurrentValue(SK_compact).empty(),
              "O3 a key only prefs.txt moved has no speaker, and is not the default");
        check(oDrag.who == SetOrigin::None && settingsCurrentValue(SK_dragPans).empty(),
              "O3 ...and an untouched key has no speaker AND is the default");
        // A key the file NAMES and this build REFUSES decided nothing. Without
        // this the panel would badge a row "settings.jsonc decides it" and send
        // the operator to edit a line that did not take.
        baseline();
        const std::string bad = writeFix("{ \"appearance\": { \"accent\": \"purple\" } }\n");
        const SettingsReport br = loadSettingsFrom(bad);
        const OriginEntry& oAcc = settingsOriginOf(SK_accent);
        fprintf(stderr, "settingsselftest: O4 refused key: rejected=%d origin=%d\n",
                br.rejected, (int)oAcc.who);
        check(br.rejected == 1 && oAcc.who == SetOrigin::None,
              "O4 a key the file named and this build refused claims nothing");
        // ...and the ledger is about THIS read: a second load whose file is
        // silent about showFps must not leave the old claim standing.
        loadSettingsFrom(writeFix("{ \"appearance\": { \"pixelGrid\": true } }\n"));
        fprintf(stderr, "settingsselftest: O4 after a second read: showFps=%d grid=%d\n",
                (int)settingsOriginOf(SK_showFps).who,
                (int)settingsOriginOf(SK_pixelGrid).who);
        check(settingsOriginOf(SK_showFps).who == SetOrigin::None &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::File,
              "O4 a reload forgets what the previous file said");
    }

    // O5: a loaded document is a fifth, later authority for the two display
    // values .vsession actually carries. Gamma starts under a CLI claim and
    // grid under a File claim, so this pins precedence rather than merely the
    // new enum value.
    {
        auto displayLines = [](const std::string& text) {
            float gamma = -1.0f;
            int grid = -1;
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line)) {
                std::istringstream ls(line);
                std::string key;
                ls >> key;
                if (key == "gamma") ls >> gamma;
                else if (key == "grid") ls >> grid;
            }
            return std::pair<float, int>{ gamma, grid };
        };

        // Before any session, prefs writing must retain its historical use of
        // the effective startup values. The first valid session header freezes
        // those same non-default values; merely setting the active bit without
        // copying them makes both assertions fail.
        baseline();
        loadSettingsFrom(writeFix(
            "{ \"appearance\": { \"displayGamma\": 2.2, \"pixelGrid\": true } }\n"));
        app.dispGamma = 1.75f;                // simulate an accepted CLI value
        settingsOriginCli(SK_gamma, "--synthetic-gamma");
        std::ostringstream prefsBeforeSession;
        writePrefsTo(prefsBeforeSession);
        const auto startupBeforeSession = displayLines(prefsBeforeSession.str());
        check(!g_prefsBaseActive && startupBeforeSession.first == 1.75f &&
              startupBeforeSession.second == 1,
              "O5 prefs without a session preserve effective File and CLI values");

        const std::string missingDisplay = writeFix(
            "viewer-session 1\n"
            "zoom 2\n");
        const std::string missingErr = loadSession(missingDisplay);
        check(missingErr.empty() && app.dispGamma == 1.75f && app.showGrid &&
              settingsOriginOf(SK_gamma).who == SetOrigin::Cli &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::File,
              "O5 an initial session with missing keys creates no Session claim");
        std::ostringstream prefsAfterMissing;
        writePrefsTo(prefsAfterMissing);
        const auto startupAfterMissing = displayLines(prefsAfterMissing.str());
        check(g_prefsBaseActive && g_prefsBase.dispGamma == 1.75f &&
              g_prefsBase.showGrid && startupAfterMissing.first == 1.75f &&
              startupAfterMissing.second == 1,
              "O5 the first session boundary captures effective startup values");

        baseline();
        loadSettingsFrom(writeFix(
            "{ \"appearance\": { \"displayGamma\": 2.2, \"pixelGrid\": true } }\n"));
        app.dispGamma = 1.5f;
        settingsOriginCli(SK_gamma, "--synthetic-gamma");
        const std::string invalidDisplay = writeFix(
            "viewer-session 1\n"
            "gamma 0\n"
            "grid not-a-number\n");
        const std::string invalidErr = loadSession(invalidDisplay);
        check(invalidErr.empty() && app.dispGamma == 1.5f && app.showGrid &&
              settingsOriginOf(SK_gamma).who == SetOrigin::Cli &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::File &&
              g_prefsBaseActive && g_prefsBase.dispGamma == 1.5f &&
              g_prefsBase.showGrid,
              "O5 invalid initial display lines leave values and origins unchanged");

        baseline();
        loadSettingsFrom(writeFix(
            "{ \"appearance\": { \"displayGamma\": 2.2, \"pixelGrid\": false } }\n"));
        app.dispGamma = 1.75f;
        settingsOriginCli(SK_gamma, "--synthetic-gamma");
        const std::string session = writeFix(
            "viewer-session 1\n"
            "gamma 1.25\n"
            "grid 1\n");
        const std::string sessionErr = loadSession(session);
        check(sessionErr.empty() && app.dispGamma == 1.25f && app.showGrid &&
              displayGammaPreset(app.dispGamma) < 0 &&
              settingsOriginOf(SK_gamma).who == SetOrigin::Session &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::Session,
              "O5 valid session gamma/grid beat File and CLI without losing Custom");
        check(settingsBadgeFor(SK_gamma) == "session (.vsession)" &&
              settingsBadgeFor(SK_pixelGrid) == "session (.vsession)",
              "O5 both Preferences rows name the loaded .vsession");

        // Loading a session changes only the effective display state. Until a
        // real GUI edit occurs, an unrelated prefs save must keep the effective
        // startup values rather than baking the session into prefs.txt.
        std::ostringstream prefsBefore;
        writePrefsTo(prefsBefore);
        const auto lowerBefore = displayLines(prefsBefore.str());
        check(lowerBefore.first == 1.75f && lowerBefore.second == 0,
              "O5 session restore alone preserves File and CLI startup values");

        // This is the shared helper called by Preferences, Inspector, menus,
        // and the G shortcut. It updates the lower preference and side effects,
        // but the current document still owns the session display state.
        app.prefsDirty = false;
        app.dispGamma = 2.2f;
        settingsDisplayGuiChanged(SK_gamma, "appearance.displayGamma");
        check(settingsOriginOf(SK_gamma).who == SetOrigin::Session &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::Session &&
              g_prefsBase.dispGamma == app.dispGamma && !g_prefsBase.showGrid &&
              app.showGrid &&
              app.prefsDirty,
              "O5 the gamma helper keeps Session origin and updates only gamma prefs base");

        std::ostringstream sessionAfterGamma, prefsAfterGamma;
        writeSessionTo(sessionAfterGamma);
        writePrefsTo(prefsAfterGamma);
        const auto savedAfterGamma = displayLines(sessionAfterGamma.str());
        const auto prefsAfterGammaDisplay = displayLines(prefsAfterGamma.str());
        check(savedAfterGamma.first == app.dispGamma && savedAfterGamma.second == 1 &&
              prefsAfterGammaDisplay.first == app.dispGamma &&
              prefsAfterGammaDisplay.second == 0,
              "O5 changing gamma does not leak the session grid into prefs");

        // The effective Session value is true while its lower File value is
        // false. Updating the row must exercise the bool branch and persist 1.
        settingsDisplayGuiChanged(SK_pixelGrid, "appearance.pixelGrid");
        check(settingsOriginOf(SK_gamma).who == SetOrigin::Session &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::Session &&
              g_prefsBase.dispGamma == app.dispGamma && g_prefsBase.showGrid,
              "O5 the grid helper keeps Session origin and records true in prefs base");

        std::ostringstream sessionAfter, prefsAfter;
        writeSessionTo(sessionAfter);          // Save and autosave share this renderer
        writePrefsTo(prefsAfter);
        const auto savedDisplay = displayLines(sessionAfter.str());
        const auto prefsDisplay = displayLines(prefsAfter.str());
        check(savedDisplay.first == app.dispGamma && savedDisplay.second == 1 &&
              prefsDisplay.first == app.dispGamma && prefsDisplay.second == 1,
              "O5 session save/autosave and prefs save both carry GUI values");

        // A later legacy session with no display lines keeps the values the
        // reader has always kept. The claims stay too; changing only the badge
        // would make the displayed origin disagree with the inherited value.
        const std::string laterMissing = writeFix("viewer-session 1\n");
        const std::string laterMissingErr = loadSession(laterMissing);
        check(laterMissingErr.empty() && app.dispGamma == 2.2f && app.showGrid &&
              settingsOriginOf(SK_gamma).who == SetOrigin::Session &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::Session,
              "O5 a later missing-key session preserves Session value and origin");

        // Per-key application: a valid gamma line can claim Session while a
        // malformed grid line neither changes the value nor steals its File claim.
        baseline();
        loadSettingsFrom(writeFix(
            "{ \"appearance\": { \"displayGamma\": 2.2, \"pixelGrid\": true } }\n"));
        const std::string oneMalformed = writeFix(
            "viewer-session 1\n"
            "gamma 1.5\n"
            "grid not-a-number\n");
        const std::string malformedErr = loadSession(oneMalformed);
        check(malformedErr.empty() && app.dispGamma == 1.5f && app.showGrid &&
              settingsOriginOf(SK_gamma).who == SetOrigin::Session &&
              settingsOriginOf(SK_pixelGrid).who == SetOrigin::File,
              "O5 only successfully restored keys claim Session origin");
    }

    // ---- W1..W4: the Preferences panel's ROW MODEL --------------------------
    // The panel is SETTING_KEYS' fourth consumer, not a fifth list. What can be
    // wrong is what a row SAYS - its widget, its value, who decides it - and
    // none of that needs a window, so it is asserted here rather than drawn.
    {
        baseline();
        loadSettingsFrom(missing);           // no file: the plain state
        std::vector<PrefRow> rows = settingsPrefRows();
        // STRUCTURAL, not a literal: the row count is the table's length, so a
        // key added to the table appears here without anybody editing this test
        // (判断16 - when a number stops matching, re-count the table).
        int readable = 0, later = 0, notHere = 0, widgeted = 0, noted = 0;
        for (const PrefRow& r : rows) {
            if (r.status == SS_Read) {
                readable++;
                if (r.widget != SW_None) widgeted++;
            } else {
                if (r.status == SS_Later) later++; else notHere++;
                if (r.note && *r.note) noted++;
            }
        }
        fprintf(stderr, "settingsselftest: W rows=%zu (table %zu) read=%d later=%d "
                        "notHere=%d widgeted=%d noted=%d\n",
                rows.size(), SETTING_KEY_COUNT, readable, later, notHere,
                widgeted, noted);
        check(rows.size() == SETTING_KEY_COUNT,
              "W1 the panel has exactly one row per table key");
        // ...and the spot check the design derives by hand
        // (docs/features/settings/preferences-panel-design.md §3.1): prefs writes 27 keys, minus
        // 5 history = 22 settings; membudget/watchfiles/watchauto are not read
        // yet = 19; `theme` splits into theme + accent = 20; plus the 3 that
        // exist only in settings.jsonc (remote.repoUrl, loading.rawRecipes,
        // readers.editor) = 23.
        // 23 was the count before 段2d/2e. Those promoted the three keys stage 1
        // could not read - measuring.memoryBudgetGB (its M1 declaration, "n of
        // N", was already required by the canon) and the two watch keys (ruled
        // 2026-08-13) - so the reconciliation closed at 26 and the table at 33.
        // #204 adds loading.folderScanDepth, which is the first key here that
        // was never a prefs.txt line before it was a setting (the ruling made
        // it one; it is written to prefs.txt now because the panel edits it):
        // 27 readable over 34 rows.
        check(readable == 27, "W1 ...and 27 of them are editable today");
        check(rows.size() == 34, "W1 ...over 34 rows, which is the whole table");
        // EVERY readable row has a control. This is S1's shape one level up: a
        // key added to the table and forgotten in settingsWidgetFor() would
        // otherwise draw a row with nothing in it.
        check(widgeted == readable,
              "W2 every readable key has a widget kind (none was forgotten)");
        // ...and every row that is NOT readable says why, in the table's own
        // words - the same sentence the template prints.
        check(noted == later + notHere,
              "W2 every 'later' / 'not here' row carries its sentence");
        // Spot checks, each with its derivation:
        //   displayGamma is a compound control: the two presets remain quick
        //   actions and Custom accepts the same positive finite domain as the
        //   file. It is not a slider and never clamps.
        //   rawRecipes is a LINK - it is a list with its own panel (#166), and
        //   editing an array of objects in a row is not a control this panel has.
        auto rowOf = [&](SKey id) -> const PrefRow* {
            for (const PrefRow& r : rows) if (r.id == id) return &r;
            return nullptr;
        };
        const PrefRow* rg = rowOf(SK_gamma);
        const PrefRow* rr = rowOf(SK_rawRecipes);
        const PrefRow* rb = rowOf(SK_compact);
        fprintf(stderr, "settingsselftest: W3 gamma widget=%d | rawRecipes widget=%d | "
                        "compact widget=%d\n",
                rg ? (int)rg->widget : -1, rr ? (int)rr->widget : -1,
                rb ? (int)rb->widget : -1);
        check(rg && rg->widget == SW_Gamma,
              "W3 displayGamma has two presets and a Custom numeric input");
        check(rr && rr->widget == SW_Link, "W3 rawRecipes is a link, not an editor");
        check(rb && rb->widget == SW_Bool, "W3 a boolean key gets a checkbox");
        // ...and the badge, which is the whole reason this panel exists: the
        // four layers have to be distinguishable ON THE ROW.
        const std::string bDefault = rb ? rb->badge : std::string();
        loadSettingsFrom(writeFix("{ \"appearance\": { \"compact\": false } }\n"));
        rows = settingsPrefRows();
        const PrefRow* rb2 = nullptr;
        for (const PrefRow& r : rows) if (r.id == SK_compact) rb2 = &r;
        fprintf(stderr, "settingsselftest: W4 compact badge: no file \"%s\" -> "
                        "with file \"%s\"\n",
                bDefault.c_str(), rb2 ? rb2->badge.c_str() : "-");
        check(bDefault == "default", "W4 an untouched key's row says 'default'");
        check(rb2 && rb2->badge.rfind("settings.jsonc:", 0) == 0,
              "W4 ...and a key the file decides names the file and the line");
    }

    // ---- W5: "Copy as JSONC" is the parser's inverse -----------------------
    // Asserted as a ROUND TRIP, not as a string comparison. The copy is written
    // as the reverse of the stage-1 parser, so composing the two has to be the
    // identity - and a string test would pin today's spacing instead of the
    // property (--rawrecipe-selftest S3 set the precedent).
    //
    // A COMPLETE document, because a fragment pasted into an empty file is a
    // parse error, and a copy button whose output must be edited first is a
    // copy button that does not work. That is what makes the round trip
    // possible to write at all: loadSettingsFrom takes a file, and this text
    // has to be one.
    {
        baseline();
        app.themeVariant = ui_theme::VariantLight;      // not the default
        const std::string txt = settingsKeyAsJsonc(SK_theme);
        const std::string rp = writeFix(txt);
        baseline();                                     // back to dark
        const SettingsReport rr = loadSettingsFrom(rp);
        fprintf(stderr, "settingsselftest: W5 copy -> \"%s\" applied=%d rejected=%d "
                        "unknown=%d theme=%d\n",
                txt.c_str(), rr.applied, rr.rejected, rr.unknown,
                app.themeVariant == ui_theme::VariantLight ? 1 : 0);
        check(rr.parsed && rr.rejected == 0 && rr.unknown == 0,
              "W5 the copied text is a document this build reads");
        check(rr.applied == 1 && app.themeVariant == ui_theme::VariantLight,
              "W5 ...and it puts the value back, exactly (parse o emit = identity)");
        // The same inverse property for a Custom float. This specifically
        // guards settingsCurrentValue's decimal precision rather than only the
        // parser's acceptance of a friendly value such as 1.25.
        baseline();
        const float customGamma = 1.2345678f;
        app.dispGamma = customGamma;
        const std::string gtxt = settingsKeyAsJsonc(SK_gamma);
        baseline();
        const SettingsReport gr = loadSettingsFrom(writeFix(gtxt));
        check(gr.parsed && gr.rejected == 0 && gr.applied == 1 &&
              displayGammaBits(app.dispGamma) == displayGammaBits(customGamma),
              "W5 Custom gamma copies as JSONC and returns bit-for-bit");
        // A key at its DEFAULT is copied as a COMMENT, not as a line: writing a
        // default and leaving it unset are different acts the day the default
        // moves (判断9). So the round trip of a defaulted key applies nothing -
        // which is the correct identity for "this key is not set".
        baseline();
        const std::string dtxt = settingsKeyAsJsonc(SK_dragPans);
        const SettingsReport dr = loadSettingsFrom(writeFix(dtxt));
        fprintf(stderr, "settingsselftest: W5 default key -> \"%s\" applied=%d\n",
                dtxt.c_str(), dr.applied);
        check(dr.parsed && dr.applied == 0 &&
              dtxt.find("// \"dragPans\"") != std::string::npos,
              "W5 a key at its default copies as a comment, and applies nothing");
    }

    // ---- M1..M3: the measuring section, and the two watch keys --------------
    // 判断10/11 in the flesh. The measuring class is read now because its
    // declaration exists: this budget's consequence is FEWER FRAMES, and a
    // short stack already says "n of N" wherever sigma_t is printed
    // (docs/analysis-layers.md §8). A measuring setting that changed a number
    // and left no trace on it is what M1 forbids - this one leaves the trace
    // the canon already required, which is why it could be opened at all.
    {
        baseline();
        app.memBudgetGB = 0;                       // automatic
        loadSettingsFrom(writeFix("{ \"measuring\": { \"memoryBudgetGB\": 8 } }\n"));
        const float got = app.memBudgetGB;
        // ...and a value outside the window is REFUSED, not clamped. A file
        // that asked for 0.1 GB meant something, and quietly giving it 0.5
        // would answer a question it did not ask (判断8, the gamma precedent).
        baseline();
        app.memBudgetGB = 2.0f;
        const SettingsReport rr =
            loadSettingsFrom(writeFix("{ \"measuring\": { \"memoryBudgetGB\": -1 } }\n"));
        fprintf(stderr, "settingsselftest: M budget: 8 -> %.9g | -1 rejected=%d value=%.9g\n",
                got, rr.rejected, app.memBudgetGB);
        check(got == 8.0f, "M1 measuring.memoryBudgetGB is read (the section is open)");
        check(rr.rejected == 1 && app.memBudgetGB == 2.0f,
              "M2 ...and a value outside 0 or 0.5..4096 is refused, never clamped");
        // 段2e: the two keys the inventory left un-adjudicated, ruled on
        // 2026-08-13. They are loading-class: whether a file that moved on disk
        // is noticed at all is a question about how this build LOADS.
        baseline();
        app.watchEnabled = true; app.watchAutoReload = false;
        const SettingsReport wr = loadSettingsFrom(writeFix(
            "{ \"loading\": { \"watchFiles\": false, \"watchAutoReload\": true } }\n"));
        fprintf(stderr, "settingsselftest: M3 watch: enabled=%d auto=%d unknown=%d\n",
                app.watchEnabled ? 1 : 0, app.watchAutoReload ? 1 : 0, wr.unknown);
        check(!app.watchEnabled && app.watchAutoReload && wr.unknown == 0,
              "M3 the watch keys are read, and are no longer 'unknown key'");
    }

    // ---- D1..D6: how far "all stacks below" reaches (#204) ------------------
    // The defect this closes was not a wrong number, it was TWO numbers: the
    // local walk used a literal 3 and the ask sent to the peer a literal 6, so
    // the same folder answered differently depending on which door opened it.
    // The ruling (2026-08-17) makes the depth a setting with 6 as its default,
    // and the shape that keeps it settled is that there is ONE reader -
    // scanDepthBelow() in core/app/state.h - which both doors call.
    //
    // So D5 does not assert "the local walk uses 6". It asserts that the local
    // walk uses WHAT THE SETTING SAYS, at two depths on either side of a tree's
    // boundary, which is the only thing a literal cannot do. The peer door needs
    // no test of its own for the same reason a shared function needs no second
    // test: nav.cpp passes scanDepthBelow() into session->scan(), so it cannot
    // disagree with the local walk without that line being edited.
    {
        baseline();
        loadSettingsFrom(missing);                  // no file: the plain state
        // D1: the default is 6, and 6 is what the reader answers. Also 判断9:
        // the default WRITES NOTHING, so "chose 6" and "never chose" stay
        // different facts the day the default moves.
        const int def = scanDepthBelow();
        const bool defaultIsSilent = settingsCurrentValue(SK_scanDepth).empty();
        // D2: ...and the file moves it. 2 is the opposite of the default in the
        // only sense that matters here - it is a depth the old literals never
        // had - so a loader that did nothing fails this line.
        const SettingsReport dr =
            loadSettingsFrom(writeFix("{ \"loading\": { \"folderScanDepth\": 2 } }\n"));
        dumpNotes(dr);
        fprintf(stderr, "settingsselftest: D default=%d silent=%d | file 2 -> "
                        "field=%d reader=%d unknown=%d\n",
                def, defaultIsSilent ? 1 : 0, app.folderScanDepth, scanDepthBelow(),
                dr.unknown);
        check(def == 6 && defaultIsSilent,
              "D1 loading.folderScanDepth defaults to 6 and writes nothing at it");
        check(dr.applied == 1 && dr.unknown == 0 && app.folderScanDepth == 2 &&
              scanDepthBelow() == 2,
              "D2 ...and the file sets it, through the one reader both doors use");

        // D3: outside the window it is REFUSED, not clamped - the accent and
        // displayGamma answer (判断8). A file that asked for 40 meant something,
        // and quietly handing back 16 would answer a question it did not ask.
        baseline();
        const SettingsReport tooDeep =
            loadSettingsFrom(writeFix("{ \"loading\": { \"folderScanDepth\": 40 } }\n"));
        const int afterDeep = app.folderScanDepth;
        baseline();
        const SettingsReport fraction =
            loadSettingsFrom(writeFix("{ \"loading\": { \"folderScanDepth\": 2.5 } }\n"));
        const int afterFrac = app.folderScanDepth;
        fprintf(stderr, "settingsselftest: D3 40 -> rejected=%d value=%d | 2.5 -> "
                        "rejected=%d value=%d\n",
                tooDeep.rejected, afterDeep, fraction.rejected, afterFrac);
        check(tooDeep.rejected == 1 && afterDeep == 6 &&
              fraction.rejected == 1 && afterFrac == 6,
              "D3 a depth outside 1..16, or a fractional one, is refused not clamped");

        // D4: the CLAMP, which is the reader's and not the file's. prefs.txt is
        // plain text a user can edit and the panel's InputInt has no bounds, so
        // a value that never passed the applier still has to reach a walk sane.
        baseline();
        app.folderScanDepth = 40;                  // as a hand-edited prefs.txt
        const int clampedHigh = scanDepthBelow();
        app.folderScanDepth = 0;                   // "all stacks below" is not "none"
        const int clampedLow = scanDepthBelow();
        fprintf(stderr, "settingsselftest: D4 reader clamp: 40 -> %d, 0 -> %d\n",
                clampedHigh, clampedLow);
        check(clampedHigh == 16 && clampedLow == 1,
              "D4 the one reader clamps a value no applier vetted (1..16)");

        // D5: THE SETTING MOVES THE LOCAL DOOR. A tree in this test's own TMP
        // (never in VIEWER_TEST_HOME, and never inside a shared fixture folder:
        // both are this suite's recorded ways to poison every test that runs
        // after it), with two stacks - one just under the root and one five
        // levels down - so that a scan is never asserted to return NOTHING. The
        // shallow group is the anti-vacuity anchor: it has to be there at every
        // depth, or "the deep one is missing" would also be produced by a
        // fixture that never got written.
        baseline();
        const std::string tree = selftestScratch("viewer_settings_scandepth");
        {
            auto writeNpy = [](const std::string& path) {
                std::vector<float> px(4, 1.0f);
                std::vector<uint8_t> b = selftestNpyBytes("<f4", { 2, 2 }, px.data(),
                                                          px.size() * sizeof(float));
                std::ofstream f(pathFromUtf8(path), std::ios::binary);
                f.write((const char*)b.data(), (std::streamsize)b.size());
            };
            std::error_code dec;
            std::filesystem::create_directories(pathFromUtf8(tree + "/L1/L2/L3/L4/L5"), dec);
            writeNpy(tree + "/L1/near_001.npy");
            writeNpy(tree + "/L1/near_002.npy");
            writeNpy(tree + "/L1/L2/L3/L4/L5/deep_001.npy");
            writeNpy(tree + "/L1/L2/L3/L4/L5/deep_002.npy");
        }
        auto scanAt = [&](int depth, bool& sawDeep) {
            app.folderScanDepth = depth;
            std::vector<App::PendingGroup> gs = scanFolderGroups(tree);
            sawDeep = false;
            for (const App::PendingGroup& g : gs)
                if (g.name.find("deep_") != std::string::npos) sawDeep = true;
            return (int)gs.size();
        };
        bool deepAt2 = false, deepAt4 = false, deepAt5 = false, deepAtDefault = false;
        const int n2 = scanAt(2, deepAt2);
        const int n4 = scanAt(4, deepAt4);
        const int n5 = scanAt(5, deepAt5);
        app.folderScanDepth = 6;
        const int nDef = scanAt(6, deepAtDefault);
        fprintf(stderr, "settingsselftest: D5 local scan of a 5-deep tree: "
                        "depth2 -> %d group(s) deep=%d | depth4 -> %d deep=%d | "
                        "depth5 -> %d deep=%d | depth6 (default) -> %d deep=%d\n",
                n2, deepAt2 ? 1 : 0, n4, deepAt4 ? 1 : 0, n5, deepAt5 ? 1 : 0,
                nDef, deepAtDefault ? 1 : 0);
        check(n2 == 1 && !deepAt2 && n4 == 1 && !deepAt4,
              "D5 a shallow setting bundles only what is within it (the deep stack is unseen)");
        // The boundary, and it is what the number MEANS: the stack lives five
        // levels under the root, so depth 5 is the first that reaches it. The
        // old local literal was 3 and could not, whatever the peer was told.
        check(n5 == 2 && deepAt5 && nDef == 2 && deepAtDefault,
              "D5 ...and 5 - the depth of the tree - reaches it, as does the default 6");
        std::filesystem::remove_all(pathFromUtf8(tree), ec);

        // D6: the badge on a key appended at the END of the enum. The origin
        // ledger is an array indexed by SKey and its bound was SK_defaultUnit,
        // the last id on the day it was written - so every key added after that
        // line (memoryBudgetGB and the two watch keys) silently lost its badge
        // and its "this change springs back" warning, which is #50's own
        // symptom inside the mechanism built to end it. The bound is SK_Count
        // now. Asserted on the NEW key because a key at the end of the enum is
        // the case that was broken, and the next one will be added there too.
        baseline();
        loadSettingsFrom(missing);
        auto badgeOf = [&](SKey id) {
            for (const PrefRow& r : settingsPrefRows()) if (r.id == id) return r.badge;
            return std::string("<no row>");
        };
        const std::string bNone = badgeOf(SK_scanDepth);
        loadSettingsFrom(writeFix("{ \"loading\": { \"folderScanDepth\": 4 } }\n"));
        const std::string bFile = badgeOf(SK_scanDepth);
        const std::string bWatch = badgeOf(SK_watchAuto);   // the same bug's other victim
        loadSettingsFrom(writeFix("{ \"loading\": { \"watchAutoReload\": true } }\n"));
        const std::string bWatch2 = badgeOf(SK_watchAuto);
        fprintf(stderr, "settingsselftest: D6 folderScanDepth badge: \"%s\" -> \"%s\" | "
                        "watchAutoReload: \"%s\" -> \"%s\"\n",
                bNone.c_str(), bFile.c_str(), bWatch.c_str(), bWatch2.c_str());
        check(bNone == "default" && bFile.rfind("settings.jsonc:", 0) == 0,
              "D6 a key at the end of the enum still names the file that decides it");
        check(bWatch2.rfind("settings.jsonc:", 0) == 0,
              "D6 ...and so do the three keys the old ledger bound had dropped");
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
