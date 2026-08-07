// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 横断(編集は行単位)
// ---------------------------------------------------------------- menu bar / dialogs

// Is what we are looking at somewhere else? A session being up counts (any
// Browse instance), and so does an image that came from one: a dropped
// connection does not make the pixels local. Drives the icon variant and the
// "[host]" in the title.
static bool viewingRemote() {
    const ImageDoc* im = cur();
    for (const auto& p : app.browsePanels)
        if (p->b.connected) return true;
    return im && !im->src->remoteUrl.empty();
}

// The host behind that, or "" for the local peer. The peer protocol is the same
// either way - deliberately - but the ANSWER to "is this somewhere else?" is
// not, and the title bar and the taskbar icon are answers to that question.
// With N instances the ACTIVE one answers first, then any connected remote.
static std::string viewingHost() {
    if (!viewingRemote()) return {};
    if (!app.browsePanels.empty() && !rbActive().b.host.empty() && rbActive().b.connected)
        return rbActive().b.host;
    for (const auto& p : app.browsePanels)
        if (p->b.connected && !p->b.host.empty()) return p->b.host;
    const ImageDoc* im = cur();
    if (im && !im->src->remoteUrl.empty()) {
        std::string h, p;
        remote::parseUrl(im->src->remoteUrl, h, p);
        return h;
    }
    return {};
}

// One title, three places: the OS title bar (when there is one), the taskbar
// button, and the integrated title bar this app draws for itself.
static std::string windowTitleText() {
    const ImageDoc* im = cur();
    // (stale) rides in the title because the title is where a report copies
    // the picture's name from (§10.1) - same vocabulary as the A/B panels.
    std::string t = im ? im->name + (avgIsStale(im) ? "  (stale)" : "") + " - viewer"
                       : std::string("viewer  ") + viewerVersion();
    // The bracket answers "which machine are these pixels on?". A local browse
    // goes through the peer protocol but the file is on this disk, so there is
    // no other machine to name - and "[local peer]" named one, which is how a
    // Browse Folder (Local) looked like an open connection. No bracket is the
    // true statement: it is a local file, exactly like File > Open.
    std::string host = viewingHost();
    if (!host.empty()) t += "  [" + host + "]";
    return t;
}

// The integrated title bar, drawn into the menu bar the app already has: the
// window title in the middle, the three window buttons on the right, and
// everything else registered as "drag here to move the window".
//
// Called from inside BeginMainMenuBar, after the last menu, so the menus'
// right edge is simply where the cursor has got to.
static void drawTitleBarExtras() {
    if (window_frame::mode() != window_frame::Integrated) return;
    const float s = app.uiScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;   // screen -> window coords
    const ImVec2 barPos = ImGui::GetWindowPos();
    const ImVec2 barSize = ImGui::GetWindowSize();
    const float menusEnd = ImGui::GetCursorScreenPos().x;
    const float bw = std::floor(46 * s), bh = barSize.y;
    const float btnX0 = barPos.x + barSize.x - bw * 3;

    // everything but the menus and the buttons drags the window
    window_frame::addCaption(barPos.x - vpPos.x, barPos.y - vpPos.y,
                             barPos.x + barSize.x - vpPos.x, barPos.y + bh - vpPos.y);
    window_frame::addExclusion(barPos.x - vpPos.x, barPos.y - vpPos.y,
                               menusEnd - vpPos.x, barPos.y + bh - vpPos.y);
    window_frame::addExclusion(btnX0 - vpPos.x, barPos.y - vpPos.y,
                               barPos.x + barSize.x - vpPos.x, barPos.y + bh - vpPos.y);

    // Title, centred in the bar but never under the menus or the buttons: it is
    // the only place the file name appears once the system title bar is gone.
    {
        const std::string title = windowTitleText();
        const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
        const float pad = 12 * s;
        const float lo = menusEnd + pad, hi = btnX0 - pad;
        if (hi - lo > 40 * s) {
            float tx = barPos.x + (barSize.x - ts.x) * 0.5f;
            tx = std::clamp(tx, lo, std::max(lo, hi - ts.x));
            dl->PushClipRect(ImVec2(lo, barPos.y), ImVec2(hi, barPos.y + bh), true);
            dl->AddText(ImVec2(tx, barPos.y + (bh - ts.y) * 0.5f),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), title.c_str());
            dl->PopClipRect();
        }
    }

    // Minimize / maximize / close. Drawn rather than typed: a glyph font would
    // have to be shipped and would still not match at every UI scale.
    const bool isMax = window_frame::maximized();
    const char* ids[3] = {"##wf_min", "##wf_max", "##wf_close"};
    for (int i = 0; i < 3; i++) {
        const ImVec2 p(btnX0 + bw * i, barPos.y);
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton(ids[i], ImVec2(bw, bh));
        const bool clicked = ImGui::IsItemClicked();
        // On Windows the OS owns the maximize button (it opens Snap Layouts on
        // hover), so its hover state has to come from there - no mouse message
        // for that rectangle ever reaches us.
        const bool hovered = ImGui::IsItemHovered() ||
                             (i == 1 && window_frame::maximizeButtonHot());
        if (hovered)
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh),
                              i == 2 ? IM_COL32(232, 72, 72, 255)
                                     : ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        const ImU32 fg = ImGui::GetColorU32(i == 2 && hovered ? ImVec4(1, 1, 1, 1)
                                                              : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        const ImVec2 c(p.x + bw * 0.5f, p.y + bh * 0.5f);
        const float g = std::floor(5 * s), th = std::max(1.0f, std::floor(s));
        if (i == 0) {
            dl->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), fg, th);
        } else if (i == 1 && !isMax) {
            dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, 0, 0, th);
        } else if (i == 1) {                       // restore: the "behind" pane too
            dl->AddRect(ImVec2(c.x - g, c.y - g + 2 * th), ImVec2(c.x + g - 2 * th, c.y + g),
                        fg, 0, 0, th);
            dl->AddLine(ImVec2(c.x - g + 2 * th, c.y - g), ImVec2(c.x + g, c.y - g), fg, th);
            dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x + g, c.y + g - 2 * th), fg, th);
        } else {
            dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), fg, th);
            dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x - g, c.y + g), fg, th);
        }
        if (i == 1)
            window_frame::setMaximizeButton(p.x - vpPos.x, p.y - vpPos.y,
                                            p.x + bw - vpPos.x, p.y + bh - vpPos.y);
        if (clicked) {
            if (i == 0) window_frame::minimize();
            else if (i == 1) window_frame::toggleMaximize();
            else window_frame::requestClose();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i == 0 ? "Minimize"
                                           : i == 1 ? (isMax ? "Restore" : "Maximize") : "Close");
    }
}

static void drawMenuBar(GLFWwindow* win) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...")) openFileDialog();
        if (ImGui::MenuItem("Open Folder...", SC_MOD "+Shift+O")) openFolderDialog();
        // §4.1: a reader must be reachable for a FOLDER as well as a file -
        // "one condition per folder" is a common way to store a sweep, so the
        // folder is often the thing that needs reading, not any file in it.
        if (ImGui::MenuItem("Open With a Reader...") && !app.rdOpenDlg) {
            app.rdOpenMode = 1;
            app.rdOpenDlg = std::make_unique<pfd::open_file>(
                "Open with a reader", ".", std::vector<std::string>{ "All files", "*" });
        }
        if (ImGui::MenuItem("Open Folder With a Reader...") && !app.rdFolderDlg) {
            app.rdFolderMode = 1;
            app.rdFolderDlg = std::make_unique<pfd::select_folder>("Open folder with a reader", ".");
        }
        // The local mirror of browsing a server: look around, preview, use the
        // server-side stats - without loading anything. Same Browse panel, the
        // peer just runs on this machine.
        if (ImGui::MenuItem("Browse Folder (Local)...")) browseFolderDialog();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("open a folder in the Browse panel: list, preview\n"
                              "and measure without loading anything");
        // "open folder" was a button in the Browse panel's "more" drawer. It is
        // a verb about a WHOLE folder, not about the panel's chrome, so it
        // belongs on the menu bar with the other openers (10.3). The crumb's
        // right-click keeps its own copy: there the subject is the folder the
        // crumb names, which is the more precise aim of the two.
        {
            App::BrowseInstance& I = rbActive();
            if (ImGui::MenuItem("Open Folder (all stacks below)...", nullptr, false,
                                I.b.connected))
                remoteScanFolder(I, I.b.dir);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(I.b.connected
                    ? "scan the folder the Browse panel is in and everything\n"
                      "below it, then pick which stacks to open:\n%s"
                    : "the Browse panel is not standing anywhere yet%s",
                    I.b.connected ? I.b.dir.c_str() : "");
        }
        // The OS dialog can only show THIS machine's disks (the NAS included,
        // since it is mounted). Files on a server need the ssh:// path, so they
        // need a place to type it.
        // The ONE remote entry point. "Open File (Remote)..." and "Open Folder
        // (Remote)..." used to sit under it and are gone: see the panel's own
        // toolbar. Revealing the panel here is what both of them really did.
        if (ImGui::MenuItem("Start Remote (ssh)...")) {
            app.showRemote = true;
            app.focusRemote = true;
            app.remoteDlgOpen = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("connect to a machine. Opening files and folders\n"
                              "happens in the Browse panel: click a file to look\n"
                              "at it, double-click to open it, \"open folder\" for\n"
                              "every stack below the folder you are in.");
        if (ImGui::MenuItem("Update remote peer", nullptr, false,
                            rbActive().b.connected)) {
            App::BrowseInstance& I = rbActive();   // the panel last worked in
            App::RbJob j; j.kind = App::RbUpdatePeer;
            j.host = I.b.host; j.port = I.b.port;
            rbEnqueue(I, std::move(j));
        }
        if (ImGui::MenuItem("Save Session...", SC_MOD "+S", false, !app.images.empty())) saveSessionDialog();
        {   // recovery: the autosave is written on exit, on crash and every 60 s.
            // With instance slots there can be several: scan them ALL, offer
            // the newest one no LIVE window owns, and say which one it is -
            // "last session" alone no longer names a single file.
            std::string offerName;
            std::string ap = recoveryOfferIn(viewerConfigDir(), currentPid(), &offerName);
            bool has = !ap.empty();
            std::string label = has ? "Restore last session (" + offerName + ")"
                                    : std::string("Restore last session");
            if (ImGui::MenuItem(label.c_str(), nullptr, false, has)) openPath(ap);
            if (has && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ap.c_str());
        }
        ImGui::Separator();
        {   // Close follows the layer canon (docs/terminology.md): Ctrl+W closes
            // the STACK when the current frame is in one, the image otherwise.
            // Ctrl+Alt+W is the one-frame escape hatch; Ctrl+Shift+W the batch.
            bool inStack = cur() && cur()->seqId != 0;
            if (ImGui::MenuItem(inStack ? "Close Stack" : "Close Image", SC_MOD "+W",
                                false, cur() != nullptr))
                closeCurrent();
            if (ImGui::MenuItem("Close Frame", SC_MOD "+Alt+W", false, inStack))
                closeCurrent(true);
            if (ImGui::MenuItem("Close Batch", SC_MOD "+Shift+W", false, cur() != nullptr))
                closeBatch(cur()->batchId);
        }
        if (ImGui::MenuItem("Close All", nullptr, false, !app.images.empty())) closeAll();
        ImGui::Separator();
        if (ImGui::BeginMenu("Stack loading")) {
            if (ImGui::MenuItem("Ask each time", nullptr, app.seqLoadMode == 0)) app.seqLoadMode = 0;
            if (ImGui::MenuItem("Always load folder", nullptr, app.seqLoadMode == 1)) app.seqLoadMode = 1;
            if (ImGui::MenuItem("Never (single file)", nullptr, app.seqLoadMode == 2)) app.seqLoadMode = 2;
            ImGui::Separator();
            {   // how many frames fit is a memory question, so say it in memory terms
                ImGui::TextDisabled("Memory budget");
                float gb = app.memBudgetGB > 0 ? app.memBudgetGB
                                               : (float)(seqMemBudget() / 1073741824.0);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                if (ImGui::InputFloat("GB##membudget", &gb, 1.0f, 8.0f, "%.1f"))
                    app.memBudgetGB = std::clamp(gb, 0.5f, 4096.0f);
                if (ImGui::MenuItem("auto (60% of RAM)", nullptr, app.memBudgetGB <= 0))
                    app.memBudgetGB = 0;
                ImGui::TextDisabled("  in use: %.1f GB of %.1f GB",
                                    residentImageBytes() / 1073741824.0,
                                    seqMemBudget() / 1073741824.0);
                ImGui::Separator();
            }
            {   // where remote analysis runs
                ImGui::TextDisabled("Remote processing");
                if (ImGui::MenuItem("  auto (server for remote data)", nullptr,
                                    app.procPolicy == App::PolAuto))
                    app.procPolicy = App::PolAuto;
                if (ImGui::MenuItem("  local fetch (bring frames here)", nullptr,
                                    app.procPolicy == App::PolLocalFetch))
                    app.procPolicy = App::PolLocalFetch;
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Load stack for current image", nullptr, false,
                                cur() && cur()->seqId == 0 && !cur()->src->path.empty())) {
                std::string pat;
                std::vector<std::string> files = findSequenceSiblings(cur()->src->path, pat);
                if (files.size() >= 2) startSequenceLoad(app.current, files, pat);
                else toast("no numbered siblings found next to this file", true);
            }
            if (ImGui::MenuItem("Stop background loading", nullptr, false, app.seqRunning))
                app.seqCancel = true;
            ImGui::EndMenu();
        }
        ImGui::Separator();
#if defined(__APPLE__)
        if (ImGui::MenuItem("Quit", "Cmd+Q")) glfwSetWindowShouldClose(win, 1);
#else
        if (ImGui::MenuItem("Exit", "Alt+F4")) glfwSetWindowShouldClose(win, 1);
#endif
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        bool has = cur() != nullptr;
        bool inSeq = cur() && cur()->seqId != 0;
        if (ImGui::MenuItem("Next frame", "Right / Ctrl+F", false, inSeq)) gotoFrame(1);
        if (ImGui::MenuItem("Previous frame", "Left / Ctrl+B", false, inSeq)) gotoFrame(-1);
        if (ImGui::MenuItem("First frame", "Home / Ctrl+A", false, inSeq)) gotoFrame(0, true, true);
        if (ImGui::MenuItem("Last frame", "End / Ctrl+E", false, inSeq)) gotoFrame(0, true, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Next stack", "Down / Ctrl+N", false, app.images.size() > 1)) gotoStack(1);
        if (ImGui::MenuItem("Previous stack", "Up / Ctrl+P", false, app.images.size() > 1)) gotoStack(-1);
        ImGui::Separator();
        if (ImGui::BeginMenu("Compare A/B", app.images.size() > 1)) {
            // every mode entry picks a B if there is none: a checked mode that
            // shows nothing is the worst state this menu could leave you in
            if (ImGui::MenuItem("Off", "\\ or C", app.compareMode == App::CmpOff))
                app.compareMode = App::CmpOff;
            if (ImGui::MenuItem("Wipe (drag the divider)", nullptr, app.compareMode == App::CmpWipe))
                { app.compareMode = App::CmpWipe; ensureCompareB(); }
            if (ImGui::MenuItem("Side by side", nullptr, app.compareMode == App::CmpSplit))
                { app.compareMode = App::CmpSplit; ensureCompareB(); }
            if (ImGui::MenuItem("Difference (A-B)", nullptr, app.compareMode == App::CmpDiff))
                { app.compareMode = App::CmpDiff; ensureCompareB(); }
            if (ImGui::MenuItem("Blink (flip A/B in place)", nullptr, app.compareMode == App::CmpFlip))
                { app.compareMode = App::CmpFlip; ensureCompareB(); }
            if (app.compareMode == App::CmpFlip) {
                if (ImGui::MenuItem("  auto blink", "Space / B toggles", app.flipAuto))
                    app.flipAuto = !app.flipAuto;
            }
            if (app.compareMode == App::CmpDiff) {
                if (ImGui::MenuItem("  magnitude only |A-B|", nullptr, app.diffAbs))
                    app.diffAbs = !app.diffAbs;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("B follows A's frame number", nullptr, app.compareFollowFrame))
                app.compareFollowFrame = !app.compareFollowFrame;
            {   // the display range relationship, where the compare modes live
                static const char* RN[3] = { "  each side keeps its own range",
                                             "  every side uses A's range",
                                             "  auto over every side (union)" };
                ImGui::TextDisabled("Display range");
                for (int r = 0; r < 3; r++)
                    if (ImGui::MenuItem(RN[r], nullptr, app.compareRangeMode == r)) {
                        app.compareRangeMode = r;
                        if (ImageDoc* bb = cmpB()) bb->texDirty = true;
                        if (cur()) cur()->texDirty = true;
                    }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Two images stretched differently cannot be compared -\n"
                                  "what you would be looking at is the stretch. Display\n"
                                  "only: B keeps its own black/white.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Two stacks: stepping A steps B to the SAME frame number,\n"
                                  "so frame 42 is compared against frame 42.\n"
                                  "Turn it off to hold B still - which is what you want when\n"
                                  "B is one reference image (a dark, a golden sample).\n"
                                  "A single-image B never moves either way.");
            if (ImGui::MenuItem("Pin this frame as B", "Shift+B", false, cur() != nullptr)) pinCurrentAsB();
            if (ImGui::MenuItem("Swap A and B", "Shift+\\", false, cmpB() != nullptr))
                swapCompare();
            ImGui::Separator();
            // ONE setting for every statistics panel: "same arrangement as the
            // image" is a property of the comparison, not of the histogram.
            ImGui::TextDisabled("Statistics panels");
            if (ImGui::MenuItem("  Auto (follow the image layout)", nullptr,
                                app.abStatsLayout == App::AbAuto))
                app.abStatsLayout = App::AbAuto;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Side by side images (split) -> side by side plots.\n"
                                  "Wipe / blink / difference have one image area,\n"
                                  "so their plots overlay.");
            // "(A solid, B dashed)" described a stroke style that no longer
            // exists - every side past A draws SOLID in its own ink, and at N
            // the parenthetical also implied a pair. Say what the mode does.
            if (ImGui::MenuItem("  Overlay (every side on one plot)", nullptr,
                                app.abStatsLayout == App::AbOverlay))
                app.abStatsLayout = App::AbOverlay;
            if (ImGui::MenuItem("  Side by side (A on the left)", nullptr,
                                app.abStatsLayout == App::AbSide))
                app.abStatsLayout = App::AbSide;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Always 50/50 and always A on the left: two shapes\n"
                                  "can only be compared on plots of equal width.\n"
                                  "Narrow panels fall back to the overlay.");
            ImGui::Separator();
            ImGui::TextDisabled("B image");
            // one row per stack (its frame in view), not one per frame: a
            // 120-frame stack must not produce a 120-row menu
            int shown = 0;
            // A itself is on the list: setting B to the document already on the
            // A side is a supported comparison (both sides draw, coinciding),
            // and leaving it out was the same omission resolveB used to make.
            for (int i = 0; i < (int)app.images.size(); i++) {
                ImageDoc* d = app.images[i].get();
                if (d->seqId != 0) {
                    App::SeqInfo* si = seqInfo(d->seqId);
                    int rep = si && si->lastImageIdx >= 0 ? si->lastImageIdx : -1;
                    if (rep < 0) { std::vector<int> fr = framesOfSeq(d->seqId); rep = fr.empty() ? -1 : fr.front(); }
                    if (rep != i) continue;                  // this stack is listed once
                }
                if (++shown > 40) { ImGui::TextDisabled("... %d more (use Files)",
                                                        (int)app.images.size() - shown); break; }
                // by STACK name, and with the uid as the ImGui id: two stacks of
                // one series list identically named frames, so "frame_000.npy"
                // twice was the whole menu's content (### also keeps a name
                // containing ## from eating its own label)
                char lbl[320];
                snprintf(lbl, sizeof lbl, "%s%s###b%llu", abDocLabel(d).c_str(),
                         d->seqId != 0 ? "   [stack]" : "",
                         (unsigned long long)d->uid);
                if (ImGui::MenuItem(lbl, nullptr, app.compareBUid == d->uid)) {
                    setCompareB(d);
                    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fit to Window", "F", false, has)) app.fitRequested = true;
        if (ImGui::MenuItem("Actual Size (100%)", "1", false, has)) app.view.zoom = 1.0f;
        if (ImGui::MenuItem("Zoom In", "+", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
        if (ImGui::MenuItem("Zoom Out", "-", false, has))
            app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        ImGui::Separator();
        ImGui::MenuItem("Pixel Grid", "G", &app.showGrid);
        ImGui::MenuItem("Wheel zooms without Ctrl", nullptr, &app.wheelZoomPlain);
        ImGui::MenuItem("Left drag pans (Shift = new ROI)", nullptr, &app.dragPans);
        if (ImGui::MenuItem("Compact UI (dense rows)", nullptr, &app.compactUi))
            ui_theme::apply(app.themeVariant, app.themeAccent, app.uiScale, app.compactUi);
        ImGui::MenuItem("Show frame time", nullptr, &app.showFps);
        ImGui::MenuItem("Low bandwidth (remote / ssh)", nullptr, &app.lowBandwidth);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cap the redraw rate at ~20-30 fps and drop the\n"
                              "post-input tail. Saves bandwidth over X11 forwarding,\n"
                              "at ~32 ms of extra latency while dragging or scrolling\n"
                              "(0.4 ms otherwise). Locally, leave this off.");
        ImGui::MenuItem("Fit view when switching images", nullptr, &app.fitOnSwitch);
        {   // the title bar: ours (in this menu bar) or the desktop's, above us
            const bool can = window_frame::available();
            bool integrated = app.frameMode != 0;
            if (ImGui::MenuItem("Integrated title bar", nullptr, &integrated, can)) {
                app.frameMode = integrated ? 1 : 0;
                window_frame::setMode(integrated ? window_frame::Integrated
                                                 : window_frame::System);
                app.prefsDirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(can ? "Menus, title and window buttons in one bar,\n"
                                        "with no system title bar above the app.\n"
                                        "(--frame system starts with the system one,\n"
                                        "in case a window manager makes a mess of it.)"
                                      : "Not available here: %s",
                                  window_frame::unavailableReason());
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Files", nullptr, &app.showFiles);
            ImGui::MenuItem("Browse", nullptr, &app.showRemote);
            // a SECOND (third, ...) view onto another place - each Browse
            // window stands somewhere of its own (docs/todo-open.md item 17)
            if (ImGui::MenuItem("New Browse Panel")) rbNewInstance();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("another Browse window with its own place -\n"
                                  "browse two folders or two machines side by side");
            ImGui::MenuItem("Inspector", nullptr, &app.showInspector);
            ImGui::MenuItem("Reader", nullptr, &app.readerPanelOpen);
            ImGui::MenuItem("Readers (remembered)", nullptr, &app.readerListOpen);
            ImGui::MenuItem("ROIs", nullptr, &app.showRois);
            ImGui::MenuItem("Analysis", nullptr, &app.showAnalysis);
            ImGui::MenuItem("Histogram", nullptr, &app.showHistogram);
            ImGui::MenuItem("Temporal", nullptr, &app.showTemporal);
            ImGui::MenuItem("Projection (H/V)", nullptr, &app.showProjection);
            ImGui::MenuItem("Series Analysis", nullptr, &app.showSeriesAnalysis);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("the series layer's analyses: linearity / PTC fit, read\n"
                                  "noise, SNR curve over one series (was \"Linearity\")");
            if (ImGui::MenuItem("Messages", nullptr, &app.showMessages)) app.msgUnreadErr = false;
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) {
                app.showFiles = app.showInspector = app.showRois = true;
                app.showAnalysis = app.showHistogram = app.showTemporal = true;
                app.showProjection = true;
                app.resetLayout = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Display Gamma")) {
            bool lin = app.dispGamma < 1.5f;
            if (ImGui::MenuItem("1.0 (linear)", nullptr, lin) && !lin) { app.dispGamma = 1.0f; markAllTexDirty(); }
            if (ImGui::MenuItem("2.2", nullptr, !lin) && lin)         { app.dispGamma = 2.2f; markAllTexDirty(); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Theme")) {
            bool changed = false;
            if (ImGui::MenuItem("Dark", nullptr, app.themeVariant == ui_theme::VariantDark))
                { app.themeVariant = ui_theme::VariantDark; changed = true; }
            if (ImGui::MenuItem("Light", nullptr, app.themeVariant == ui_theme::VariantLight))
                { app.themeVariant = ui_theme::VariantLight; changed = true; }
            ImGui::Separator();
            ImGui::TextDisabled("accent");
            for (int i = 0; i < ui_theme::accentCount(); i++)
                if (ImGui::MenuItem(ui_theme::accents()[i].name, nullptr, app.themeAccent == i))
                    { app.themeAccent = i; changed = true; }
            if (changed) ui_theme::apply(app.themeVariant, app.themeAccent, app.uiScale, app.compactUi);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (!plugin_host::analyzers().empty() && ImGui::BeginMenu("Measure")) {
        const auto& anas = plugin_host::analyzers();
        ImageDoc* im = cur();
        app.anaSel = std::clamp(app.anaSel, 0, (int)anas.size() - 1);
        // Rerun accelerator first: stepping through frames/images and pressing
        // M is the comparison loop, and the menu is where M gets discovered.
        std::string again = "Measure again: " + anas[app.anaSel].name;
        if (ImGui::MenuItem(again.c_str(), "M", false, im != nullptr))
            requestMeasure(app.anaSel);
        if (ImGui::MenuItem("Auto re-run on change", nullptr, app.anaAuto))
            app.anaAuto = !app.anaAuto;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("re-measure whenever the image, ROI set,\n"
                              "display range or CFA layout changes");
        ImGui::Separator();
        // one pass per question group so co-answering plugins sit together
        // regardless of registration order; then unknown categories verbatim
        std::vector<std::string> headers;
        for (const auto& g : MEASURE_GROUPS) {
            bool present = false, seen = false;
            for (const auto& a : anas) {
                std::string c, n;
                splitAnalyzerName(a.name, c, n);
                if (c == g.cat) present = true;
            }
            for (const auto& h : headers) if (h == g.group) seen = true;
            if (present && !seen) headers.push_back(g.group);
        }
        for (const auto& a : anas) {
            std::string c, n;
            splitAnalyzerName(a.name, c, n);
            if (measureGroupOf(c)) continue;
            bool seen = false;
            for (const auto& h : headers) if (h == c) seen = true;
            if (!seen) headers.push_back(c);
        }
        for (size_t hi = 0; hi < headers.size(); hi++) {
            if (hi) ImGui::Separator();
            ImGui::TextDisabled("%s", headers[hi].c_str());
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                const char* grp = measureGroupOf(c);
                if ((grp ? grp : c.c_str()) != headers[hi]) continue;
                const char* reason = analyzerDisabledReason(anas[i], im);
                // the right column keeps the plugin's identity (the category is
                // the standard number for iso* measurements); the precondition
                // moved into the tooltip, where there is room to say it fully
                if (ImGui::MenuItem(n.c_str(), c.c_str(), app.anaSel == i, !reason))
                    requestMeasure(i);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (reason) ImGui::SetTooltip("%s", reason);
                    else if (!anas[i].desc.empty()) ImGui::SetTooltip("%s", anas[i].desc.c_str());
                }
            }
            // Temporal noise is the stack half of the noise question. It is
            // host-computed (Temporal panel), not a frame analyzer, but the
            // user asking "how noisy?" must find it HERE, not by knowing the
            // implementation boundary.
            if (headers[hi] == "Noise / SNR") {
                bool inStack = im && im->seqId != 0;
                if (ImGui::MenuItem("temporal (Temporal panel)", nullptr, false, inStack)) {
                    app.showTemporal = true;
                    ImGui::SetWindowFocus("Temporal");
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(inStack
                        ? "sigma_t / sigma_fpn over the loaded stack (whole frames or the selected ROI)"
                        : "needs a stack: load numbered files as a stack first");
            }
        }
        ImGui::EndMenu();
    }
    if (!plugin_host::processors().empty() && ImGui::BeginMenu("Process")) {
        for (int i = 0; i < (int)plugin_host::processors().size(); i++)
            if (ImGui::MenuItem(plugin_host::processors()[i].name.c_str(), nullptr, false, cur() != nullptr))
                runProcessor(i);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard Shortcuts", "H")) app.showHelp = true;
        if (ImGui::MenuItem("About viewer")) app.showAbout = true;
        ImGui::EndMenu();
    }
    drawTitleBarExtras();
    ImGui::EndMainMenuBar();
}

// File > Open Remote: the one dialog the OS cannot provide, because the files it
// lists live on another machine. URL + peer path, both remembered in prefs.
bool isNpyName(const std::string& n) {          // FRAME_001.NPY is still a .npy
    if (n.size() < 4) return false;
    std::string ext = n.substr(n.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".npy";
}

// File > Start Remote: ask for a host, not a path. Connecting first is what
// removes the guesswork - once the session is up, the Files panel browses the
// server and nobody has to know whether their data is under ~ or /data.
static void drawRemoteOpenModal() {
    if (app.remoteDlgOpen && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        ImGui::OpenPopup("Start remote (ssh)");
        app.remoteDlgOpen = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Start remote (ssh)", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    static char hostbuf[512];
    static bool advanced = false;
    static char exebuf[256];
    if (ImGui::IsWindowAppearing()) {
        snprintf(hostbuf, sizeof hostbuf, "%s",
                 app.lastRemoteUrl.empty() ? "user@host" : app.lastRemoteUrl.c_str());
        snprintf(exebuf, sizeof exebuf, "%s", app.remoteExe.c_str());
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::TextDisabled("The window stays here; only the pixels being looked at travel.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
    bool go = ImGui::InputText("host", hostbuf, sizeof hostbuf,
                               ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("user@host, or a Host alias from ~/.ssh/config.\n"
                          "A full ssh://host/path or host:~/path works too.\n"
                          "Public-key authentication is required - this pipe has\n"
                          "no way to ask for a password.");
    ImGui::TextDisabled("the peer installs itself into ~/.viewer on first connect");
    if (ImGui::TreeNode("advanced")) {
        advanced = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26);
        ImGui::InputTextWithHint("peer path", "~/.viewer/viewer-serve (default)",
                                 exebuf, sizeof exebuf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Override where viewer-serve lives on that machine.\n"
                              "Leave empty to use ~/.viewer/viewer-serve.");
        ImGui::TreePop();
    }
    if (!g_bootstrapLog.empty() && g_bootstrapLog.find("VIEWER_SERVE_OK") == std::string::npos) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextColored(ImVec4(1, 0.55f, 0.4f, 1), "%s", g_bootstrapLog.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    if (ImGui::Button("Connect") || go) {
        if (advanced) app.remoteExe = exebuf;
        app.lastRemoteUrl = hostbuf;
        app.prefsDirty = true;
        savePrefs();
        ImGui::CloseCurrentPopup();
        // errors arrive as toasts and inline in the instance that connects.
        // Which instance that is: not blindly the active one -- connecting to a
        // machine while a local listing is open used to replace it.
        App::BrowseInstance& I = rbInstanceFor(hostbuf);
        startRemote(I, hostbuf);
        rbShowInstance(I);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// The whole failure text, copyable. A remote install that fails deserves more
// than a truncated line: the loader's own words are what identify a glibc
// mismatch, and the user needs to be able to paste them.
static void drawRemoteErrorWindow() {
    if (!app.showRemoteError) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 42, ImGui::GetFontSize() * 18),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Remote connection details", &app.showRemoteError)) {
        const App::RemoteBrowse& B = rbActive().b;
        std::string all = "host: " + (B.host.empty() ? std::string("(none)") : B.host) + "\n";
        if (!B.err.empty()) all += B.err + "\n";
        if (!g_bootstrapLog.empty()) all += "\ninstall log:\n" + g_bootstrapLog;
        if (ImGui::Button("copy to clipboard")) {
            ImGui::SetClipboardText(all.c_str());
            toast("copied");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or select any part of it below and press %s+C", SC_MOD);
        ImGui::Separator();
        // selectable, because this text exists to be pasted somewhere else
        ImGui::InputTextMultiline("##rberrtext", all.data(), all.size() + 1,
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::End();
}

// Everything the app has said, as text you can select with the mouse and copy
// with Ctrl+C. ImGui's Text() is painted, not selectable - so a read-only
// InputTextMultiline is what makes an error message quotable.
static void drawMessagesPanel() {
    static std::string flat;          // rebuilt only when the log changes
    // Keyed on msgSeq, not msgLog.size(): the log is capped at 300, so past the
    // cap the size is 300 for every subsequent message and a size-keyed cache
    // never rebuilds again - the panel went blind exactly when messages were
    // arriving fastest, which is when it is being read.
    static size_t builtFrom = (size_t)-1;
    if (builtFrom != app.msgSeq) {
        builtFrom = app.msgSeq;
        flat.clear();
        for (const auto& m : app.msgLog) {
            flat += m.err ? "[error] " : "        ";
            flat += m.text;
            flat += "\n";
        }
    }
    if (ImGui::Button("copy all")) { ImGui::SetClipboardText(flat.c_str()); toast("copied"); }
    ImGui::SameLine();
    if (ImGui::Button("clear")) { app.msgLog.clear(); app.msgSeq++; }
    ImGui::SameLine();
    ImGui::TextDisabled("select any part and press %s+C", SC_MOD);
    ImGui::InputTextMultiline("##msgs", flat.data(), flat.size() + 1,
                              ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
}

static void drawHelpAbout() {
    if (app.showHelp) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("Keyboard Shortcuts", &app.showHelp, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (ImGui::BeginTable("sc", 2)) {
                auto row = [](const char* k, const char* d) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", d);
                };
                row("Right / Left",  "next / previous frame (time axis)");
                row("Down / Up",     "next / previous stack");
                row("Ctrl+F / Ctrl+B", "next / previous frame (Emacs style)");
                row("Ctrl+N / Ctrl+P", "next / previous stack");
                row("Ctrl+A / Ctrl+E", "first / last frame");
                row(SC_MOD "+O",       "open what is selected in Browse (or go there)");
                row(SC_MOD "+Shift+O", "open a folder");
                row(SC_MOD "+S",     "save session (view state + images)");
                row(SC_MOD "+W",     "close current image");
                row("F / double-click", "fit to window");
                row("1",             "actual size (100%)");
                row("+ / -",         "zoom in / out");
                row("drag",          "new ROI (inside a ROI = move, on a corner = resize)");
                row("P",             "drop a pin at the cursor");
                row("right-click",   "actions here (pin / band / crop / delete)");
                row("Ctrl+wheel",    "zoom at cursor (invertible in View menu)");
                row("wheel / Shift+wheel", "pan vertical / horizontal");
                row("middle-drag / Space+drag", "pan");
                row("Shift+drag",    "the other one of pan / new ROI");
                row("X / Y",         "select the row / column: through the selected pin, the "
                                     "cursor, or widen the selected ROI (press again to restore)");
                row("Del / Esc",     "delete / deselect annotation");
                row("\\ or C",       "A/B compare: off -> wipe -> side by side -> "
                                     "difference -> blink");
                row("Shift+\\",       "swap A and B (also the status bar button)");
                row("Shift+C",       "side by side with the slots past B (C, D, ...)");
                row("B (hold)",      "show B full-frame while held");
                row("Shift+B",       "pin this frame as B (then move A: frame vs frame)");
                row("[ / ]",         "move the divider 1% (Shift: 10%)");
                row("G",             "pixel grid (zoom >= 8x)");
                row("M",             "measure again (rerun the selected analyzer, focus Analysis)");
                row("H",             "this help");
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("--- Browse panel (when it has focus) ---");
                row("up / down",     "walk the listing, previewing as it goes");
                row("Home / End",    "first / last row");
                row("Enter",         "open for real (the double-click: a stack opens whole)");
                row("right / left",  "tree mode: expand / collapse the folder under the cursor");
                row("Backspace",     "up to the parent folder");
                row(SC_MOD "+F",     "focus the filter box");
                row(", / .",         "step the previewed stack");
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }
    if (app.showAbout) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin("About viewer", &app.showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("viewer  %s", viewerVersion());
            ImGui::TextDisabled("cross-platform image viewer for engineering data");
            ImGui::Separator();
            ImGui::TextDisabled("Dear ImGui %s  |  GLFW %s", IMGUI_VERSION, glfwGetVersionString());
            ImGui::TextDisabled("npy / bin / raw loaders, pixel inspection, coordinate rulers");
        }
        ImGui::End();
    }
}

