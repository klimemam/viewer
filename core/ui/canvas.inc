// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
// The recipe panel below owns this; the dialog uses it too, because the moment
// a user has just typed a geometry in is the moment they want to keep it.
static void rawRecipeCopy(const RawRecipe& r);

static void drawRawModal() {
    // LEVEL-triggered, like the picker / sequence-ask / Start-remote dialogs.
    // It used to be one-shot (`rawDlg.open` consumed on the frame it opened),
    // and ImGui's OpenPopupEx CLOSES whatever sits at the current stack level
    // when a different id is opened there - so an F2 rename or a drag-drop
    // sequence prompt destroyed this dialog permanently. When it had been
    // opened for a queued folder that also wedged rawDlg.forQueue true forever,
    // and forQueue is only ever cleared inside this modal: every later Open
    // Folder then sat at "queued" until restart. rawDlg.open now means "the
    // dialog is alive", cleared only by Load or Cancel, so a replaced dialog
    // comes back and its precondition cannot outlive it.
    if (rawDlg.open && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::OpenPopup("RAW load settings");
    g_rawPopupAlive = ImGui::IsPopupOpen("RAW load settings");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("RAW load settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("%s  (%zu bytes)%s", baseName(rawDlg.path).c_str(), rawDlg.fileSize,
                rawDlg.replaceIdx >= 0 ? "  -  reinterpret" : "");
    if (rawDlg.forQueue)
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1),
                           "these settings apply to all %d queued stack(s)", rawDlg.queueCount);
    // The library, at the moment it is useful (#166). It fills the fields in and
    // stops there: Load is where the choice is made, so a mis-click is one
    // combo away from being undone and nothing has been decoded yet.
    if (!g_rawLibrary.empty()) {
        if (ImGui::BeginCombo("recipe", "pick a saved recipe...")) {
            for (const RawRecipe& r : g_rawLibrary) {
                const std::string lb = r.name + "   (" + rawRecipeSummary(r) + ")";
                if (ImGui::Selectable(lb.c_str())) {
                    rawRecipeIntoDialog(r, rawDlg);
                    rawGuessDims(rawDlg);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
    int prevDt = rawDlg.dtype, prevIn = rawDlg.interp, prevOff = rawDlg.offset;
    // axis 1: how one sample is stored
    if (ImGui::BeginCombo("pixel format", RAW_DTYPE_NAMES[rawDlg.dtype])) {
        for (int i = 0; i < RD_COUNT; i++)
            if (ImGui::Selectable(RAW_DTYPE_NAMES[i], i == rawDlg.dtype)) rawDlg.dtype = i;
        ImGui::EndCombo();
    }
    // axis 2: what the samples mean
    if (ImGui::BeginCombo("interpretation", RAW_INTERP_NAMES[rawDlg.interp])) {
        for (int i = 0; i < RI_COUNT; i++)
            if (ImGui::Selectable(RAW_INTERP_NAMES[i], i == rawDlg.interp)) rawDlg.interp = i;
        ImGui::EndCombo();
    }
    if (rawDlg.interp == RI_BAYER || rawDlg.interp == RI_QUAD) {
        if (ImGui::BeginCombo("CFA pattern", CFA_PATTERNS[rawDlg.cfaPattern])) {
            for (int i = 0; i < 4; i++)
                if (ImGui::Selectable(CFA_PATTERNS[i], i == rawDlg.cfaPattern)) rawDlg.cfaPattern = i;
            ImGui::EndCombo();
        }
    }
    if (!rawDlg.guesses.empty()) {
        char cursz[64]; snprintf(cursz, 64, "%d x %d", rawDlg.w, rawDlg.h);
        if (ImGui::BeginCombo("size candidates", cursz)) {
            for (auto& g : rawDlg.guesses) {
                char lb[64]; snprintf(lb, 64, "%d x %d", g.first, g.second);
                if (ImGui::Selectable(lb)) { rawDlg.w = g.first; rawDlg.h = g.second; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputInt("width (px)", &rawDlg.w, 1, 16);          // step / fast-step: round numbers
    ImGui::InputInt("height (px)", &rawDlg.h, 1, 16);
    ImGui::InputInt("offset (bytes)", &rawDlg.offset, 1, 512);
    rawDlg.w = std::clamp(rawDlg.w, 1, 32768);
    rawDlg.h = std::clamp(rawDlg.h, 1, 32768);
    rawDlg.offset = std::max(0, rawDlg.offset);
    ImGui::Checkbox("little endian", &rawDlg.littleEndian);
    ImGui::Checkbox("crop on load", &rawDlg.cropOn);
    if (rawDlg.cropOn) {
        if (rawDlg.cropW <= 0) { rawDlg.cropW = rawDlg.w; rawDlg.cropH = rawDlg.h; }
        ImGui::InputInt("crop x (px)", &rawDlg.cropX, 1, 16);
        ImGui::InputInt("crop y (px)", &rawDlg.cropY, 1, 16);
        ImGui::InputInt("crop width (px)", &rawDlg.cropW, 1, 16);
        ImGui::InputInt("crop height (px)", &rawDlg.cropH, 1, 16);
        rawDlg.cropX = std::clamp(rawDlg.cropX, 0, rawDlg.w - 1);
        rawDlg.cropY = std::clamp(rawDlg.cropY, 0, rawDlg.h - 1);
        rawDlg.cropW = std::clamp(rawDlg.cropW, 1, rawDlg.w - rawDlg.cropX);
        rawDlg.cropH = std::clamp(rawDlg.cropH, 1, rawDlg.h - rawDlg.cropY);
    }
    if (rawDlg.dtype != prevDt || rawDlg.interp != prevIn || rawDlg.offset != prevOff)
        rawGuessDims(rawDlg);

    size_t need = (size_t)rawDlg.w * rawDlg.h *
                  RAW_DTYPE_SIZE[rawDlg.dtype] * RAW_INTERP_CH[rawDlg.interp] + rawDlg.offset;
    if (need > rawDlg.fileSize)
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "need %zu bytes > file %zu bytes", need, rawDlg.fileSize);
    else if (need < rawDlg.fileSize)
        ImGui::TextDisabled("need %zu bytes (%zu bytes unused)", need, rawDlg.fileSize - need);
    else
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "size matches exactly");

    ImGui::Separator();
    bool ok = ImGui::Button(rawDlg.replaceIdx >= 0 ? "Reload" : "Load", ImVec2(120 * app.uiScale, 0));
    ImGui::SameLine();
    if (ImGui::Button("Copy as JSONC", ImVec2(140 * app.uiScale, 0))) {
        // The app does not write settings.jsonc (判断3), so keeping a recipe is
        // "here is the line, and here is the file to paste it into".
        RawRecipe r = rawRecipeFromDialog(rawDlg);
        if (!r.fromLibrary) r.name = baseName(rawDlg.path) + " " + rawRecipeSummary(r);
        rawRecipeCopy(r);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("keep these settings as a NAMED recipe: this copies the\n"
                          "JSONC line to paste into settings.jsonc (the app never\n"
                          "writes that file - it is yours). Rename it there.");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * app.uiScale, 0))) {
        rawDlg.replaceIdx = -1;
        if (rawDlg.forQueue) {
            rawDlg.forQueue = false;
            // Only the Open that ASKED. A blanket clear threw away the stacks a
            // DIFFERENT Open had queued - in silence, since the queue spans
            // Opens now. And say how many did not open: dropping a folder's
            // worth of stacks is not something to do without a word.
            int oid = app.seqQueue.empty() ? 0 : app.seqQueue.front().openId;
            int dropped = 0;
            for (auto it = app.seqQueue.begin(); it != app.seqQueue.end();)
                if (it->openId == oid) { it = app.seqQueue.erase(it); dropped++; }
                else ++it;
            if (dropped)
                toast("cancelled: " + std::to_string(dropped) + " stack(s) not opened", true);
            pruneEmptyBatches();
        }
        rawDlg.open = false;              // dismissed for real, not replaced
        ImGui::CloseCurrentPopup();
    }
    if (ok) {
        // rawDialogAccept, not loadRaw: the Load button is also the moment the
        // user STATES how files of this byte count are read this session (#166),
        // and that statement is made in one place so the headless test drives
        // the same code the button does.
        std::string err = rawDialogAccept(rawDlg);
        if (!err.empty()) toast(err, true);
        else if (rawDlg.forQueue) {
            // first frame of the first queued stack: reuse these settings for the rest
            g_folderRecipe = rawDlg;
            g_folderRecipe.forQueue = false;
            g_folderRecipe.replaceIdx = -1;
            app.folderRecipeValid = true;
            rawDlg.forQueue = false;
            rawDlg.open = false;
            App::PendingGroup g = app.seqQueue.front();
            app.folderRecipeOpenId = g.openId;   // this Open's geometry, nobody else's
            app.seqQueue.erase(app.seqQueue.begin());
            if (g.files.size() >= 2) startSequenceLoad(app.current, g.files, g.name);
            if (app.current >= 0 && app.current < (int)app.images.size())
                noteGroupStack(g.token, app.images[app.current]->seqId);
            ImGui::CloseCurrentPopup();
        } else {
            bool fresh = rawDlg.replaceIdx < 0;
            toast((fresh ? "loaded " : "reinterpreted ") + baseName(rawDlg.path));
            rawDlg.replaceIdx = -1;
            rawDlg.open = false;
            ImGui::CloseCurrentPopup();
            if (fresh) maybeOfferSequence(app.current);
        }
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------- RAW recipes panel
// Issue #166, and it is a PANEL rather than a second modal for the reason the
// issue was filed: headerless files are the frequent case here, and the dialog
// is a thing you answer and dismiss. A panel can sit beside Readers and be
// consulted, which is what a list of "our formats" is for.
//
// It shows two lists that must never be confused with each other, so they are
// drawn apart, headed differently, and each says its own lifetime:
//
//   LIBRARY        from settings.jsonc. Persistent, hand-written, INERT - a
//                  recipe here does nothing until it is picked.
//   THIS SESSION   byte count -> the recipe picked for it. Process-only,
//                  written to no file, gone at exit.
//
// Nothing in this panel writes settings.jsonc: 判断3 is that the app never
// writes that file, so "save this recipe" is "here is the line to paste", and
// the button says exactly that.
static void rawRecipeCopy(const RawRecipe& r) {
    const std::string line = rawRecipeJsonc(r) + ",";
    ImGui::SetClipboardText(line.c_str());
    // Both, because the clipboard is not a record: the Messages panel is where
    // the user finds it again after the next copy, and the PATH is half the
    // instruction - "paste it where" is the question a clipboard cannot answer.
    logMsg("recipe to paste under \"loading\": { \"rawRecipes\": [ ... ] } in " +
           (settingsPath().empty() ? std::string("settings.jsonc") : settingsPath()) +
           "\n    " + line);
    toast("copied - paste it into settings.jsonc under loading.rawRecipes");
}

static void drawRawRecipePanel() {
    if (!app.rawRecipeListOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 46, ImGui::GetFontSize() * 24),
                             ImGuiCond_FirstUseEver);
    if (ImGui::Begin("RAW recipes", &app.rawRecipeListOpen)) {
        // The five come from sequence.inc's array, not from a sentence typed
        // here: this panel exists to explain which files reach it, so it must
        // not be able to name a different five than the ones that do.
        ImGui::TextDisabled("Headerless files (%s) carry no shape, so somebody has to state it.",
                            headerlessExtList().c_str());
        ImGui::TextDisabled("A saved recipe NEVER applies on its own - you pick it.");

        ImGui::SeparatorText("Library (settings.jsonc)");
        if (g_rawLibrary.empty()) {
            ImGui::TextDisabled("no named recipes yet.");
            ImGui::TextDisabled("Set a file up in the RAW dialog, press \"Copy as JSONC\", "
                                "and paste the line into");
            ImGui::TextDisabled("  %s", settingsPath().empty() ? "settings.jsonc"
                                                               : settingsPath().c_str());
            ImGui::TextDisabled("  under  \"loading\": { \"rawRecipes\": [ ... ] }");
        }
        const bool dlgOpen = rawDlg.open;
        ImageDoc* im = cur();
        const bool canReinterpret = im && im->src->rawDtype >= 0 && !im->src->path.empty();
        for (size_t i = 0; i < g_rawLibrary.size(); i++) {
            const RawRecipe& r = g_rawLibrary[i];
            ImGui::PushID((int)i);
            ImGui::BeginDisabled(!dlgOpen && !canReinterpret);
            if (ImGui::SmallButton(dlgOpen ? "Use" : "Apply")) {
                if (dlgOpen) {
                    // The dialog is the thing being answered: fill it in and let
                    // the user press Load, which is where the choice is made.
                    rawRecipeIntoDialog(r, rawDlg);
                    rawGuessDims(rawDlg);
                    toast("\"" + r.name + "\" filled in - press Load to use it");
                } else {
                    std::string err = rawRecipeReinterpretCurrent(r);
                    if (!err.empty()) toast(err, true);
                    else toast("re-read " + baseName(im->src->path) + " as \"" + r.name + "\"");
                }
            }
            ImGui::EndDisabled();
            if (!dlgOpen && !canReinterpret && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("open a headerless file first - a recipe is applied\n"
                                  "to the RAW dialog, or to a file already read that way");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) rawRecipeCopy(r);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("copy this recipe as the JSONC line it is written as");
            ImGui::SameLine();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", rawRecipeSummary(r).c_str());
            ImGui::PopID();
        }

        ImGui::SeparatorText("This session, by file size");
        ImGui::TextDisabled("What you have already told this viewer, and the ONLY thing that "
                            "opens a file without asking.");
        ImGui::TextDisabled("It is in no file - not settings.jsonc, not prefs.txt, not the "
                            "session - and ends with this process.");
        if (g_rawSizeRecipe.empty()) {
            ImGui::TextDisabled("nothing yet: the first headerless file of a session always asks.");
        }
        size_t forget = 0;
        int rowId = 0;
        for (const auto& kv : g_rawSizeRecipe) {
            ImGui::PushID(rowId++);
            if (ImGui::SmallButton("Forget")) forget = kv.first;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("the next file of this size asks again");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy as JSONC")) rawRecipeCopy(kv.second);
            ImGui::SameLine();
            ImGui::Text("%zu bytes", kv.first);
            ImGui::SameLine();
            if (kv.second.fromLibrary) {
                ImGui::TextUnformatted(kv.second.name.c_str());
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s", rawRecipeSummary(kv.second).c_str());
            ImGui::PopID();
        }
        if (forget) g_rawSizeRecipe.erase(forget);
        if (!g_rawSizeRecipe.empty()) {
            if (ImGui::SmallButton("Forget all")) g_rawSizeRecipe.clear();
        }
    }
    ImGui::End();
}

// ---------------- side-by-side tiles: the pane geometry --------------------
// More than two compare slots means side by side and nothing else. A wipe, a
// draggable split, a difference and a blink each mean exactly TWO images -
// there is no honest three-image version of any of them - so those four keep
// doing what they always did with A and B, and the extras get tiles.
//
// This lives out here, free of ImGui and of any frame state, because the
// geometry IS the feature: --tile-selftest has to be able to assert the
// rectangles without opening a window (this machine cannot screenshot GL).
struct TilePane { float x0 = 0, w = 0; };

// A pane narrower than this is not a comparison, it is a sliver: the badge that
// says WHICH image it holds no longer fits, and two 40 px strips of a sensor
// cannot be judged against each other. Say so rather than draw them.
static float tileMinPaneW(float uiScale) { return 110.0f * uiScale; }

// n equal-width panes across [x0, x0+width), separated by a band of 2*gutter -
// the same band the A/B divider occupies, so the two layouts read as relatives.
//
// Equal width is the rule, not a simplification: app.splitFrac exists for A
// against B, and applied to five panes it would make one of them a sliver.
// Returns false, and leaves `out` empty, when the panes would be too narrow.
static bool tileLayout(float x0, float width, int n, float gutter, float minPaneW,
                       std::vector<TilePane>& out) {
    out.clear();
    if (n <= 0) return false;
    const float paneW = (width - (n - 1) * 2.0f * gutter) / (float)n;
    if (!(paneW >= minPaneW)) return false;      // written so a NaN fails too
    out.reserve(n);
    for (int i = 0; i < n; i++)
        out.push_back({ x0 + i * (paneW + 2.0f * gutter), paneW });
    return true;
}

// The ONE mapping every pane uses. Zoom and centre come from the view, never
// from the pane: two images of the same scene at different magnifications,
// side by side, is exactly how a reader compares sharpness or noise between
// them and reaches a wrong conclusion. A pane that cannot show its image at
// the shared zoom clips - it is never quietly rescaled.
static ImVec2 tileMap(ImVec2 p0, ImVec2 sz, ImVec2 center, float zoom, float ix, float iy) {
    return ImVec2(p0.x + sz.x * 0.5f + (ix - center.x) * zoom,
                  p0.y + sz.y * 0.5f + (iy - center.y) * zoom);
}
static ImVec2 tileUnmap(ImVec2 p0, ImVec2 sz, ImVec2 center, float zoom, ImVec2 sc) {
    return ImVec2(center.x + (sc.x - p0.x - sz.x * 0.5f) / zoom,
                  center.y + (sc.y - p0.y - sz.y * 0.5f) / zoom);
}

// Who gets a pane, in slot order: A, then B if compare is armed and has one,
// then every extra slot. The letter travels WITH the document so a badge can
// never drift from the slot list that produced it - which matters now that
// follow-frame can resolve a slot onto a sibling of the pinned uid. `stale`
// is that side's follow-frame divergence: the pane is showing the last frame
// its stack had, and the badge must say so.
struct TileSlot { ImageDoc* doc; std::string tag; bool stale = false; };
static std::vector<TileSlot> tileSlots() {
    std::vector<TileSlot> out;
    ImageDoc* a = cur();
    if (!a) return out;
    out.push_back({ a, "A", false });
    ImageDoc* b = cmpB();                      // sets g_abFollowDiverged
    if (b) out.push_back({ b, "B", g_abFollowDiverged });
    // One pane per LETTER, not per document. Two letters resolving to one
    // document get two panes showing the same pixels - which is the point:
    // side by side is where you SEE that they coincide.
    for (const ResolvedSlot& rs : resolveSlots())
        out.push_back({ rs.doc, slotName(rs.idx), rs.diverged });
    return out;
}
// Tiling engages only when there is a third slot to show, and ONLY in Split -
// the mode that already means "side by side". Compare OFF is A alone, extras
// or not: off must always read as "not comparing", or there is no way OUT of
// the tiles (defect 2026-07-30, 「A,B,Cから抜けられない」). Wipe, difference
// and blink are two-image modes and are left alone.
static bool tileEngaged() {
    return cur() && !compareExtras().empty() && app.compareMode == App::CmpSplit;
}

// What drawCanvas ACTUALLY tiled on the last frame it drew: -1 = it did not
// tile, 0 = it wanted to and refused because the panes would be slivers. A
// geometry function that computes the right rectangles and a canvas that never
// calls it look identical from a green build - and from a screenshot this
// machine cannot take - so the frame writes down what it drew and
// --bench-tiles reads it back.
static int g_tilePanesDrawn = -1;
static float g_tilePaneX0[16] = {}, g_tilePaneW[16] = {};
static float g_tileCanvasW = 0;    // what it had to work with, when it refused

// What the pane badges ACTUALLY said on the last frame drawCanvas drew, one
// "letter|context|name;" entry per badge, as drawn (i.e. after any eliding).
// Same reasoning as g_tilePanesDrawn: this machine cannot screenshot GL, so
// --tile-selftest T12 asserts the composition as state.
static std::string g_paneBadgeProbe;
// What the footer strip under the canvas ACTUALLY said on the last frame
// drawCanvas drew: "ctx=<batch  >  series>;zoom=<zoom NN%>;name=<file>;count=
// <n/N>;", empty when there was no image and so no strip. Same reasoning as
// g_paneBadgeProbe, and the same rule: it records the STRINGS that went to
// AddText, not the numbers they were built from - the zoom readout has two
// formats and the one it chose is the thing worth asserting, which a test
// re-deriving "zoom * 100" would never see. (docs/verification/ui.md E3: the row was
// moved out of the A section precisely because nothing here was observable.)
static std::string g_footerProbe;
static std::string elideFront(const std::string& s, size_t keep);   // fwd
// The batch a document belongs to - the context line of the two-line identity
// (the footer already leads with it; the pane badges follow the same rule).
static std::string batchNameOf(const ImageDoc* d) {
    if (d) for (const auto& b : app.batches) if (b.id == d->batchId) return b.name;
    return std::string();
}

static void drawCanvas(ImVec2 avail) {
    g_paneBadgeProbe.clear();
    g_footerProbe.clear();
    // DPI/font-aware ruler geometry (fixed px constants break on 150-200% Windows scaling)
    const float s = app.uiScale;
    const float RULER_H = ImGui::GetFontSize() + 5.0f * s;
    const float RULER_W = ImGui::CalcTextSize("00000").x + 8.0f * s;
    const float TICK = 7.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    ImageDoc* im = cur();
    // The footer strip (name + scrub bar) is RESERVED below the canvas, never
    // drawn over the pixels: overlapping the image was vetoed outright, and a
    // bar over the bottom rows would sit exactly where shading is judged.
    // two lines: batch above, file/stack below - a filename alone cannot say
    // WHICH frame_000.npy this is when every level folder holds one
    const float FOOT_H = im ? ImGui::GetFontSize() * 2 + 12.0f * s : 0.0f;
    ImVec2 canvasP0 = ImVec2(origin.x + RULER_W, origin.y + RULER_H);
    ImVec2 canvasSize = ImVec2(std::max(avail.x - RULER_W, 50.0f),
                               std::max(avail.y - RULER_H - FOOT_H, 50.0f));
    ImVec2 canvasP1 = ImVec2(canvasP0.x + canvasSize.x, canvasP0.y + canvasSize.y);

    // interaction region = canvas (excluding rulers)
    ImGui::SetCursorScreenPos(canvasP0);
    ImGui::InvisibleButton("canvas", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    // ---- A/B compare geometry -------------------------------------------------
    // Split: two panes side by side, each showing the same image region, so the
    // eye compares by saccade. Wipe: one pane, B clipped to the right of a
    // draggable divider, so the eye compares by edge. Both share app.view.
    ImageDoc* imB = cmpB();
    const float GUTTER = 3.0f * s;
    // ---- more than two slots: tile them, side by side -------------------------
    // The tiled layout REPLACES the A/B split when it engages (see the `split`
    // line below): the same gesture, one pane per slot, one shared view.
    std::vector<TileSlot> slots;
    std::vector<TilePane> tiles;
    bool tiled = false;
    std::string tileNarrow;             // why there are no tiles, when there are none
    if (tileEngaged()) {
        slots = tileSlots();
        const float minW = tileMinPaneW(s);
        tiled = tileLayout(canvasP0.x, canvasSize.x, (int)slots.size(),
                           GUTTER, minW, tiles);
        if (!tiled) {
            // Degrade by SAYING SO. Slivers of five images side by side look
            // like a comparison and cannot be read as one.
            char m[224];
            snprintf(m, sizeof m,
                     "%d compare panes need at least %.0f px each - this canvas is "
                     "%.0f px. Widen the window, or drop a slot.",
                     (int)slots.size(), minW, canvasSize.x);
            tileNarrow = m;
            slots.clear();
        }
    }
    // A shared range moves as you step (mode 2 refits to the compared frames,
    // slots included), and a
    // texture built with the old one is a wrong picture, not a stale label - so
    // both sides are checked against what they would be built with NOW.
    for (ImageDoc* d : { im, imB })
        if (d && (d->texBlack != effBlack(*d) || d->texWhite != effWhite(*d)))
            d->texDirty = true;
    // ...and so is every extra slot: a tile is on screen exactly as much as B is
    for (const TileSlot& t : slots)
        if (t.doc && (t.doc->texBlack != effBlack(*t.doc) || t.doc->texWhite != effWhite(*t.doc)))
            t.doc->texDirty = true;
    const bool split = imB && app.compareMode == App::CmpSplit && !tiled;
    const bool wipe  = imB && app.compareMode == App::CmpWipe;
    const bool diffMode = imB && im && app.compareMode == App::CmpDiff &&
                          imB->w == im->w && imB->h == im->h;
    const bool flipMode = imB && app.compareMode == App::CmpFlip;
    if (flipMode && app.flipAuto) {          // alternate on its own
        double now = ImGui::GetTime();
        if (now >= app.flipNext) {
            app.flipShowB = !app.flipShowB;
            app.flipNext = now + std::max(app.flipPeriod, 0.05f);
        }
    }
    // split panes are clamped harder than the wipe divider: a 3%-wide pane is a
    // useless pane, and "fit" into it would shrink the image to nothing
    if (split) app.splitFrac = std::clamp(app.splitFrac, 0.12f, 0.88f);
    float splitX = canvasP0.x + canvasSize.x * app.splitFrac;   // pane boundary (split)
    float wipeX  = canvasP0.x + canvasSize.x * app.wipeFrac;    // divider (wipe)
    ImVec2 paneAp0 = canvasP0, paneAsz = canvasSize, paneBp0 = canvasP0, paneBsz = canvasSize;
    if (split) {
        // no artificial floor here: the pane the image is centred on must be the
        // pane it is clipped to, or it draws off-centre from the visible strip
        paneAsz.x = std::max(splitX - canvasP0.x - GUTTER, 1.0f);
        paneBp0.x = std::min(splitX + GUTTER, canvasP1.x - 1.0f);
        paneBsz.x = std::max(canvasP1.x - paneBp0.x, 1.0f);
    }
    // Panes BY INDEX. 0 = A, 1 = B: with no extras there are exactly the two the
    // split has always had (identical rectangles unless split is on), and `bool`
    // still indexes them, so every A/B call site below reads as it always did.
    // With extras there is one pane per slot, in slot order.
    std::vector<ImVec2> paneOrg, paneDim;
    if (tiled) {
        for (const TilePane& t : tiles) {
            paneOrg.push_back(ImVec2(t.x0, canvasP0.y));
            paneDim.push_back(ImVec2(t.w, canvasSize.y));
        }
    } else {
        paneOrg = { paneAp0, paneBp0 };
        paneDim = { paneAsz, paneBsz };
    }
    const int paneCount = (int)paneOrg.size();
    // In split mode the pane under the cursor defines image coordinates; in wipe
    // mode both images occupy the same pane, so there is nothing to switch.
    auto paneP0 = [&](int i) { return paneOrg[std::clamp(i, 0, paneCount - 1)]; };
    auto paneSz = [&](int i) { return paneDim[std::clamp(i, 0, paneCount - 1)]; };
    auto mapIn = [&](int p, float ix, float iy) {
        return tileMap(paneP0(p), paneSz(p), app.view.center, app.view.zoom, ix, iy);
    };
    auto unmapIn = [&](int p, ImVec2 sc) {
        return tileUnmap(paneP0(p), paneSz(p), app.view.center, app.view.zoom, sc);
    };
    auto inBPane = [&](ImVec2 sc) { return split && sc.x >= paneBp0.x; };
    // Which pane the pointer is in: tiled, the last one whose left edge it has
    // passed, so the gutter belongs to the pane on its left instead of to nobody.
    auto paneAt = [&](ImVec2 sc) -> int {
        if (!tiled) return inBPane(sc) ? 1 : 0;
        int k = 0;
        for (int i = 0; i < paneCount; i++) if (sc.x >= paneOrg[i].x) k = i;
        return k;
    };
    // "fit" must fit the pane the image is drawn in, not the whole canvas. A is
    // the reference, so fit A's pane; every other pane is the same view by
    // construction - which is the point, and why fit is not computed per pane.
    ImVec2 fitSize = (tiled || split) ? ImVec2(paneSz(0).x, canvasSize.y) : canvasSize;
    if (im && app.fitRequested) { fitToCanvas(fitSize); app.fitRequested = false; }

    auto imgToScr = [&](float ix, float iy) { return mapIn(0, ix, iy); };
    auto scrToImg = [&](ImVec2 sc) { return unmapIn(paneAt(sc), sc); };
    // A drag must stay in the pane it started in. Re-resolving per call would jump
    // the coordinates by half a canvas the moment the cursor crosses the boundary,
    // collapsing the ROI being drawn (or teleporting the one being moved).
    static int dragPane = 0;
    auto dragToImg = [&](ImVec2 sc) { return unmapIn(dragPane, sc); };

    // the divider is grabbed anywhere along its height, not just on the handle.
    // Tiled there is no divider: the panes are equal by rule, and splitFrac
    // belongs to the two-pane case alone.
    bool onDivider = imB && !tiled && app.compareMode != App::CmpDiff &&
                     fabsf(io.MousePos.x - (split ? splitX : wipeX)) <= 7.0f * s &&
                     io.MousePos.y >= canvasP0.y && io.MousePos.y <= canvasP1.y;
    if (onDivider && (hovered || ImGui::IsItemActive())) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // drag state: pan / new-ROI / move / resize (tool-mode interaction model)
    enum { DK_NONE, DK_PAN, DK_ROI_NEW, DK_ANN_MOVE, DK_ANN_RESIZE, DK_DIVIDER };
    static int dk = DK_NONE;
    static bool dragMoved = false;
    static bool clickEligible = false;   // left-button, no pan modifiers: clicks act on tools
    static ImVec2 drag0;
    static int dragAnnId = -1, dragCorner = -1;
    static bool dragSplitDivider = false;   // which divider a DK_DIVIDER drag grabbed
    static float dragDividerDx = 0;         // cursor offset from it, so it does not snap
    static int annOrig[4] = {};
    static int tmpRect[4] = {};
    static bool tmpActive = false;

    auto hitTest = [&](ImVec2 mimg, int& cornerOut) -> int {   // returns Ann::id or -1
        cornerOut = -1;
        float tol = 8.0f / app.view.zoom;
        for (int i = (int)app.anns.size() - 1; i >= 0; i--) {  // topmost first
            const App::Ann& a = app.anns[i];
            if (!a.visible) continue;
            if (a.type == 1) {
                if (fabsf(mimg.x - (a.x + 0.5f)) < tol && fabsf(mimg.y - (a.y + 0.5f)) < tol)
                    return a.id;
            } else {
                float xs[2] = { (float)a.x, (float)(a.x + a.w) };
                float ys[2] = { (float)a.y, (float)(a.y + a.h) };
                for (int cy = 0; cy < 2; cy++)
                    for (int cx = 0; cx < 2; cx++)
                        if (fabsf(mimg.x - xs[cx]) < tol && fabsf(mimg.y - ys[cy]) < tol) {
                            cornerOut = cy * 2 + cx;
                            return a.id;
                        }
                if (mimg.x >= a.x && mimg.x <= a.x + a.w && mimg.y >= a.y && mimg.y <= a.y + a.h)
                    return a.id;
            }
        }
        return -1;
    };

    if (!im) {   // image closed mid-drag: do not leave a stale preview or drag kind
        dk = DK_NONE; tmpActive = false; dragAnnId = -1; app.annBusy = false;
    }
    if (im) {
        if (ImGui::IsItemActivated()) {
            dragPane = paneAt(io.MousePos);        // latched for the whole drag
            drag0 = dragToImg(io.MousePos);
            dragMoved = false;
            dragAnnId = -1; dragCorner = -1; tmpActive = false;
            bool mid = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
            bool space = ImGui::IsKeyDown(ImGuiKey_Space);
            clickEligible = !mid && !space && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            // Modeless: middle / Space always pan. Otherwise the press position
            // decides - on a handle = resize, inside a ROI = move, empty = new ROI
            // (or pan, if the user prefers that; Shift inverts either way).
            if (mid || space) {
                dk = DK_PAN;
            } else {
                int corner; int hit = hitTest(drag0, corner);
                App::Ann* a = hit >= 0 ? findAnn(hit) : nullptr;
                // the divider yields to annotations: "the press position decides"
                // must keep meaning what it says even under the divider
                if (onDivider && hit < 0) {
                    dk = DK_DIVIDER;
                    dragSplitDivider = split;
                    dragDividerDx = io.MousePos.x - (split ? splitX : wipeX);
                    clickEligible = false;   // grabbing the divider is not a click on the image
                } else if (a && a->type == 0 && corner >= 0) {
                    dk = DK_ANN_RESIZE; dragAnnId = hit; dragCorner = corner;
                    annOrig[0] = a->x; annOrig[1] = a->y; annOrig[2] = a->w; annOrig[3] = a->h;
                    app.selectedAnn = hit;
                } else if (a && a->type == 0) {
                    dk = DK_ANN_MOVE; dragAnnId = hit;
                    annOrig[0] = a->x; annOrig[1] = a->y;
                    app.selectedAnn = hit;
                } else {
                    bool wantPan = app.dragPans != io.KeyShift;   // Shift inverts the default
                    dk = wantPan ? DK_PAN : DK_ROI_NEW;
                }
            }
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f)))
            dragMoved = true;
        bool draggingAny = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) ||
                           ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0);
        if (active && draggingAny) {
            if (dk == DK_DIVIDER) {
                // keep the grab offset: the divider must not snap to the cursor
                float f = (io.MousePos.x - dragDividerDx - canvasP0.x) / std::max(canvasSize.x, 1.0f);
                f = std::clamp(f, dragSplitDivider ? 0.12f : 0.03f, dragSplitDivider ? 0.88f : 0.97f);
                // which divider was grabbed is decided at press time: switching
                // mode mid-drag must not move the other one
                (dragSplitDivider ? app.splitFrac : app.wipeFrac) = f;
            } else if (dk == DK_PAN) {
                app.view.center.x -= io.MouseDelta.x / app.view.zoom;
                app.view.center.y -= io.MouseDelta.y / app.view.zoom;
            } else if (dk == DK_ROI_NEW && dragMoved) {
                ImVec2 q = dragToImg(io.MousePos);
                float x0 = std::clamp(std::min(drag0.x, q.x), 0.0f, (float)im->w);
                float x1 = std::clamp(std::max(drag0.x, q.x), 0.0f, (float)im->w);
                float y0 = std::clamp(std::min(drag0.y, q.y), 0.0f, (float)im->h);
                float y1 = std::clamp(std::max(drag0.y, q.y), 0.0f, (float)im->h);
                tmpRect[0] = (int)x0; tmpRect[1] = (int)y0;
                tmpRect[2] = std::max(0, (int)ceilf(x1) - tmpRect[0]);
                tmpRect[3] = std::max(0, (int)ceilf(y1) - tmpRect[1]);
                tmpActive = true;
            } else if (dk == DK_ANN_MOVE && dragMoved) {
                if (App::Ann* a = findAnn(dragAnnId)) {
                    ImVec2 q = dragToImg(io.MousePos);
                    int dx = (int)roundf(q.x - drag0.x), dy = (int)roundf(q.y - drag0.y);
                    a->x = std::clamp(annOrig[0] + dx, 0, std::max(0, im->w - std::max(a->w, 1)));
                    a->y = std::clamp(annOrig[1] + dy, 0, std::max(0, im->h - std::max(a->h, 1)));
                    app.annRev++;
                }
            } else if (dk == DK_ANN_RESIZE && dragMoved) {
                if (App::Ann* a = findAnn(dragAnnId)) {
                    int fx = (dragCorner % 2 == 0) ? annOrig[0] + annOrig[2] : annOrig[0];
                    int fy = (dragCorner / 2 == 0) ? annOrig[1] + annOrig[3] : annOrig[1];
                    ImVec2 q = dragToImg(io.MousePos);
                    int mx = std::clamp((int)roundf(q.x), 0, im->w);
                    int my = std::clamp((int)roundf(q.y), 0, im->h);
                    a->x = std::min(fx, mx); a->w = std::max(1, std::abs(mx - fx));
                    a->y = std::min(fy, my); a->h = std::max(1, std::abs(my - fy));
                    app.annRev++;
                }
            }
        }
        if (ImGui::IsItemDeactivated()) {
            ImVec2 q = dragToImg(io.MousePos);
            if (dk == DK_ROI_NEW && dragMoved && tmpActive) {
                // A long thin drag is a deliberate row/column band, so require
                // extent in EITHER axis, not both - the old "3px in both" rule
                // made a 1px-tall band impossible to draw.
                if (tmpRect[2] >= 1 && tmpRect[3] >= 1 &&
                    (tmpRect[2] >= 3 || tmpRect[3] >= 3) && !annBlockedOnPreview())
                    addAnn(0, tmpRect[0], tmpRect[1], tmpRect[2], tmpRect[3]);
            } else if (!dragMoved && clickEligible) {   // middle/Space clicks never select
                int corner;
                int hit = hitTest(q, corner);
                app.selectedAnn = hit >= 0 ? hit : 0;   // empty click -> back to "All"
            }
            tmpActive = false;
            dk = DK_NONE; dragAnnId = -1;
        }
        app.annBusy = dk == DK_ANN_MOVE || dk == DK_ANN_RESIZE || dk == DK_ROI_NEW;

        // wheel: Ctrl(/Cmd)+wheel = zoom, plain wheel = pan (View menu can invert)
        if (hovered && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
            bool zoomMod = io.KeyCtrl || io.KeySuper;
            // plain-wheel-zoom mode still leaves Shift+wheel as horizontal pan
            bool zoomGesture = app.wheelZoomPlain ? (!zoomMod && !io.KeyShift) : zoomMod;
            if (zoomGesture && io.MouseWheel != 0) {
                float wheel = std::clamp(io.MouseWheel, -3.0f, 3.0f);   // tame trackpad inertia
                ImVec2 mImg = scrToImg(io.MousePos);
                float z = std::clamp(app.view.zoom * powf(1.25f, wheel), 1.0f / 512, 256.0f);
                // anchor in the pane under the cursor: in split (and tiled) mode
                // the image is centred on its pane, not on the canvas
                const int wp = paneAt(io.MousePos);
                ImVec2 ap0 = paneP0(wp), asz = paneSz(wp);
                app.view.center.x = mImg.x - (io.MousePos.x - ap0.x - asz.x * 0.5f) / z;
                app.view.center.y = mImg.y - (io.MousePos.y - ap0.y - asz.y * 0.5f) / z;
                app.view.zoom = z;
            } else {
                float step = 80.0f / app.view.zoom;                     // image px per notch
                if (io.MouseWheel != 0) {
                    if (io.KeyShift) app.view.center.x -= io.MouseWheel * step;
                    else app.view.center.y -= io.MouseWheel * step;
                }
                if (io.MouseWheelH != 0) app.view.center.x += io.MouseWheelH * step;
            }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fitToCanvas(fitSize);

        // right-click: actions at this point, no modes and no modifiers needed
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) {
            ImVec2 q = scrToImg(io.MousePos);
            app.ctxX = (int)floorf(q.x); app.ctxY = (int)floorf(q.y);
            int corner; int hit = hitTest(q, corner);
            if (hit >= 0) app.selectedAnn = hit;
            ImGui::OpenPopup("canvasctx");
        }
        if (ImGui::BeginPopup("canvasctx")) {
            bool inRange = app.ctxX >= 0 && app.ctxY >= 0 && app.ctxX < im->w && app.ctxY < im->h;
            if (ImGui::MenuItem("Add pin here", "P", false, inRange) && !annBlockedOnPreview())
                addAnn(1, app.ctxX, app.ctxY, 0, 0);
            // NOT a cached pointer. app.anns is a std::vector, the two band
            // items below push_back into it, and ImGui's MenuItem only marks
            // the popup closed - the rest of this body still runs, so the four
            // items after them would read (and, on a later frame, write)
            // through freed heap when the vector reallocated. addAnn also
            // reassigns app.selectedAnn, so even without a reallocation a
            // cached pointer stops denoting the selected annotation.
            auto sel = [] { return findAnn(app.selectedAnn); };
            ImGui::Separator();
            // a single row / column is one pixel thick and cannot be dragged out
            if (ImGui::MenuItem("Select this row (1 px)", "X", false, inRange)) {
                addAnn(0, 0, app.ctxY, im->w, 1);
                if (App::Ann* n = findAnn(app.selectedAnn)) {
                    n->prevX = app.ctxX; n->prevW = 1;
                    n->label = "row " + std::to_string(app.ctxY);
                }
            }
            if (ImGui::MenuItem("Select this column (1 px)", "Y", false, inRange)) {
                addAnn(0, app.ctxX, 0, 1, im->h);
                if (App::Ann* n = findAnn(app.selectedAnn)) {
                    n->prevY = app.ctxY; n->prevH = 1;
                    n->label = "col " + std::to_string(app.ctxX);
                }
            }
            {
                App::Ann* s1 = sel();
                if (ImGui::MenuItem("Widen ROI to full width", nullptr, false,
                                    s1 && s1->type == 0))
                    if (App::Ann* a1 = sel()) {
                        a1->prevX = a1->x; a1->prevW = a1->w;
                        a1->x = 0; a1->w = im->w; app.annRev++;
                    }
                App::Ann* s2 = sel();
                if (ImGui::MenuItem("Widen ROI to full height", nullptr, false,
                                    s2 && s2->type == 0))
                    if (App::Ann* a2 = sel()) {
                        a2->prevY = a2->y; a2->prevH = a2->h;
                        a2->y = 0; a2->h = im->h; app.annRev++;
                    }
                App::Ann* s3 = sel();
                if (ImGui::MenuItem("Crop image to this ROI", nullptr, false,
                                    s3 && s3->type == 0))
                    cropCurrentToSelectedRoi();
                if (ImGui::MenuItem("Delete", "Del", false, sel() != nullptr))
                    deleteAnn(app.selectedAnn);
            }
            ImGui::Separator();
            {   // out of the tool and into a report
                ImageDoc* ci = cur();
                // The label is the CHORD, printed by ImGui from the same
                // constant core/main.cpp tests for. It used to be the string
                // "Ctrl+Shift+C", and nothing anywhere listened for it.
                if (ImGui::MenuItem("Copy image (dot by dot)",
                                    ImGui::GetKeyChordName(CHORD_COPY_DOT),
                                    false, ci != nullptr))
                    copyViewDotByDot();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("one image pixel per copied pixel, with the range,\n"
                                      "gamma and colormap you see. No resampling.");
                if (ImGui::MenuItem("Save image as PNG...", nullptr, false, ci != nullptr)) {
                    if (!pfd::settings::available())
                        toast("no file-dialog backend found (install zenity or kdialog)", true);
                    else
                        app.pngDlgUid = ci->uid;   // the name below is ci's: the pixels must be too
                        app.pngDlg = std::make_unique<pfd::save_file>("Save image as PNG",
                            baseName(ci->name) + ".png",
                            std::vector<std::string>{ "PNG (*.png)", "*.png" });
                }
                const bool inStack = ci && ci->seqId != 0;
                App::Ann* ra = findAnn(app.selectedAnn);
                const bool roiOk = ra && ra->type == 0;
                if (ImGui::BeginMenu("Montage ROI across the stack", inStack && roiOk)) {
                    std::string merr;
                    // one toggle for both orientations, not four menu lines
                    static bool mPerFrame = false;
                    if (ImGui::MenuItem("side by side (horizontal)"))
                        if (!montageROI(true, merr, mPerFrame)) toast(merr, true);
                    if (ImGui::MenuItem("stacked (vertical)"))
                        if (!montageROI(false, merr, mPerFrame)) toast(merr, true);
                    ImGui::Separator();
                    ImGui::MenuItem("per-frame auto range", nullptr, &mPerFrame);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("each tile normalised to its own min..max (0..1):\n"
                                          "an exposure sweep reads instead of the dark frames\n"
                                          "going black. The values stop being DN - the montage\n"
                                          "says so in its name and note. View only.");
                    ImGui::EndMenu();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !(inStack && roiOk))
                    ImGui::SetTooltip(!inStack ? "the current image is not part of a stack"
                                               : "select a rectangle ROI first");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Fit to window", "F")) app.fitRequested = true;
            if (ImGui::MenuItem("Actual size", "1")) app.view.zoom = 1.0f;
            ImGui::EndPopup();
        }
    }

    // hover position; in split mode the pane under the cursor decides which image
    // the readout is about, so a bigger B is not cut off at A's extent
    app.hoverX = app.hoverY = -1;
    app.hoverInB = false;
    if (im && hovered) {
        const int p = paneAt(io.MousePos);
        const bool inB = imB && p == 1 && (split || tiled);
        // The status readout can name A and B and nothing else. Over a C/D/E
        // pane it would print A's DN under A's label - a wrong number, in a
        // tool whose entire job is the number - so it prints none. (Interim:
        // the extra slots are drawn, not yet inspected.)
        const bool readable = !tiled || p < (imB ? 2 : 1);
        const ImageDoc* ref = inB ? imB : im;
        ImVec2 q = unmapIn(p, io.MousePos);
        int ix = (int)floorf(q.x), iy = (int)floorf(q.y);
        if (readable && ix >= 0 && iy >= 0 && ix < ref->w && iy < ref->h) {
            app.hoverX = ix; app.hoverY = iy; app.hoverInB = inB;
        }
    }

    // ---- draw canvas ----
    g_tilePanesDrawn = tileNarrow.empty() ? -1 : 0;   // 0: wanted tiles, too narrow
    g_tileCanvasW = canvasSize.x;
    dl->AddRectFilled(canvasP0, canvasP1, IM_COL32(12, 14, 16, 255));
    dl->PushClipRect(canvasP0, canvasP1, true);
    // One image and its overlays, drawn through the given pane's mapping. Used
    // once when compare is off, twice (A and B) when it is on, once per slot
    // when the extra slots are tiled.
    auto drawImageOnly = [&](ImageDoc* d, int p) {
        auto map = [&](float ix, float iy) { return mapIn(p, ix, iy); };
        if (d->texDirty) rebuildTexture(*d);
        else touchTex(*d);                 // keep it resident: it is on screen
        setFilter(*d, app.view.zoom >= 1.0f);
        ImVec2 p0 = map(0, 0), p1 = map((float)d->w, (float)d->h);
        dl->AddImage((ImTextureID)(intptr_t)d->tex, p0, p1);
        dl->AddRect(p0, p1, IM_COL32(90, 100, 110, 255));
    };
    // Overlays are drawn ONCE per pane. In wipe mode both images share one pane,
    // so drawing them per image would double the alpha of every ROI fill and grid
    // line right of the divider - a brightness step exactly where you are looking.
    auto drawOverlays = [&](ImageDoc* d, int p) {
        auto map = [&](float ix, float iy) { return mapIn(p, ix, iy); };
        // pixel grid at high zoom
        if (app.showGrid && app.view.zoom >= 8.0f) {
            ImVec2 tl = unmapIn(p, canvasP0), br = unmapIn(p, canvasP1);
            int x0 = std::max(0, (int)floorf(tl.x)), x1 = std::min(d->w, (int)ceilf(br.x));
            int y0 = std::max(0, (int)floorf(tl.y)), y1 = std::min(d->h, (int)ceilf(br.y));
            ImU32 gc = IM_COL32(120, 120, 120, 70);
            for (int x = x0; x <= x1; x++) { ImVec2 a = map((float)x, (float)y0), b2 = map((float)x, (float)y1); dl->AddLine(a, b2, gc); }
            for (int y = y0; y <= y1; y++) { ImVec2 a = map((float)x0, (float)y), b2 = map((float)x1, (float)y); dl->AddLine(a, b2, gc); }
        }
        // hovered pixel outline
        if (app.hoverX >= 0 && app.view.zoom >= 2.0f) {
            ImVec2 a = map((float)app.hoverX, (float)app.hoverY);
            ImVec2 b2 = map((float)app.hoverX + 1, (float)app.hoverY + 1);
            dl->AddRect(a, b2, IM_COL32(255, 184, 77, 255), 0, 0, 1.5f);
        }
        // annotations (ROIs + POIs)
        for (const auto& a : app.anns) {
            if (!a.visible) continue;
            ImU32 col = ANN_COLORS[a.color & 7];
            bool sel = a.id == app.selectedAnn;
            if (a.type == 0) {
                ImVec2 ra = map((float)a.x, (float)a.y);
                ImVec2 rb = map((float)(a.x + a.w), (float)(a.y + a.h));
                dl->AddRectFilled(ra, rb, (col & 0x00FFFFFF) | 0x1E000000);
                dl->AddRect(ra, rb, col, 0, 0, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), col, a.label.c_str());
                if (sel) {                                        // resize handles
                    ImVec2 hs[4] = { ra, ImVec2(rb.x, ra.y), ImVec2(ra.x, rb.y), rb };
                    for (const auto& hp : hs)
                        dl->AddRectFilled(ImVec2(hp.x - 3, hp.y - 3), ImVec2(hp.x + 3, hp.y + 3), col);
                }
            } else {
                if (a.x >= d->w || a.y >= d->h) continue;         // outside this image
                ImVec2 cpt = map(a.x + 0.5f, a.y + 0.5f);
                float r = sel ? 10.0f : 8.0f;
                dl->AddLine(ImVec2(cpt.x - r, cpt.y), ImVec2(cpt.x + r, cpt.y), col, sel ? 2.5f : 1.5f);
                dl->AddLine(ImVec2(cpt.x, cpt.y - r), ImVec2(cpt.x, cpt.y + r), col, sel ? 2.5f : 1.5f);
                dl->AddText(ImVec2(cpt.x + 5, cpt.y + 4), col, a.label.c_str());
            }
        }
        // ROI being created (live preview)
        if (tmpActive && tmpRect[2] > 0 && tmpRect[3] > 0) {
            ImVec2 ra = map((float)tmpRect[0], (float)tmpRect[1]);
            ImVec2 rb = map((float)(tmpRect[0] + tmpRect[2]), (float)(tmpRect[1] + tmpRect[3]));
            dl->AddRectFilled(ra, rb, IM_COL32(77, 163, 255, 26));
            dl->AddRect(ra, rb, IM_COL32(77, 163, 255, 220), 0, 0, 1.5f);
            char lb[48];
            snprintf(lb, 48, "%dx%d", tmpRect[2], tmpRect[3]);
            dl->AddText(ImVec2(ra.x + 3, ra.y - ImGui::GetFontSize() - 2), IM_COL32(120, 190, 255, 255), lb);
        }
    };

    // Hold B to see B full-frame: flicking between two full images is how small
    // differences become visible - a fixed divider cannot show them.
    bool flashB = imB && !flipMode && !ImGui::GetIO().WantTextInput && ImGui::IsKeyDown(ImGuiKey_B) &&
                  !ImGui::IsKeyDown(ImGuiKey_LeftShift) && !ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (im) {
        if (flipMode) {
            ImageDoc* d = app.flipShowB ? imB : im;
            drawImageOnly(d, 0);
            drawOverlays(d, 0);
        } else if (flashB) {
            drawImageOnly(imB, 0);
            drawOverlays(imB, 0);
        } else if (diffMode) {
            ensureDiffTexture(*im, *imB);
            if (app.diff.tex) {
                ImVec2 p0 = mapIn(0, 0, 0);
                ImVec2 p1 = mapIn(0, (float)app.diff.w, (float)app.diff.h);
                dl->AddImage((ImTextureID)(intptr_t)app.diff.tex, p0, p1);
                dl->AddRect(p0, p1, IM_COL32(90, 100, 110, 255));
            }
            drawOverlays(im, 0);
        } else if (tiled) {
            // One pane per slot, in slot order, all on the SAME view. Each is
            // clipped to its own pane, so a pane that cannot hold its image at
            // the shared zoom shows less of it - never a different zoom.
            g_tilePanesDrawn = (int)slots.size();
            for (int i = 0; i < (int)slots.size(); i++) {
                dl->PushClipRect(paneP0(i), ImVec2(paneP0(i).x + paneSz(i).x, canvasP1.y), true);
                drawImageOnly(slots[i].doc, i);
                drawOverlays(slots[i].doc, i);
                dl->PopClipRect();
                if (i < 16) { g_tilePaneX0[i] = paneP0(i).x; g_tilePaneW[i] = paneSz(i).x; }
            }
        } else if (split) {
            dl->PushClipRect(paneAp0, ImVec2(paneAp0.x + paneAsz.x, canvasP1.y), true);
            drawImageOnly(im, 0); drawOverlays(im, 0);
            dl->PopClipRect();
            dl->PushClipRect(paneBp0, ImVec2(paneBp0.x + paneBsz.x, canvasP1.y), true);
            drawImageOnly(imB, 1); drawOverlays(imB, 1);
            dl->PopClipRect();
        } else if (wipe) {
            drawImageOnly(im, 0);                       // A everywhere...
            dl->PushClipRect(ImVec2(wipeX, canvasP0.y), canvasP1, true);
            drawImageOnly(imB, 0);                      // ...B over the right side
            dl->PopClipRect();
            drawOverlays(im, 0);                        // ...and one set of overlays
        } else {
            drawImageOnly(im, 0);
            drawOverlays(im, 0);
        }
        // ---- compare chrome: slot badges + the divider you drag ----
        // The badge lives out here, not inside `if (imB)`: a tiled canvas can
        // hold A + C + D with no B at all, and every pane still has to say
        // which slot and which file it is.
        {
            const ImU32 colA = IM_COL32(120, 200, 255, 255), colB = IM_COL32(255, 190, 120, 255);
            const float pad = 4.0f * s;
            // Each pane carries the SAME two-line identity the footer below
            // establishes: line 1 = letter + batch (the context, dimmer),
            // line 2 = the file name (the subject) - user report 2026-07-30,
            // 「一行目にBatch名，二行目がファイル名」. The letter keeps the
            // slot colour (it is what tells the panes apart); the name takes
            // the footer's own neutral ink. A badge must never sit over a
            // neighbour's pixels, so each line is fitted to its own pane:
            // elide the FRONT (the tail is what tells two captures apart),
            // then drop the line entirely - and the plate covers only the
            // rows its surviving lines need, because it sits over the image.
            auto badge = [&](float x0, float x1, bool rightAlign, const char* tag,
                             const std::string& ctx, const std::string& nm, ImU32 col) {
                float avail = x1 - x0 - pad * 2;
                float tagW = ImGui::CalcTextSize(tag).x;
                if (tagW > avail) return;                     // no room at all
                const float gap = 8.0f * s;
                std::string l1 = ctx, l2 = nm;
                auto fitLine = [&](std::string& t, float room) {
                    if (t.empty() || ImGui::CalcTextSize(t.c_str()).x <= room) return;
                    for (size_t keep = t.size() > 4 ? t.size() - 1 : 0; keep >= 4; keep--) {
                        std::string cand = elideFront(t, keep);
                        if (ImGui::CalcTextSize(cand.c_str()).x <= room) { t = cand; return; }
                    }
                    t.clear();                    // narrower than "...abcd": drop it
                };
                fitLine(l1, avail - tagW - gap);
                fitLine(l2, avail);
                const float lh = ImGui::GetTextLineHeight();
                float w1 = tagW + (l1.empty() ? 0.0f : gap + ImGui::CalcTextSize(l1.c_str()).x);
                float w2 = l2.empty() ? 0.0f : ImGui::CalcTextSize(l2.c_str()).x;
                float bw = std::max(w1, w2);
                float bh = lh * (l2.empty() ? 1 : 2) + pad;
                float bx = rightAlign ? x1 - bw - pad * 2 : x0;
                float by = canvasP0.y + 6 * s;
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw + pad * 2, by + bh),
                                  IM_COL32(0, 0, 0, 180), 3.0f * s);
                dl->AddText(ImVec2(bx + pad, by + pad * 0.5f), col, tag);
                if (!l1.empty())
                    dl->AddText(ImVec2(bx + pad + tagW + gap, by + pad * 0.5f),
                                IM_COL32(150, 158, 166, 220), l1.c_str());
                if (!l2.empty())
                    dl->AddText(ImVec2(bx + pad, by + pad * 0.5f + lh),
                                IM_COL32(210, 216, 226, 235), l2.c_str());
                g_paneBadgeProbe += std::string(tag) + "|" + l1 + "|" + l2 + ";";
            };
            bool hot = onDivider || (ImGui::IsItemActive() && dk == DK_DIVIDER);
            // C, D, E...: one hue each. "Which pane is D" has to be answerable
            // from the badge alone - A's blue and B's orange stay where they are.
            static const ImU32 TILE_COLS[6] = {
                IM_COL32(105, 220, 130, 255), IM_COL32(200, 120, 255, 255),
                IM_COL32(90, 220, 220, 255),  IM_COL32(255, 150, 200, 255),
                IM_COL32(180, 200, 90, 255),  IM_COL32(255, 120, 120, 255),
            };
            // Hold-B outranks the tiles: it deliberately takes the whole canvas,
            // so its badge - not four badges over one full-frame image - is what
            // has to be on screen.
            if (flashB) {
                badge(canvasP0.x + 6 * s, canvasP1.x, false, "B", batchNameOf(imB),
                      imB->name + "  (hold B)", colB);
            } else if (tiled) {
                const int nAB = imB ? 2 : 1;                 // how many letters are A/B
                for (int i = 0; i < (int)slots.size(); i++) {
                    ImU32 c = slots[i].tag == "A" ? colA
                            : slots[i].tag == "B" ? colB
                            : TILE_COLS[std::max(0, i - nAB) % 6];
                    // a diverged side is the last frame its stack had, not A's
                    // partner - the same [stale] statement the panels make
                    badge(paneP0(i).x + 6 * s, paneP0(i).x + paneSz(i).x, false,
                          slots[i].tag.c_str(), batchNameOf(slots[i].doc),
                          slots[i].stale ? slots[i].doc->name + "  [stale]"
                                         : slots[i].doc->name, c);
                    if (i)   // the gutter band, the same one the A/B divider fills
                        dl->AddRectFilled(ImVec2(paneP0(i).x - 2 * GUTTER, canvasP0.y),
                                          ImVec2(paneP0(i).x, canvasP1.y),
                                          IM_COL32(60, 66, 74, 255));
                }
            } else if (!imB) {
                // compare is on but B is gone (closed, renamed, never picked).
                // A standing on the pinned B used to land here too, under an
                // "A = B (paused)" banner; it now resolves as an ordinary B and
                // draws as one, so the only case left is the honest absence.
                if (app.compareMode != App::CmpOff) {
                    const char* msg =
                        "compare is on, but no B image - View > Compare A/B > B image";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    float y = canvasP0.y + 6 * s;
                    dl->AddRectFilled(ImVec2(canvasP0.x + 6 * s, y),
                                      ImVec2(canvasP0.x + 6 * s + ts.x + 12 * s, y + ts.y + 6 * s),
                                      IM_COL32(90, 60, 20, 210), 3.0f * s);
                    dl->AddText(ImVec2(canvasP0.x + 12 * s, y + 3 * s), IM_COL32(255, 200, 120, 255), msg);
                }
            } else if (flipMode) {
                // which side is on screen has to be readable at a glance, or a
                // blink comparison tells you nothing about WHICH one differs
                ImageDoc* d = app.flipShowB ? imB : im;
                badge(canvasP0.x + 6 * s, canvasP1.x, false,
                      app.flipShowB ? "B" : "A", batchNameOf(d),
                      d->name + (app.flipAuto ? "   (auto)" : "   (B / Space to toggle)"),
                      app.flipShowB ? colB : colA);
                // a coloured edge so peripheral vision registers the switch too
                dl->AddRect(canvasP0, canvasP1, app.flipShowB ? colB : colA, 0, 0, 3.0f * s);
            } else if (app.compareMode == App::CmpDiff) {
                // the operation is the context line; the pair of names is the
                // subject (two batches may meet here, so neither claims line 1)
                badge(canvasP0.x + 6 * s, canvasP1.x, false,
                      app.diffAbs ? "|A-B|" : "A-B", std::string(),
                      im->name + "  -  " + imB->name,
                      IM_COL32(220, 225, 235, 255));
                if (!diffMode) {
                    const char* msg = "difference needs A and B to be the same size";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    dl->AddRectFilled(ImVec2(canvasP0.x + 6 * s, canvasP0.y + 30 * s),
                                      ImVec2(canvasP0.x + 18 * s + ts.x, canvasP0.y + 30 * s + ts.y + 6 * s),
                                      IM_COL32(90, 60, 20, 210), 3.0f * s);
                    dl->AddText(ImVec2(canvasP0.x + 12 * s, canvasP0.y + 33 * s),
                                IM_COL32(255, 200, 120, 255), msg);
                } else {
                    // Colour bar with the scale spelled out: a difference image
                    // without its scale is a picture, not a measurement.
                    float gain = app.diff.gain;
                    bool intType = !im->dtype.empty() && (im->dtype[0] == 'u' || im->dtype[0] == 'i');
                    char loS[48], hiS[48];
                    snprintf(hiS, sizeof hiS, "+%g%s", gain, intType ? " DN" : "");
                    snprintf(loS, sizeof loS, app.diffAbs ? "0" : "-%g%s", gain, intType ? " DN" : "");
                    float barW = 180 * s, barH = 10 * s;
                    float bx = canvasP0.x + 10 * s;
                    float by = canvasP1.y - barH - ImGui::GetFontSize() - 12 * s;
                    dl->AddRectFilled(ImVec2(bx - 6 * s, by - 6 * s),
                                      ImVec2(bx + barW + 6 * s, by + barH + ImGui::GetFontSize() + 8 * s),
                                      IM_COL32(0, 0, 0, 180), 3.0f * s);
                    for (int i = 0; i < (int)barW; i++) {   // same ramp as the pixels
                        float t = app.diffAbs ? (float)i / barW : (float)i / barW * 2.0f - 1.0f;
                        float r, g2, bl;
                        if (app.diffAbs)   { float m = t; r = g2 = bl = m; }
                        else if (t >= 0)   { r = t; g2 = 0.35f * t; bl = 0.10f * t; }
                        else               { float m = -t; r = 0.10f * m; g2 = 0.45f * m; bl = m; }
                        ImU32 c = IM_COL32((int)(std::clamp(r, 0.f, 1.f) * 255),
                                           (int)(std::clamp(g2, 0.f, 1.f) * 255),
                                           (int)(std::clamp(bl, 0.f, 1.f) * 255), 255);
                        dl->AddLine(ImVec2(bx + i, by), ImVec2(bx + i, by + barH), c);
                    }
                    dl->AddRect(ImVec2(bx, by), ImVec2(bx + barW, by + barH), IM_COL32(90, 100, 110, 255));
                    ImU32 tc = IM_COL32(210, 216, 226, 255);
                    dl->AddText(ImVec2(bx, by + barH + 2 * s), tc, loS);
                    ImVec2 hs = ImGui::CalcTextSize(hiS);
                    dl->AddText(ImVec2(bx + barW - hs.x, by + barH + 2 * s), tc, hiS);
                    if (app.diff.clipped > 0) {
                        char cs[64];
                        snprintf(cs, sizeof cs, "%.2f%% off scale", app.diff.clipped * 100);
                        ImVec2 cts = ImGui::CalcTextSize(cs);
                        dl->AddText(ImVec2(bx + (barW - cts.x) * 0.5f, by - cts.y - 2 * s),
                                    IM_COL32(255, 235, 120, 255), cs);
                    }
                }
            } else if (split) {
                badge(paneAp0.x + 6 * s, paneAp0.x + paneAsz.x, false, "A",
                      batchNameOf(im), im->name, colA);
                badge(paneBp0.x + 6 * s, paneBp0.x + paneBsz.x, false, "B",
                      batchNameOf(imB), imB->name, colB);
                dl->AddRectFilled(ImVec2(splitX - GUTTER, canvasP0.y), ImVec2(splitX + GUTTER, canvasP1.y),
                                  hot ? IM_COL32(120, 132, 148, 255) : IM_COL32(60, 66, 74, 255));
            } else {
                // the wipe's two labels follow the same two-line format
                badge(canvasP0.x + 6 * s, wipeX, false, "A",
                      batchNameOf(im), im->name, colA);
                badge(wipeX + 8 * s, canvasP1.x - 6 * s, true, "B",
                      batchNameOf(imB), imB->name, colB);
                dl->AddLine(ImVec2(wipeX, canvasP0.y), ImVec2(wipeX, canvasP1.y),
                            IM_COL32(255, 255, 255, hot ? 255 : 200), 1.5f * s);
            }
            // grab handle: a stubby bar at mid-height, the thing you aim at.
            // There is none when the panes are tiled: nothing is draggable, and
            // a handle you cannot move is worse than no handle.
            if (imB && !tiled && !flashB && !flipMode && app.compareMode != App::CmpDiff) {
                float dx = split ? splitX : wipeX;
                float hy = (canvasP0.y + canvasP1.y) * 0.5f, hh = 22 * s, hw = 5 * s;
                dl->AddRectFilled(ImVec2(dx - hw, hy - hh), ImVec2(dx + hw, hy + hh),
                                  hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 234, 240, 220), 3.0f * s);
                for (float o = -1.5f; o <= 1.5f; o += 3.0f)
                    dl->AddLine(ImVec2(dx + o * s, hy - hh * 0.4f), ImVec2(dx + o * s, hy + hh * 0.4f),
                                IM_COL32(40, 44, 50, 255), 1.0f * s);
            }
            // Slots asked for and not shown: never silently. Either the canvas
            // is too narrow for them, or the mode is one of the four that mean
            // exactly two images - and in both cases the missing letters are
            // the surprise that has to be named.
            std::string unshown = tileNarrow;
            const bool twoOnlyMode = app.compareMode == App::CmpWipe ||
                                     app.compareMode == App::CmpDiff ||
                                     app.compareMode == App::CmpFlip;
            if (unshown.empty() && twoOnlyMode) {
                size_t nx = compareExtras().size();
                if (nx) {
                    const char* mode = app.compareMode == App::CmpWipe ? "wipe"
                                     : app.compareMode == App::CmpDiff ? "difference" : "blink";
                    char m[192];
                    snprintf(m, sizeof m, "%d extra slot%s not shown: %s compares exactly "
                                          "two images. Use Split for side by side.",
                             (int)nx, nx == 1 ? " is" : "s are", mode);
                    unshown = m;
                }
            }
            if (!unshown.empty()) {
                // bottom RIGHT: the badges own the top, and the difference
                // colour bar owns the bottom left
                ImVec2 ts = ImGui::CalcTextSize(unshown.c_str());
                float x = std::max(canvasP0.x + 6 * s, canvasP1.x - ts.x - 18 * s);
                float y = canvasP1.y - ts.y - 10 * s;
                dl->AddRectFilled(ImVec2(x, y - 3 * s),
                                  ImVec2(std::min(x + ts.x + 12 * s, canvasP1.x - 6 * s),
                                         y + ts.y + 3 * s),
                                  IM_COL32(90, 60, 20, 210), 3.0f * s);
                dl->AddText(ImVec2(x + 6 * s, y), IM_COL32(255, 200, 120, 255), unshown.c_str());
            }
        }
        // Remote preview: say so ON the image. Measuring a 1/3-sampled preview
        // without knowing it is how a wrong number gets written down.
        if (im->src->remoteStep > 1) {
            char msg[160];
            if (im->src->remoteErr.empty())
                snprintf(msg, sizeof msg, "PREVIEW 1/%d - fetching full resolution...",
                         im->src->remoteStep);
            else
                snprintf(msg, sizeof msg, "PREVIEW 1/%d - fetch failed: %s",
                         im->src->remoteStep, im->src->remoteErr.c_str());
            ImVec2 ts = ImGui::CalcTextSize(msg);
            float bx = std::max(canvasP0.x + 6 * s, (canvasP0.x + canvasP1.x - ts.x) * 0.5f);
            float by = canvasP0.y + 8 * s;
            ImU32 bg = im->src->remoteErr.empty() ? IM_COL32(120, 80, 20, 225) : IM_COL32(120, 30, 30, 225);
            dl->AddRectFilled(ImVec2(bx - 8 * s, by - 4 * s),
                              ImVec2(bx + ts.x + 8 * s, by + ts.y + 4 * s), bg, 4.0f * s);
            dl->AddText(ImVec2(bx, by), IM_COL32(255, 215, 140, 255), msg);
        }
        // The value highlight declares itself ON the canvas for as long as it
        // is armed (#68 §5). The Statistics panel that made the selection can
        // be closed, docked away or scrolled off while the paint is still on
        // the pixels, and magenta with nothing on screen to explain it is the
        // one failure this feature must not ship. Top RIGHT: the slot badges
        // own the top left, the preview notice the top centre, and the
        // difference colour bar the bottom left.
        {
            const std::string hb = histHlBadgeSaid();
            histHlProbeSet("badge", hb);
            if (!hb.empty()) {
                ImVec2 ts = ImGui::CalcTextSize(hb.c_str());
                float bx = std::max(canvasP0.x + 6 * s, canvasP1.x - ts.x - 18 * s);
                float by = canvasP0.y + 6 * s;
                dl->AddRectFilled(ImVec2(bx - 6 * s, by - 3 * s),
                                  ImVec2(bx + ts.x + 6 * s, by + ts.y + 3 * s),
                                  IM_COL32(0, 0, 0, 190), 3.0f * s);
                // The WORDS are neutral; the swatch is what ties the sentence
                // to the colour on the pixels. Never the other way round - the
                // colour is not the signal (a user LUT could hold magenta, and
                // this machine can read neither a screenshot nor a hue).
                dl->AddRectFilled(ImVec2(bx - 3 * s, by + ts.y * 0.25f),
                                  ImVec2(bx, by + ts.y * 0.75f), HL_INK);
                dl->AddText(ImVec2(bx, by), IM_COL32(230, 234, 240, 240), hb.c_str());
            }
        }
    } else {
        const char* msg = "Drop .npy / .bin / .raw files here   (O: open file)";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((canvasP0.x + canvasP1.x - ts.x) / 2, (canvasP0.y + canvasP1.y - ts.y) / 2),
                    IM_COL32(120, 130, 140, 255), msg);
    }
    dl->PopClipRect();

    // ---- rulers ----
    ImU32 rulerBg = IM_COL32(24, 27, 31, 255), tickCol = IM_COL32(140, 150, 160, 255),
          txtCol = IM_COL32(170, 180, 190, 255), markCol = IM_COL32(255, 184, 77, 255);
    dl->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(canvasP1.x, origin.y + RULER_H), rulerBg);          // top
    dl->AddRectFilled(ImVec2(origin.x, canvasP0.y), ImVec2(origin.x + RULER_W, canvasP1.y), rulerBg);        // left
    dl->AddText(ImVec2(origin.x + 6, origin.y + 3), IM_COL32(100, 110, 120, 255), "px");

    if (im) {
        // tick spacing derived from label width so 5-digit coords never collide
        float minSpacing = std::max(48.0f * s, ImGui::CalcTextSize("00000").x * 1.5f);
        float step = niceStep(minSpacing / app.view.zoom);
        if (step < 1) step = 1;
        // Top ruler (X): once per pane. A single ruler across both panes would
        // label pane B with pane A's coordinates - a ruler that lies.
        //
        // How far the ticks run: A's width, as it always has. Tiled, the widest
        // image on screen - the panes share ONE coordinate system, so stopping
        // at A's width would leave a wider D unlabelled where it still has pixels.
        float tickMaxX = (float)im->w;
        if (tiled) for (const TileSlot& t : slots) tickMaxX = std::max(tickMaxX, (float)t.doc->w);
        auto xRuler = [&](int p) {
            ImVec2 p0 = paneP0(p), sz = paneSz(p);
            dl->PushClipRect(ImVec2(p0.x, origin.y), ImVec2(p0.x + sz.x, origin.y + RULER_H), true);
            float ix0 = unmapIn(p, p0).x, ix1 = unmapIn(p, ImVec2(p0.x + sz.x, p0.y)).x;
            for (float t = floorf(ix0 / step) * step; t <= ix1; t += step) {
                if (t < 0 || t > tickMaxX) continue;
                float sx = mapIn(p, t, 0).x;
                dl->AddLine(ImVec2(sx, origin.y + RULER_H - TICK), ImVec2(sx, origin.y + RULER_H), tickCol);
                char lb[32]; snprintf(lb, 32, "%.0f", t);
                dl->AddText(ImVec2(sx + 3 * s, origin.y + 2), txtCol, lb);
            }
            if (app.hoverX >= 0) {
                float sx = mapIn(p, (float)app.hoverX + 0.5f, 0).x;
                dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + RULER_H), markCol, 1.5f);
            }
            dl->PopClipRect();
        };
        xRuler(0);
        if (split) xRuler(1);
        else if (tiled) for (int i = 1; i < (int)slots.size(); i++) xRuler(i);
        // left ruler (Y)
        dl->PushClipRect(ImVec2(origin.x, canvasP0.y), ImVec2(origin.x + RULER_W, canvasP1.y), true);
        {
            float iy0 = scrToImg(canvasP0).y, iy1 = scrToImg(canvasP1).y;
            for (float t = floorf(iy0 / step) * step; t <= iy1; t += step) {
                if (t < 0 || t > im->h) continue;
                float sy = imgToScr(0, t).y;
                dl->AddLine(ImVec2(origin.x + RULER_W - TICK, sy), ImVec2(origin.x + RULER_W, sy), tickCol);
                char lb[32]; snprintf(lb, 32, "%.0f", t);
                dl->AddText(ImVec2(origin.x + 4 * s, sy + 2), txtCol, lb);
            }
            if (app.hoverY >= 0) {
                float sy = imgToScr(0, (float)app.hoverY + 0.5f).y;
                dl->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + RULER_W, sy), markCol, 1.5f);
            }
        }
        dl->PopClipRect();
    }

    // ---- the footer strip: whose pixels these are, and where in the stack ----
    // Its own reserved band under the canvas (FOOT_H above): name at the left,
    // scrub bar filling the middle, frame counter at the right. Nothing here
    // ever covers a pixel being judged.
    if (im) {
        float fy0 = canvasP1.y + 3.0f * s;
        std::vector<int> fr = im->seqId != 0 ? framesOfSeq(im->seqId) : std::vector<int>();
        const App::SeqInfo* si = im->seqId != 0 ? seqInfo(im->seqId) : nullptr;
        // numbered-file stack: the pixels on screen came from ONE file, so say
        // which. Only a single-file stack (all members share a name) shows the
        // pattern - there the frame counter is the only honest address anyway.
        const char* name = im->name.c_str();
        if (si && fr.size() > 1) {
            bool perFile = false;
            for (int idx : fr)
                if (idx >= 0 && idx < (int)app.images.size() &&
                    app.images[idx]->name != im->name) { perFile = true; break; }
            if (!perFile) name = si->name.c_str();
        }
        // line 1: the batch (and the series, when this stack is in a sweep) -
        // dimmer than the name, because it is the context, not the subject
        const char* bname = nullptr;
        for (const auto& b : app.batches) if (b.id == im->batchId) bname = b.name.c_str();
        const App::Series* fser = si ? seriesOfStack(si->id) : nullptr;
        char ctx[320];
        if (bname && fser) snprintf(ctx, sizeof ctx, "%s  >  %s", bname, fser->name.c_str());
        else if (bname)   snprintf(ctx, sizeof ctx, "%s", bname);
        else              ctx[0] = 0;
        const float lh = ImGui::GetFontSize();
        float topY = fy0 + 1.0f * s, botY = topY + lh + 2.0f * s;
        if (ctx[0]) {
            dl->PushClipRect(ImVec2(canvasP0.x, topY),
                             ImVec2(canvasP0.x + canvasSize.x * 0.6f, topY + lh), true);
            dl->AddText(ImVec2(canvasP0.x, topY), IM_COL32(140, 148, 156, 190), ctx);
            dl->PopClipRect();
        }
        // ...and as state a selftest can read: the context line is written down
        // whether or not it was drawn, because "no batch, so no context line"
        // is itself part of what the strip said.
        g_footerProbe += std::string("ctx=") + ctx + ";";
        {   // zoom, right end of the context line (C3): the magnification is a
            // property of the pixels being judged, so it lives beside them.
            // Moved here FROM the status bar, not duplicated.
            char zs[40];
            if (app.view.zoom >= 0.095f) snprintf(zs, sizeof zs, "zoom %.0f%%", app.view.zoom * 100);
            else                         snprintf(zs, sizeof zs, "zoom %.3g%%", app.view.zoom * 100);
            ImVec2 zts = ImGui::CalcTextSize(zs);
            dl->AddText(ImVec2(canvasP1.x - zts.x, topY), IM_COL32(140, 148, 156, 190), zs);
            g_footerProbe += std::string("zoom=") + zs + ";";
        }
        ImVec2 ts = ImGui::CalcTextSize(name);
        float nameW = std::min(ts.x, canvasSize.x * 0.45f);
        dl->PushClipRect(ImVec2(canvasP0.x, botY), ImVec2(canvasP0.x + nameW, botY + lh), true);
        dl->AddText(ImVec2(canvasP0.x, botY), IM_COL32(175, 183, 191, 200), name);
        dl->PopClipRect();
        g_footerProbe += std::string("name=") + name + ";";
        if (si && fr.size() > 1) {
            int pos = 0;
            for (int k = 0; k < (int)fr.size(); k++) if (fr[k] == app.current) pos = k;
            // n/N, not n/n. fr.size() is what is RESIDENT; a stack still
            // loading has fewer frames here than it will have, and printing the
            // resident count as the total states a stack size that is not true
            // (docs/terminology.md - a partial load says n/N). The Files rows
            // already say "8/24f"; this said "3/8" and looked complete.
            char cnt[48];
            int expF = si->expectedFrames;
            if (expF > (int)fr.size())
                snprintf(cnt, sizeof cnt, "%d/%d of %d", pos + 1, (int)fr.size(), expF);
            else
                snprintf(cnt, sizeof cnt, "%d/%d", pos + 1, (int)fr.size());
            ImVec2 cs = ImGui::CalcTextSize(cnt);
            dl->AddText(ImVec2(canvasP1.x - cs.x, botY), IM_COL32(175, 183, 191, 200), cnt);
            g_footerProbe += std::string("count=") + cnt + ";";
            float barH = 5.0f * s;
            ImVec2 b0(canvasP0.x + nameW + 12 * s, botY + lh * 0.5f - barH * 0.5f);
            ImVec2 b1(canvasP1.x - cs.x - 12 * s, botY + lh * 0.5f + barH * 0.5f);
            if (b1.x > b0.x + 40 * s) {
                ImGui::SetCursorScreenPos(ImVec2(b0.x, botY));
                ImGui::InvisibleButton("scrub", ImVec2(b1.x - b0.x, lh));
                bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
                int alpha = sh || sa ? 235 : 150;
                // The bar spans the stack's TRUE length, not the part of it
                // that happens to be resident: a 24-frame stack with 8 loaded
                // used to draw a full bar, so the missing 16 were invisible and
                // the handle sat at the end of a stack that had barely started.
                // Three states, and the middle one is the point of the design:
                //   resident  - the frame is here and can be shown
                //   loading   - this stack is being read right now
                //   absent    - not here, and nothing is fetching it
                int total = std::max((int)fr.size(), si->expectedFrames);
                bool loadingThis = app.seqRunning && app.seqLoadingId == si->id;
                dl->AddRectFilled(b0, b1, IM_COL32(120, 130, 140, 60), barH * 0.5f);
                if (total > (int)fr.size()) {
                    // Where the resident frames actually ARE, one mark per
                    // ordinal. Drawn from seqIndex, so a stack that arrived out
                    // of order says so instead of pretending to be a prefix.
                    float w = (b1.x - b0.x) / (float)total;
                    for (int k = 0; k < (int)fr.size(); k++) {
                        int ord = app.images[fr[k]]->seqIndex;
                        if (ord < 0 || ord >= total) continue;
                        float x0 = b0.x + w * (float)ord;
                        dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x0 + std::max(w, 1.0f), b1.y),
                                          IM_COL32(110, 160, 210, alpha));
                    }
                    if (loadingThis) {   // the tail still coming, dimmer than resident
                        float x0 = b0.x + w * (float)fr.size();
                        dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(b1.x, b1.y),
                                          IM_COL32(110, 160, 210, 70));
                    }
                } else {
                    float fx0 = b0.x + (b1.x - b0.x) * ((float)pos / (float)(fr.size() - 1));
                    dl->AddRectFilled(b0, ImVec2(fx0, b1.y), IM_COL32(110, 160, 210, alpha),
                                      barH * 0.5f);
                }
                int curOrd = total > (int)fr.size() ? app.images[fr[pos]]->seqIndex : pos;
                float fx = b0.x + (b1.x - b0.x) *
                           (total > 1 ? (float)curOrd / (float)(total - 1) : 0.0f);
                dl->AddCircleFilled(ImVec2(fx, botY + lh * 0.5f), barH * (sh || sa ? 1.4f : 1.0f),
                                    IM_COL32(160, 200, 240, alpha));
                if (sa) {
                    float t = std::clamp((io.MousePos.x - b0.x) / (b1.x - b0.x), 0.0f, 1.0f);
                    // Land on the nearest frame that is HERE. Dragging over a
                    // gap leaves the picture and the number where they are:
                    // there is no image to show for an ordinal we do not have,
                    // and a number that moves without the picture moving is the
                    // one thing this bar must not do.
                    int wantOrd = (int)(t * (float)(total - 1) + 0.5f);
                    int best = pos, bestD = INT_MAX;
                    for (int k = 0; k < (int)fr.size(); k++) {
                        int ord = total > (int)fr.size() ? app.images[fr[k]]->seqIndex : k;
                        int d = std::abs(ord - wantOrd);
                        if (d < bestD) { bestD = d; best = k; }
                    }
                    if (best != pos) selectImage(fr[best]);
                }
                // The keys named here are the ones that work HERE. It used to
                // say ". and ," - those step the Browse panel's preview, and
                // have never done anything in this view. Left/Right is what
                // gotoFrame is bound to.
                if (sh) {
                    if (total > (int)fr.size())
                        ImGui::SetTooltip("frame %d of %d\n"
                                          "%d loaded - the bar spans all %d, and the gaps "
                                          "are frames that are not here%s\n"
                                          "dragging lands on the nearest loaded frame; "
                                          "Left / Right step",
                                          curOrd + 1, total, (int)fr.size(), total,
                                          loadingThis ? " yet (still loading)" : "");
                    else
                        ImGui::SetTooltip("frame %d / %d  (drag, or Left / Right)",
                                          pos + 1, (int)fr.size());
                }
            }
        }
        // ---- the same bar, for a preview ------------------------------------
        // A preview is one throwaway doc with seqId 0, so the branch above never
        // ran for it and the stepping lived in the Browse panel instead: a
        // second frame-stepping row on screen, under the list rather than under
        // the picture it steps. The control belongs where the pixels are.
        //
        // previewFiles = a stack of numbered files, previewFrames = one file
        // with a frame axis. Either way the bar walks it one fetched frame at a
        // time, which is why it steps rather than jumping: each position is a
        // fetch, and stepPreviewFrame is the one place that knows how.
        else if (im->preview) {
            int pvN = app.previewFiles.size() >= 2 ? (int)app.previewFiles.size()
                                                   : app.previewFrames;
            if (pvN >= 2) {
                char cnt[48];
                snprintf(cnt, sizeof cnt, "%d/%d", app.previewIndex + 1, pvN);
                ImVec2 cs = ImGui::CalcTextSize(cnt);
                dl->AddText(ImVec2(canvasP1.x - cs.x, botY), IM_COL32(175, 183, 191, 200), cnt);
                g_footerProbe += std::string("previewcount=") + cnt + ";";
                float barH = 5.0f * s;
                ImVec2 b0(canvasP0.x + nameW + 12 * s, botY + lh * 0.5f - barH * 0.5f);
                ImVec2 b1(canvasP1.x - cs.x - 12 * s, botY + lh * 0.5f + barH * 0.5f);
                if (b1.x > b0.x + 40 * s) {
                    ImGui::SetCursorScreenPos(ImVec2(b0.x, botY));
                    ImGui::InvisibleButton("pvscrub", ImVec2(b1.x - b0.x, lh));
                    bool sh = ImGui::IsItemHovered(), sa = ImGui::IsItemActive();
                    int alpha = sh || sa ? 235 : 150;
                    dl->AddRectFilled(b0, b1, IM_COL32(120, 130, 140, 60), barH * 0.5f);
                    float fx = b0.x + (b1.x - b0.x) *
                               ((float)app.previewIndex / (float)(pvN - 1));
                    dl->AddRectFilled(b0, ImVec2(fx, b1.y), IM_COL32(110, 160, 210, alpha),
                                      barH * 0.5f);
                    dl->AddCircleFilled(ImVec2(fx, botY + lh * 0.5f),
                                        barH * (sh || sa ? 1.4f : 1.0f),
                                        IM_COL32(160, 200, 240, alpha));
                    if (sa) {
                        float t = std::clamp((io.MousePos.x - b0.x) / (b1.x - b0.x), 0.0f, 1.0f);
                        int want = (int)(t * (float)(pvN - 1) + 0.5f);
                        if (want != app.previewIndex) stepPreviewFrame(want - app.previewIndex);
                    }
                    if (sh)
                        ImGui::SetTooltip("previewing %s\n%d / %d  (drag, or , and .)\n"
                                          "double-click the row in Browse to open it for real",
                                          app.previewLabel.c_str(), app.previewIndex + 1, pvN);
                }
            }
        }
    }
}

// Where the 256 bins go. Pure, so a selftest can pin every case of it without a
// window (--abstats-selftest, group W).
//
// Spreading 256 bins evenly over [black, white] makes the bin width fractional
// whenever the range is not a multiple of 256: 64..1023 gives 3.746 DN/bin, so
// one bin swallows FOUR integer DN values and its neighbour swallows three, and
// the panel draws a periodic comb that is not in the data. Reading the shape of
// a noise distribution is what the histogram is for, so that is fatal
// (docs/background/project/todo-open.md 9, user report 2026-07-29).
//
// So the grid is snapped to a round width and a round origin:
//   integer dtype  w = ceil(span/256) as a whole number, at least 1 - one bin
//                  is then a whole number of DN and every bin holds the same
//                  count of them. 0..4095 gives exactly 16, 0..255 exactly 1.
//   float          w = the smallest power of two >= span/256, so the bin EDGES
//                  are exact in binary and re-binning accumulates no error.
//   both           origin = floor(black/w)*w.
// It always widens OUTWARD: origin <= black and origin + 256w >= white, so the
// snapped grid contains the requested range and no pixel is hidden by it. What
// that costs is that "outside the bins" is no longer "outside the display
// range" - see the clip counting in recomputeHistogramIfNeeded.
static void histBinGrid(const std::string& dtype, float black, float white,
                        float& origin, float& w) {
    float span = white - black;
    if (!(span > 0) || !std::isfinite(span)) span = 1.0f;   // degenerate: 1 DN across
    const float q = span / 256.0f;                          // the unrounded width
    const bool intType = !dtype.empty() &&
                         (dtype[0] == 'u' || dtype[0] == 'i' || dtype == "bool");
    if (intType) {
        w = std::ceil(q);
        if (!(w >= 1.0f)) w = 1.0f;             // an integer bin is never sub-DN
    } else {
        // frexpf, not ceil(log2 q): log2 of an exact power of two can come back
        // a hair under it, and ceil would then DOUBLE a width that was already
        // right. frexp is exact - q = m * 2^e with m in [0.5, 1), so q is a
        // power of two exactly when m is 0.5.
        int e = 0;
        if (frexpf(q, &e) == 0.5f) e--;
        // ...and never let the exponent leave float's normal range: a width
        // that underflowed to zero would divide by zero below, and rounding UP
        // to the smallest normal still widens the grid, so it still contains.
        w = ldexpf(1.0f, std::clamp(e, -126, 127));
    }
    // In double: black/w can be far outside float's exactly-representable
    // integers, and a floor() computed there would not be a floor.
    origin = (float)(std::floor((double)black / (double)w) * (double)w);
    if (origin > black) origin -= w;            // rounding must never narrow it
}

// The cache slot is a parameter (slot 0 = A, slot 1 = compare B). Everything
// below is the previous body with `app.hist` replaced by `H`.
//
// binBlack / binWhite: the value range the 256 bins span. NaN (the default, and
// what A always passes) means the image's own display range. The B slot is
// handed A's range instead: two histograms sharing one x axis have to be BINNED
// alike or the overlay is a lie - and it keeps the axis A's, which is what the
// user set. It is a cache key, so changing A's range re-bins B.
static void recomputeHistogramIfNeeded(ImageDoc* im, App::HistState& H,
                                       float binBlack = std::numeric_limits<float>::quiet_NaN(),
                                       float binWhite = std::numeric_limits<float>::quiet_NaN()) {
    const float wantBlack = std::isfinite(binBlack) ? binBlack : effBlack(*im);
    const float wantWhite = std::isfinite(binWhite) ? binWhite : effWhite(*im);
    // The grid is decided HERE, above the cache comparison, so that the cache
    // key, the binning, the axis label and the shared A/B grid all see one
    // decision made in one place. Snapping below the comparison would miss the
    // cache every frame and re-scan a million pixels.
    float gOrigin = 0, gW = 0;
    histBinGrid(im->dtype, wantBlack, wantWhite, gOrigin, gW);
    // Key on the RESOLVED rect, never on annRev: annotation churn (dragging,
    // renaming, toggling visibility) must not re-scan a million pixels.
    int rx = 0, ry = 0, rw = im->w, rh = im->h;
    bool roiUsed = false;
    if (App::Ann* a = findAnn(app.selectedAnn)) {
        if (a->type == 0) {           // selected ROI drives the histogram
            int cx = std::clamp(a->x, 0, im->w), cy = std::clamp(a->y, 0, im->h);
            int cw = std::clamp(a->w, 0, im->w - cx), ch2 = std::clamp(a->h, 0, im->h - cy);
            if (cw > 0 && ch2 > 0) { rx = cx; ry = cy; rw = cw; rh = ch2; roiUsed = true; }
        }
    }
    if (H.uid == im->uid && H.dataRev == im->dataRev && H.black == wantBlack &&
        H.white == wantWhite && H.cfa == im->cfa && H.cfaPattern == im->cfaPattern &&
        H.rx == rx && H.ry == ry && H.rw == rw && H.rh == rh)
        return;
    if (app.annBusy && H.uid == im->uid) return;   // mid-drag: keep the last result
    H.img = im; H.uid = im->uid; H.dataRev = im->dataRev;
    H.black = wantBlack; H.white = wantWhite;
    H.binOrigin = gOrigin; H.binW = gW;
    H.cfa = im->cfa; H.cfaPattern = im->cfaPattern;
    H.rx = rx; H.ry = ry; H.rw = rw; H.rh = rh; H.roiUsed = roiUsed;
    memset(H.bins, 0, sizeof H.bins);
    H.maxBin = 1; H.clipLo = H.clipHi = 0; H.sampled = 0;
    bool cfa = im->ch == 1 && im->cfa != 0;
    H.nSeries = cfa ? 4 : std::min(im->ch, 3);
    static const char* CFA_SERIES[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_SERIES[4] = { "ch0", "ch1", "ch2", "ch3" };
    for (int s = 0; s < 4; s++) H.seriesNames[s] = cfa ? CFA_SERIES[s] : CH_SERIES[s];
    // DIVIDE, never multiply by a reciprocal. 1/w is inexact for every integer
    // w that is not a power of two, and (k*w) * (1/w) then lands a hair under k
    // and floors to k-1: an off-by-one bin at the very edges the snapping exists
    // to make exact. In double the quotient is correctly rounded and floor(t) is
    // the right bin for every w below 2^53.
    const double binW = H.binW, binOrigin = H.binOrigin;
    size_t total = (size_t)rw * rh;
    // A CFA sample stride has to move by WHOLE mosaic cells and take every
    // pixel of the cell it lands on - the shape 47eec0c settled on for the
    // Projection pass. This loop used to stride a LINEAR index p with
    // x = rx + p % rw: the moment step and rw were both even (a sensor width
    // essentially always is; step first turns even at 2 Mpx - 1920x1080 is
    // already past it), p stayed even, x stayed even, and no odd column was
    // ever read. For RGGB that gave Gr and B ZERO samples while R and Gb kept
    // printing normal numbers, so nothing on screen showed it.
    const int cell = cfa ? (im->cfa == 2 ? 4 : 2) : 1;   // quad Bayer repeats every 4
    // ...and the budget has to know that each landing covers `cell` rows
    size_t step = std::max<size_t>(1, total * cell / 1000000);   // sample <= ~1M px
    if (cell > 1) step = ((step + cell - 1) / cell) * cell;      // whole cells only
    // below/above are counted against black/white - the range the USER SET -
    // and never against the bins. Since the grid snaps OUTWARD they are no
    // longer the same question: at a display range of 101..1000 the bins run
    // 100..1124, so DN 1050 is a pixel the user sees blown out that lands in an
    // ordinary bin. clipLo/clipHi are printed as "<black / >white", which is a
    // statement about the DISPLAY range; counted off the bins instead, the
    // saturated-pixel count silently drops the moment the grid widens - it went
    // from 3.00% to 0.00% on the --abstats-selftest W5 fixture while this was
    // being written.
    size_t below = 0, above = 0, cnt = 0, values = 0;
    double sum[4] = {}, sum2[4] = {};
    size_t n[4] = {};
    for (int y0 = 0; y0 < rh; y0 += (int)step)
        for (int y = y0; y < y0 + cell && y < rh; y++)
            for (int x = 0; x < rw; x++) {
                const float* src = &im->px()[((size_t)(ry + y) * im->w + (rx + x)) * im->ch];
                if (cfa) {
                    float v = src[0];
                    if (std::isfinite(v)) {
                        int s = cfaChannelAt(*im, rx + x, ry + y);
                        double t = ((double)v - binOrigin) / binW;
                        H.bins[s][t < 0 ? 0 : t >= 256 ? 255 : (int)t]++;
                        if (v < H.black) below++;
                        else if (v >= H.white) above++;
                        sum[s] += v; sum2[s] += (double)v * v; n[s]++;
                        values++;
                    }
                } else {
                    for (int c = 0; c < H.nSeries; c++) {
                        float v = src[c];
                        if (!std::isfinite(v)) continue;
                        double t = ((double)v - binOrigin) / binW;
                        H.bins[c][t < 0 ? 0 : t >= 256 ? 255 : (int)t]++;
                        if (v < H.black) below++;
                        else if (v >= H.white) above++;
                        sum[c] += v; sum2[c] += (double)v * v; n[c]++;
                        values++;
                    }
                }
                cnt++;
            }
    for (int s = 0; s < H.nSeries; s++) {          // always-on mean / std
        if (!n[s]) { H.mean[s] = H.sd[s] = 0; continue; }
        double m = sum[s] / n[s], var = sum2[s] / n[s] - m * m;
        H.mean[s] = m;
        H.sd[s] = sqrt(var > 0 ? var : 0);
    }
    for (int s = 0; s < H.nSeries; s++)
        for (int b = 0; b < 256; b++)
            H.maxBin = std::max(H.maxBin, H.bins[s][b]);
    H.sampled = cnt;
    H.clipLo = values ? (double)below / values : 0;
    H.clipHi = values ? (double)above / values : 0;
}

// ---- L2 plot service ---------------------------------------------------------
// Every plot in the app goes through this: axes are always labelled (quantity +
// unit) and ticks land on 1/2/5*10^k values (integer axes never show fractions).
struct PlotRect {
    ImVec2 p0, p1;                       // inner drawing area
    float xmin, xmax, ymin, ymax;
    bool ok = false;
    ImVec2 at(float x, float y) const {
        return ImVec2(p0.x + (x - xmin) / (xmax - xmin) * (p1.x - p0.x),
                      p1.y - (y - ymin) / (ymax - ymin) * (p1.y - p0.y));
    }
    // screen -> data, the exact inverse of at() - a box-zoom is only honest if
    // the rectangle the user drew maps back to precisely the range they get
    ImVec2 unmap(ImVec2 sc) const {
        return ImVec2(xmin + (sc.x - p0.x) / std::max(p1.x - p0.x, 1e-6f) * (xmax - xmin),
                      ymin + (p1.y - sc.y) / std::max(p1.y - p0.y, 1e-6f) * (ymax - ymin));
    }
};

// How far a curve swings over the part of it a plot is SHOWING, as a fraction
// of its own level. The user's question, in their words: "the displayed graph's
// displayed region - how many percent is it off, peak to peak".
//
// The quantity and the wording are NOT new. A detrend product's note already
// says "p-p 12.3% -> 4.5% (p-p 100 -> 40 DN)", and its denominator is the
// plane's mean LEVEL (docs/features/analysis/flat-field-stats.md). Using the same pair here means
// a reader who has seen one has seen both, and nobody has to learn a second
// spelling of "how flat is this".
//
// It is computed over the VISIBLE x range, so it changes when the plot is
// zoomed or panned - which makes it presentation-dependent, and therefore
// something that has to SAY what it was measured over. The line below prints
// the range with the number for exactly that reason (the same discipline M1
// applies to measuring-class settings: a number that depends on a choice
// travels with the choice).
struct PlotSpread {
    bool ok = false;
    int n = 0;                 // samples inside the range
    double lo = 0, hi = 0;     // min and max of those samples
    double pp = 0, mean = 0;   // hi - lo, and the level it is a fraction of
    double pct = 0;            // 100 * pp / mean; 0 when there is no level
    bool hasPct = false;       // a mean at or below 0 has no ratio to take
    float x0 = 0, x1 = 0;      // the range it was measured over
};
// `xs` may be empty: then the series is indexed 0..n-1, which is what a profile
// is (column x, row y) and what a frame plot falls back to.
static PlotSpread plotSpreadIn(const std::vector<float>& xs, const std::vector<float>& ys,
                               float x0, float x1) {
    PlotSpread s;
    s.x0 = x0; s.x1 = x1;
    double sum = 0;
    for (size_t i = 0; i < ys.size(); i++) {
        const double x = xs.empty() ? (double)i : (i < xs.size() ? (double)xs[i] : 0.0);
        if (x < x0 || x > x1) continue;
        const double v = ys[i];
        if (!std::isfinite(v)) continue;         // excluded, never folded in
        if (!s.n || v < s.lo) s.lo = v;
        if (!s.n || v > s.hi) s.hi = v;
        sum += v;
        s.n++;
    }
    if (s.n < 2) return s;                       // one sample has no spread
    s.pp = s.hi - s.lo;
    s.mean = sum / s.n;
    // A ratio against a mean that is zero or negative is not a percentage of
    // anything - it is a division by a level that does not exist. Said as a
    // missing ratio rather than printed as a large or negative number, which is
    // the same call the detrend report makes (`pctOk`).
    if (s.mean > 0) { s.pct = 100.0 * s.pp / s.mean; s.hasPct = true; }
    s.ok = true;
    return s;
}
// ...and the sentence it owes the reader. Under the plot, in the plot's own
// units, naming the window - because the number moves when the window does.
static void plotSpreadLine(const PlotSpread& s, const char* what) {
    if (!s.ok) return;
    char b[224];
    if (s.hasPct)
        snprintf(b, sizeof b, "%s in view [%.6g .. %.6g]: p-p %.4g (%.4g%% of mean %.6g), "
                              "%d point(s)",
                 what, (double)s.x0, (double)s.x1, s.pp, s.pct, s.mean, s.n);
    else
        snprintf(b, sizeof b, "%s in view [%.6g .. %.6g]: p-p %.4g (no positive mean to "
                              "take a ratio against), %d point(s)",
                 what, (double)s.x0, (double)s.x1, s.pp, s.n);
    ImGui::TextDisabled("%s", b);
}

// A drag rectangle in screen space -> the data ranges it spans. Pure, so the
// selftest can pin the mapping without a mouse. Returns false for a drag too
// small to be an intention (a click with jitter must not zoom into nothing).
static bool zoomRangeFromDrag(const PlotRect& pr, ImVec2 a, ImVec2 b,
                              float& x0, float& x1, float& y0, float& y1) {
    if (!pr.ok) return false;
    if (fabsf(a.x - b.x) < 4.0f || fabsf(a.y - b.y) < 4.0f) return false;
    ImVec2 da = pr.unmap(a), db = pr.unmap(b);
    x0 = std::min(da.x, db.x); x1 = std::max(da.x, db.x);
    y0 = std::min(da.y, db.y); y1 = std::max(da.y, db.y);
    return x1 > x0 && y1 > y0;
}

// A drag on the histogram's x axis -> the value interval it selects (#68,
// docs/features/histogram/histogram-select-design.md §2). Pure, and here beside zoomRangeFromDrag
// for its reason: a selftest can then pin the whole mapping with no mouse.
//
// Three rules, and each earns its place:
//   * the ends SNAP OUTWARD onto the BIN grid. A plot cannot show a distinction
//     finer than a bin, so an endpoint of 103.7 would name a precision the
//     picture never offered; the bin the cursor touched is taken whole, in the
//     same direction histBinGrid itself widens.
//   * the interval is HALF-OPEN [lo, hi) - the same edge convention a bin has
//     (state.h: bin b spans [origin + b*w, origin + (b+1)*w)), so a selection
//     and the bars under it agree about which side an edge belongs to.
//   * touching an END bin OPENS that side. recomputeHistogramIfNeeded folds
//     everything below the grid into bin 0 and everything above it into bin
//     255, so those two bars mean "and everything beyond". Read literally
//     instead, a drag over the saturation tail would return a count far below
//     the bar it covered, dropping exactly the pixels the user was hunting.
//     A selection means what the bar means - and the dividend is that "painted
//     count == sum of the selected bins" then holds at the edges too.
// Only the HORIZONTAL extent is an intention: y is frequency, and the band is
// always full height. The 4 px threshold is zoomRangeFromDrag's, for the same
// reason - a click with jitter in it is not a drag.
struct HistHlRange {
    bool ok = false;
    int b0 = 0, b1 = 0;                    // the bins touched, inclusive
    float lo = 0, hi = 0;
    bool loOpen = false, hiOpen = false;
};
static HistHlRange histHlFromDrag(const PlotRect& pr, ImVec2 a, ImVec2 b,
                                  float origin, float w) {
    HistHlRange r;
    if (!pr.ok || !(w > 0) || !std::isfinite(w) || !std::isfinite(origin)) return r;
    if (fabsf(a.x - b.x) < 4.0f) return r;
    float xa = pr.unmap(a).x, xb = pr.unmap(b).x;
    if (!std::isfinite(xa) || !std::isfinite(xb)) return r;
    if (xb < xa) std::swap(xa, xb);
    // DIVIDE in double, the same discipline the binning keeps: a reciprocal is
    // inexact for every integer width that is not a power of two, and the
    // floor would then land one bin low at exactly the edges the snapping
    // exists to make exact.
    const double t0 = std::floor(((double)xa - (double)origin) / (double)w);
    const double t1 = std::floor(((double)xb - (double)origin) / (double)w);
    r.b0 = (int)std::clamp(t0, 0.0, 255.0);
    r.b1 = (int)std::clamp(t1, 0.0, 255.0);
    r.lo = (float)((double)origin + (double)r.b0 * (double)w);
    r.hi = (float)((double)origin + (double)(r.b1 + 1) * (double)w);
    r.loOpen = r.b0 == 0;
    r.hiOpen = r.b1 == 255;
    r.ok = true;
    return r;
}

// ImDrawList has no dashed line, and the A/B panels need one: hue is already
// spoken for by the CFA plane (R/Gr/Gb/B each own a colour), so the DASH is the
// only thing left that can say "this curve is B". Dash length is measured along
// the polyline, so a dense curve does not turn the dashes into a solid line.
static void addDashedPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col,
                              float thick, float dash, float gap) {
    if (n < 2 || dash <= 0 || gap <= 0) return;
    float phase = 0;                          // distance walked inside dash+gap
    for (int i = 1; i < n; i++) {
        ImVec2 a = pts[i - 1], b = pts[i];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (!(len > 0) || !std::isfinite(len)) continue;
        float t = 0;
        while (t < len) {
            float period = dash + gap;
            float inPeriod = fmodf(phase, period);
            bool on = inPeriod < dash;
            float left = on ? dash - inPeriod : period - inPeriod;
            float step = std::min(left, len - t);
            if (on) {
                ImVec2 p0(a.x + dx * (t / len), a.y + dy * (t / len));
                ImVec2 p1(a.x + dx * ((t + step) / len), a.y + dy * ((t + step) / len));
                dl->AddLine(p0, p1, col, thick);
            }
            t += step;
            phase += step;
        }
    }
}

// Long file names would push the legend off the plot; elide the FRONT, because
// the tail ("_gain10_000.npy") is the part that tells two captures apart.
static std::string elideFront(const std::string& s, size_t keep) {
    return s.size() <= keep ? s : ("..." + s.substr(s.size() - keep));
}

// The one amber this UI warns in: the fallback notices, the mismatch notices,
// and the [stale] marker below. Anything the reader must NOTICE is this colour.
// AB_AMBER sheds static (P7): the Browse status line warns in the same amber,
// and host.h declares it extern for the panel TU.
const ImVec4 AB_AMBER(0.95f, 0.72f, 0.35f, 1.0f);
static const ImU32  AB_AMBER32 = IM_COL32(242, 184, 89, 255);
// B's numbers are one frame behind while A is being stepped. The marker used to
// be appended to B's NAME and drawn in the surrounding grey, which during fast
// stepping is indistinguishable from part of the file name - the one moment it
// exists to be read. It is a separate token now, in the warning amber.
static const char* AB_STALE_TOKEN = "[stale]";
// The one RED, and it is a step past the amber on purpose (issue #56). Amber
// says "shown, and out of date" - a thing the user can refresh. This says "the
// re-read you asked for did not happen, and what is on screen is not one
// generation any more": nothing the program can put right by itself, and the
// numbers taken off it are wrong rather than old. It outlines the Files row
// (the user's own proposal on #56) and inks the Inspector sentence.
static const ImVec4 RELOAD_FAIL_RED(0.92f, 0.36f, 0.33f, 1.0f);
static const ImU32  RELOAD_FAIL_RED32 = IM_COL32(235, 92, 84, 255);

// ...and AMBER, one step down from it (docs/features/watch/watch-design.md §5: 「琥珀の1行」).
// The two say different things and must not wear one colour: red is "what is on
// screen is wrong", amber is "what is on screen is fine, and the FILES behind it
// have moved". Nothing about the pixels in memory is in doubt when this shows -
// which is exactly why it has a button and the red one has none.
static const ImU32  WATCH_AMBER32 = IM_COL32(235, 175, 70, 255);

// What A or B is CALLED, everywhere the two are set against each other.
// A capture series names every stack's frames identically - 00/frame_000.npy
// and 01/frame_000.npy are both "frame_000.npy" - so the FILE name identifies
// neither side, and the legend, the side-by-side bands, the status bar, the
// Inspector and the B-image menu all read "frame_000.npy" twice. The STACK is
// what tells them apart, and SeqInfo::name already carries the folder the
// Files panel disambiguates with ("00/frame_000..004.npy"). A loose frame
// belongs to no stack and keeps its own name, which is unambiguous by
// definition - there is only one of it.
static std::string abDocLabel(const ImageDoc* d) {
    if (!d) return std::string();
    if (d->seqId != 0)
        if (const App::SeqInfo* si = seqInfo(d->seqId))
            if (!si->name.empty()) return si->name;
    return d->name;
}

static void fmtTick(char* buf, size_t n, double v, bool integer) {
    if (integer) { snprintf(buf, n, "%.0f", v); return; }
    // Axis labels are read at a glance, and C's scientific notation makes an
    // ordinary sensor value look like an implementation detail: "2e+04" for
    // 20000 DN. It is worst on log paper, where several of them sit in a
    // column. Keep the significand and say the scale once, with the prefix the
    // reader already uses for it.
    //
    // The ladder is closed at both ends on purpose. A suffix is only a
    // shorthand while the reader knows it, so it stops at G and at n; outside
    // that, e-notation is the honest form. Leaving the small end open lets
    // 1e-15 print as "1e-06n", which is neither.
    const double a = fabs(v);
    double scale = 1.0;
    const char* suffix = "";
    if      (a >= 1e9  && a < 1e12) { scale = 1e9;  suffix = "G"; }
    else if (a >= 1e6  && a < 1e9 ) { scale = 1e6;  suffix = "M"; }
    else if (a >= 1e3  && a < 1e6 ) { scale = 1e3;  suffix = "k"; }
    else if (a >= 1e-6 && a < 1e-3) { scale = 1e-6; suffix = "u"; }
    else if (a >= 1e-9 && a < 1e-6) { scale = 1e-9; suffix = "n"; }
    if (*suffix)                            snprintf(buf, n, "%.4g%s", v / scale, suffix);
    else if (a != 0 && (a >= 1e12 || a < 1e-9)) snprintf(buf, n, "%.2e", v);
    else                                    snprintf(buf, n, "%.4g", v);
}

// xLabel / yLabel must carry the quantity and its unit, e.g. "frequency (cycles/px)"
static PlotRect beginPlot(const char* xLabel, const char* yLabel,
                          float xmin, float xmax, float ymin, float ymax,
                          bool xInt, bool yInt, float height) {
    PlotRect pr;
    if (!(xmax > xmin)) xmax = xmin + 1;
    if (!(ymax > ymin)) ymax = ymin + 1;
    if (xInt && xmax - xmin < 1) xmax = xmin + 1;
    pr.xmin = xmin; pr.xmax = xmax; pr.ymin = ymin; pr.ymax = ymax;

    const float s = app.uiScale;
    const float fh = ImGui::GetFontSize();
    char tb[48];
    // measure EVERY tick label, not just the endpoints: on a 0..1 axis the
    // intermediate "0.25" is wider than both ends and would draw off-panel
    double ystep = niceStep((float)((ymax - ymin) / 4.0));
    if (yInt && ystep < 1) ystep = 1;
    float wy = 0.0f;
    for (double v = ceil(ymin / ystep) * ystep; v <= ymax + 1e-9; v += ystep) {
        fmtTick(tb, sizeof tb, v, yInt);
        wy = std::max(wy, ImGui::CalcTextSize(tb).x);
    }
    const float marginL = wy + 9 * s;   // 5*s tick gap + breathing room
    const float marginB = fh * 2 + 6 * s;          // x tick labels + x axis title
    const float marginT = fh + 4 * s;              // y axis title sits on top

    float wid = ImGui::GetContentRegionAvail().x;
    ImVec2 org = ImGui::GetCursorScreenPos();
    pr.p0 = ImVec2(org.x + marginL, org.y + marginT);
    pr.p1 = ImVec2(org.x + wid, org.y + marginT + height);
    if (pr.p1.x - pr.p0.x < 20 || height < 20) { ImGui::Dummy(ImVec2(wid, height)); return pr; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = IM_COL32(12, 14, 16, 255), grid = IM_COL32(46, 52, 58, 255),
          txt = IM_COL32(150, 160, 170, 255), axis = IM_COL32(90, 100, 110, 255);
    dl->AddRectFilled(pr.p0, pr.p1, bg);

    // Y ticks (ystep computed above, where the margin was measured)
    for (double v = ceil(ymin / ystep) * ystep; v <= ymax + 1e-9; v += ystep) {
        float y = pr.at(pr.xmin, (float)v).y;
        dl->AddLine(ImVec2(pr.p0.x, y), ImVec2(pr.p1.x, y), grid);
        fmtTick(tb, sizeof tb, v, yInt);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(pr.p0.x - 5 * s - ts.x, y - fh * 0.5f), txt, tb);
    }
    // X ticks
    double xstep = niceStep((float)((xmax - xmin) / 4.0));
    if (xInt && xstep < 1) xstep = 1;
    for (double v = ceil(xmin / xstep) * xstep; v <= xmax + 1e-9; v += xstep) {
        float x = pr.at((float)v, pr.ymin).x;
        dl->AddLine(ImVec2(x, pr.p0.y), ImVec2(x, pr.p1.y), grid);
        fmtTick(tb, sizeof tb, v, xInt);
        ImVec2 ts = ImGui::CalcTextSize(tb);
        dl->AddText(ImVec2(std::clamp(x - ts.x * 0.5f, pr.p0.x, pr.p1.x - ts.x),
                           pr.p1.y + 2 * s), txt, tb);
    }
    dl->AddRect(pr.p0, pr.p1, axis);
    // axis titles: quantity + unit (mandatory)
    dl->AddText(ImVec2(org.x, org.y), txt, yLabel ? yLabel : "y");
    {
        const char* xl = xLabel ? xLabel : "x";
        ImVec2 ts = ImGui::CalcTextSize(xl);
        dl->AddText(ImVec2((pr.p0.x + pr.p1.x - ts.x) * 0.5f, pr.p1.y + 2 * s + fh), txt, xl);
    }
    ImGui::Dummy(ImVec2(wid, height + marginT + marginB));
    pr.ok = true;
    return pr;
}

