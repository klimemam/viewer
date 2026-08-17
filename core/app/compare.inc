// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView/compare
// 先頭の cur() はモデル層の 1 行だが AB/compare 帯の先頭に居る。住所のまま移動(§1)。
static ImageDoc* cur() { return app.current >= 0 && app.current < (int)app.images.size() ? app.images[app.current].get() : nullptr; }

static void promotePreview(ImageDoc* d);   // fwd: preview -> registered open

// Set by resolveB when follow-frame is on, both sides are stacks, and B has no
// frame carrying A's number - B is then the LAST latched frame, which is not
// A's partner any more. The A/B surfaces render it with the [stale] vocabulary
// they already have; silently returning a mismatched pair is the part that had
// to go. (docs/terminology.md: stack-to-stack comparison holds the frame
// NUMBER, and abDocLabel names the STACK, so nothing on screen said otherwise.)
static bool g_abFollowDiverged = false;

// ONE follow-frame rule for every letter past A. The pin (a member uid) names
// the STACK; when both sides are stacks, follow answers the sibling carrying
// A's frame number. A single-image side (a dark frame, a golden sample) has no
// frame axis and stays put; so does a pin into A's OWN stack - that is how one
// frame is compared against another of the same capture. A stack with no frame
// at A's number hands back the pinned frame with `diverged` set: the caller
// must SAY it (the [stale] vocabulary), never show it as a matched pair.
static ImageDoc* followFrame(ImageDoc* d, bool& diverged) {
    diverged = false;
    ImageDoc* a = cur();
    if (!d || !a || !app.compareFollowFrame) return d;
    if (d->seqId == 0 || a->seqId == 0 || d->seqId == a->seqId ||
        d->seqIndex == a->seqIndex)
        return d;
    for (const auto& q : app.images)
        if (q->seqId == d->seqId && q->seqIndex == a->seqIndex) return q.get();
    // The stack is shorter, or its remote frames have not arrived. Falling
    // through SILENTLY here is what used to hand the difference image, the
    // statistics and the wipe a pair that was no longer a pair.
    diverged = true;
    return d;
}

// The B side of an A/B compare, or null when compare is off / B is gone.
//
// B RESOLVING TO THE SAME DOCUMENT AS A IS NOT A SPECIAL CASE. This used to
// answer null there ("paused"), and every surface downstream - the histogram,
// the projection and temporal panels, the Inspector's B column, the canvas -
// then rendered as if there were no comparison at all. The kindness was
// counterproductive (user report, 2026-08-04: 「A/B比較で，A=Bの時，ヒストグラム
// の表示を切り替えているけど，このケアはかえってみずらい．比較の際に同じもの
// を選んだ時に，同じことを確認できる方がよいので，こういうの全般不要です」):
// putting the same document on both sides is something people do ON PURPOSE,
// to confirm the two sides agree, and a panel that hides B at that moment makes
// the confirmation impossible - "B is identical to A" and "B is not being
// drawn" look the same. So B is B, and two coinciding sides are DRAWN
// coinciding: two curves in their own slotInk, two rows, two labels.
static ImageDoc* resolveB() {
    g_abFollowDiverged = false;
    if (app.compareBUid) {
        ImageDoc* b = nullptr;
        for (const auto& d : app.images)
            if (d->uid == app.compareBUid) { b = d.get(); break; }
        if (!b) { app.compareBUid = 0; return nullptr; }      // B was closed
        return followFrame(b, g_abFollowDiverged);
    }
    if (app.compareB.empty()) return nullptr;
    // session fallback: match the saved name (and frame, for a stack), then latch
    // the uid so navigation cannot re-point B at a different frame
    for (const auto& d : app.images) {
        if (d->name != app.compareB) continue;
        if (app.compareBSeq >= 0 && d->seqId != 0 && d->seqIndex != app.compareBSeq) continue;
        app.compareBUid = d->uid;
        return d.get();
    }
    return nullptr;
}
static ImageDoc* cmpB() { return app.compareMode == App::CmpOff ? nullptr : resolveB(); }

// fwd: A and B are named by their STACK wherever the two are set against each
// other - two stacks of a series hold identically named frames
static std::string abDocLabel(const ImageDoc* d);
// fwd: the extra compare slots (defined with compareExtras below)
static std::string slotOf(const ImageDoc* d);
static void removeCompareSlot(const ImageDoc* d);
static void toast(const std::string& msg, bool err);

// remember B as an identity, not as a name
static void setCompareB(const ImageDoc* d) {
    // making a preview the B side IS using it: promote before it can vanish
    if (d && d->preview) promotePreview(const_cast<ImageDoc*>(d));
    // One image, one letter: choosing a lettered image as B is a PROMOTION -
    // the slot is released, or the same pixels would hold two panes and two
    // table rows. (The default-B paths in ensureCompareB never reach here for
    // a lettered image; an explicit choice outranks the letter.)
    if (d) {
        std::string held = slotOf(d);
        if (!held.empty()) {
            removeCompareSlot(d);
            toast("slot " + held + " -> B  (" + abDocLabel(d) + ")", false);
        }
    }
    // Remember the last B that was CHOSEN, and keep remembering it after
    // compare is switched off. Turning compare back on should return to the
    // pair you were working with, not to whatever happens to be adjacent.
    if (d) app.lastCompareBUid = d->uid;
    app.compareBUid = d ? d->uid : 0;
    app.compareB = d ? d->name : std::string();
    app.compareBSeq = d && d->seqId != 0 ? d->seqIndex : -1;
}

// The uid holding the B SEAT: the live pin, or the remembered one after an
// exit path cleared it (A9: compare off and on returns to the same B). The
// Files letters are seat assignments, so they key on this - never on cmpB(),
// which answers null while compare is off, and a letter that vanishes there
// reads as the assignment being lost.
static uint64_t bSeatUid() {
    return app.compareBUid ? app.compareBUid : app.lastCompareBUid;
}

// The B side FOR THE STATISTICS PANELS: the compare partner, but only when it
// has pixels to measure - a remote preview placeholder has none.
static ImageDoc* abStatsB() {
    ImageDoc* b = cmpB();
    if (!b || b->w < 1 || b->h < 1 || b->px().empty()) return nullptr;
    return b;
}

// Side by side, or overlaid? One answer for every panel (App::abStatsLayout).
//
// Auto mirrors the IMAGE, and the ruling on #60 (2026-08-14) is that it keeps
// doing so at any N rather than splitting by panel kind: CmpSplit TILES A, B
// and every armed slot, so the plots tile the same way and there are as many
// panes as there are tiles; wipe / blink / difference collapse to ONE image
// area, so the plots overlay onto one axis. What made the histogram stop at
// two was never this function - it was the panels' side-by-side path having
// exactly two halves nailed into it.
static bool abSideBySide() {
    if (app.abStatsLayout == App::AbOverlay) return false;
    if (app.abStatsLayout == App::AbSide) return true;
    return app.compareMode == App::CmpSplit;
}

// WHICH side the blink is showing this instant, as a side index: 0 = A,
// 1 = B, 2 + i = the slot at cmpExtra[i]. -1 when nothing is blinking.
//
// `app.flipShowB` is the STATE and stays where it is - the canvas owns it, and
// the timer that flips it. This is the question a READER asks of it, given a
// name once so that a panel following the blink cannot answer it slightly
// differently: the ROI table prints the numbers of whichever frame the canvas
// is painting (the ruling on #76), and the two disagreeing for even one frame
// is a table labelled with the wrong letter.
//
// It also gives N-blink one line to land in. `flipShowB` is a two-image
// spelling - a bool cannot name C - so when the cycle generalises
// (docs/compare-n.md 12, "巡回順は A -> B -> C -> ...") the position is computed
// HERE, and every reader follows it without being touched.
static int blinkSideIndex() {
    if (app.compareMode != App::CmpFlip) return -1;
    return app.flipShowB ? 1 : 0;
}

// ...and which side an OVERLAID plot has to make stand out, as the same side
// index. The other half of the 2026-08-14 ruling on #60: collapsed modes put
// every curve on one axis, and "whatever the image is showing right now must
// be the lead on the numeric side too" is then not said by the layout - it has
// to be said by the drawing. Weight says it (a heavier stroke), because hue is
// already spent naming the letter and cannot be borrowed.
//
// This is the chart-side twin of the ROI table following the blink (#76): the
// table shows ONE side because rows cannot be superimposed, the chart shows
// ALL of them because curves can - and both point at the same letter.
//
// -1 = nothing to emphasise, and wipe and difference are that case on purpose:
// they put both sides on screen at the same instant, so no side is "the one
// you are looking at" and inventing a lead there would be a claim the image
// does not make.
static int abEmphSide() { return blinkSideIndex(); }

// The stroke a curve takes on a SHARED axis, and the one the emphasised side
// takes. Twice over is the smallest ratio that survives a dense histogram and
// the profile decimation: at 1.4 vs 1.8 px the difference reads as antialiasing.
static float abCurveW(float base, bool emph) { return emph ? base * 2.2f : base; }

// Can A's and B's profiles be drawn on ONE axis? Only when both sampled the
// same range - same origin and same length. Anything else would have to be
// stretched to line up, and stretching invents a correspondence between two
// captures that nothing in the data supports. False means: side by side, and
// say why (docs/ab-stats-plan.md 5).
static bool abProjOverlayable(const App::ProjState& A, const App::ProjState& B) {
    return A.rx == B.rx && A.rw == B.rw && A.ry == B.ry && A.rh == B.rh;
}

// --no-ab-throttle: developer flag, so the cost the throttle removes can be
// measured on the same binary instead of quoted from an older one.
static bool g_abNoThrottle = false;
// True while frames are being stepped continuously. The B slots skip their
// recompute then; whoever draws B must say "stale" (docs/ab-stats-plan.md 1).
static bool abStepBusy() {
    return !g_abNoThrottle && app.abStepBusyUntil > nowSec();
}

// Slot 1 of every statistics cache belongs to the B side and is filled only
// while a B exists. Called once per frame BEFORE the panels draw: the first
// frame after compare goes off clears it, and every frame after that does
// nothing at all - so compare-off costs exactly what it did before A/B stats.
static void abStatsFrame() {
    if (cmpB()) { app.abSlot1Live = true; return; }
    if (!app.abSlot1Live) return;
    app.hist[1] = App::HistState{};
    app.proj[1] = App::ProjState{};
    app.temporal[1] = App::TemporalState{};
    app.abSlot1Live = false;
}

// Put a line in the Messages panel WITHOUT showing a toast. Everything that
// writes to msgLog goes through here: the cap and the de-duplication are the
// log's rules, and three call sites that pushed straight onto the vector were
// keeping neither.
static void logMsg(const std::string& msg, bool err = false) {
    if (!app.msgLog.empty() && app.msgLog.back().text == msg) return;
    app.msgLog.push_back({ msg, err });
    if (app.msgLog.size() > 300) app.msgLog.erase(app.msgLog.begin());
    app.msgSeq++;
    if (err) app.msgUnreadErr = true;
}

static void toast(const std::string& msg, bool err = false) {
    app.toast = msg; app.toastErr = err;
    // ImGui::GetTime() dereferences the current context, and there are code
    // paths with NO context: the windowless selftests (--remote-selftest and
    // the other NOGL runs) drive real open/close machinery with no ImGui at
    // all, and every one of them was one toast away from a segfault - which is
    // what a selftest exercising openRemote found. The MESSAGE still lands in
    // the log below, so a headless run loses only the fading overlay it has no
    // window to fade in.
    app.toastUntil = ImGui::GetCurrentContext() ? ImGui::GetTime() + (err ? 6.0 : 2.5)
                                                : 0.0;
    // A toast fades after six seconds, which is not long enough to paste it into
    // a bug report. Every message is kept here and shown, selectable, in the
    // Messages panel.
    logMsg(msg, err);
}

// One batch per OPEN ACTION: reopening a folder deliberately makes a fresh
// one - "calling the same folder twice" must give two groupings you can name
// apart, or the second load is invisible.
static std::string uniqueBatchName(std::string base);   // fwd
static int newBatch(const std::string& name, const std::string& srcDir = std::string()) {
    app.batches.push_back({ app.nextBatchId, name.empty() ? "opened" : name, srcDir });
    app.imagesRev++;                      // the Files grouping caches on this
    return app.nextBatchId++;
}
// Loose single-file opens share a batch instead of making one each - the right
// behaviour for "clicked three files in a row". The identity is the source
// DIRECTORY, not its leaf name: two directories that happen to end in the same
// component are different measurements. `dir` empty = match by name, which is
// what session restore does (the session persists batches BY NAME, by design).
static int batchReuse(const std::string& name, const std::string& dir = std::string()) {
    if (!dir.empty()) {
        for (const auto& b : app.batches) if (b.srcDir == dir) return b.id;
        return newBatch(uniqueBatchName(name), dir);
    }
    for (const auto& b : app.batches) if (b.name == name) return b.id;
    return newBatch(name);
}
// Batch names must be UNIQUE (docs/terminology.md): sessions restore batches BY
// NAME (imgbatch -> batchReuse), so two batches sharing one name silently merge
// on the next load. Collisions get " (2)", " (3)", ...
static std::string uniqueBatchName(std::string base) {
    if (base.empty()) base = "opened";
    auto taken = [](const std::string& n) {
        for (const auto& b : app.batches) if (b.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int k = 2;; k++) {
        std::string n = base + " (" + std::to_string(k) + ")";
        if (!taken(n)) return n;
    }
}
// The Files header's rename, as a function so it can be tested and so it goes
// through the uniquifier every creation path already uses. It was the ONE
// name-write that did not, and the canon says why that matters: a session
// restores batches by name, so two live batches sharing one MERGE on the next
// load. That used to be cosmetic. With series persisted it is not: two copies
// of the same folder (which the canon blesses) inside the merged batch defeat
// preferBatch, so the second restored series resolves onto the stacks the first
// already holds, takes them, and pruneEmptySeries deletes the emptied first one
// - while the toast still counts it as restored, because `made` was incremented
// before the theft. The next autosave makes it permanent.
static void renameBatch(int batchId, const std::string& want) {
    for (auto& b : app.batches) {
        if (b.id != batchId) continue;
        if (want.empty() || want == b.name) return;
        std::string uniq = uniqueBatchName(want);   // skips names nobody holds
        if (uniq != want)
            toast("batch renamed to \"" + uniq + "\": \"" + want + "\" is taken, and a "
                  "session restores batches by name");
        b.name = uniq;
        app.imagesRev++;                  // the Files grouping caches on this
        return;
    }
}

static void selectImage(int idx);   // fwd (defined with the sequence helpers)

// The extra slots, resolved and pruned in one place. A slot whose image was
// closed simply stops existing - the same rule B follows - and A can never
// occupy one, or a column would compare a thing with itself. The slots resolve
// EXACTLY as B does (resolveB): the uid pins the stack, follow-frame picks the
// sibling carrying A's number, and a stack with no such frame is `diverged`.
// `idx` is the position in cmpExtra - the LETTER - and it travels with the
// resolved document, because after a follow the document is no longer the
// pinned uid and a uid lookup would lose the letter.
struct ResolvedSlot { ImageDoc* doc; size_t idx; bool diverged; };
static std::vector<uint8_t> g_slotDiverged;   // by cmpExtra index, last resolve
// A slot standing on the same document as A resolves like any other: the same
// rule resolveB now follows, for the same reason. Dropping it here made a
// letter's row, curve and pane vanish the moment A walked onto it, which is
// indistinguishable from the slot having been lost.
static std::vector<ResolvedSlot> resolveSlots() {
    std::vector<ResolvedSlot> out;
    g_slotDiverged.assign(app.cmpExtra.size(), 0);
    for (size_t i = 0; i < app.cmpExtra.size();) {
        ImageDoc* d = nullptr;
        for (const auto& q : app.images) if (q->uid == app.cmpExtra[i]) { d = q.get(); break; }
        if (!d) {   // closed
            app.cmpExtra.erase(app.cmpExtra.begin() + i);
            g_slotDiverged.erase(g_slotDiverged.begin() + i);
            continue;
        }
        bool div = false;
        ImageDoc* r = followFrame(d, div);
        g_slotDiverged[i] = div ? 1 : 0;
        out.push_back({ r, i, div });
        i++;
    }
    return out;
}
static std::vector<ImageDoc*> compareExtras() {
    std::vector<ImageDoc*> out;
    for (const ResolvedSlot& rs : resolveSlots()) out.push_back(rs.doc);
    return out;
}
static const char* SLOT_LETTERS = "CDEFGHIJKLMNOP";
static std::string slotName(size_t i) {
    return std::string(1, i < strlen(SLOT_LETTERS) ? SLOT_LETTERS[i] : '?');
}
// Which letter this image already holds, or "" - so a row can say "remove"
// instead of silently adding the same image twice.
static std::string slotOf(const ImageDoc* d) {
    if (!d) return "";
    for (size_t i = 0; i < app.cmpExtra.size(); i++)
        if (app.cmpExtra[i] == d->uid) return slotName(i);
    return "";
}
static void addCompareSlot(ImageDoc* d) {
    if (!d) return;
    // A is the CURSOR, not a seat, so the image under it may hold a letter like
    // any other - that is how you park the frame you are on as C and go looking
    // for what to set against it. (It used to be refused outright, which is the
    // same "you cannot compare a thing with itself" care resolveB carried.)
    if (d->uid == app.compareBUid) {   // one image, one letter: it is B already
        toast("that image is B - wipe / split / difference compare it already", true);
        return;
    }
    for (uint64_t u : app.cmpExtra) if (u == d->uid) return;
    if (app.cmpExtra.size() >= strlen(SLOT_LETTERS)) {
        toast("no free compare slot", true);
        return;
    }
    app.cmpExtra.push_back(d->uid);
    toast("slot " + slotName(app.cmpExtra.size() - 1) + " = " + abDocLabel(d));
}
static void removeCompareSlot(const ImageDoc* d) {
    if (!d) return;
    for (size_t i = 0; i < app.cmpExtra.size(); i++)
        if (app.cmpExtra[i] == d->uid) { app.cmpExtra.erase(app.cmpExtra.begin() + i); return; }
}

// What the slot item on a Files row OFFERS for this document - and `letter` is
// the letter the offer would name ("Add as compare slot D" / "Remove from
// compare slot C"). Out here rather than in the menu for the same reason
// abRowItem is: so it can be checked without a frame (--abstats-selftest, N5).
// Issue #60 established what that costs when it is skipped: every panel could
// HANDLE letters past B, the selftests proved it - by arming slots with a
// direct call - and whether the one path a user has to arm them still offered
// anything was checked by nobody, over an equation that holds on the empty set.
enum SlotRowItem {
    SlotRowNone,       // no document, or the pinned B: its row's B items say so,
                       // and a slot on top would put one image in two panes
    SlotRowAdd,        // "Add as compare slot <letter>" - the next free letter
    SlotRowRemove,     // "Remove from compare slot <letter>" - the letter held
};
static SlotRowItem slotRowItem(const ImageDoc* pick, std::string& letter) {
    letter.clear();
    if (!pick) return SlotRowNone;
    letter = slotOf(pick);
    if (!letter.empty()) return SlotRowRemove;
    if (pick->uid == app.compareBUid) return SlotRowNone;
    letter = slotName(app.cmpExtra.size());
    return SlotRowAdd;
}

// Did slot i's follow-frame land on nothing the last time the slots resolved?
// The per-slot g_abFollowDiverged (--abstats-selftest S3).
static bool slotFollowDiverged(size_t i) {
    return i < g_slotDiverged.size() && g_slotDiverged[i];
}

// A session names its slots by path (+ frame, for a stack member); the images
// arrive later and asynchronously, so this is retried until it lands or the
// load settles. Same shape as the B fallback below it.
static void pumpCompareSlotRestore() {
    if (app.cmpSlotRestore.empty()) return;
    for (size_t i = 0; i < app.cmpSlotRestore.size();) {
        const auto& w = app.cmpSlotRestore[i];
        ImageDoc* hit = nullptr;
        for (const auto& d : app.images) {
            if (d->src->path != w.path) continue;
            // (path, member): two image members of one .npz are one path, and a
            // slot that latched the first of them pinned the wrong picture -
            // and took the second want down with it on the dup check. An empty
            // want-member is a session written before the key: unconstrained,
            // resolving exactly as it did then.
            if (!w.member.empty() && d->src->member != w.member) continue;
            if (w.frame >= 0 && d->seqId != 0 && d->seqIndex != w.frame) continue;
            hit = d.get(); break;
        }
        if (!hit) { i++; continue; }
        bool dup = false;
        for (uint64_t u : app.cmpExtra) if (u == hit->uid) dup = true;
        if (!dup) app.cmpExtra.push_back(hit->uid);
        app.cmpSlotRestore.erase(app.cmpSlotRestore.begin() + i);
    }
}

// Default B = the image next to A in the list: with two images open, "compare"
// should just work without first picking a partner.
static void ensureCompareB() {
    if (resolveB()) return;       // mode-independent: called before the mode changes
    setCompareB(nullptr);
    if (app.images.empty()) return;
    // A DEFAULT B never lands on a lettered image: wipe / difference / blink
    // are strictly A against B, and a B that doubled as C both tiled the same
    // pixels twice and read as an "A,C compare" (defect, 2026-07-30). An
    // explicit choice still may - setCompareB then releases the letter.
    auto free = [](const ImageDoc* d) { return d != cur() && slotOf(d).empty(); };
    // 1. the B you last chose, if it is still open. Coming back to a comparison
    //    should come back to the SAME comparison.
    if (app.lastCompareBUid)
        for (const auto& d : app.images)
            if (d->uid == app.lastCompareBUid && free(d.get())) { setCompareB(d.get()); return; }
    // 2. otherwise the image you were just looking at
    // (after Process > demosaic that is the source, not images[0])
    if (app.prevImageUid)
        for (const auto& d : app.images)
            if (d->uid == app.prevImageUid && free(d.get())) { setCompareB(d.get()); return; }
    for (int i = 0; i < (int)app.images.size(); i++) {
        int j = (app.current + 1 + i) % (int)app.images.size();
        if (free(app.images[j].get())) { setCompareB(app.images[j].get()); break; }
    }
}

// Shift+C: bring up the comparison BEYOND B. The slots already show in the
// statistics tables; what this does is put the images on screen next to each
// other, which is the one image layout that means more than two. Wipe,
// difference and blink are left alone - they mean exactly two, and cycling
// into them with four slots open would quietly show two of them.
static void showCompareSlots() {
    if (app.cmpExtra.empty()) {
        toast("no slots past B yet - Files: right-click a row, \"Add as compare slot C\"", true);
        return;
    }
    if (app.compareMode == App::CmpOff) ensureCompareB();
    app.compareMode = App::CmpSplit;      // the side-by-side family
    toast("side by side: A, B and " + std::to_string(app.cmpExtra.size()) + " more");
}

static void cycleCompare() {
    if (app.images.size() < 2) { toast("compare needs a second open image", true); return; }
    ensureCompareB();
    app.compareMode = (app.compareMode + 1) % 5;
    toast(app.compareMode == App::CmpOff   ? "compare off"
        : app.compareMode == App::CmpWipe  ? "compare: wipe  (drag the divider)"
        : app.compareMode == App::CmpSplit ? "compare: side by side"
        : app.compareMode == App::CmpDiff  ? "compare: difference A-B"
                                           : "compare: blink  (B or Space toggles, "
                                             "'auto blink' in Inspector)");
}

// ESC, one step OUTWARD per press, innermost claim first (user report,
// 2026-07-30: 「抜ける方法が自明ではない．ESCボタンでぬける，でよいのは？」):
//   1. an open popup / menu takes it (the ClosePopupsExceptModals pass)
//   2. an active text item takes it (reverts the edit)
//   3. a selected annotation / ROI is deselected, and ONLY that
//   4. an active comparison is switched off, and ONLY that
// Steps 1 and 2 are the gates on the shortcut block that calls this
// (!popupOpen, !io.WantTextInput) - by the time it runs, ESC belongs to the
// canvas state. Steps 3 and 4 live HERE, as one dispatch, so a single press
// can never do two of them at once; the return value is what the press
// consumed, which is how --abstats-selftest proves that ordering.
enum class EscTook { Nothing, RoiDeselected, CompareOff };
// Which layer ACTUALLY took each Escape, one name per press in the order the
// presses arrived: "popup;textedit;roi;compare;nothing;". The four steps above
// are decided in four different places - two of them inside ImGui, where this
// app's state never hears about them - so "one press, one step outward" was
// only ever checkable for the two that end in escapePressed(). A14/A14b assert
// the ROI/compare pair and, separately, that a popup went away; neither can
// say that ONE press did not do two of them, which is the whole rule.
// (docs/verify-ui.md E7.) Recorded at the point each layer acts, so a press
// consumed twice appears as two entries rather than being reasoned away.
static std::string g_escProbe;
static int g_escProbeN = 0;              // entries, so a caller can take a delta
static int g_escProbeFrame = -1;         // the frame the last entry was made in
static void escProbeNote(const char* layer) {
    g_escProbe += std::string(layer) + ";";
    g_escProbeN++;
    if (ImGui::GetCurrentContext()) g_escProbeFrame = ImGui::GetFrameCount();
}
static EscTook escapePressed() {
    if (app.selectedAnn != 0) {          // back to All - the old ESC meaning
        app.selectedAnn = 0;
        escProbeNote("roi");
        return EscTook::RoiDeselected;
    }
    if (app.compareMode != App::CmpOff) {
        // OUT of the comparison, keeping the seats: the B pin survives
        // (compareBUid / lastCompareBUid) and the slot letters stay armed
        // (cmpExtra) - settled A9 behavior, compare off and on returns to
        // the same pair. The toast says so, or leaving reads as losing the
        // setup; the Files letters stay visible too, dimmed (see the badge
        // rule in drawFileList).
        app.compareMode = App::CmpOff;
        toast("comparison off - B/C assignments kept (C to resume)");
        escProbeNote("compare");
        return EscTook::CompareOff;
    }
    escProbeNote("nothing");
    return EscTook::Nothing;
}

// Pin the frame you are looking at as B, then walk A somewhere else: this is how
// you compare frame 12 against frame 13 of one stack, or a source against its
// processed result. Staying put is a comparison too - A and B on one document,
// both sides drawn coinciding - so the toast invites the walk rather than
// instructing it ("move A somewhere else" read as a precondition, back when
// the pair really did stay silent until you did).
static void pinCurrentAsB() {
    if (!cur()) return;
    setCompareB(cur());
    if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
    toast("B = " + abDocLabel(cur()) + "  (A and B agree - move A to compare)");
}

static void swapCompare() {
    ImageDoc* b = cmpB();
    if (!cur()) return;
    if (!b) {   // silence here reads as "the feature does not exist"
        toast(app.compareMode == App::CmpOff ? "compare is off  (\\ or C turns it on)"
                                             : "no B image  -  View > Compare A/B > B image", true);
        return;
    }
    const ImageDoc* a = cur();
    // Both sides on one document is a legal comparison now, and swapping it
    // moves nothing. Saying "swapped" over an unchanged screen is how a no-op
    // becomes a bug report, so say what actually happened instead.
    if (b == a) {
        toast("A and B are the same image - nothing to swap  (" + abDocLabel(a) + ")");
        return;
    }
    std::string an = abDocLabel(a), bn = abDocLabel(b);
    uint64_t bUid = b->uid;
    setCompareB(a);                       // set B before moving A: cur() changes
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->uid == bUid) { selectImage(i); break; }
    toast("A / B swapped:  A = " + bn + "   B = " + an);
}

// The status-bar compare chip, as one sentence --abstats-selftest can read.
// Returns "" while compare is off - the bar then says nothing about comparing.
static std::string abStatusChipText() {
    if (app.compareMode == App::CmpOff) return "";
    ImageDoc* b = cmpB();
    char buf[320];
    // One silence left, and it means one thing: there is no B. The second
    // sentence here used to be "A = B (paused)" - the chip explaining why the
    // panels had gone quiet while A stood on the pinned B. Nothing goes quiet
    // any more (resolveB), so the chip names the pair like any other.
    if (!b) return "A/B: no B image";
    // §3.1: A and B mapping the SAME source is a true comparison whose
    // difference is all zero - the chip says why. A slot sharing A's source
    // is said the same way. (When A IS B - allowed now - the sentence is
    // still true, and stops pretending otherwise.)
    std::string share;
    if (const ImageDoc* a = cur()) {
        // EVERY side sharing A's pixels, by the letters the panes use: B and
        // the slots, follow-frame-resolved exactly as the panes are
        // (resolveSlots IS that resolution). Testing the pinned uid instead
        // claimed or withheld the share against a frame nobody was comparing -
        // and stopping at the first match hid every sharer past it.
        std::string letters;
        if (a->src == b->src) letters = "B";
        for (const ResolvedSlot& rs : resolveSlots())
            if (rs.doc->src == a->src)
                letters += (letters.empty() ? "" : ", ") + slotName(rs.idx);
        if (!letters.empty())
            share = "  -  A and " + letters + " share the same pixels";
    }
    if (app.compareMode == App::CmpDiff) {
        snprintf(buf, sizeof buf, "%s +-%g  B: %s", app.diffAbs ? "|A-B|" : "A-B",
                 app.diff.gain, abDocLabel(b).c_str());
        return buf + share;
    }
    float fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
    snprintf(buf, sizeof buf, "A/B %s %.0f%%  B: %s",
             app.compareMode == App::CmpSplit ? "split" : "wipe", fr * 100,
             abDocLabel(b).c_str());
    return buf + share;
}

// What the A/B item on a Files row offers, for the frame or stack that row
// would hand to B.
//
// EVERY row offers it, the one you are looking at included. That row used to
// offer nothing at all, on the reasoning that it IS A and a thing cannot be
// compared with itself - which is the very care the user threw out (2026-08-04,
// 「比較の際に同じものを選んだ時に，同じことを確認できる方がよい」). Setting
// this row as B is now a legitimate and useful move: both sides show the same
// document, the panels draw two curves and two rows on top of each other, and
// that coincidence is the confirmation you were after.
//
// (It also used to offer "Swap A and B" here, and THAT line really was care
// nobody needed - user report 2026-07-30: 「swapはショートカットキーで対応され
// ているから右クリックメニューでのケアは不要では」. The swap lives on Shift+\ ,
// the View menu and the status-bar button; it is not coming back.)
//
// Out here rather than in the menu so it can be checked without a frame
// (--abstats-selftest, A7).
enum AbRowItem {
    AbSetB,        // this row can become B - and every row can
    AbNone         // reserved: nothing on this row (no image at all)
};
static AbRowItem abRowItem(const ImageDoc* pick) {
    return pick ? AbSetB : AbNone;
}

static void computeMinMax(ImageDoc& im) {
    float mn = FLT_MAX, mx = -FLT_MAX;
    for (float v : im.px()) {
        if (std::isfinite(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
    }
    if (mn > mx) { mn = 0; mx = 1; }
    if (mn == mx) mx = mn + 1;
    im.src->vmin = mn; im.src->vmax = mx;   // the source's truth...
    im.vmin = mn; im.vmax = mx;             // ...and the doc's mirror of it
}
// Effective display range: the shared value when linking is on, the image's own
// otherwise. Linking never overwrites an image's range (defined below with App).
static float effBlack(const ImageDoc& im);
static float effWhite(const ImageDoc& im);

static void defaultRange(ImageDoc& im) {
    if (im.dtype == "u8" || im.dtype == "i8")       { im.black = 0; im.white = 255; }
    else if (im.dtype == "u16")                     { im.black = 0; im.white = 65535; }
    else if (im.dtype == "i16")                     { im.black = 0; im.white = 32767; }
    else if (im.vmin >= -0.001f && im.vmax <= 1.2f && im.vmax > 0.005f) { im.black = 0; im.white = 1; }
    else                                            { im.black = im.vmin; im.white = im.vmax; }
}

// Keep only the most recent textures resident: a 200-frame sequence would
// otherwise pin gigabytes of VRAM. Ordered by USE, not by creation - with A/B
// compare, B is often an old doc that is needed every frame, and an insertion-
// ordered list would evict and re-upload it constantly.
static void touchTex(ImageDoc& im) {
    auto it = std::find(app.texLru.begin(), app.texLru.end(), &im);
    if (it != app.texLru.end()) app.texLru.erase(it);
    app.texLru.push_back(&im);
    const size_t TEX_KEEP = 12;
    for (size_t i = 0; i < app.texLru.size() && app.texLru.size() > TEX_KEEP; ) {
        ImageDoc* old = app.texLru[i];
        if (old == &im || old == cur() || old == cmpB()) { i++; continue; }  // on screen
        if (old->tex) { glDeleteTextures(1, &old->tex); old->tex = 0; old->texDirty = true; }
        app.texLru.erase(app.texLru.begin() + i);
    }
}

// Round up to 1/2/5 x 10^n: a difference scale of "+-2000 DN" is readable, one of
// "+-1873.4 DN" is not (and the axis rule applies to legends too).
static float niceCeil(float v) {
    if (!(v > 0) || !std::isfinite(v)) return 1.0f;
    float e = powf(10.0f, floorf(log10f(v)));
    float m = v / e;
    return (m <= 1.0f ? 1.0f : m <= 2.0f ? 2.0f : m <= 5.0f ? 5.0f : 10.0f) * e;
}

// A-B as a signed image. Rebuilt only when an input, the scale or the mode
// changes: at 12 Mpx this is a ~60 ms pass, not something to do per frame.
static void ensureDiffTexture(const ImageDoc& a, const ImageDoc& b) {
    App::DiffTex& D = app.diff;
    int w = std::min(a.w, b.w), h = std::min(a.h, b.h);
    int ch = std::min(a.ch, b.ch);
    if (w <= 0 || h <= 0) return;

    if (app.diffGain <= 0) {   // auto scale: the 99.9th percentile of |A-B|
        static uint64_t autoKeyA = 0, autoKeyB = 0; static int autoRevA = -1, autoRevB = -1;
        if (autoKeyA != a.uid || autoKeyB != b.uid || autoRevA != a.dataRev || autoRevB != b.dataRev) {
            autoKeyA = a.uid; autoKeyB = b.uid; autoRevA = a.dataRev; autoRevB = b.dataRev;
            std::vector<float> mags;
            size_t total = (size_t)w * h;
            size_t step = std::max<size_t>(1, total / 200000);   // bounded sample
            // ...and the step is forced ODD, which is the f35a79e defect in its
            // last hiding place. Striding a one-dimensional index and wrapping
            // it (x = p % w) samples only even columns the moment step and w are
            // both even - and a sensor width essentially always is. On an RGGB
            // frame that leaves R and Gb with samples and Gr and B with none, so
            // this percentile - and with it the whole A/B difference display's
            // scaling - would be decided by half the sensor. An odd step walks
            // every column parity, and every row parity with it, because
            // consecutive samples advance x by an odd amount and wrap.
            if (step % 2 == 0) step++;
            mags.reserve(total / step + 1);
            for (size_t p = 0; p < total; p += step) {
                int x = (int)(p % (size_t)w), y = (int)(p / (size_t)w);
                float m = 0;
                for (int c = 0; c < ch; c++)
                    m = std::max(m, fabsf(a.sample(x, y, c) - b.sample(x, y, c)));
                if (std::isfinite(m)) mags.push_back(m);
            }
            float g = 1.0f;
            if (!mags.empty()) {
                size_t k = (size_t)(mags.size() * 0.999);
                if (k >= mags.size()) k = mags.size() - 1;
                std::nth_element(mags.begin(), mags.begin() + k, mags.end());
                g = mags[k];
            }
            app.diffAutoGain = niceCeil(g > 0 ? g : 1.0f);
        }
    }
    float gain = app.diffGain > 0 ? app.diffGain : app.diffAutoGain;

    if (D.tex && D.uidA == a.uid && D.uidB == b.uid && D.revA == a.dataRev &&
        D.revB == b.dataRev && D.gain == gain && D.absMode == app.diffAbs &&
        D.w == w && D.h == h)
        return;                                     // cache hit: nothing changed

    double t0 = nowSec();
    static std::vector<uint8_t> rgba;
    rgba.resize((size_t)w * h * 4);
    const float inv = 1.0f / std::max(gain, 1e-20f);
    size_t clipped = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = 0;
            for (int c = 0; c < ch; c++) {          // the channel that differs most
                float v = a.sample(x, y, c) - b.sample(x, y, c);
                if (fabsf(v) > fabsf(d)) d = v;
            }
            if (!std::isfinite(d)) d = 0;
            float t = d * inv;
            size_t o = ((size_t)y * w + x) * 4;
            float r, g2, bl;
            if (fabsf(t) > 1.0f) {                  // out of scale: unmistakable
                clipped++;
                r = 1.0f; g2 = 1.0f; bl = 0.2f;     // yellow
            } else if (app.diffAbs) {
                float m = fabsf(t);
                r = m; g2 = m; bl = m;
            } else if (t >= 0) {                    // A brighter: warm
                r = t; g2 = 0.35f * t; bl = 0.10f * t;
            } else {                                // B brighter: cool
                float m = -t;
                r = 0.10f * m; g2 = 0.45f * m; bl = m;
            }
            rgba[o + 0] = (uint8_t)(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 1] = (uint8_t)(std::clamp(g2, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 2] = (uint8_t)(std::clamp(bl, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[o + 3] = 255;
        }
    }
    if (!D.tex) glGenTextures(1, &D.tex);
    glBindTexture(GL_TEXTURE_2D, D.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    D.uidA = a.uid; D.uidB = b.uid; D.revA = a.dataRev; D.revB = b.dataRev;
    D.gain = gain; D.absMode = app.diffAbs; D.w = w; D.h = h;
    D.clipped = (double)clipped / ((double)w * h);
    fprintf(stderr, "diff: %s - %s  full scale +-%g  (%.3f%% off scale, %.0f ms)\n",
            a.name.c_str(), b.name.c_str(), gain, D.clipped * 100,
            (nowSec() - t0) * 1000);
}

// ---- value highlight (#68, docs/histogram-select-design.md) ----------------
// The ink, in ONE constant, because §11 left the colour as the single thing the
// user might want to change after seeing it on a real image.
//
// Magenta, opaque replacement. Nothing the display pipe emits FOR A VALUE is
// magenta - not greyscale, not the CFA tints (pure R/G/B), not the jet/turbo
// falsecolor ramps, not the difference view's warm/cool/yellow - and no sensor
// image carries magenta as a scene colour. The theme ACCENT was rejected on
// purpose: it is one of five colours the user picks between, so Aurora Blue
// would collide with the CFA B tint and Coral with the R tint, and a colour
// over the pixels whose meaning depends on a preference is not a colour that
// means anything. And the colour is never the SIGNAL - a user's own display LUT
// could contain magenta, so the fact is carried by the sentences below and the
// paint only points at where.
static const ImU32 HL_INK = IM_COL32(255, 0, 255, 255);
// ...unpacked once: the paint writes bytes, the plot band draws an ImU32, and
// two spellings of one colour is how they come to disagree.
static const uint8_t HL_RGBA[4] = {
    (uint8_t)((HL_INK >> IM_COL32_R_SHIFT) & 0xFF),
    (uint8_t)((HL_INK >> IM_COL32_G_SHIFT) & 0xFF),
    (uint8_t)((HL_INK >> IM_COL32_B_SHIFT) & 0xFF),
    (uint8_t)((HL_INK >> IM_COL32_A_SHIFT) & 0xFF),
};

static int abSeriesMatch(const char* const* names, int n, const char* want);  // fwd
static std::string abValueUnit(const std::string& dtype);                     // fwd: "DN"

// What the panel footer and the canvas badge ACTUALLY said about the highlight
// on the last frame each of them drew - the g_histSideProbe reasoning exactly:
// this machine cannot screenshot GL, and the defect a highlight can ship with
// is SILENCE (paint on the pixels that nothing on screen explains). Keyed, so
// each writer replaces its own entry whatever order the frame draws in, and an
// empty string erases the key: off, the probe is empty.
static std::string g_histHlProbe;
static void histHlProbeSet(const char* key, const std::string& said) {
    const std::string k = std::string(key) + "=";
    size_t at = g_histHlProbe.find(k);
    if (at != std::string::npos) {
        size_t e = g_histHlProbe.find(';', at);
        g_histHlProbe.erase(at, e == std::string::npos ? std::string::npos
                                                       : e - at + 1);
    }
    if (!said.empty()) g_histHlProbe += k + said + ";";
}

// "" = a drag bound to `plane` may arm on this document; otherwise the sentence
// saying why not. One rule: the highlight is DEFINED only where a pixel has
// exactly one value under the plane filter. A mosaiced frame always does (one
// photosite, one number), a single channel always does, and a chosen channel of
// an RGB frame does - but RGB with "all" selected does not, because "in range"
// would have to mean one of three things and v1 declines to guess.
static std::string histHlRefusalFor(const ImageDoc* im, int plane) {
    if (!im) return "no image";
    const bool cfa = im->ch == 1 && im->cfa != 0;
    if (plane < 0 && !cfa && im->ch > 1)
        return "highlight needs ONE channel - pick it above";
    return {};
}

// Arming, re-dragging and clearing all change what every open document's pixels
// should look like, so every one is marked dirty; the rebuild itself is lazy
// (it happens when a texture is next drawn), which is what makes marking cheap.
// The counts go with it - they were measured for the PREVIOUS interval, and a
// sentence reading them afterwards would answer a question nobody asked.
static void histHlTouchAll() {
    app.hlCount.clear();
    for (auto& d : app.images) d->texDirty = true;
}
static void histHlClear() {
    if (!app.highlight.on) return;
    app.highlight = App::HistHighlight{};
    histHlProbeSet("refused", "");
    histHlTouchAll();
}
static void histHlArm(float lo, float hi, bool loOpen, bool hiOpen,
                      int plane, const char* name) {
    App::HistHighlight& H = app.highlight;
    H.on = true;
    H.lo = lo; H.hi = hi; H.loOpen = loOpen; H.hiOpen = hiOpen;
    H.plane = plane;
    snprintf(H.planeName, sizeof H.planeName, "%s",
             plane >= 0 && name ? name : "");
    histHlProbeSet("refused", "");
    histHlTouchAll();
}

// The plane vocabulary of a document, by the SAME rule the bins use
// (recomputeHistogramIfNeeded): a mosaiced 1ch image is four named planes,
// anything else is its channels. One rule, or a name a highlight is bound to
// would mean one thing in the panel and another on the pixels.
static int histHlPlanesOf(const ImageDoc& im, const char* (&names)[4]) {
    static const char* CFA_SERIES[4] = { "R", "Gr", "Gb", "B" };
    static const char* CH_SERIES[4] = { "ch0", "ch1", "ch2", "ch3" };
    const bool cfa = im.ch == 1 && im.cfa != 0;
    for (int s = 0; s < 4; s++) names[s] = cfa ? CFA_SERIES[s] : CH_SERIES[s];
    return cfa ? 4 : std::min(im.ch, 3);
}

// The paint, and the count that comes out of the same loop.
//
// A SECOND pass over the pixels rather than a branch inside renderDocRGBA's
// hot loop: switched off the cost is one test OUTSIDE the loop and the bytes
// above are the ones that were always produced, which makes "off is unchanged"
// structural instead of a thing to keep re-checking. Switched on it is one read
// of the value and one comparison per pixel.
//
// It counts what it PAINTS - never what the bins hold - because the two are
// different populations (§5), and it counts the denominator (finite pixels of
// the plane) in the same walk so the share cannot be a ratio of two different
// passes.
static void histHlPaint(ImageDoc& im, std::vector<uint8_t>& rgba) {
    const App::HistHighlight& H = app.highlight;
    if (!H.on) return;
    const char* names[4] = {};
    App::HlCount C;
    C.nSeries = histHlPlanesOf(im, names);
    for (int s = 0; s < 4; s++) C.names[s] = names[s];
    if (H.plane >= 0) {
        // by NAME, never by channel number: the panel forbade that already, and
        // a B matched positionally would paint Gr where the user asked for R
        C.plane = abSeriesMatch(names, C.nSeries, H.planeName);
        if (C.plane < 0) {
            C.why = std::string("no ") + H.planeName + " plane";
            app.hlCount[im.uid] = C;      // painting nothing, and SAYING so
            return;
        }
    } else if (std::string no = histHlRefusalFor(&im, -1); !no.empty()) {
        C.why = no;
        app.hlCount[im.uid] = C;
        return;
    }
    C.painted = true;
    const bool cfa = im.ch == 1 && im.cfa != 0;
    int px = 0, py = 0;
    for (size_t p = 0; p < (size_t)im.w * im.h; p++, px++) {
        if (px == im.w) { px = 0; py++; }
        const float* src = &im.px()[p * im.ch];
        int s;                       // the plane THIS pixel belongs to
        float v;
        if (cfa) { s = cfaChannelAt(im, px, py); v = src[0]; }
        else     { s = C.plane < 0 ? 0 : C.plane; v = src[s]; }
        if (s < 0 || s >= 4) continue;
        if (C.plane >= 0 && s != C.plane) continue;      // outside the binding
        if (!std::isfinite(v)) continue;                 // NaN is never in range
        C.fin[s]++;
        if (!H.loOpen && !(v >= H.lo)) continue;
        if (!H.hiOpen && !(v < H.hi)) continue;          // half-open [lo, hi)
        C.hit[s]++;
        memcpy(&rgba[p * 4], HL_RGBA, 4);
    }
    app.hlCount[im.uid] = C;
}

// The interval, in words. ONE builder for the panel footer, the canvas badge
// and the export note, on abHistXLabel's rule: three surfaces must not be able
// to say three different things about one selection.
static std::string histHlRangeSaid(const std::string& dtype) {
    const App::HistHighlight& H = app.highlight;
    if (!H.on) return {};
    const std::string u = abValueUnit(dtype);
    char b[128];
    // The open cases are not decoration: they are what the END BARS mean, so
    // the sentence has to read the way the bar does.
    if (H.loOpen && H.hiOpen) snprintf(b, sizeof b, "all values");
    else if (H.loOpen)        snprintf(b, sizeof b, "< %.6g %s", (double)H.hi, u.c_str());
    else if (H.hiOpen)        snprintf(b, sizeof b, ">= %.6g %s", (double)H.lo, u.c_str());
    else snprintf(b, sizeof b, "%.6g..%.6g %s", (double)H.lo, (double)H.hi, u.c_str());
    std::string head = "highlight ";
    if (H.plane >= 0 && H.planeName[0]) head += std::string(H.planeName) + " ";
    return head + b;
}

// The COUNT half, built once and worn by both surfaces: the footer passes the
// region it is always measured over ("whole frame" - it is named EVERY time,
// because the panel header above may be saying "(selected ROI)" about the
// curves), the badge passes nothing and gets the short form.
static std::string histHlCountPhrase(const App::HlCount& C, const char* region) {
    char b[352];
    const std::string tail = region ? std::string(", ") + region : std::string();
    if (C.plane >= 0) {
        const double sh = C.fin[C.plane] ? 100.0 * (double)C.hit[C.plane] /
                                           (double)C.fin[C.plane] : 0.0;
        snprintf(b, sizeof b, "%zu px (%.2f%% of finite %s%s)",
                 C.hit[C.plane], sh, C.names[C.plane], tail.c_str());
        return b;
    }
    if (C.nSeries <= 1) {
        const double sh = C.fin[0] ? 100.0 * (double)C.hit[0] / (double)C.fin[0] : 0.0;
        snprintf(b, sizeof b, "%zu px (%.2f%% of finite pixels%s)",
                 C.hit[0], sh, tail.c_str());
        return b;
    }
    // A mosaiced frame under an "all" binding. The counts stay PER PLANE:
    // statistics are never mixed across planes anywhere else in this tree, and
    // one pooled percentage would be exactly that mixing.
    std::string s;
    for (int i = 0; i < C.nSeries; i++) {
        const double sh = C.fin[i] ? 100.0 * (double)C.hit[i] / (double)C.fin[i] : 0.0;
        char one[96];
        if (region) snprintf(one, sizeof one, "%s%s %zu (%.2f%%)",
                             i ? " | " : "", C.names[i], C.hit[i], sh);
        else        snprintf(one, sizeof one, "%s%s %zu",
                             i ? " | " : "", C.names[i], C.hit[i]);
        s += one;
    }
    return s + " px" + (region ? std::string("  (") + region + ")" : std::string());
}

// What ONE document answered, with its letter in front. Read from the paint,
// never re-derived: a second count wearing the same face is the trap.
static std::string histHlSideSaid(const ImageDoc* d, const std::string& letter) {
    if (!d || !app.highlight.on) return {};
    auto it = app.hlCount.find(d->uid);
    if (it == app.hlCount.end()) return {};
    const App::HlCount& C = it->second;
    if (!C.painted) return letter + ": " + C.why;
    return letter + ": " + histHlCountPhrase(C, "whole frame");
}

// The canvas badge: A's summary, short. It exists because the panel that made
// the selection can be CLOSED while the paint is still on the pixels, and a
// lens that paints in silence is the one failure this feature must not ship.
static std::string histHlBadgeSaid() {
    if (!app.highlight.on) return {};
    // The difference view is built from DIFFERENCES, not values, on a separate
    // path that never reaches renderDocRGBA - so it is not painted, and that
    // limit is spoken rather than left to be discovered. A stated limit is not
    // a defect; an unstated one is.
    if (app.compareMode == App::CmpDiff)
        return "highlight armed - not shown in difference view";
    ImageDoc* a = cur();
    const std::string head = histHlRangeSaid(a ? a->dtype : std::string());
    if (!a) return head;
    auto it = app.hlCount.find(a->uid);
    if (it == app.hlCount.end()) return head;
    const App::HlCount& C = it->second;
    if (!C.painted) return head + ": " + C.why;
    return head + ": " + histHlCountPhrase(C, nullptr);
}

// upload/normalize into RGBA8 texture
// float pixels -> display RGBA, exactly as the screen shows them (range, gamma,
// colormap, CFA colorize). Split out of rebuildTexture so that EXPORTING a view
// runs the same code: a picture pasted into a report that does not match the
// screen is worse than no picture.
static void renderDocRGBA(ImageDoc& im, std::vector<uint8_t>& rgba) {
    rgba.resize((size_t)im.w * im.h * 4);
    const float ib = effBlack(im), iw = effWhite(im);
    float inv = 1.0f / std::max(iw - ib, 1e-20f);
    float invGamma = 1.0f / app.dispGamma;
    bool doGamma = fabsf(app.dispGamma - 1.0f) > 1e-3f;
    // Gamma through a table instead of 36 million powf() calls at 12 Mpx (235 ms
    // -> 76 ms measured), which is what makes dragging the range slider usable at
    // gamma 2.2. The table is in the SQRT domain: x^(1/g) is vertical at x=0, so
    // interpolating it there is worth 3 output codes of error, while the same
    // table over sqrt(x) is nearly straight and lands within 0.001 of exact.
    static float gammaLut[1025];
    static float gammaLutFor = -1;
    if (doGamma && gammaLutFor != app.dispGamma) {
        for (int i = 0; i <= 1024; i++) gammaLut[i] = powf(i / 1024.0f, 2.0f * invGamma);
        gammaLutFor = app.dispGamma;
    }
    auto applyGamma = [&](float x) {
        float f = sqrtf(x) * 1024.0f;
        int i = std::min((int)f, 1023);
        return gammaLut[i] + (gammaLut[i + 1] - gammaLut[i]) * (f - i);
    };
    bool cfaColor = im.ch == 1 && im.cfa != 0 && im.cfaColorize;
    const uint8_t* lut = nullptr;   // display plugin colormap (1ch only; CFA colorize wins)
    if (im.ch == 1 && !cfaColor && im.displayLut >= 0 &&
        im.displayLut < (int)plugin_host::displays().size())
        lut = plugin_host::displays()[im.displayLut].lut.data();
    int px = 0, py = 0;
    for (size_t p = 0; p < (size_t)im.w * im.h; p++, px++) {
        if (px == im.w) { px = 0; py++; }
        const float* src = &im.px()[p * im.ch];
        float r, g, b;
        if (lut) {
            float x = std::clamp((src[0] - ib) * inv, 0.0f, 1.0f);
            if (doGamma) x = applyGamma(x);
            int idx = (int)(x * 255.0f + 0.5f);
            rgba[p * 4 + 0] = lut[idx * 3];
            rgba[p * 4 + 1] = lut[idx * 3 + 1];
            rgba[p * 4 + 2] = lut[idx * 3 + 2];
            rgba[p * 4 + 3] = 255;
            continue;
        }
        if (cfaColor) {
            // running x/y instead of p%w and p/w: a 64-bit div+mod per pixel is
            // ~40% of this loop at 12 Mpx
            int c = cfaChannelAt(im, px, py);
            r = c == 0 ? src[0] : ib;
            g = (c == 1 || c == 2) ? src[0] : ib;
            b = c == 3 ? src[0] : ib;
        }
        else if (im.ch == 1) { r = g = b = src[0]; }
        else if (im.ch == 2) { r = src[0]; g = src[1]; b = ib; }
        else                 { r = src[0]; g = src[1]; b = src[2]; }
        float v[3] = { (r - ib) * inv, (g - ib) * inv, (b - ib) * inv };
        for (int c = 0; c < 3; c++) {
            float x = std::clamp(v[c], 0.0f, 1.0f);
            if (doGamma) x = applyGamma(x);
            rgba[p * 4 + c] = (uint8_t)(x * 255.0f + 0.5f);
        }
        rgba[p * 4 + 3] = 255;
    }
    // The value highlight goes HERE and nowhere else. This is the one place a
    // pixel becomes RGBA, so the screen, the view export and every thumbnail
    // agree by CONSTRUCTION rather than by three implementations happening to
    // agree - which is the same contract the comment above this function
    // states for the range, the gamma and the colormap. Note what follows from
    // that: the interval is tested against the RAW value, so it does not follow
    // the display range or the gamma. That is the intended behaviour, not an
    // oversight - what was selected is a value, not a brightness.
    histHlPaint(im, rgba);
}

static void rebuildTexture(ImageDoc& im) {
    // One scratch buffer for the whole app: a 12 Mpx image is a 48 MB allocation,
    // and this runs on every range/gamma/LUT change.
    static std::vector<uint8_t> rgba;
    im.texBlack = effBlack(im); im.texWhite = effWhite(im);
    renderDocRGBA(im, rgba);
    if (!im.tex) glGenTextures(1, &im.tex);
    touchTex(im);
    glBindTexture(GL_TEXTURE_2D, im.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, im.texNearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, im.texNearest ? GL_NEAREST : GL_LINEAR);
    im.texDirty = false;
}
