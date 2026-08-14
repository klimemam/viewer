// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
//
// --rawrecipe-selftest: named recipes for headerless RAW, issue #166.
//
// THE RULING THIS PINS (the user's, 2026-08-11). Nothing is ever auto-applied
// from a SAVED recipe. What this session was told to use is remembered BY FILE
// SIZE and applies to the next file of that size, saying which recipe it was.
// That binding is process-only: it is in neither settings.jsonc, prefs.txt nor
// the session file, and it dies with the process. The LIBRARY of named recipes
// persists; the BINDING never does.
//
// Why the split is worth a test of its own: a headerless file is the one shape
// that cannot contradict a wrong reading. 4096x3000 and 3000x4096 are the same
// byte count and a transposed frame looks perfectly normal. So the only thing
// allowed to apply unpicked is a choice the user made minutes ago and still
// remembers - and "still remembers" is exactly what a restart ends.
//
// FIXTURES GO IN THIS TEST'S OWN TEMP DIRECTORY, at paths this test states.
// Its subject is how a settings.jsonc is READ, so the file it reads must be one
// it placed, not one the app discovered. Until 2026-08-14 there was a second
// reason and it was harsher: APPDATA/HOME were ONE folder shared by every
// selftest (only TMP/TEMP/TMPDIR were per-test), so a settings.jsonc written
// there was read by every test that ran afterwards AND survived the run - the
// a9b7973 shape. CMakeLists.txt now mints a config dir per test, so that half
// is gone; this half is not, and it is the one that made the rule.
// It is also not built inside a shared fixture folder (ad2d4fc): a leftover
// subdirectory of tools/testdata/rb is a ROW of that folder.
//
// ANTI-VACUITY, and it is the whole difficulty here. "The cache applied the
// recipe" passes for free if the dialog would have produced the same picture
// anyway. So the fixtures are built so that only the cache can explain the
// outcome, and the DEFAULT IS ASSERTED TOO (B0):
//
//   * the files are called *_48x16.bin, so openRawDialogFor's filename scan
//     sets the dialog to 48 x 16, and no hint in them says anything but u8;
//   * 48*16*1 = 768 bytes, which is INSIDE the 1536-byte file, so the default
//     decodes perfectly well and produces a plausible picture. The two readings
//     do not differ by one failing;
//   * the recipe says 32 x 24 u16, which is 1536 bytes exactly;
//   * the first two bytes differ per file (alpha 34 12, beta 78 56), so the
//     first pixel is 4660 for alpha and 22136 for beta as u16 LE, and 52 / 120
//     as u8. Every assertion below names the number it expects and where it
//     came from.
//
// Derivations, by hand: 0x1234 = 4096 + 2*256 + 3*16 + 4 = 4660.
//                       0x5678 = 5*4096 + 6*256 + 7*16 + 8 = 22136.
//                       32 * 24 * 2 = 1536 = the whole file.
//                       48 * 16 * 1 = 768  = half of it, and it fits.
static int rawRecipeSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "rawrecipeselftest: %-66s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    auto say = [&](const std::string& s) {
        fprintf(stderr, "rawrecipeselftest:   %s\n", s.c_str());
    };
    auto anyNote = [](const SettingsReport& r, const std::string& needle) {
        for (const std::string& n : r.notes)
            if (n.find(needle) != std::string::npos) return true;
        return false;
    };
    auto dumpNotes = [&](const SettingsReport& r) {
        if (!r.parseError.empty()) say("refused: " + r.parseError);
        for (const std::string& n : r.notes) say("note: " + n);
    };

    std::error_code ec;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "viewer_rawrecipes_selftest";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    fprintf(stderr, "rawrecipeselftest: fixtures in %s\n", root.u8string().c_str());

    // A headerless file: n bytes, the first two chosen so the first pixel is a
    // number this test can name, the rest a ramp so nothing is uniformly zero.
    auto writeRaw = [&](const std::filesystem::path& p, size_t n, uint8_t b0, uint8_t b1) {
        std::vector<uint8_t> buf(n);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)((i * 3 + 11) & 0xFF);
        if (n >= 2) { buf[0] = b0; buf[1] = b1; }
        std::ofstream f(p, std::ios::binary);
        f.write((const char*)buf.data(), (std::streamsize)n);
        return p.u8string();
    };
    const std::string A = writeRaw(root / "alpha_48x16.bin", 1536, 0x34, 0x12);
    const std::string B = writeRaw(root / "beta_48x16.bin",  1536, 0x78, 0x56);
    const std::string G = writeRaw(root / "gamma_48x16.bin", 1024, 0x34, 0x12);

    int fixN = 0;
    auto writeFix = [&](const std::string& body) {
        std::string p = (root / ("s" + std::to_string(++fixN) + ".jsonc")).u8string();
        std::ofstream f(pathFromUtf8(p), std::ios::binary);
        f.write(body.data(), (std::streamsize)body.size());
        return p;
    };

    // The library is the SUBJECT of half of this test, so nothing below
    // subscripts it directly. A run in which it failed to load would die on the
    // first `g_rawLibrary[0]` and every assertion after that would be silent -
    // and that run is precisely the RED one, the only run whose whole output is
    // the evidence. The L group checks the size before indexing; everything
    // else goes through here and gets a default recipe, which then fails its
    // own assertions honestly instead of taking the process with it.
    auto libAt = [&](size_t i) {
        return i < g_rawLibrary.size() ? g_rawLibrary[i] : RawRecipe{};
    };

    auto reset = [&]() {
        closeAll();
        app.seqQueue.clear();
        app.folderRecipeValid = false;
        rawDlg = RawDialog{};
        g_rawSizeRecipe.clear();
        app.toast.clear();
        app.seqLoadMode = 2;              // "never": the stack offer is not the subject
    };

    // ======================================================================
    //  L — THE LIBRARY, in settings.jsonc
    //
    //  It persists, it is written by hand, and every word in it is a word
    //  that already existed: RAW_DTYPE_NAMES, RAW_INTERP_CLI, CFA_PATTERNS.
    // ======================================================================
    const std::string libText =
        "{\n"
        "  \"loading\": {\n"
        "    \"rawRecipes\": [\n"
        "      // our 32x24 sensor dump\n"
        "      { \"name\": \"MyCam 32x24\", \"size\": [32, 24], \"dtype\": \"u16\",\n"
        "        \"interpretation\": \"gray\" },\n"
        "      { \"name\": \"MyCam transposed\", \"size\": [16, 48], \"dtype\": \"u16\",\n"
        "        \"interpretation\": \"gray\" },\n"
        "      { \"name\": \"every field\", \"size\": [32, 24], \"dtype\": \"u16\",\n"
        "        \"interpretation\": \"bayer\", \"bayerPattern\": \"GBRG\",\n"
        "        \"byteOrder\": \"big\", \"headerBytes\": 0, \"crop\": [0, 0, 16, 8] },\n"
        "    ],\n"
        "  },\n"
        "}\n";
    const std::string libPath = writeFix(libText);
    {
        reset();
        SettingsReport r = loadSettingsFrom(libPath);
        dumpNotes(r);
        say("library holds " + std::to_string(g_rawLibrary.size()) + " recipe(s), applied " +
            std::to_string(r.applied) + ", rejected " + std::to_string(r.rejected) +
            ", unknown " + std::to_string(r.unknown));
        check(r.parsed && g_rawLibrary.size() == 3 && r.rejected == 0 && r.unknown == 0,
              "L1 loading.rawRecipes reads three named recipes");
        // ONE key applied, whatever the array's length - the accounting
        // readers.searchPath keeps and --settings-selftest S1 counts.
        check(r.applied == 1, "L1 ...as ONE applied key, not one per recipe");
        check(g_rawLibrary.size() == 3 && g_rawLibrary[0].name == "MyCam 32x24" &&
              g_rawLibrary[0].w == 32 && g_rawLibrary[0].h == 24 &&
              g_rawLibrary[0].dtype == RD_U16 && g_rawLibrary[0].interp == RI_GRAY &&
              g_rawLibrary[0].littleEndian && g_rawLibrary[0].offset == 0 &&
              !g_rawLibrary[0].cropOn && g_rawLibrary[0].fromLibrary,
              "L2 name / size / dtype / interpretation land where they say");
        // Every remaining field, on one recipe, so a field that parses into the
        // wrong member cannot hide behind a default that happens to match.
        // GBRG is index 3 of CFA_PATTERNS { RGGB, BGGR, GRBG, GBRG }.
        check(g_rawLibrary.size() == 3 && g_rawLibrary[2].interp == RI_BAYER &&
              g_rawLibrary[2].cfaPattern == 3 && !g_rawLibrary[2].littleEndian &&
              g_rawLibrary[2].cropOn && g_rawLibrary[2].cropX == 0 &&
              g_rawLibrary[2].cropY == 0 && g_rawLibrary[2].cropW == 16 &&
              g_rawLibrary[2].cropH == 8,
              "L3 bayerPattern / byteOrder / crop land too (GBRG is index 3)");
    }
    {
        // L4: one bad recipe is skipped BY NAME and the others still load -
        // 判断8's granularity, one level down into the array. A user with six
        // good recipes and a typo in the seventh keeps six.
        reset();
        std::string p = writeFix(
            "{ \"loading\": { \"rawRecipes\": [\n"
            "  { \"name\": \"bad dtype\", \"size\": [8, 8], \"dtype\": \"u12\" },\n"
            "  { \"name\": \"good\", \"size\": [8, 8], \"dtype\": \"u8\" }\n"
            "] } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && g_rawLibrary.size() == 1 && g_rawLibrary[0].name == "good",
              "L4 a recipe that cannot be read is skipped, the rest still load");
        check(anyNote(r, "rawRecipes[0].dtype") && anyNote(r, "\"u16\""),
              "L4 ...named by its index and field, with the alphabet it wanted");
    }
    {
        // L5: no name and no size are both refusals, not defaults. A recipe
        // with no name cannot be picked or reported; one with no size is not a
        // recipe for a headerless file at all, and a defaulted geometry is a
        // picture of the wrong shape that nobody can tell is wrong.
        reset();
        std::string p = writeFix(
            "{ \"loading\": { \"rawRecipes\": [\n"
            "  { \"size\": [8, 8], \"dtype\": \"u8\" },\n"
            "  { \"name\": \"no size\", \"dtype\": \"u8\" },\n"
            "  { \"name\": \"typo\", \"size\": [8, 8], \"dtyep\": \"u8\" }\n"
            "] } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && g_rawLibrary.size() == 1 && g_rawLibrary[0].name == "typo",
              "L5 a recipe with no name, or no size, is refused - not defaulted");
        check(anyNote(r, "a \"name\"") && anyNote(r, "\"size\": [width, height]"),
              "L5 ...each refusal says which of the two is missing");
        check(anyNote(r, "rawRecipes[2].dtyep") && anyNote(r, "not a field of a raw recipe"),
              "L5 ...and a misspelt FIELD is named while its recipe still loads");
    }

    // ======================================================================
    //  N — WHAT A SAVED RECIPE DOES ON ITS OWN: NOTHING
    //
    //  This is the ruling's first line and the reason option A was chosen
    //  over the one I recommended. It has to be an assertion, because it is
    //  the behaviour that is easiest to "improve" away.
    // ======================================================================
    {
        reset();
        loadSettingsFrom(libPath);                // three recipes, one of them 32x24
        const size_t before = app.images.size();
        openPath(A);                              // 1536 bytes: recipe 0 fits exactly
        check(rawDlg.open && app.images.size() == before,
              "N1 with a library loaded, the first headerless file still ASKS");
        check(g_rawSizeRecipe.empty(),
              "N1 ...and nothing was bound to a size by merely having a library");
        rawDlg.open = false;
    }

    // ======================================================================
    //  B — THE SESSION'S SIZE BINDING
    // ======================================================================
    {
        reset();
        loadSettingsFrom(libPath);

        // B0 - THE ANTI-VACUITY ANCHOR. What the dialog does with beta when
        // nothing has been chosen. Every "the cache did it" assertion below is
        // a claim only because this one holds: 48x16 u8 is a DIFFERENT and
        // perfectly successful reading of the same bytes.
        openRawDialogFor(B);
        say("a fresh dialog for beta_48x16.bin: " + std::to_string(rawDlg.w) + "x" +
            std::to_string(rawDlg.h) + " " + RAW_DTYPE_NAMES[rawDlg.dtype]);
        check(rawDlg.open && rawDlg.w == 48 && rawDlg.h == 16 && rawDlg.dtype == RD_U8,
              "B0 a fresh dialog defaults to 48x16 u8 here (the filename's hint)");
        {   // ...and it really would have decoded. 48*16*1 = 768 <= 1536.
            RawDialog probe = rawDlg;
            probe.replaceIdx = -1;
            std::string err = loadRaw(probe);
            const bool got = err.empty() && !app.images.empty() &&
                             app.images.back()->w == 48 && app.images.back()->h == 16 &&
                             app.images.back()->src->data[0] == 120.0f;
            check(got, "B0 ...and that default DECODES: 48x16, first pixel 0x78 = 120");
        }
        reset();
        loadSettingsFrom(libPath);

        // B1: the first file of the session goes through the dialog.
        size_t before = app.images.size();
        openPath(A);
        check(rawDlg.open && app.images.size() == before,
              "B1 the first headerless file of a session raises the dialog");

        // B2: the user picks "MyCam 32x24" in the dialog and presses Load. That
        // is rawDialogAccept - the same call the button makes.
        rawRecipeIntoDialog(libAt(0), rawDlg);
        std::string err = rawDialogAccept(rawDlg);
        rawDlg.open = false;
        const ImageDoc* a = app.images.empty() ? nullptr : app.images.back().get();
        check(err.empty() && a && a->w == 32 && a->h == 24 &&
              a->src->data[0] == 4660.0f,
              "B2 Load decodes 32x24 u16, first pixel 0x1234 = 4660");
        const RawRecipe* bound = rawRecipeForSize(1536);
        check(bound != nullptr, "B2 ...and 1536 bytes is now bound to a recipe");
        // The NAME is the library's, which is only possible if the library was
        // consulted: an anonymous recipe would have called itself "32x24 u16 gray".
        check(bound && bound->fromLibrary && bound->name == "MyCam 32x24",
              "B2 ...under the LIBRARY's name, not a name derived from the fields");
        check(rawRecipeForSize(1024) == nullptr,
              "B2 ...and no other size was bound by it");

        // B3: THE FEATURE. Another file of the same byte count, no dialog, and
        // the recipe rather than the dialog's own 48x16 u8 default.
        before = app.images.size();
        app.toast.clear();
        openPath(B);
        const ImageDoc* b = app.images.empty() ? nullptr : app.images.back().get();
        say("beta opened as " + (b ? std::to_string(b->w) + "x" + std::to_string(b->h) +
                                     " first pixel " + std::to_string((double)b->src->data[0])
                                   : std::string("nothing")));
        check(!rawDlg.open && app.images.size() == before + 1,
              "B3 the next file of the same size opens WITHOUT the dialog");
        check(b && b->w == 32 && b->h == 24,
              "B3 ...with the recipe's 32x24, not the dialog's 48x16 default");
        check(b && b->src->data[0] == 22136.0f,
              "B3 ...and beta's own bytes read as u16 LE: 0x5678 = 22136");
        // B4: it says which recipe, once, and why it was in a position to apply.
        say("the line: " + app.toast);
        check(app.toast.find("MyCam 32x24") != std::string::npos &&
              app.toast.find("1536-byte") != std::string::npos &&
              app.toast.find("beta_48x16.bin") != std::string::npos,
              "B4 the applied recipe is NAMED, with the byte count that matched");

        // B5: the binding is by SIZE and by nothing else. gamma has the same
        // name shape and the same extension in the same folder, and 1024 bytes.
        before = app.images.size();
        openPath(G);
        check(rawDlg.open && app.images.size() == before,
              "B5 a file of a DIFFERENT size still asks (the key is the byte count)");
        rawDlg.open = false;

        // B6: last explicit choice wins. The user re-reads what is on screen
        // with the transposed recipe - the exact confusion a headerless file
        // cannot defend itself against - and the binding follows.
        std::string rerr = rawRecipeReinterpretCurrent(libAt(1));
        const RawRecipe* now = rawRecipeForSize(1536);
        check(rerr.empty() && now && now->name == "MyCam transposed" &&
              now->w == 16 && now->h == 48,
              "B6 applying another recipe to that size REPLACES the binding");
        before = app.images.size();
        openPath(B);
        const ImageDoc* b2 = app.images.empty() ? nullptr : app.images.back().get();
        check(app.images.size() == before + 1 && b2 && b2->w == 16 && b2->h == 48,
              "B6 ...and the next file of that size uses the NEW one");
        check(g_rawSizeRecipe.size() == 1,
              "B6 ...one entry per size, updated rather than added to");
    }

    // ======================================================================
    //  F — the same rule at the OTHER headerless door: Open Folder
    //
    //  "I have already told you how to read files of this size" cannot mean
    //  two different things depending on which door the file came through.
    // ======================================================================
    {
        reset();
        loadSettingsFrom(libPath);
        // Bind 1536 through the dialog, exactly as B2 did.
        openPath(A);
        rawRecipeIntoDialog(libAt(0), rawDlg);
        rawDialogAccept(rawDlg);
        rawDlg.open = false;
        closeAll();

        App::PendingGroup g;
        g.files.push_back(B);
        g.name = "folder/beta";
        g.isRaw = true;
        std::vector<App::PendingGroup> gs;
        gs.push_back(g);
        enqueueGroups(std::move(gs));
        startNextQueuedGroup();               // a no-op if enqueue already drained it
        const ImageDoc* f = app.images.empty() ? nullptr : app.images.back().get();
        check(!rawDlg.open && f && f->w == 32 && f->h == 24,
              "F1 Open Folder does not ask again for a size this session answered");
        check(app.folderRecipeValid,
              "F1 ...and it becomes that Open's recipe, through the existing machinery");
    }

    // ======================================================================
    //  P — IT IS WRITTEN NOWHERE, AND IT SURVIVES NOTHING
    //
    //  The half a happy-path test would pass without. Every one of these
    //  would fail on an implementation that persisted the binding.
    // ======================================================================
    {
        reset();
        loadSettingsFrom(libPath);
        openPath(A);
        rawRecipeIntoDialog(libAt(1), rawDlg);   // "MyCam transposed", 16x48
        rawDialogAccept(rawDlg);
        rawDlg.open = false;
        check(rawRecipeForSize(1536) != nullptr,
              "P0 a binding is live (so the four checks below are not vacuous)");

        // P1/P2: the two files the app DOES write. The recipe's name is the
        // marker - nothing else in this program knows that string - and the
        // byte count is the other half, because an implementation could
        // serialise the geometry without the name and still have persisted it.
        std::ostringstream prefs;
        writePrefsTo(prefs);
        const std::string ptxt = prefs.str();
        check(ptxt.find("MyCam transposed") == std::string::npos &&
              ptxt.find("1536") == std::string::npos,
              "P1 prefs.txt carries neither the recipe nor the byte count");
        std::ostringstream sess;
        writeSessionTo(sess);
        const std::string stxt = sess.str();
        check(stxt.find("MyCam transposed") == std::string::npos &&
              stxt.find("1536") == std::string::npos,
              "P2 the session file carries neither either");
        // ...and the session DOES carry the open document, so P2 is a statement
        // about the binding rather than about an empty file.
        check(stxt.find("alpha_48x16.bin") != std::string::npos,
              "P2 ...while still carrying the document that was open");

        // P3: the template. The LIBRARY is legitimately in it - it is the
        // migration, and a template that dropped the user's recipes would lose
        // them - so what must be absent is the byte count, i.e. the binding.
        const std::string tmpl = settingsTemplateText();
        check(tmpl.find("MyCam transposed") != std::string::npos &&
              tmpl.find("rawRecipes") != std::string::npos,
              "P3 --settings-template carries the library it read");
        check(tmpl.find("1536") == std::string::npos,
              "P3 ...and no size binding: that is not a setting");

        // P4: THE RESTART. A new process starts with an empty map by
        // construction - nothing initialises it from anywhere - so what has to
        // be asserted is that nothing a restart READS puts anything back. Every
        // loader a start runs is replayed here over files written while a
        // binding was live.
        const std::string sessPath = (root / "restart.vsession").u8string();
        saveSession(sessPath, true);
        const size_t liveBefore = g_rawSizeRecipe.size();
        reset();                                  // the process ends here
        std::string serr = loadSession(sessPath); // ...and starts again
        loadSettingsFrom(libPath);
        say("after the restart: " + std::to_string(g_rawSizeRecipe.size()) +
            " binding(s), " + std::to_string(g_rawLibrary.size()) + " library recipe(s)" +
            (serr.empty() ? "" : ", session error: " + serr));
        check(liveBefore == 1, "P4 a binding existed before the restart");
        check(g_rawSizeRecipe.empty(),
              "P4 ...and NOTHING a restart reads puts one back");
        check(g_rawLibrary.size() == 3,
              "P4 ...while the library, which does persist, came back whole");
        // The document came back, so the session really was reloaded and P4 is
        // not passing because the restart did nothing at all.
        check(!app.images.empty(),
              "P4 ...and the session really did restore, so this is not a no-op");
    }

    // ======================================================================
    //  S — SAVING A RECIPE, which is "here is the line to paste"
    //
    //  判断3: the app never writes settings.jsonc. So the round trip is the
    //  promise - what the panel copies is what this reader reads back.
    // ======================================================================
    {
        reset();
        // Every field away from its default, so a field the emitter forgets
        // cannot hide behind a default that happens to match on the way back.
        RawDialog d;
        d.w = 37; d.h = 11; d.offset = 13;
        d.dtype = RD_F32; d.interp = RI_QUAD;
        d.littleEndian = false;
        d.cfaPattern = 2;                    // GRBG
        d.cropOn = true;
        d.cropX = 4; d.cropY = 4; d.cropW = 8; d.cropH = 4;
        RawRecipe made = rawRecipeFromDialog(d);
        say("the derived name: " + made.name);
        check(!made.fromLibrary && made.name == rawRecipeSummary(made),
              "S1 a recipe nobody named calls itself what it is");
        const std::string line = rawRecipeJsonc(made);
        say("the line: " + line);
        std::string p = writeFix("{ \"loading\": { \"rawRecipes\": [ " + line + " ] } }\n");
        SettingsReport r = loadSettingsFrom(p);
        dumpNotes(r);
        check(r.parsed && r.rejected == 0 && r.unknown == 0 && g_rawLibrary.size() == 1,
              "S2 what the panel copies is read back by this file's own reader");
        check(g_rawLibrary.size() == 1 && g_rawLibrary[0].name == made.name &&
              rawRecipeSameFields(g_rawLibrary[0], made),
              "S2 ...field for field, name included");

        // S3: the template's round trip, with a library in it - the T3 promise
        // of --settings-selftest, extended to the one key whose value is a list
        // of objects rather than a scalar.
        loadSettingsFrom(libPath);
        const std::string tmpl = settingsTemplateText();
        std::string tp = writeFix(tmpl);
        SettingsReport tr = loadSettingsFrom(tp);
        dumpNotes(tr);
        check(tr.parsed && tr.rejected == 0 && tr.unknown == 0 && tr.notHere == 0,
              "S3 the template this program prints parses with its own reader");
        check(g_rawLibrary.size() == 3 && g_rawLibrary[0].name == "MyCam 32x24" &&
              g_rawLibrary[2].cfaPattern == 3 && !g_rawLibrary[2].littleEndian &&
              g_rawLibrary[2].cropOn && g_rawLibrary[2].cropW == 16,
              "S3 ...and every recipe comes back exactly as it went in");
    }

    // ======================================================================
    //  M — THE READ DECLARES ITSELF (docs/settings-inventory.md §4.1 M1)
    //
    //  Everything a headerless read uses is a measuring-class setting: the
    //  file states none of it, so every number taken off the picture depends
    //  on choices somebody made. M1 says such a setting has to appear in its
    //  own results. The note said "Bayer RGGB u16" and stopped - the two
    //  fields already visible on screen - and left out the three that are
    //  dangerous BECAUSE they are not visible: a transposed size, a wrong
    //  header offset and a swapped byte order each draw a picture that looks
    //  entirely normal.
    //
    //  Stated as "the note contains each field", not as one string equality,
    //  so re-wording the sentence does not fail this and dropping a field
    //  does.
    // ======================================================================
    {
        reset();
        loadSettingsFrom(libPath);
        openPath(A);
        rawRecipeIntoDialog(libAt(1), rawDlg);      // "MyCam transposed", 16x48
        rawDlg.offset = 0;
        rawDlg.littleEndian = false;                // ...and the field nothing shows
        rawDialogAccept(rawDlg);
        rawDlg.open = false;
        const ImageDoc* m = app.images.empty() ? nullptr : app.images.back().get();
        const std::string note = m ? m->note : std::string();
        auto has = [&](const char* s) { return note.find(s) != std::string::npos; };
        const bool size = has("16x48"), hdr = has("header 0 B"),
                   order = has("big-endian"), what = has("declared");
        fprintf(stderr, "rawrecipeselftest: M1 note = \"%s\"\n", note.c_str());
        check(m != nullptr, "M1 the fixture opened under a known recipe");
        check(size, "M1 the note carries the SIZE that was declared");
        check(hdr, "M1 ...the header offset, written even when it is zero");
        check(order, "M1 ...and the byte order, which nothing on screen shows");
        check(what, "M1 ...and says the shape was declared, not read");
        // anti-vacuity: a note that contained every substring by accident
        // would also contain one that is not true of this read
        check(!has("little-endian") && !has("32x24"),
              "M1 the note is about THIS read and not a template");
    }

    reset();
    g_rawLibrary.clear();
    std::filesystem::remove_all(root, ec);
    fprintf(stderr, "rawrecipeselftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
