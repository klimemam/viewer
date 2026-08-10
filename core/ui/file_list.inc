// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
// One heading in the Files panel: a batch, the stacks under it, and the folder
// its first stack came from. Grouped by BATCH, not by folder - a batch is one
// open action, named by the user (the folder name is only the starting value),
// and two loads from the same folder are two batches, which is the point.
struct FileGroup {
    int batch = 0;
    std::string dir, label;
    std::vector<const std::vector<int>*> stacks;
};
// Extracted from drawFileList so the ORDER can be asserted without a frame: it
// is the thing "Move to batch" must not change.
//
// The order is app.batches', which is the order the batches were CREATED in.
// It used to be the order they were first met while walking the stacks - and
// that is a property of the stacks, not of the batches. Moving one stack into
// another batch moved that batch's whole heading to wherever the moved stack
// happened to sit, which for a move "upwards" is the top of the list, so every
// other heading shifted and the user had to find their work again. A move
// changes which heading a row sits UNDER. It must not reorder the headings.
static std::vector<FileGroup> buildFileGroups(const std::vector<std::vector<int>>& stacks) {
    std::vector<FileGroup> groups;
    for (const auto& stack : stacks) {
        const ImageDoc& head = *app.images[stack.front()];
        FileGroup* g = nullptr;
        for (auto& q : groups) if (q.batch == head.batchId) { g = &q; break; }
        if (!g) {
            const std::string& p = head.src->path;
            size_t slash = p.find_last_of("/\\");
            std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
            std::string label = "opened";
            for (const auto& b : app.batches) if (b.id == head.batchId) label = b.name;
            groups.push_back({ head.batchId, dir, label, {} });
            g = &groups.back();
        }
        g->stacks.push_back(&stack);
    }
    auto rank = [](int id) {
        for (size_t i = 0; i < app.batches.size(); i++)
            if (app.batches[i].id == id) return (int)i;
        return (int)app.batches.size();    // no batch names it: last, order kept
    };
    std::stable_sort(groups.begin(), groups.end(),
                     [&](const FileGroup& a, const FileGroup& b) {
                         return rank(a.batch) < rank(b.batch);
                     });
    // The transient preview is PINNED LAST and never reorders the rest.
    // It used to sort in wherever its batch was created, so glancing at a
    // file in the browser inserted a heading in the middle of the list and
    // pushed everything the user was working with down a row - the opposite
    // of what a throwaway look should cost.
    for (size_t i = 0; i + 1 < groups.size(); i++)
        if (groups[i].label == "preview") { std::rotate(groups.begin() + i,
                                                        groups.begin() + i + 1,
                                                        groups.end()); break; }
    return groups;
}

// What letters each Files row carried on the last draw, "name=letters;" per
// badged row. The panel is OpenGL this machine cannot screenshot, so the
// letters are asserted as state (--abstats-selftest S4), like g_tilePanesDrawn.
static std::string g_filesBadgeProbe;

// §4: the share mark. U+29C9 (two joined squares) when the atlas has it - the
// CJK fonts jpFontPath picks vary here - else the plain word, which is longer
// but never a '?' box. Checked once: the font file does not change at runtime.
static const char* shareGlyph() {
    static int have = -1;
    if (have < 0) {
        ImFont* f = ImGui::GetFont();
        have = (f && f->FindGlyphNoFallback((ImWchar)0x29C9)) ? 1 : 0;
    }
    return have ? "\xE2\xA7\x89" : "";
}
// The tooltip's counterpart sentence for a row whose pixels are shared (§4):
// `shared` = the row's frames whose source has another holder. Counterparts
// are counted PER STACK - `4 frame(s) shared - 2 with "D1", 2 with "D2"` -
// never the row-wide total pinned on whichever holder happens to sit first
// in app.images; a single frame names ALL its other holders. At most 3 are
// named, the rest are "(+N more)". The row itself only carries the mark;
// WHO ELSE has the pixels is said here, in reading order.
static std::string filesShareSentence(const std::vector<const ImageDoc*>& shared) {
    if (shared.empty()) return "";
    struct CP { const ImageDoc* rep; int frames; };
    std::vector<uint64_t> keys;            // stack seqId / loose uid, first-seen
    std::vector<CP> cps;
    for (const ImageDoc* f : shared) {
        std::vector<uint64_t> seenHere;    // one count per counterpart per frame
        for (const auto& e : app.images) {
            if (e.get() == f || e->src->srcId != f->src->srcId) continue;
            if (f->seqId && e->seqId == f->seqId) continue;   // the row itself
            uint64_t key = e->seqId ? ((uint64_t)1 << 63) | (uint64_t)e->seqId
                                    : e->uid;
            if (std::find(seenHere.begin(), seenHere.end(), key) != seenHere.end())
                continue;
            seenHere.push_back(key);
            size_t k = std::find(keys.begin(), keys.end(), key) - keys.begin();
            if (k == keys.size()) { keys.push_back(key); cps.push_back({ e.get(), 0 }); }
            cps[k].frames++;
        }
    }
    if (cps.empty()) return "";
    const int n = (int)shared.size();
    std::string s = *shareGlyph() ? std::string(shareGlyph()) + " " : "";
    char b[320];
    if (n == 1) {
        s += "shared - also ";
    } else {
        snprintf(b, sizeof b, "%d frame(s) shared - ", n);
        s += b;
    }
    const int named = std::min((int)cps.size(), 3);
    for (int k = 0; k < named; k++) {
        const ImageDoc* rep = cps[k].rep;
        const App::SeqInfo* osi = rep->seqId ? seqInfo(rep->seqId) : nullptr;
        const std::string where = osi ? osi->name : rep->name;
        if (k) s += ", ";
        if (n == 1 && rep->seqId)
            snprintf(b, sizeof b, "frame %d of \"%s\"", rep->seqIndex, where.c_str());
        else if (n == 1)
            snprintf(b, sizeof b, "open as \"%s\"", where.c_str());
        else
            snprintf(b, sizeof b, "%d with \"%s\"", cps[k].frames, where.c_str());
        s += b;
    }
    if ((int)cps.size() > named)
        s += " (+" + std::to_string((int)cps.size() - named) + " more)";
    return s;
}

// The ink for one layer of the canon, in whichever theme is on. Wrapped so the
// Files rows read as "this row is a series" rather than as a colour lookup.
static ImVec4 inkOf(int layer) { return ui_theme::layerInk(app.themeVariant, layer); }

// Which set row / role row is SELECTED (click = select; double-click = go to
// the bound member - the click/commit convention every list here follows).
// Selection of a role is a reading position, not app model state, so it lives
// with the panel.
static int g_aSetSelId = 0;      // ASet::id, 0 = none
static int g_aSetSelRole = -1;   // index into its roles, -1 = the set node itself

// One condition, printed to the format the WHOLE series shares, so the values
// read as a column. Formatting each on its own with %g gives 1, 2, 5, 10, 20
// four different widths and nothing lines up. The digits of the UI font are
// tabular, so a shared width and a shared number of decimals really do align -
// padding with spaces would not, the font being proportional.
//
// The decimals are the fewest that still say every member exactly: a sweep of
// whole milliseconds stays whole, and one member of 2.5 gives the column one
// decimal rather than giving 2.5 six.
static std::string conditionText(const App::Series& S, double v) {
    if (!std::isfinite(v)) return "value unset";
    int dec = 0;
    for (const auto& m : S.members) {
        if (!std::isfinite(m.value)) continue;
        int d = 0;
        for (; d < 6; d++) {
            char t[64];
            snprintf(t, sizeof t, "%.*f", d, m.value);
            double back = atof(t);
            if (std::fabs(back - m.value) <= 1e-9 * std::max(1.0, std::fabs(m.value))) break;
        }
        dec = std::max(dec, d);
    }
    int w = 0;
    for (const auto& m : S.members) {
        if (!std::isfinite(m.value)) continue;
        char t[64];
        snprintf(t, sizeof t, "%.*f", dec, m.value);
        w = std::max(w, (int)strlen(t));
    }
    char b[96];
    snprintf(b, sizeof b, "%*.*f", w, dec, v);
    return b;
}

// The stack name as a SERIES MEMBER row prints it.
//
// Opening a container names its stacks after their position inside it -
// "sweep.vnz:0", "sweep.vnz:1" (loadVnzNodes) - and that compound is why a
// member row could be read at all when the series it belongs to had scrolled
// off the top: the row had to say which sweep it came from, because nothing
// above it was still on screen to say so. The heading is held there now, and
// the row leads with its own [n], so the ":n" is the same fact printed twice.
// What is left is the file name, which goes to the END of the row - where a
// narrowing panel drops it first and the index and the condition survive.
//
// DISPLAY ONLY, and only here. The name itself is unchanged: it is what the
// Linearity table's stack column, the Series menu, "Move to batch" and every
// tooltip print, and in those places there is no [n] beside it and no pinned
// heading above it - three stacks all reading "sweep.vnz" would be a real loss
// of information. This is the one row that has somewhere else to put it.
//
// A DIGIT tail only. An .npz member builds the same shape out of the array's
// NAME (loadNpzMembers: "data.npz:arr_0"), and that is not a position - the
// row's [n] does not repeat it, so it stays.
static std::string filesMemberName(const std::string& name) {
    size_t c = name.find_last_of(':');
    if (c == std::string::npos || c == 0 || c + 1 >= name.size()) return name;
    for (size_t i = c + 1; i < name.size(); i++)
        if (name[i] < '0' || name[i] > '9') return name;
    return name.substr(0, c);
}

// ---- what the Files panel holds at the top while it scrolls ------------------
// One level of the hierarchy and the rows it owns, in the list's own content
// coordinates (scroll-independent: ImGui::GetCursorPos, not GetCursorScreenPos).
// Recorded by the walk that draws the list and read at the top of the NEXT
// frame, which is where the band has to be submitted.
//
// Files needs a different mechanism from Browse and it is worth saying why,
// because they look like the same feature: Browse's rows are virtualised, so
// the only thing it can reason about is row INDICES, and it hands its band to
// the table's own frozen-row machinery. Files has no clipper - every row is
// submitted, every frame - so here the exact geometry is already known and the
// band is simply drawn above the scrolling region, which is the plainest way
// there is to make it occupy space rather than cover the first row.
struct FilesSpan {
    int batch = 0;             // the batch this level belongs to
    int series = 0;            // 0 = the batch's own heading, else the series'
    float y0 = 0;              // top of the heading row
    float hy = 0;              // ...and its bottom: above this it is still visible
    float y1 = 0;              // bottom of the last row under it
};
static std::vector<FilesSpan> g_filesSpans;
static float g_filesScrollY = 0;        // where the list was scrolled to, last frame
// What the band actually held, outermost first, ";"-joined - the same shape of
// probe as RbToolbarGeom::pinnedNames and read for the same reason: the claim
// is about what is on screen.
static std::string g_filesPinProbe;

static void drawFileList() {
    g_filesBadgeProbe.clear();
    // No Open / Close buttons here (2026-08-03). Browse is how a file is found
    // and opened, and it reaches remote machines as well, which the OS dialog
    // never did. Close was the worse of the two: it silently escalated from the
    // frame to the whole stack, and it was the ONLY Close that did not name the
    // layer it closes -- the row menus say Close batch / Close stack / Close
    // series, and the File menu says Close Frame / Close Batch / Close All.
    if (app.seqRunning || !app.seqQueue.empty()) {
        int done = app.seqDone, total = app.seqTotal;
        if (app.seqRunning) ImGui::TextDisabled("loading %d/%d", done, total);
        else ImGui::TextDisabled("queued");
        if (!app.seqQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d stacks)", (int)app.seqQueue.size());
        }
        {   // keep Stop reachable: it is the only way to cancel a huge load
            const ImGuiStyle& st = ImGui::GetStyle();
            float stopW = ImGui::CalcTextSize("Stop").x + st.FramePadding.x * 2;
            float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x -
                         ImGui::GetCursorStartPos().x;
            if (ImGui::GetContentRegionAvail().x - used - st.ItemSpacing.x >= stopW) ImGui::SameLine();
        }
        if (ImGui::SmallButton("Stop")) { app.seqCancel = true; app.seqQueue.clear(); }
    }
    if (app.rfPending > 0) {
        ImGui::SameLine();
        // the ONLY way to cancel a remote fetch used to be quitting the app
        if (ImGui::SmallButton("Stop##rf")) {
            app.rfGen++;
            app.rbOpenQueue.clear();   // or the queue would just start the next stack
            std::lock_guard<std::mutex> lk(app.rfMtx);
            app.rfQueue.clear();
            app.rfPending = 0; app.rfTotal = 0; app.rfFetched = 0;
            app.rfBytesInFlight = 0;
        }
    }
    if (app.rfPending > 0)
        ImGui::TextDisabled("remote: fetching %d/%d", app.rfFetched.load(), app.rfTotal.load());
    if (!app.rbOpenQueue.empty()) {   // stacks a folder scan has still to open
        ImGui::TextDisabled("remote: %d stack(s) queued", (int)app.rbOpenQueue.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##rq")) app.rbOpenQueue.clear();
    }
    if (!app.seqNote.empty()) {          // why the stack is short, kept in view
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(app.seqNote.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("dismiss##seqnote")) app.seqNote.clear();
    }
    ImGui::Separator();
    // Context-menu closes and moves are DEFERRED to the end of the walk:
    // closing erases images mid-iteration while the row loop still holds
    // indices into the pre-close stacksCached() snapshot, and a move prunes
    // app.batches while the submenu is iterating it. Every series command is
    // deferred for the same reason squared - the walk is iterating app.series
    // itself, and one join would resize it under the loop drawing it.
    int pendingCloseSeq = 0, pendingCloseBatch = 0;
    int pendingMoveSeq = 0, pendingMoveImg = -1, pendingMoveTarget = 0;
    int pendingSerEdit = 0, pendingSerMove = 0, pendingSerMoveTo = 0;
    int pendingSerUngroup = 0, pendingSerClose = 0, pendingSerRename = 0;
    std::string pendingSerName;
    int pendingJoinSeq = 0, pendingJoinSer = 0, pendingLeaveSeq = 0, pendingNewSerBatch = 0;
    int pendingDeriveSeq = 0;   // opens the Derive stack dialog after the walk
    // Reload from disk, deferred like the rest: reloadSource's walk calls
    // forgetImage while the row loop still holds this frame's cached indices.
    int pendingReloadImg = -1, pendingReloadSeq = 0;
    int pendingRecomputeImg = -1;   // Recompute from stack (§10.1): forgetImage
                                    // again, on the reduced frame itself
    // Set commands, deferred for the same reason: the walk iterates
    // app.analysisSets, and a close would erase under the loop drawing it.
    int pendingCloseSet = 0, pendingSetRename = 0;
    std::string pendingSetName;
    int pendingAvgSeq = 0;      // ...and the frame average, for the same reason:
                                // it appends to app.images, which this walk is
                                // iterating over
    int pendingSumSeq = 0;      // the sum fold, deferred identically
    // The detrend stage's product (docs/flat-field-stats.md 判断6): a NEW stack,
    // so it appends to app.images and defers for the same reason as the two
    // above. The spec rides with the id - "which removal" is the whole content
    // of the request, and a product made with the wrong one is not fixable by
    // renaming it afterwards.
    int pendingDetrendSeq = 0;
    DetrendSpec pendingDetrendSpec;
    // "Move to batch" submenu, shared by the stack row (seqctx) and the
    // standalone frame row (imgctx). Returns the chosen batch id, 0 = none.
    // Batch names go through a ### suffix: user text must not become the ID.
    auto moveToBatchMenu = [&](int curBatch) -> int {
        int target = 0;
        if (ImGui::BeginMenu("Move to batch")) {
            for (const auto& b : app.batches) {
                if (b.id == curBatch) continue;           // already there
                if (b.name == "preview") continue;        // pseudo-batch
                char lb2[300];
                snprintf(lb2, sizeof lb2, "%s###mb%d", b.name.c_str(), b.id);
                if (ImGui::MenuItem(lb2)) target = b.id;
            }
            ImGui::Separator();
            static char nbBuf[256] = "";
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            bool go = ImGui::InputTextWithHint("##newbatch", "New batch...",
                                               nbBuf, sizeof nbBuf,
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            go |= ImGui::SmallButton("create");
            if (go && nbBuf[0]) {
                target = newBatch(uniqueBatchName(nbBuf));
                nbBuf[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        return target;
    };
    // The A/B item both kinds of row carry, by IDENTITY rather than by name:
    // the B-image menu lists names, and two batches full of frame_001.npy made
    // that a coin toss. `pick` is the frame this row would hand to B.
    // The extra slots hang off the same row item. Shown as a plain entry rather
    // than behind Shift+right-click: a modifier that changes what a menu
    // contains is invisible until someone tells you about it, and the letter in
    // the label ("Add as compare slot D") says what will happen before it does.
    auto compareSlotItem = [](ImageDoc* pick) {
        // the current row gets it too, like every other; the decision of WHAT
        // to offer lives in slotRowItem, where a selftest can reach it
        std::string letter;
        SlotRowItem it = slotRowItem(pick, letter);
        if (it == SlotRowNone) return;
        if (it == SlotRowRemove) {
            if (ImGui::MenuItem(("Remove from compare slot " + letter).c_str()))
                removeCompareSlot(pick);
            return;
        }
        if (ImGui::MenuItem(("Add as compare slot " + letter).c_str()))
            addCompareSlot(pick);
        if (ImGui::IsItemHovered())
            // Split is NOT in the two-only list: it is precisely the mode that
            // tiles every letter (tileSlots engages there and only there), and
            // saying it "stays A against B" contradicted the sentence before it.
            ImGui::SetTooltip("slots beyond B show up in the statistics tables and,\n"
                              "in Split, as side-by-side panes. Wipe, difference and\n"
                              "blink stay A against B - those only mean two.");
    };
    // Returns whether it put anything in the menu, so a caller can skip the
    // separator that would otherwise lead an empty group. Every row with a
    // document contributes now, the current one included (see abRowItem); the
    // false return is left for a row that has no document at all.
    auto compareBItem = [&compareSlotItem](ImageDoc* pick) -> bool {
        if (abRowItem(pick) != AbSetB) return false;
        if (ImGui::MenuItem("Set as compare B")) {
            setCompareB(pick);
            if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
        }
        compareSlotItem(pick);
        return true;
    };
    // Group by source folder. Opening a folder of folders gives one stack per
    // subfolder, which reads fine - but opening several leaf folders in a row
    // produced a flat list of bare filenames with nothing saying where each came
    // from. The folder is the only thing that distinguishes them.
    const auto& stacks = stacksCached();
    static std::vector<FileGroup> groups;
    // §4: srcId -> how many memberships hold it. A shared frame keeps one row
    // per referencing stack (the tree stays the membership tree); rows whose
    // pixels also live elsewhere carry the share mark and name the counterpart
    // in the tooltip. Cached with the groups: sharing changes exactly when the
    // membership list does (imagesRev - the crop CoW bumps it too).
    static std::unordered_map<uint64_t, int> srcOwners;
    static uint64_t groupsRev = 0;
    if (groupsRev != app.imagesRev) {
        groupsRev = app.imagesRev;
        groups = buildFileGroups(stacks);
        srcOwners.clear();
        for (const auto& d : app.images) srcOwners[d->src->srcId]++;
    }
    // The batch heading is ALWAYS drawn. It used to appear only once there was
    // more than one thing open, so opening a second folder made a heading
    // materialise above rows that had none - and a batch you can rename and
    // move things into cannot be a thing that exists only sometimes.
    const bool showHeaders = !groups.empty();
    // ---- one heading, drawn one way -----------------------------------------
    // `pinned` says only that this copy is the one being held above the
    // scrolling list. It is the same widget, with the same ink, the same
    // tooltip and the same context menu, and clicking it collapses the same
    // thing - a row that changed meaning because of where it happened to be
    // drawn would be worse than one that scrolled away.
    //
    // A pinned heading is OPEN by definition: what put it in the band is that
    // its rows are the ones on screen. So its state is forced rather than
    // stored, and a click on it is routed to the real node, which is drawn
    // later in the same frame - no second copy of the open/closed state exists
    // to fall out of step with the first.
    int collapseBatch = 0, collapseSeries = 0;      // written by the band, read by the walk
    auto drawBatchHead = [&](const FileGroup& group, bool pinned) -> bool {
        bool isPreview = group.label == "preview";
        // The batch ink, unless this is the preview batch - "transient" is
        // what that row has to say first, and dim already says it.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              isPreview ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                        : inkOf(ui_theme::LayerBatch));
        if (pinned) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        else if (collapseBatch == group.batch) ImGui::SetNextItemOpen(false);
        bool open = isPreview
            ? ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                         ImGuiTreeNodeFlags_SpanAvailWidth,
                                "preview (transient)   (%d)", (int)group.stacks.size())
            : ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                         ImGuiTreeNodeFlags_SpanAvailWidth,
                                "%s   (%d)", group.label.c_str(), (int)group.stacks.size());
        ImGui::PopStyleColor();
        if (pinned && ImGui::IsItemClicked()) collapseBatch = group.batch;
        if (ImGui::IsItemHovered() && !group.dir.empty())
            ImGui::SetTooltip("%s\n\n(right-click to rename this batch)",
                              dispPath(group.dir).c_str());
        // the batch is the user's grouping, so its name is theirs to change
        if (ImGui::BeginPopupContextItem("batchctx")) {
            static char nameBuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(nameBuf, sizeof nameBuf, "%s", group.label.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##bname", nameBuf, sizeof nameBuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && nameBuf[0]) {
                renameBatch(group.batch, nameBuf);   // uniquified: names restore sessions
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close batch"))    // the batch layer's Close
                pendingCloseBatch = group.batch;
            ImGui::EndPopup();
        }
        return open;
    };
    auto drawSeriesHead = [&](const App::Series& S, bool pinned) -> bool {
        const ImGuiStyle& st = ImGui::GetStyle();
        char smeta[96];
        // the unit is the series', and "not set" is a state to read, not a
        // blank to guess at (the panel says the same thing the same way)
        if (S.unit[0]) snprintf(smeta, sizeof smeta, "[%s]  (%d)", S.unit,
                                (int)S.members.size());
        else snprintf(smeta, sizeof smeta, "[unit not set]  (%d)", (int)S.members.size());
        float avail = ImGui::GetContentRegionAvail().x;
        float mw = ImGui::CalcTextSize(smeta).x;
        bool room = avail > mw + ImGui::GetFontSize() * 8.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        // same rule as the stack rows: ONE item spanning the row, the unit
        // and the count PAINTED on it. A second widget on the right is what
        // made a context menu unreachable on the name once already.
        if (room) ImGui::PushClipRect(p0, ImVec2(p0.x + avail - mw - st.ItemSpacing.x,
                                                 p0.y + ImGui::GetFrameHeight()), true);
        ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerSeries));
        if (pinned) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        else if (collapseSeries == S.id) ImGui::SetNextItemOpen(false);
        bool sopen = ImGui::TreeNodeEx("##ser", ImGuiTreeNodeFlags_DefaultOpen |
                                                ImGuiTreeNodeFlags_SpanAvailWidth,
                                       "%s", S.name.c_str());
        ImGui::PopStyleColor();
        if (room) ImGui::PopClipRect();
        if (pinned && ImGui::IsItemClicked()) collapseSeries = S.id;
        if (room) {
            ImVec2 m = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(m.x + avail - mw, m.y + (ImGui::GetItemRectSize().y -
                                                ImGui::GetTextLineHeight()) * 0.5f),
                S.unit[0] ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                          : IM_COL32(255, 184, 90, 255), smeta);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("series (系列): %d stack(s) of one swept %s\n\n"
                              "(right-click: rename, edit, move, ungroup, close)",
                              (int)S.members.size(),
                              S.paramName.empty() ? "parameter" : S.paramName.c_str());
        if (ImGui::BeginPopupContextItem("serctx")) {
            static char sbuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(sbuf, sizeof sbuf, "%s", S.name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##sname", sbuf, sizeof sbuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && sbuf[0]) {
                pendingSerRename = S.id;
                pendingSerName = sbuf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            // the same modal creation uses: members, values, order, unit
            if (ImGui::MenuItem("Edit series...")) {
                pendingSerEdit = S.id;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            {   // the SERIES moves - every member with it, so that
                // series ⊂ batch never stops being true on the way
                int t = moveToBatchMenu(S.batchId);
                if (t) { pendingSerMove = S.id; pendingSerMoveTo = t; }
                ImGui::TextDisabled("   (all %d member(s) move with it)",
                                    (int)S.members.size());
            }
            ImGui::Separator();
            // Two DIFFERENT operations, spelled out rather than one word
            // with a modifier: one keeps the data, the other destroys it.
            if (ImGui::MenuItem("Ungroup (keep the stacks)")) pendingSerUngroup = S.id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Removes the series only. All %d stack(s) stay open,\n"
                                  "exactly where they are - they just stop being a sweep.",
                                  (int)S.members.size());
            if (ImGui::MenuItem("Close series (discard its stacks)"))
                pendingSerClose = S.id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Closes the %d stack(s) in it, every frame with them.",
                                  (int)S.members.size());
            ImGui::EndPopup();
        }
        return sopen;
    };
    // ---- the band: every level the list has scrolled past --------------------
    // Drawn ABOVE the scrolling region, so it takes its height out of the list
    // rather than painting over the list's first row. The levels it holds come
    // from the spans the walk recorded last frame against the scroll offset it
    // ended on, which is the only place either number exists: ImGui applies a
    // wheel event inside BeginChild, and the band has to be submitted before
    // that. One frame of lag while the wheel is actually turning, and none at
    // rest - the same settling every layout-dependent probe in this file has.
    g_filesPinProbe.clear();
    int pinPushed = 0;
    {
        // Its OWN ID scope, and not a detail. Without it the pinned copy and
        // the real heading are one ImGui id, which means one open/closed state
        // between them - and the copy forces that state open every frame,
        // because being open is what put it in the band. Collapsing the real
        // heading would be undone by its own reflection on the next frame.
        // Separate ids, one direction: the copy is forced open, a click on it
        // sets collapseBatch / collapseSeries, and the real node reads that.
        ImGui::PushID("filespin");
        std::vector<const FilesSpan*> pins;
        for (const auto& s : g_filesSpans)
            if (s.hy <= g_filesScrollY && g_filesScrollY < s.y1) pins.push_back(&s);
        // outermost first: a batch's span opens before the span of any series
        // inside it, so top-of-list order IS ancestor order
        std::sort(pins.begin(), pins.end(),
                  [](const FilesSpan* a, const FilesSpan* b) { return a->y0 < b->y0; });
        for (const FilesSpan* p : pins) {
            if (p->series == 0) {
                const FileGroup* g = nullptr;
                for (const auto& q : groups) if (q.batch == p->batch) g = &q;
                if (!g) continue;
                ImGui::PushID(g->batch);
                g_filesPinProbe += g->label + ";";
                // left pushed on purpose: TreeNodeEx indents what follows it,
                // and the band has to reproduce the indentation of the rows it
                // stands in for. Unwound below, innermost first.
                drawBatchHead(*g, true);
                pinPushed++;
            } else {
                const App::Series* S = nullptr;
                for (const auto& s : app.series) if (s.id == p->series) S = &s;
                if (!S) continue;
                ImGui::PushID(S->id);
                g_filesPinProbe += S->name + ";";
                drawSeriesHead(*S, true);
                pinPushed++;
            }
        }
        for (int i = 0; i < pinPushed; i++) { ImGui::TreePop(); ImGui::PopID(); }
        ImGui::PopID();
        if (pinPushed) ImGui::Separator();
    }
    // The list scrolls on its own now. It has to: a band that is part of the
    // scrolling content is a band that scrolls away, which is the entire
    // complaint. Everything above - the load progress, the stop buttons, the
    // note - stays outside it, where it was already behaving as a header.
    bool listOpen = ImGui::BeginChild("fileslist", ImVec2(0, 0), ImGuiChildFlags_None);
    if (listOpen) {
    g_filesSpans.clear();
    for (const auto& group : groups) {
      float gy0 = ImGui::GetCursorPos().y;
      ImGui::PushID(group.batch);
      bool open = true;
      float ghy = gy0;
      if (showHeaders) {
          open = drawBatchHead(group, false);
          ghy = ImGui::GetCursorPos().y;
      }
      if (open) {
        // name and format share one row: the dim/format part is right-aligned and dimmed.
        // `badge` is every compare letter this row's image (or stack) holds -
        // "B", "C", "BC", ... - drawn at the left edge in the B ink, one visual
        // weight for all of them: a slot the Files panel does not show is a
        // slot nobody can find again (defect 2026-07-30).
        auto rowWithMeta = [](const ImageDoc& d, const char* label, bool selected,
                              const char* extra = nullptr,
                              const std::string& badge = std::string(),
                              bool badgeDim = false,
                              const std::string& share = std::string()) -> bool {
            const ImGuiStyle& st = ImGui::GetStyle();
            char meta[112];
            // §4: the share mark leads the meta strip - painted onto the row,
            // so no new column and no wider row; the tooltip names the
            // counterpart (`share` is filesShareSentence's output).
            const char* mark = share.empty() ? ""
                             : *shareGlyph() ? "\xE2\xA7\x89  " : "shared  ";
            snprintf(meta, sizeof meta, "%s%dx%d %dch %s%s", mark, d.w, d.h, d.ch,
                     d.dtype.c_str(), extra ? extra : "");
            float avail = ImGui::GetContentRegionAvail().x;
            float metaW = ImGui::CalcTextSize(meta).x;
            // The row is ONE item spanning the full width, and the metadata is
            // painted onto it rather than being a second item. It used to be a
            // name-width Selectable followed by a TextDisabled, which made the
            // metadata the "last item" - so right-clicking a stack opened the
            // context menu only over that dim strip on the right, and never on
            // the name, where everyone aims.
            float nameW = std::max(avail - metaW - st.ItemSpacing.x, ImGui::GetFontSize() * 4.0f);
            bool room = avail > metaW + ImGui::GetFontSize() * 6.0f;   // else: name only
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(p0, ImVec2(p0.x + (room ? nameW : avail),
                                           p0.y + ImGui::GetFrameHeight()), true);
            bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns,
                                             ImVec2(avail, 0));
            ImGui::PopClipRect();
            bool hov = ImGui::IsItemHovered();
            if (!badge.empty()) {   // which rows hold letters, without hunting a menu
                ImVec2 m = ImGui::GetItemRectMin();
                float h = ImGui::GetTextLineHeight();
                ImVec2 tp(m.x + 2 * app.uiScale,
                          m.y + (ImGui::GetItemRectSize().y - h) * 0.5f);
                // dim = "armed, not active": compare is off, the seats are
                // kept. Same ink, lower alpha - one lettering system.
                ImGui::GetWindowDrawList()->AddText(
                    tp, badgeDim ? IM_COL32(120, 190, 255, 110)
                                 : IM_COL32(120, 190, 255, 255), badge.c_str());
                // ...and as state the selftest can read (S4/S6) - the dim
                // state is part of what was drawn, so it is recorded, not
                // inferable from a colour the probe cannot see
                const char* p = label;
                while (*p == ' ') p++;
                std::string t(p);
                size_t hh = t.find("##");
                if (hh != std::string::npos) t.resize(hh);
                g_filesBadgeProbe += t + "=" + badge + (badgeDim ? "(dim);" : ";");
            }
            if (room) {   // draw-list, not an item: the row must stay the last item
                ImVec2 m = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(m.x + avail - metaW, m.y + (ImGui::GetItemRectSize().y -
                                                       ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), meta);
            }
            // A COMPUTED frame has no path, so this tooltip led with a blank
            // line and told the reader nothing at all - and the Files panel is
            // where a frame average sits beside the captures it will be
            // confused with. Its provenance note takes the path's place, which
            // is what the note is for. The share sentence (§4) follows either
            // lead: whose pixels these also are is true of computed frames too.
            if (hov) {
                std::string lead = d.src->path.empty() && !d.note.empty()
                                 ? d.note : dispPath(d.src->path);
                // the generation statements, where the provenance is read
                // (§10.1 / §10.2): stale leads - it is the warning; "same
                // picture" trails - it is the reassurance two results of one
                // recipe can give when their recorded input tuples match.
                if (avgIsStale(&d))
                    lead = "(stale) the source stack was reloaded after this frame "
                           "was computed - the picture shows the OLD pixels. "
                           "Right-click > Recompute from stack.\n" + lead;
                else if (!d.avgOfPath.empty())
                    for (const auto& q : app.images)
                        if (avgSamePicture(&d, q.get()) && !avgIsStale(q.get())) {
                            lead += "\nsame picture as \"" + q->name +
                                    "\" (same recipe over the same source generation)";
                            break;
                        }
                // #56: what the last reload of THIS ROW'S STACK left behind,
                // and it leads even (stale) - a stale average is one Recompute
                // from being true again, a stack of two generations is not,
                // and this sentence is the one that explains the red outline
                // the row is wearing. (In practice the two never meet: a
                // reduced frame belongs to no stack and a stack head is never
                // a reduced frame. The order is stated anyway, because "it
                // cannot happen" is not a thing a reader can see here.)
                if (std::string rn = seqReloadNote(d.seqId); !rn.empty())
                    lead = rn + "\n" + lead;
                if (share.empty())
                    ImGui::SetTooltip("%s\n%s", lead.c_str(), meta);
                else
                    ImGui::SetTooltip("%s\n%s\n\n%s", lead.c_str(), meta, share.c_str());
            }
            return clicked;
        };
        // One row. mem != nullptr means the row is being drawn INSIDE a series
        // node: it is indented by the node and the member's VALUE leads the
        // label. Everything else about the row is identical - a member is not
        // a different kind of stack, it is a stack that is also in a sweep.
        auto drawStackRow = [&](const std::vector<int>& stack, const App::Series* ser,
                                const App::Series::Member* mem, int memIdx) {
          const ImageDoc& head = *app.images[stack.front()];
          if (head.seqId == 0) {
            int i = stack.front();
            ImGui::PushID(i);
            // Two marks meet on this row and both stay: the compare letters
            // and the [preview] tag (the ONE transient slot must not read as a
            // registered file - a single click in Browse "adding a file to
            // Files" was reported as exactly that misreading).
            //
            // One lettering system, two kinds of letter, one asymmetric rule -
            // do NOT "simplify" the asymmetry away, it is the spec:
            //   - B/C/D... are SEATS (assignments). They show whenever
            //     assigned: full ink while a comparison runs, dimmed while
            //     compare is off ("armed, not active" - user report
            //     2026-07-30, 「B,Cは薄く表示」). B's seat is the PIN
            //     (bSeatUid), never cmpB(): the letter must not vanish while
            //     A stands on the pin (paused) or while compare is off.
            //   - A is the CURSOR, not a seat (docs/compare-n.md §1). It
            //     marks the current row only while a comparison is ACTIVE:
            //     compare off means nothing is being compared, so no A.
            const bool cmpOn = app.compareMode != App::CmpOff;
            std::string badge;
            if (cmpOn && i == app.current) badge += "A";
            if (bSeatUid() && head.uid == bSeatUid()) badge += "B";
            badge += slotOf(&head);
            char lb[512];
            // the leading spaces reserve the badge's ground: two per letter,
            // and at least the two the rows have always had.
            // (stale) / (recomputed): the generation marks of a reduced frame
            // (§10.1 / §10.2) - stale means the picture shows pixels its stack
            // no longer holds; recomputed means a restore rebuilt it from
            // files newer than the session's record. Same row position as
            // [preview]: facts about THIS row's pixels, next to the name.
            const char* genMark = head.preview ? "   [preview]"
                                : avgIsStale(&head) ? "   (stale)"
                                : head.avgGenChanged ? "   (recomputed from newer files)"
                                : "";
            snprintf(lb, 512, "%*s%s%s##%d", (int)std::max<size_t>(badge.size() * 2, 2),
                     "", head.name.c_str(), genMark, i);
            std::string share;                 // §4: this frame's pixels, elsewhere
            {
                auto sh = srcOwners.find(head.src->srcId);
                if (sh != srcOwners.end() && sh->second > 1)
                    share = filesShareSentence({ &head });
                // ras §6: the member side gets a SENTENCE, never a mark - the
                // ⧉ tooltip precedent, one line per binding this frame holds
                std::string b2 = aSetBoundSentence(0, head.uid, 0);
                if (!b2.empty()) share += (share.empty() ? "" : "\n") + b2;
            }
            // A loose image is a frame. Preview keeps the dim ink: "transient"
            // outranks "which layer" on that one row.
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  head.preview ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                               : inkOf(ui_theme::LayerFrame));
            bool rowHit = rowWithMeta(head, lb, i == app.current, nullptr, badge, !cmpOn,
                                      share);
            ImGui::PopStyleColor();
            if (head.preview && ImGui::IsItemHovered())
                ImGui::SetTooltip("a throwaway preview - the one slot the Browse panel\n"
                                  "reuses; the next click there replaces it. Double-click /\n"
                                  "Enter in Browse (or measuring it) registers it for real.");
            if (rowHit) {
                selectImage(i);
                if (app.fitOnSwitch) app.fitRequested = true;
            }
            // a standalone frame hangs off the batch directly, so it gets the
            // move menu itself (stacks move via seqctx below)
            if (ImGui::BeginPopupContextItem("imgctx")) {
                if (compareBItem(app.images[i].get())) ImGui::Separator();
                // a second, independent viewer on this frame (item 28)
                if (ImGui::MenuItem("Open in new window"))
                    openInNewWindow(app.images[i].get());
                {   // manual reload (reference-design §3.2 / §8 row 5): re-read
                    // this frame's file and swap the pixels in place. WHY the
                    // item may be greyed is reloadSource's own answer, asked
                    // once (reloadUnavailable) rather than transcribed here -
                    // a second list of refusals is a list that goes stale
                    // behind the walk.
                    const std::string why = reloadUnavailable(*app.images[i]->src);
                    if (ImGui::MenuItem("Reload from disk", nullptr, false, why.empty()))
                        pendingReloadImg = i;
                    if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", why.c_str());
                }
                // Recompute (§10.1): the reduced frame's own "reload" - it has
                // no file, it has a stack, and the picture NEVER refreshes
                // itself. This item is the one way it gets new pixels.
                if (!head.avgOfPath.empty()) {
                    bool canRe = head.avgSeqId && seqInfo(head.avgSeqId);
                    if (!canRe)
                        for (const auto& q : app.images)
                            if (q->seqId != 0 && q->src->path == head.avgOfPath &&
                                q->src->member == head.avgOfMember) {
                                canRe = true; break;
                            }
                    if (ImGui::MenuItem("Recompute from stack", nullptr, false, canRe))
                        pendingRecomputeImg = i;
                    if (!canRe && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("the stack this frame was computed from is not "
                                          "open - there is nothing to recompute over.");
                }
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveImg = i; pendingMoveTarget = t; }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            return;
          }
          App::SeqInfo* si = seqInfo(head.seqId);
          // Switching A to another stack KEEPS the frame number, clipped to what
          // this stack has. Landing on frame 0 threw away the position every
          // time - and comparing two captures means looking at the same frame
          // of each, so the number is the thing worth preserving, not the
          // stack's own history. (Same rule B already follows: see
          // compareFollowFrame in resolveB.)
          int pos = 0;
          bool active = false;
          if (const ImageDoc* a = cur())
              if (a->seqId != 0)
                  pos = std::clamp(a->seqIndex, 0, (int)stack.size() - 1);
          for (int k = 0; k < (int)stack.size(); k++)
              if (stack[k] == app.current) { pos = k; active = true; }
          ImGui::PushID(head.seqId);
          // Every letter this stack holds, in A-then-B-then-slot order - the
          // order the letters read everywhere else. B is a FRAME, and so is a
          // slot's pin: the stack is where they LIVE, so the stack row
          // carries them. Same asymmetric rule as the frame rows above (and
          // do not "simplify" it): the seats (B/C/D..., keyed on the PIN via
          // bSeatUid, never on cmpB()) show whenever assigned - dimmed while
          // compare is off; A is the cursor and marks the active stack only
          // while a comparison is running.
          const bool cmpOn = app.compareMode != App::CmpOff;
          std::string badge = cmpOn && active ? "A" : "";
          bool stackHasB = false;
          if (bSeatUid())
              for (int idx : stack)
                  if (app.images[idx]->uid == bSeatUid()) stackHasB = true;
          if (stackHasB) badge += "B";
          for (size_t k = 0; k < app.cmpExtra.size(); k++)
              for (int idx : stack)
                  if (app.images[idx]->uid == app.cmpExtra[k]) { badge += slotName(k); break; }
          char lb[600];
          const char* sname = si ? si->name.c_str() : "stack";
          // #56: the WORD beside the red outline below. A colour alone says
          // "something", never what - and a report quotes text, not a border -
          // so the row states the fact in the same place (stale) states its
          // own, and the outline is what makes it findable across a full
          // panel. Both, or neither is worth having.
          const char* rmark = seqReloadMark(head.seqId);
          if (mem) {
              // Reading order: [n], then the condition, then the file name.
              //
              // The value used to LEAD, because it is the reason the row is in
              // this series at all - and it still comes before the name, for
              // exactly that reason. What is new in front of it is the member's
              // position in the series, and what moved behind it is the file.
              //
              // The order IS the feature. A Files panel dragged narrow drops
              // what is on its right, so this row loses the file name first and
              // keeps the two things that say where the row sits in the sweep -
              // while the pinned heading above it, which no width can push off,
              // keeps saying which sweep that is. That is the whole reason there
              // is no setting for any of this: the columns already do the work
              // an option would have to be asked for.
              //
              // An unset value has to be readable as unset, not inferable from a
              // blank at the right edge - conditionText prints the words.
              //
              // The "##m<seqId>" suffix is the row's ID, so renaming the stack
              // does not reset its widget state. It does NOT protect the text:
              // ImGui cuts a label at the FIRST "##", so a stack the user
              // renames to "a##b" renders as "a" in either shape of row.
              char vb[64];
              {
                  std::string vs = conditionText(*ser, mem->value);
                  if (std::isfinite(mem->value) && ser->unit[0])
                      snprintf(vb, sizeof vb, "%s %s", vs.c_str(), ser->unit);
                  else
                      snprintf(vb, sizeof vb, "%s", vs.c_str());
              }
              snprintf(lb, sizeof lb, "%*s[%d]  %s  %s%s##m%d", (int)(badge.size() * 2), "",
                       memIdx, vb, filesMemberName(sname).c_str(), rmark, head.seqId);
          } else {
              snprintf(lb, sizeof lb, "%*s%s%s", (int)std::max<size_t>(badge.size() * 2, 2),
                       "", sname, rmark);
          }
          char frames[32];
          {   // "8f" for an 8-of-300 stack states nothing; say the ratio while
              // the total is known and the load is short of it
              const App::SeqInfo* fsi = seqInfo(head.seqId);
              int exp = fsi ? fsi->expectedFrames : 0;
              if (exp > (int)stack.size())
                  snprintf(frames, sizeof frames, "  %d/%df", (int)stack.size(), exp);
              else
                  snprintf(frames, sizeof frames, "  %df", (int)stack.size());
          }
          std::string share;                   // §4: any member's pixels, elsewhere
          {
              std::vector<const ImageDoc*> sharedFrames;
              for (int fidx : stack) {
                  const ImageDoc& f = *app.images[fidx];
                  auto sh = srcOwners.find(f.src->srcId);
                  if (sh != srcOwners.end() && sh->second > 1)
                      sharedFrames.push_back(&f);
              }
              if (!sharedFrames.empty()) share = filesShareSentence(sharedFrames);
              // ras §6: `bound as "dark" in "PRNU 10lx"` - sentence, no mark
              std::string b2 = aSetBoundSentence(head.seqId, 0, 0);
              if (!b2.empty()) share += (share.empty() ? "" : "\n") + b2;
          }
          ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerStack));
          bool stackHit = rowWithMeta(head, lb, active, frames, badge, !cmpOn, share);
          ImGui::PopStyleColor();
          // #56, the user's own proposal: the red outline. Drawn straight onto
          // the row rather than as another item, exactly as the compare letters
          // and the meta strip are - the row must stay the LAST item, or the
          // right-click below opens over the wrong thing. Immediately after the
          // call for the same reason: no widget between it and GetItemRect*.
          if (*rmark) {
              ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
              ImGui::GetWindowDrawList()->AddRect(a0, a1, RELOAD_FAIL_RED32,
                                                 2.0f * app.uiScale, 0,
                                                 std::max(1.0f, 1.5f * app.uiScale));
          }
          if (stackHit) {
              selectImage(stack[pos]);
              if (app.fitOnSwitch) app.fitRequested = true;
          }
          // The stack is the unit measurements attach to, so it earns a name of
          // its own: "the 25C dark set", not whatever the capture script called
          // the folder. F2 / right-click renames; the folder name is only the
          // starting value.
          if (si && ImGui::BeginPopupContextItem("seqctx")) {
            static char renameBuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(renameBuf, sizeof renameBuf, "%s", si->name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##rename", renameBuf, sizeof renameBuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && renameBuf[0]) {
                si->name = renameBuf;
                app.lin.rev++;            // linearity rows show stack names
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            {   // B = the frame this stack is showing; with "B follows A's
                // frame number" on, it tracks A from there anyway. The stack A
                // stands in gets the group too - setting it as B is how you put
                // one document on both sides and see that they agree.
                ImageDoc* bpick = app.images[stack.front()].get();
                if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
                    app.images[si->lastImageIdx]->seqId == si->id)
                    bpick = app.images[si->lastImageIdx].get();
                if (compareBItem(bpick)) ImGui::Separator();
            }
            // the whole stack again, independently: the child regroups the
            // numbered siblings from the head file (item 28)
            if (ImGui::MenuItem("Open in new window"))
                openInNewWindow(app.images[stack.front()].get());
            {   // manual reload (reference-design §3.2 / §8 row 5): every
                // member's file re-read, pixels swapped in place. ONE member
                // the walk can serve is enough to offer the item - the summary
                // counts the rest; when none can, the FIRST member's own reason
                // is the one shown, from the same predicate the frame menu asks
                // (reloadUnavailable, beside reloadSource).
                bool anyLocal = false;
                std::string why;
                for (int idx : stack) {
                    std::string w = reloadUnavailable(*app.images[idx]->src);
                    if (w.empty()) { anyLocal = true; break; }
                    if (why.empty()) why = std::move(w);
                }
                if (ImGui::MenuItem("Reload from disk", nullptr, false, anyLocal))
                    pendingReloadSeq = si->id;
                if (!anyLocal && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", why.c_str());
            }
            // The series (系列) this stack is in, or could be. Multi-select is
            // a later phase; until then this menu is how a stack joins a sweep
            // without going through the Linearity panel.
            if (ImGui::BeginMenu("Series")) {
                const App::Series* mine = seriesOfStack(si->id);
                // The value joining would give this stack, BEFORE the click. The
                // modal shows its proposal and the picker paints one per row;
                // this was the one place a number entered a measurement without
                // having been on screen first (docs/series-plan.md §2 = 確認).
                std::string jfrom;
                double jv = extractLevelFromName(si->name, &jfrom);
                if (std::isfinite(jv)) ImGui::TextDisabled("joins at %.6g (from \"%s\")",
                                                           jv, jfrom.c_str());
                else ImGui::TextDisabled("joins with NO value (nothing numeric in the name)");
                int listed = 0;
                for (const auto& s : app.series) {
                    bool same = s.batchId == head.batchId;
                    bool isMine = mine && mine->id == s.id;
                    char lb2[416];
                    if (std::isfinite(jv) && same && !isMine)
                        snprintf(lb2, sizeof lb2, "%s   -> %.6g %s###addser%d", s.name.c_str(),
                                 jv, s.unit[0] ? s.unit : "[unit not set]", s.id);
                    else
                        snprintf(lb2, sizeof lb2, "%s###addser%d", s.name.c_str(), s.id);
                    if (ImGui::MenuItem(lb2, nullptr, isMine, same && !isMine)) {
                        pendingJoinSeq = si->id;
                        pendingJoinSer = s.id;
                    }
                    // A series cannot widen to reach another batch: the answer
                    // is to move the stack first, and the menu says so instead
                    // of leaving a dead item with no reason.
                    if (!same && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("\"%s\" is in batch \"%s\".\n"
                                          "A series lives in ONE batch - use Move to batch\n"
                                          "first, then add it here.",
                                          s.name.c_str(), batchNameOf(s.batchId).c_str());
                    listed++;
                }
                if (!listed) ImGui::TextDisabled("(no series yet)");
                ImGui::Separator();
                if (ImGui::MenuItem("New series...")) pendingNewSerBatch = head.batchId;
                if (mine && ImGui::MenuItem("Remove from this series"))
                    pendingLeaveSeq = si->id;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // A stack that is ALREADY open gets the same operation Browse
            // offers at open time - one item, one meaning, one code path
            // (requestStackAverage). Sitting beside Derive is deliberate: both
            // make a NEW thing out of this stack and leave this stack alone,
            // and this is the one that crosses a layer (stack -> frame).
            if (ImGui::MenuItem("Open frame average")) pendingAvgSeq = si->id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AVG_TIP);
            // The fold is a CHOICE (docs/analysis-layers.md §3.2): mean and sum
            // are two items with two names, not a mode hidden in a dialog - the
            // item's name is the declaration of what the resulting pixels hold,
            // exactly as the result's own name will be.
            if (ImGui::MenuItem("Open frame sum")) pendingSumSeq = si->id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", SUM_TIP);
            // ---- the AnalysisPreProcessor (docs/analysis-layers.md §5) ------
            // The detrend stage lives HERE and nowhere else (判断6): it makes a
            // PRODUCT, and every analyzer then applies to that product
            // unchanged. There is deliberately no detrend option on Histogram /
            // Projection / ROIs / Temporal - one would make the same column
            // name carry a different quantity.
            //
            // The items are named after what they DO, the frame average / sum
            // precedent: the settled default is one item that says it is the
            // default, and every other band is an item of its own rather than a
            // number in a dialog nobody re-reads. The choice ends up in the
            // product's name, note and cutoff, so what is chosen here is what a
            // report will cite.
            if (si->dt.made()) {
                ImGui::TextDisabled("already a detrend product");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("This stack IS the output of the stage:\n%s\n"
                                      "One stage, one cutoff - change the method on the "
                                      "source stack\ninstead of removing a surface twice.",
                                      detrendCutoffText(si->dt.spec).c_str());
            } else if (ImGui::BeginMenu("Detrend (remove shading)")) {
                auto item = [&](const char* label, const DetrendSpec& s) {
                    if (ImGui::MenuItem(label)) {
                        pendingDetrendSeq = si->id;
                        pendingDetrendSpec = s;
                    }
                };
                {   // 判断7: "ON にしたときの既定方式 = 多項式2次・減算・プレーン別"
                    DetrendSpec s; s.method = DTR_POLY; s.degree = 2;
                    item("polynomial degree 2, subtractive  (the settled default)", s);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", DETREND_TIP);
                if (ImGui::BeginMenu("other bands")) {
                    ImGui::TextDisabled("a higher degree removes more, and every");
                    ImGui::TextDisabled("statistic below it is a function of that band");
                    for (int deg = 1; deg <= 4; deg++) {
                        if (deg == 2) continue;
                        char lb[64];
                        snprintf(lb, sizeof lb, "polynomial degree %d, subtractive", deg);
                        DetrendSpec s; s.method = DTR_POLY; s.degree = deg;
                        item(lb, s);
                    }
                    ImGui::Separator();
                    for (int blk : { 32, 64, 128 }) {
                        char lb[64];
                        snprintf(lb, sizeof lb, "block median, %d px window, subtractive", blk);
                        DetrendSpec s; s.method = DTR_BLOCKMED; s.block = blk;
                        item(lb, s);
                    }
                    ImGui::Separator();
                    {   // 約束6: offered, and warned about where it is offered
                        DetrendSpec s; s.method = DTR_POLY; s.degree = 2; s.divide = true;
                        item("polynomial degree 2, DIVIDE (multiplicative shading)", s);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Dividing by the surface scales the noise by 1/s, so\n"
                                          "the spatial variance stops being uniform across the\n"
                                          "field - and the estimators downstream assume it is.\n"
                                          "Prefer subtraction for noise statistics; the product\n"
                                          "says which one it was made with either way.");
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            // A NEW stack from a subset of this one - the dialog offers the
            // matching rules (same names / numbers as another stack, a range,
            // hand-picked). References frames; this stack is not touched.
            if (ImGui::MenuItem("Derive stack (a subset, by reference)..."))
                pendingDeriveSeq = si->id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Make a NEW stack from a subset of this one - e.g. only\n"
                                  "the frames that also exist in another stack - so two\n"
                                  "stacks with mismatched membership can be compared\n"
                                  "frame by frame. Frames are SHARED by reference (no\n"
                                  "pixel copy); this stack keeps every frame it has.");
            ImGui::Separator();
            {   // the STACK moves, whole - per the canon's frame ⊂ stack ⊂ batch
                if (const App::Series* mine = seriesOfStack(si->id))
                    ImGui::TextDisabled("in series \"%s\": moving it alone leaves it",
                                        mine->name.c_str());
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveSeq = si->id; pendingMoveTarget = t; }
            }
            if (ImGui::MenuItem("Close stack"))     // the stack layer's Close
                pendingCloseSeq = si->id;
            ImGui::EndPopup();
          }
          // The only shortcut in the program that used to act on an UNFOCUSED
          // panel, with no text-input and no popup gate - so F2 while typing in
          // the Browse filter opened a rename popup, and F2 while a modal was up
          // REPLACED that modal (OpenPopup at root level evicts what is there).
          if (active && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
              !ImGui::GetIO().WantTextInput &&
              !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
              ImGui::IsKeyPressed(ImGuiKey_F2, false))
              ImGui::OpenPopup("seqctx");
          // No frame slider here: it used to appear under the active row, and a
          // row that grows on selection makes the whole list jump - the scrub bar
          // lives at the bottom of the Image View now, where the frames are.
          ImGui::PopID();
        };
        // The stack of one seqId, out of this batch's cached list (members are
        // named by seqId; the panel walks by stack).
        auto stackOfSeq = [&](int seqId) -> const std::vector<int>* {
            for (const auto* sp : group.stacks)
                if (app.images[sp->front()]->seqId == seqId) return sp;
            return nullptr;
        };
        // ---- the batch's series come FIRST, each with its members inside -----
        // Non-member stacks keep their place and their indentation below: a
        // sweep appearing must not push everything else down a level.
        for (const auto& S : app.series) {
            if (S.batchId != group.batch) continue;
            float sy0 = ImGui::GetCursorPos().y;
            ImGui::PushID(S.id);
            bool sopen = drawSeriesHead(S, false);
            float shy = ImGui::GetCursorPos().y;
            if (sopen) {
                // memIdx is the member's place in the series, and it is the
                // row's leading column - see the label built in drawStackRow.
                for (int mi = 0; mi < (int)S.members.size(); mi++)
                    if (const std::vector<int>* sp = stackOfSeq(S.members[mi].seqId))
                        drawStackRow(*sp, &S, &S.members[mi], mi);
                ImGui::TreePop();
            }
            ImGui::PopID();
            g_filesSpans.push_back({ group.batch, S.id, sy0, shy, ImGui::GetCursorPos().y });
        }
        // ---- the batch's SETS: one node each, its role rows under it --------
        // (docs/reader-analysisset.md §6). Reading order inside the node: set
        // -> role -> member. A role row is the BINDING's display; the member's
        // home stays the member's own stack row - the tree is not duplicated,
        // so a role row jumps (double-click) rather than re-parents.
        for (const auto& AS : app.analysisSets) {
            if (AS.batchId != group.batch) continue;
            ImGui::PushID(-AS.id);        // negative: never collides with seqIds
            ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerSet));
            bool sopen = ImGui::TreeNodeEx("##aset", ImGuiTreeNodeFlags_DefaultOpen |
                                                     ImGuiTreeNodeFlags_SpanAvailWidth,
                                           "%s", aSetRowText(AS).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) { g_aSetSelId = AS.id; g_aSetSelRole = -1; }
            if (ImGui::IsItemHovered()) {
                std::string tip = "analysis set: " + std::to_string((int)AS.roles.size()) +
                                  " role binding(s)";
                if (!AS.readerSpec.empty()) tip += "\nmade by reader " + AS.readerSpec;
                tip += "\n\n(right-click: rename, close - Close drops the bindings"
                       "\nonly; the members are untouched)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (ImGui::BeginPopupContextItem("asetctx")) {
                static char abuf[256];
                if (ImGui::IsWindowAppearing())
                    snprintf(abuf, sizeof abuf, "%s", AS.name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                bool done = ImGui::InputText("##aname", abuf, sizeof abuf,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                done |= ImGui::SmallButton("rename");
                if (done && abuf[0]) {
                    pendingSetRename = AS.id;
                    pendingSetName = abuf;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close set (drop bindings, keep members)"))
                    pendingCloseSet = AS.id;
                ImGui::EndPopup();
            }
            if (sopen) {
                for (int ri = 0; ri < (int)AS.roles.size(); ri++) {
                    const App::ASetRole& r = AS.roles[ri];
                    const bool sel = g_aSetSelId == AS.id && g_aSetSelRole == ri;
                    ImGui::PushID(ri);
                    if (!r.bound()) {
                        // unbound: the reason is ON the row (ras §3.2), in the
                        // same warning ink the partial-load note uses
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
                        if (ImGui::Selectable(aSetRoleText(r).c_str(), sel))
                            { g_aSetSelId = AS.id; g_aSetSelRole = ri; }
                        ImGui::PopStyleColor();
                    } else {
                        if (ImGui::Selectable(aSetRoleText(r).c_str(), sel,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            g_aSetSelId = AS.id;
                            g_aSetSelRole = ri;
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                // double-click COMMITS: go to the bound member
                                int jump = -1;
                                if (r.seqId != 0) {
                                    const App::SeqInfo* si2 = seqInfo(r.seqId);
                                    if (si2 && si2->lastImageIdx >= 0 &&
                                        si2->lastImageIdx < (int)app.images.size() &&
                                        app.images[si2->lastImageIdx]->seqId == r.seqId)
                                        jump = si2->lastImageIdx;
                                    else {
                                        std::vector<int> fr = framesOfSeq(r.seqId);
                                        if (!fr.empty()) jump = fr.front();
                                    }
                                } else if (r.frameUid != 0) {
                                    for (int q = 0; q < (int)app.images.size(); q++)
                                        if (app.images[q]->uid == r.frameUid) { jump = q; break; }
                                } else if (r.seriesId != 0) {
                                    for (const auto& s2 : app.series)
                                        if (s2.id == r.seriesId && !s2.members.empty()) {
                                            std::vector<int> fr =
                                                framesOfSeq(s2.members.front().seqId);
                                            if (!fr.empty()) jump = fr.front();
                                        }
                                }
                                if (jump >= 0) {
                                    selectImage(jump);
                                    if (app.fitOnSwitch) app.fitRequested = true;
                                }
                            }
                        }
                        if (ImGui::IsItemHovered() && !r.refPath.empty())
                            ImGui::SetTooltip("Ref %s\nclick selects; double-click goes "
                                              "to the bound member", r.refPath.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // ---- then everything this batch holds that is NOT in a series -------
        for (const auto& stackPtr : group.stacks) {
            const ImageDoc& head = *app.images[stackPtr->front()];
            if (head.seqId != 0 && seriesOfStack(head.seqId)) continue;   // drawn above
            drawStackRow(*stackPtr, nullptr, nullptr, -1);
        }
      }
      if (showHeaders && open) ImGui::TreePop();
      ImGui::PopID();
      if (showHeaders)
          g_filesSpans.push_back({ group.batch, 0, gy0, ghy, ImGui::GetCursorPos().y });
    }
    g_filesScrollY = ImGui::GetScrollY();
    }
    ImGui::EndChild();
    if (pendingCloseSeq) closeStack(pendingCloseSeq);
    if (pendingCloseBatch) closeBatch(pendingCloseBatch);
    if (pendingMoveTarget && pendingMoveSeq) moveStackToBatch(pendingMoveSeq, pendingMoveTarget);
    if (pendingMoveTarget && pendingMoveImg >= 0) moveImageToBatch(pendingMoveImg, pendingMoveTarget);
    if (pendingDeriveSeq) deriveModalOpen(pendingDeriveSeq);
    if (pendingReloadImg >= 0 && pendingReloadImg < (int)app.images.size()) {
        ImageDoc* d = app.images[pendingReloadImg].get();
        std::string err, note;
        if (reloadSource(d->src, err, &note)) {
            std::string msg = "reloaded " + baseName(d->src->path) +
                              (note.empty() ? "" : " - dimensions changed: " + note);
            // the §3.2 walk refreshed EVERY membership of this source, not
            // just this row: numbers moving in a stack the user never touched
            // need a stated cause - the same clause reloadStackFromDisk earns
            int sharedN = 0;
            std::vector<std::string> sharedNames;
            for (const auto& q : app.images)
                if (q.get() != d && q->src == d->src) {
                    sharedN++;
                    std::string where = q->name;
                    if (q->seqId != 0)
                        if (const App::SeqInfo* si2 = seqInfo(q->seqId)) where = si2->name;
                    if (std::find(sharedNames.begin(), sharedNames.end(), where) ==
                        sharedNames.end())
                        sharedNames.push_back(where);
                }
            if (sharedN) {
                char b2[192];
                snprintf(b2, sizeof b2, " - also refreshed %d shared frame(s) in \"%s\"",
                         sharedN, sharedNames[0].c_str());
                msg += b2;
                if (sharedNames.size() > 1)
                    msg += " (+" + std::to_string((int)sharedNames.size() - 1) + " more)";
            }
            // the walk latched staleness on every reduced frame of every stack
            // this source belongs to (§10.1) - the toast must say so too. The
            // row is a loose frame, but a shared source can sit in stacks too,
            // so the criterion is the walk's own: memberships of THIS source.
            {
                int staleAvg = 0;
                for (const auto& q : app.images) {
                    if (q->avgOfPath.empty() || !q->avgStale || !q->avgSeqId) continue;
                    for (const auto& r : app.images)
                        if (r->src == d->src && r->seqId == q->avgSeqId) {
                            staleAvg++; break;
                        }
                }
                if (staleAvg)
                    msg += " - " + std::to_string(staleAvg) + " computed frame(s) now "
                           "stale (right-click > Recompute from stack)";
            }
            toast(msg);
        } else {
            toast("reload failed - " + baseName(d->src->path) + ": " + err, true);
        }
    }
    if (pendingReloadSeq) {
        int failed = 0;
        std::string msg = reloadStackFromDisk(pendingReloadSeq, &failed);
        toast(msg, failed > 0);
    }
    if (pendingRecomputeImg >= 0 && pendingRecomputeImg < (int)app.images.size()) {
        std::string err;   // success toasts inside (the reduction's own voice)
        if (!recomputeStackAverage(app.images[pendingRecomputeImg].get(), err))
            toast("recompute failed - " + err, true);
    }
    if (pendingAvgSeq) requestStackAverage(pendingAvgSeq);
    if (pendingSumSeq) requestStackAverage(pendingSumSeq, FOLD_SUM);
    if (pendingDetrendSeq) {
        std::string err;   // the success toast is the product's own voice
        if (!detrendStackProduct(pendingDetrendSeq, pendingDetrendSpec, err)) toast(err, true);
    }
    if (pendingCloseSet) closeAnalysisSet(pendingCloseSet);
    if (pendingSetRename) {
        std::string why;
        if (!renameAnalysisSet(pendingSetRename, pendingSetName, &why) && !why.empty())
            toast(why, true);   // the one refusal: same name in one batch (ras 1.4)
    }
    // ---- the series commands, after the walk that drew them -----------------
    if (pendingSerRename)
        if (App::Series* S = seriesById(pendingSerRename)) {
            S->name = pendingSerName;
            app.imagesRev++;
            app.lin.rev++;                // the panel's selector shows this name
        }
    if (pendingSerEdit)
        if (App::Series* S = seriesById(pendingSerEdit)) openSeriesModal(S->batchId, S->id);
    if (pendingSerMove && pendingSerMoveTo) moveSeriesToBatch(pendingSerMove, pendingSerMoveTo);
    if (pendingSerUngroup) ungroupSeries(pendingSerUngroup);
    if (pendingSerClose) closeSeries(pendingSerClose);
    if (pendingNewSerBatch) openSeriesModal(pendingNewSerBatch, 0);
    if (pendingLeaveSeq) {
        App::Series* S = seriesOfStack(pendingLeaveSeq);
        App::SeqInfo* si = seqInfo(pendingLeaveSeq);
        if (S) {
            std::string n = S->name;
            removeFromSeries(pendingLeaveSeq);
            pruneEmptySeries();
            linFitStale();          // the fit below counted this point
            toast((si ? si->name : std::string("stack")) + " left series \"" + n + "\"");
        }
    }
    if (pendingJoinSeq && pendingJoinSer) {
        std::string msg;
        bool joined = seriesJoinFromMenu(pendingJoinSer, pendingJoinSeq, &msg);
        if (!msg.empty()) toast(msg, !joined);
    }
}

static void drawSequenceModal() {
    if (app.seqAskImage >= 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::OpenPopup("Load stack?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Load stack?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("%d files match %s", (int)app.seqAskFiles.size(),
                dispPath(app.seqAskPattern).c_str());
    ImGui::TextDisabled("Loading them as one stack enables temporal analysis.");
    ImGui::TextDisabled("Frames decode in the background; you can keep working.");
    ImGui::Separator();
    static bool remember = false;
    ImGui::Checkbox("remember my choice (File > Stack loading)", &remember);
    if (ImGui::Button("Load stack", ImVec2(150 * app.uiScale, 0))) {
        startSequenceLoad(app.seqAskImage, app.seqAskFiles, app.seqAskPattern);
        if (remember) app.seqLoadMode = 1;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("This file only", ImVec2(150 * app.uiScale, 0))) {
        if (remember) app.seqLoadMode = 2;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

