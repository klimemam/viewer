// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: Browse
    // --browse-keys-selftest: connect the local peer and open the panel; the
    // action list is replayed inside the loop, below.
    std::vector<std::string> keyActs;
    size_t keyAct = 0;
    int keyPhase = 0;
    int keysCheckBad = 0;      // panel-geometry and empty-state checks that failed
    bool keysOk = false;
    if (!g_browseKeys.empty()) {
        std::string d = g_browseKeys;
        std::replace(d.begin(), d.end(), '\\', '/');
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        startRemote(rbMain(), "local://" + d);
        app.showRemote = true;
        // Scripted row indices assume the DEFAULT view: the user's real prefs
        // must not leak a flat/tree state into what "down,down,down" lands on.
        // With rbtree left on, a click+double-click pair on a folder row
        // expanded it in place and its children took over the row indices
        // every later action was aimed at - reproduced, two hours of forensics.
        rbMain().flat = app.rbFlat = false;
        rbMain().tree = app.rbTree = false;
        // ...and the same for the NAME ORDER, for the same reason: with
        // rbnatural off in the developer's prefs, rb/unpadded lists lv1, lv10,
        // lv2 and every chkatrow below it is aimed at the wrong row.
        rbMain().nameNatural = app.rbNatural = true;
        // Key REPEAT is a human affordance; for injected keys it is a race.
        // Each scripted press is held for ~one frame, but a trickle-delayed
        // release across a slow frame (texture uploads after a stack lands)
        // crossed the 275 ms delay and stepped the cursor twice per Down.
        ImGui::GetIO().KeyRepeatDelay = 10.0f;
        for (size_t i = 0, j; i <= g_browseKeysActs.size(); i = j + 1) {
            j = g_browseKeysActs.find(',', i);
            if (j == std::string::npos) j = g_browseKeysActs.size();
            if (j > i) keyActs.push_back(g_browseKeysActs.substr(i, j - i));
        }
        fprintf(stderr, "browsekeys: %d action(s) on %s (sizeof(BrowseInstance)=%zu)\n",
                (int)keyActs.size(), d.c_str(), sizeof(App::BrowseInstance));
    }
