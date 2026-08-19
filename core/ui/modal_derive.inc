// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ==== derive a stack from a stack (docs/background/project/todo-open.md item 0) ================
// A NEW stack from a subset of an existing one, so two stacks with mismatched
// membership can be lined up and compared frame by frame. BY REFERENCE since
// stage 3 (docs/reference-design.md §6.1): the source stays intact, and a
// chosen frame joins as a second membership of the RESIDENT source - NEVER
// re-read from disk and NEVER re-fetched from the remote. A re-fetch would
// give remote-sourced stacks a second code path and a second answer to the
// same question, which is the defect class TODO_one_implementation.md exists
// to stamp out. One path, both origins.
//
// The structure is deliberately two-phase - it let stage 3 swap the stage-1
// pixel copy for the reference without touching the dialog or the tests:
//   - buildDerivePlan  = WHICH frames are chosen (pure, no mutation)
//   - applyDerivePlan  = HOW a chosen frame becomes a member; that lives in
//     materializeDerivedFrame and ONLY there.

// One member of a stack, as the FILE LIST knows it. For a remote folder stack
// the membership is SeqInfo::remoteFiles - matching must work before (or
// whether) the frames arrive, or the feature is useless exactly where it is
// needed. imageIdx then says whether that member is actually resident; a
// decimated preview (remoteStep > 1) is not pixels and does not count.
struct DeriveEntry {
    std::string base;             // basename: the name-matching key
    int seqIndex = 0;             // the frame number: the number-matching key
    int imageIdx = -1;            // index into app.images, or -1 = not resident
};
static std::vector<DeriveEntry> stackMembers(int seqId) {
    std::vector<DeriveEntry> out;
    const App::SeqInfo* si = seqInfo(seqId);
    if (si && !si->remoteFiles.empty()) {
        for (int i = 0; i < (int)si->remoteFiles.size(); i++)
            out.push_back({ baseName(si->remoteFiles[i]), i, -1 });
        for (int k = 0; k < (int)app.images.size(); k++) {
            const ImageDoc* d = app.images[k].get();
            if (d->seqId != seqId || d->src->remoteStep > 1) continue;
            for (auto& e : out)
                if (e.seqIndex == d->seqIndex && e.imageIdx < 0) { e.imageIdx = k; break; }
        }
    } else {
        for (int k = 0; k < (int)app.images.size(); k++) {
            const ImageDoc* d = app.images[k].get();
            if (d->seqId != seqId || d->src->remoteStep > 1) continue;
            out.push_back({ baseName(d->src->path.empty() ? d->name : d->src->path),
                            d->seqIndex, k });
        }
        std::sort(out.begin(), out.end(),
                  [](const DeriveEntry& a, const DeriveEntry& b) {
                      return a.seqIndex < b.seqIndex;
                  });
    }
    return out;
}

// The matching rules item 0 lists. Hand-picked is the required escape hatch.
enum { DR_NAME_IN = 0, DR_NAME_OUT, DR_IDX_IN, DR_IDX_OUT, DR_RANGE, DR_PICK };

struct DerivePlan {
    int srcSeqId = 0, otherSeqId = 0, rule = DR_NAME_IN;
    int lo = 0, hi = 0;                    // DR_RANGE, inclusive
    struct Pick { int srcImageIdx; int newIndex; std::string base; };
    std::vector<Pick> picks;               // resident frames to take, in order
    // Counts in BOTH directions, computed against the membership lists above.
    // One-directional counts read as "aligned" when they are not.
    int srcKnown = 0;                      // members known here (file list or resident)
    int srcMatched = 0;                    // members the rule takes
    int pairs = 0;                         // members with a counterpart over there
    int otherKnown = 0, otherUnmatched = 0;
    int notResident = 0;                   // taken by the rule, but not loaded:
                                           // REPORTED and left out, never fetched
    // per-source-member echo for the dialog's table (parallel to stackMembers)
    std::vector<char> taken;
    std::vector<int> newIndexOf;           // -1 = not taken
};

// WHICH frames are chosen. Pure: reads memberships, mutates nothing.
// DR_NAME_IN adopts the OTHER stack's frame number for each matched name:
// two stacks that were renumbered by their own opens (a stack with missing
// files loads as a contiguous 0..n-1) pair the WRONG files silently today -
// after adoption, the same name carries the same number on both sides, which
// is what lets compareFollowFrame stop diverging. Every other rule keeps the
// source's numbers (there is no counterpart to adopt).
static DerivePlan buildDerivePlan(int srcSeqId, int rule, int otherSeqId,
                                  int lo, int hi, const std::vector<char>* pick) {
    DerivePlan P;
    P.srcSeqId = srcSeqId; P.rule = rule; P.otherSeqId = otherSeqId;
    P.lo = lo; P.hi = hi;
    std::vector<DeriveEntry> src = stackMembers(srcSeqId);
    P.srcKnown = (int)src.size();
    std::vector<DeriveEntry> oth;
    if (rule <= DR_IDX_OUT) {
        oth = stackMembers(otherSeqId);
        P.otherKnown = (int)oth.size();
    }
    // an other-side member pairs with AT MOST ONE member here, so duplicate
    // basenames stay honest and the reverse count stays exact
    std::vector<char> used(oth.size(), 0);
    P.taken.assign(src.size(), 0);
    P.newIndexOf.assign(src.size(), -1);
    for (int i = 0; i < (int)src.size(); i++) {
        const DeriveEntry& e = src[i];
        bool take = false;
        int newIndex = e.seqIndex;
        int j = -1;
        switch (rule) {
        case DR_NAME_IN: case DR_NAME_OUT:
            for (int k = 0; k < (int)oth.size(); k++)
                if (!used[k] && oth[k].base == e.base) { j = k; break; }
            take = (rule == DR_NAME_IN) == (j >= 0);
            if (j >= 0) { used[j] = 1; P.pairs++; }
            if (rule == DR_NAME_IN && j >= 0) newIndex = oth[j].seqIndex;
            break;
        case DR_IDX_IN: case DR_IDX_OUT:
            for (int k = 0; k < (int)oth.size(); k++)
                if (!used[k] && oth[k].seqIndex == e.seqIndex) { j = k; break; }
            take = (rule == DR_IDX_IN) == (j >= 0);
            if (j >= 0) { used[j] = 1; P.pairs++; }
            break;
        case DR_RANGE: take = e.seqIndex >= lo && e.seqIndex <= hi; break;
        case DR_PICK:  take = pick && i < (int)pick->size() && (*pick)[i]; break;
        }
        if (!take) continue;
        P.srcMatched++;
        P.taken[i] = 1;
        P.newIndexOf[i] = newIndex;
        if (e.imageIdx < 0) { P.notResident++; continue; }
        P.picks.push_back({ e.imageIdx, newIndex, e.base });
    }
    for (char u : used) if (!u) P.otherUnmatched++;
    std::sort(P.picks.begin(), P.picks.end(),
              [](const DerivePlan::Pick& a, const DerivePlan::Pick& b) {
                  return a.newIndex < b.newIndex;
              });
    return P;
}

// The rule, in words - it goes into the derived stack's NAME (the montage
// naming is the precedent: a derived thing names its origin and its rule).
static std::string deriveRuleText(const DerivePlan& P) {
    const App::SeqInfo* o = seqInfo(P.otherSeqId);
    const std::string on = o ? "\"" + o->name + "\"" : "?";
    char b[64];
    switch (P.rule) {
    case DR_NAME_IN:  return "same names as " + on;
    case DR_NAME_OUT: return "names not in " + on;
    case DR_IDX_IN:   return "same frame numbers as " + on;
    case DR_IDX_OUT:  return "frame numbers not in " + on;
    case DR_RANGE:    snprintf(b, sizeof b, "frames %d\xE2\x80\xA5%d", P.lo, P.hi);
                      return b;
    default:          return "picked by hand";
    }
}

// HOW a chosen frame becomes a member: a NEW membership on the SAME source
// (stage 3, §6.1) - the reference replaces the stage-1 pixel copy, exactly the
// one-function swap the two-phase structure above promised. Pixels, provenance
// and the Watch baseline travel with the source; what is set here is the
// per-membership half of the split (§2.1). A later crop of either side goes
// through the CoW in cropInPlace, so neither stack can change under the other.
static ImageDoc* materializeDerivedFrame(const ImageDoc& s, int seqId, int seqIndex,
                                         int batchId, const std::string& note) {
    auto d = std::make_unique<ImageDoc>();
    d->name = s.name;
    d->src = s.src;                       // the reference IS the feature
    d->syncMirrors();
    d->black = s.black; d->white = s.white;   // frames stay directly comparable
    d->cfa = s.cfa; d->cfaPattern = s.cfaPattern; d->cfaColorize = s.cfaColorize;
    d->displayLut = s.displayLut;
    d->note = note;    // the SAME note on every frame: a stack-constant row, so
                       // the Inspector never gains or loses a line mid-stack
    d->seqId = seqId; d->seqIndex = seqIndex; d->batchId = batchId;
    d->texDirty = true;
    d->uid = app.nextUid++;
    ImageDoc* out = d.get();
    app.images.push_back(std::move(d));
    app.imagesRev++;
    return out;
}

// Create the derived stack. Returns its seqId, or 0 when there is nothing to
// reference. The source is not touched. The derived stack joins NO series -
// the canon says membership is explicit (Series::members is the only truth),
// and a derived stack is a different measurement subject. expectedFrames =
// what was actually taken: the stack is complete AS ITSELF; what the rule
// matched but could not take is said in the note and by the caller, not
// smuggled into n/N.
static int applyDerivePlan(const DerivePlan& P, bool intoNewBatch) {
    const App::SeqInfo* src = seqInfo(P.srcSeqId);
    if (!src || P.picks.empty()) return 0;
    const std::string srcName = src->name;
    const std::string rule = deriveRuleText(P);
    App::SeqInfo si;
    si.id = app.nextSeqId++;
    si.cfaType = src->cfaType; si.cfaPattern = src->cfaPattern;  // planes travel;
                                                    // whole frames, never mixed
    si.expectedFrames = (int)P.picks.size();
    // No remote fields on purpose: every member is resident by construction,
    // so the derived stack behaves identically whether the source was local
    // or remote - same panels, same measurements, same answers.
    char nm[512];
    snprintf(nm, sizeof nm, "%s (%s: %d of %d)", srcName.c_str(), rule.c_str(),
             (int)P.picks.size(), P.srcKnown);
    si.name = nm;
    int batchId = app.images[P.picks[0].srcImageIdx]->batchId;
    if (intoNewBatch)
        batchId = newBatch(uniqueBatchName(batchNameOf(batchId) + " (derived)"));
    std::string note = "derived from \"" + srcName + "\" - " + rule + ": " +
                       std::to_string((int)P.picks.size()) + " of " +
                       std::to_string(P.srcKnown) + " frame(s) referenced";
    if (P.notResident > 0)
        note += "; " + std::to_string(P.notResident) +
                " matched frame(s) were not loaded and were left out";
    // `src` is dead from here on - not "may be", IS: App::seqs reseats on every
    // mutation (state.h), so this push_back frees the storage it points into on
    // every platform rather than only where the allocator chose to grow. srcName
    // and rule were copied out above for exactly this reason.
    app.seqs.push_back(si);
    for (const auto& pk : P.picks)
        materializeDerivedFrame(*app.images[pk.srcImageIdx], si.id, pk.newIndex,
                                batchId, note);
    return si.id;
}

// ---- the dialog ------------------------------------------------------------
struct DeriveModalState {
    bool open = false;
    int srcSeqId = 0;
    int rule = DR_NAME_IN;
    int otherSeqId = 0;
    int lo = 0, hi = 0;
    bool intoNewBatch = false;
    std::vector<char> pick;               // DR_PICK, parallel to stackMembers(src)
};
static DeriveModalState g_derive;

static void deriveModalOpen(int seqId) {
    DeriveModalState& D = g_derive;
    D = DeriveModalState();
    D.srcSeqId = seqId;
    // the other stack defaults to B's stack when B is pinned - lining a stack
    // up against B is the reason this dialog exists
    for (const auto& d : app.images)
        if (app.compareBUid && d->uid == app.compareBUid && d->seqId != 0 &&
            d->seqId != seqId) { D.otherSeqId = d->seqId; break; }
    if (!D.otherSeqId)
        for (const auto& s : app.seqs)
            if (s.id != seqId) { D.otherSeqId = s.id; break; }
    std::vector<DeriveEntry> ms = stackMembers(seqId);
    D.pick.assign(ms.size(), 1);
    D.lo = 0;
    D.hi = 0;
    for (const auto& e : ms) D.hi = std::max(D.hi, e.seqIndex);
    if (!D.otherSeqId) D.rule = DR_RANGE;   // nothing to match against
    D.open = true;
}

static void drawDeriveModal() {
    DeriveModalState& D = g_derive;
    const char* POPUP = "Derive stack###derivemodal";
    if (D.open && !ImGui::IsPopupOpen(POPUP)) ImGui::OpenPopup(POPUP);
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600 * app.uiScale, 560 * app.uiScale),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(POPUP, nullptr, ImGuiWindowFlags_None)) return;
    if (!D.open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const App::SeqInfo* src = seqInfo(D.srcSeqId);
    if (!src) {   // the source stack was closed under the dialog
        D.open = false;
        ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        return;
    }
    std::vector<DeriveEntry> ms = stackMembers(D.srcSeqId);
    if (D.pick.size() != ms.size()) D.pick.resize(ms.size(), 1);
    int resident = 0;
    for (const auto& e : ms) if (e.imageIdx >= 0) resident++;
    ImGui::TextWrapped("from: %s", src->name.c_str());
    int expected = std::max(src->expectedFrames, (int)ms.size());
    if (resident < expected)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                           "%d of %d frame(s) loaded - matching uses the file list, "
                           "but only loaded frames can be referenced", resident, expected);
    else
        ImGui::TextDisabled("%d frame(s), all loaded", resident);
    ImGui::Separator();

    // the rule (item 0's list; hand-picked is the escape hatch)
    bool haveOther = false;
    for (const auto& s : app.seqs) if (s.id != D.srcSeqId) { haveOther = true; break; }
    ImGui::BeginDisabled(!haveOther);
    ImGui::RadioButton("keep frames whose file name is also in the other stack",
                       &D.rule, DR_NAME_IN);
    ImGui::RadioButton("keep frames whose file name is NOT in the other stack",
                       &D.rule, DR_NAME_OUT);
    ImGui::RadioButton("keep frames whose frame number is also in the other stack",
                       &D.rule, DR_IDX_IN);
    ImGui::RadioButton("keep frames whose frame number is NOT in the other stack",
                       &D.rule, DR_IDX_OUT);
    ImGui::EndDisabled();
    if (!haveOther)
        ImGui::TextDisabled("(no other stack is open to match against)");
    ImGui::RadioButton("keep a frame number range", &D.rule, DR_RANGE);
    ImGui::RadioButton("pick frames by hand", &D.rule, DR_PICK);

    if (D.rule <= DR_IDX_OUT) {
        // the other stack. B's stack is the prefill; any stack can be chosen.
        const App::SeqInfo* osel = seqInfo(D.otherSeqId);
        if (!osel || D.otherSeqId == D.srcSeqId) {
            D.otherSeqId = 0;
            for (const auto& s : app.seqs)
                if (s.id != D.srcSeqId) { D.otherSeqId = s.id; break; }
            osel = seqInfo(D.otherSeqId);
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        if (ImGui::BeginCombo("other stack", osel ? osel->name.c_str() : "-")) {
            for (const auto& s : app.seqs) {
                if (s.id == D.srcSeqId) continue;
                char lb[360];
                snprintf(lb, sizeof lb, "%s###drv%d", s.name.c_str(), s.id);
                if (ImGui::Selectable(lb, s.id == D.otherSeqId)) D.otherSeqId = s.id;
            }
            ImGui::EndCombo();
        }
        if (const App::SeqInfo* o2 = seqInfo(D.otherSeqId)) {
            std::vector<DeriveEntry> oms = stackMembers(D.otherSeqId);
            int ores = 0;
            for (const auto& e : oms) if (e.imageIdx >= 0) ores++;
            int oexp = std::max(o2->expectedFrames, (int)oms.size());
            if (ores < oexp)
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                                   "other stack: %d of %d frame(s) loaded", ores, oexp);
        }
    } else if (D.rule == DR_RANGE) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputInt("from##drlo", &D.lo);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputInt("to (frame numbers, inclusive)##drhi", &D.hi);
    }

    DerivePlan P = buildDerivePlan(D.srcSeqId, D.rule, D.otherSeqId,
                                   D.lo, D.hi, &D.pick);
    ImGui::Separator();
    // counts, in BOTH directions - one-directional counts read as "aligned"
    if (D.rule <= DR_IDX_OUT) {
        const App::SeqInfo* o2 = seqInfo(D.otherSeqId);
        ImGui::Text("%d of %d frame(s) here have a counterpart over there",
                    P.pairs, P.srcKnown);
        ImGui::Text("%d of the other stack's %d have no counterpart here",
                    P.otherUnmatched, P.otherKnown);
        if (D.rule == DR_NAME_IN && o2)
            ImGui::TextDisabled("matched frames take \"%s\"'s frame numbers, so\n"
                                "A/B follow-frame pairs the same file on both sides",
                                o2->name.c_str());
    }
    if (P.notResident > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f),
                           "%d matched frame(s) are not loaded: they are left out "
                           "of the derive, never re-fetched", P.notResident);
    ImGui::Text("will reference %d frame(s)%s (no pixel copy); the source stack is not touched",
                (int)P.picks.size(), D.rule == DR_PICK ? " (ticked below)" : "");

    // the member list: in PICK mode the first column is live, otherwise it
    // echoes what the rule decided - what will be referenced is on screen
    // before it happens, never only after
    float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() * 3.4f;
    if (listH < ImGui::GetFrameHeight() * 4) listH = ImGui::GetFrameHeight() * 4;
    if (ImGui::BeginTable("derivelist", 5,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("take", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("frame #", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("-> #", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("pixels", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)ms.size(); i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (D.rule == DR_PICK) {
                bool v = D.pick[i] != 0;
                if (ImGui::Checkbox("##p", &v)) D.pick[i] = v ? 1 : 0;
            } else {
                bool v = P.taken[i] != 0;
                ImGui::BeginDisabled(true);
                ImGui::Checkbox("##p", &v);
                ImGui::EndDisabled();
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ms[i].base.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", ms[i].seqIndex);
            ImGui::TableNextColumn();
            if (P.taken[i] && ms[i].imageIdx >= 0) ImGui::Text("%d", P.newIndexOf[i]);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (ms[i].imageIdx >= 0) ImGui::TextDisabled("loaded");
            else ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "not loaded");
        }
        ImGui::EndTable();
    }
    if (D.rule == DR_PICK) {
        if (ImGui::SmallButton("all")) std::fill(D.pick.begin(), D.pick.end(), (char)1);
        ImGui::SameLine();
        if (ImGui::SmallButton("none")) std::fill(D.pick.begin(), D.pick.end(), (char)0);
    }
    ImGui::Checkbox("derive into a NEW batch", &D.intoNewBatch);
    ImGui::SameLine();
    ImGui::TextDisabled("(off: same batch as the source)");

    bool can = !P.picks.empty();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Create stack", ImVec2(140 * app.uiScale, 0))) {
        DerivePlan Pf = P;
        int id = applyDerivePlan(Pf, D.intoNewBatch);
        if (id) {
            char m[256];
            if (Pf.notResident > 0)
                snprintf(m, sizeof m, "derived stack: %d frame(s) referenced; %d matched "
                         "but not loaded were left out; source unchanged",
                         (int)Pf.picks.size(), Pf.notResident);
            else
                snprintf(m, sizeof m, "derived stack: %d frame(s) referenced; "
                         "source unchanged", (int)Pf.picks.size());
            toast(m, Pf.notResident > 0);
        }
        D.open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110 * app.uiScale, 0))) {
        D.open = false;
        ImGui::CloseCurrentPopup();
    }
    if (!can) {
        ImGui::SameLine();
        ImGui::TextDisabled("nothing to derive: the rule selects no loaded frame");
    }
    ImGui::EndPopup();
}

