// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// §2 の地図に無いファイル: close の前方宣言群(defaults: first decl)+ series(系列)モデル+ series 行の動作。§2 は remote_client の帯にこの並びを含めて数えていた(判断は PR 参照)。
// ---- close, per layer (docs/terminology.md) -----------------------------------
// The canon: Close on a stack member closes the STACK - frames vanishing one at
// a time from a measurement set was the reported bug. Ctrl+Alt+W stays as the
// one-frame escape hatch.
static std::vector<int> framesOfSeq(int seqId);   // fwd (sequence helpers below)
static App::SeqInfo* seqInfo(int id);             // fwd - never hold what it
                                    // returns across an open/close (sequence.inc)
static void stopSequenceLoader();                 // fwd
static void pruneEmptyBatches();                  // fwd (defined with the moves)
// The value a stack's NAME suggests, and the text it was read out of. Declared
// here because the picker proposes one per group long before the linearity
// code that defines it (the default argument lives on this declaration only).
static double extractLevelFromName(const std::string& name, std::string* srcOut = nullptr);

// ---- series (系列), the model (docs/terminology.md, docs/series-plan.md) -------
// No UI here on purpose: the invariants have to hold before anything can draw
// them. Series are few and members are tens, so every lookup is a plain walk.
static App::Series* seriesById(int id) {
    for (auto& s : app.series) if (s.id == id) return &s;
    return nullptr;
}
// Reverse membership. The canon: a stack belongs to AT MOST ONE series, so the
// first hit is the only hit (seriesAudit proves it, addToSeries maintains it).
static App::Series* seriesOfStack(int seqId) {
    if (seqId == 0) return nullptr;
    for (auto& s : app.series)
        for (const auto& m : s.members)
            if (m.seqId == seqId) return &s;
    return nullptr;
}
// ---- frame members (docs/analysis-layers.md §3.3, 判断record 7) --------------
// A series member may be a STANDALONE FRAME: the doc named by uid, seqId 0,
// not a preview. A frame that sits inside a stack is never a member by itself
// - it belongs to the series through its stack (the canon's containment; the
// 省略 rule covers a frame with no stack, not a frame escaping one).
static ImageDoc* frameMemberDoc(uint64_t uid) {
    if (!uid) return nullptr;
    for (const auto& d : app.images)
        if (d->uid == uid && d->seqId == 0 && !d->preview) return d.get();
    return nullptr;
}
// Reverse membership for frames - the frame twin of seriesOfStack, with the
// same first-hit-is-the-only-hit contract (seriesAudit proves it).
static App::Series* seriesOfFrame(uint64_t uid) {
    if (uid == 0) return nullptr;
    for (auto& s : app.series)
        for (const auto& m : s.members)
            if (m.frameUid == uid) return &s;
    return nullptr;
}
// Drop one frame from whatever series holds it. Returns the series' id, or 0.
static int removeFrameFromSeries(uint64_t uid) {
    if (uid == 0) return 0;
    for (auto& s : app.series)
        for (auto it = s.members.begin(); it != s.members.end(); ++it)
            if (it->frameUid == uid) {
                s.members.erase(it);
                app.imagesRev++;
                return s.id;
            }
    return 0;
}
// Add a standalone frame to a series - the frame twin of addToSeries, with the
// same strict-containment refusal (the frame's batch must be the series' - the
// caller's answer is "Move to batch first") and the same at-most-one rule
// (joining moves the frame out of whatever series held it).
static bool addFrameToSeries(int seriesId, uint64_t uid, double value,
                             bool include = true) {
    App::Series* S = seriesById(seriesId);
    ImageDoc* d = frameMemberDoc(uid);
    if (!S || !d) return false;
    if (d->batchId != S->batchId) return false;
    for (auto& m : S->members)
        if (m.frameUid == uid) { m.value = value; m.include = include; return true; }
    removeFrameFromSeries(uid);
    App::Series::Member m;
    m.frameUid = uid;
    m.value = value;
    m.include = include;
    S->members.push_back(m);
    app.imagesRev++;
    return true;
}
// The batch a stack is in, read off its frames (the frames own batchId).
// 0 = the stack has no frames resident, so it claims no batch.
static int batchOfStack(int seqId) {
    for (const auto& d : app.images) if (d->seqId == seqId) return d->batchId;
    return 0;
}
static std::string batchNameOf(int batchId) {
    for (const auto& b : app.batches) if (b.id == batchId) return b.name;
    return {};
}
// Default name until a human gives it one (docs/terminology.md).
static int newSeries(int batchId, const std::string& name) {
    App::Series s;
    s.id = app.nextSeriesId++;
    s.batchId = batchId;
    s.name = name.empty() ? batchNameOf(batchId) + " 掃引" : name;
    app.series.push_back(std::move(s));
    app.imagesRev++;                  // the Files grouping caches on this
    return app.series.back().id;
}
// Drop one stack from whatever series holds it. Returns the series' id, or 0.
static int removeFromSeries(int seqId) {
    for (auto& s : app.series)
        for (auto it = s.members.begin(); it != s.members.end(); ++it)
            if (it->seqId == seqId) {
                s.members.erase(it);
                app.imagesRev++;
                return s.id;
            }
    return 0;
}
// Add a stack to a series. Fails (false) when the stack is not in the series'
// batch - the canon's strict containment, enforced rather than papered over:
// the caller's answer is "Move to batch first", never "we widened the series".
// Adding a stack that already sits in another series MOVES it (at most one).
static bool addToSeries(int seriesId, int seqId, double value, bool include = true) {
    App::Series* S = seriesById(seriesId);
    if (!S || seqId == 0 || !seqInfo(seqId)) return false;
    int b = batchOfStack(seqId);
    if (b != S->batchId) return false;
    for (auto& m : S->members)
        if (m.seqId == seqId) { m.value = value; m.include = include; return true; }
    removeFromSeries(seqId);
    S->members.push_back({ seqId, value, include });
    app.imagesRev++;
    return true;
}
// A series with nothing in it is not a work in progress, it is litter: the
// creation path leaves one member behind, closes take the rest away.
static void pruneEmptySeries() {
    for (auto it = app.series.begin(); it != app.series.end();)
        if (it->members.empty()) {
            if (app.curSeriesId == it->id) app.curSeriesId = 0;
            it = app.series.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
}
// Text -> a parameter value, and NOTHING ELSE to a number. atof() answers 0 for
// every string it cannot read, and 0 in a series is not a missing point: it is
// the dark stack the offset is anchored to and the read noise is MEASURED in
// (linRecompute). "-", "", "1e", "notanumber" and a truncated write all mean the
// same thing here - UNSET, which the whole layer already knows how to show and
// how to keep out of a fit.
static double parseSeriesValue(const char* s) {
    const double NOTSET = std::numeric_limits<double>::quiet_NaN();
    if (!s) return NOTSET;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || (s[0] == '-' && s[1] == '\0')) return NOTSET;   // "-" = unset
    char* end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return NOTSET;                     // nothing numeric at all
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return NOTSET;                         // trailing junk: "1e", "12x"
    return std::isfinite(v) ? v : NOTSET;            // inf/nan are not measurements
}
// A double as text that reads back as the SAME double. The session file is
// written at setprecision(9), which is right for a view offset and wrong for the
// axis a fit is taken against: 1234567.89 comes back as 1234570. Nine digits
// first, so every value that already round-tripped keeps its old spelling.
static std::string fmtExact(double v) {
    char b[48];
    for (int p = 9; p < 17; p++) {
        snprintf(b, sizeof b, "%.*g", p, v);
        if (strtod(b, nullptr) == v) return b;
    }
    snprintf(b, sizeof b, "%.17g", v);
    return b;
}
// The Linearity panel's numbers describe ONE set of members with ONE set of
// values on the axis, and every label around them is read live off the series.
// Change the series and a fit left standing is not merely old, it is relabelled:
// lux measurements headed "DN/ms". linFitStale keeps the per-stack MEASUREMENTS
// (they are DN whatever the axis says) and drops only the fit; linInvalidate
// drops the rows too, for when the membership itself moved.
static void linFitStale() { app.lin.fitValid = false; app.lin.rev++; }
static void linInvalidate() {
    app.lin.rows.clear();
    app.lin.seriesId = 0;
    app.lin.nPts = 0;
    linFitStale();
}
// Points a fit would actually use: included, value SET (NaN is "unset", never
// 0), and the member - stack or frame - still there.
static int seriesFitPoints(const App::Series& S) {
    int n = 0;
    for (const auto& m : S.members) {
        if (!m.include || !std::isfinite(m.value)) continue;
        if (m.seqId != 0 ? seqInfo(m.seqId) != nullptr
                         : frameMemberDoc(m.frameUid) != nullptr) n++;
    }
    return n;
}
static const char* seriesKindName(int k) {
    switch (k) {
        case App::Series::KPtc:         return "PTC";
        case App::Series::KTemperature: return "temperature";
        case App::Series::KOther:       return "other";
        default:                        return "linearity";
    }
}
// The ONE line that names the layer of everything this panel prints (canon
// invariant (a): 解析結果は自分の層を名乗る - a number that does not carry its
// layer cannot be quoted in a report). Screen, TSV comment and selftest all
// read this string, so they cannot drift apart.
static std::string seriesProvLine(const App::Series& S, const App::LinState& L) {
    int st = 0, fr = 0;
    for (const auto& r : L.rows) (r.isFrame ? fr : st)++;
    char b[320];
    snprintf(b, sizeof b,
             "layer: series \"%s\" (batch \"%s\") | kind %s | members: %d stack + %d frame",
             S.name.c_str(), batchNameOf(S.batchId).c_str(), seriesKindName(S.kind),
             st, fr);
    return b;
}
// The window that was in force, ALWAYS said - one string for the panel and
// the TSV, so the screen and a pasted document cannot disagree (the interim
// frame-lin's frameLinWindowLabel, migrated with the machinery).
static std::string seriesWinLabel(const App::LinState& L) {
    char b[160];
    if (L.winMode == 2)
        snprintf(b, sizeof b, "fit window: manual [%.9g, %.9g] DN (all planes)",
                 std::min(L.winLo, L.winHi), std::max(L.winLo, L.winHi));
    else if (L.winMode == 1)
        snprintf(b, sizeof b,
                 "fit window: auto = 5%%..95%% of each plane's max member mean");
    else
        snprintf(b, sizeof b, "fit window: none (all included points)");
    return b;
}
static const char* seriesFitMethodName(int m) {
    return m == 1 ? "relative-weighted LS (rev4-style)" : "OLS";
}
// Whether the Linearity panel's EQUATIONS are the ones this series asked for.
// The canon (docs/terminology.md: 測定の種類 ... fit の式と出す量が決まる) makes
// the kind decide that, and this panel knows exactly two: a straight response
// against the swept parameter, and the photon transfer taken alongside it. A
// temperature run pushed through them comes out as a "sensitivity" in DN per
// degree, a system gain K read off stacks that differ in temperature rather
// than in signal, and a read noise "measured in the level-0 stack" - meaning
// whichever stack happened to sit at 0 degrees.
static bool seriesFitKind(const App::Series& S) {
    return S.kind == App::Series::KLinearity || S.kind == App::Series::KPtc;
}
// Two points, a named unit, and a kind whose equation this is. A series of one
// is perfectly legal (it is being built); it just cannot be fitted yet. An unset
// unit is the same kind of "not yet" - fitting without one would print numbers
// per nothing.
static bool seriesCanFit(const App::Series& S) {
    return seriesFitKind(S) && S.unit[0] != '\0' && seriesFitPoints(S) >= 2;
}
// Every invariant the canon states, checked exhaustively. Used by
// --series-selftest after every mutation; cheap enough to call from anywhere.
static bool seriesAudit(std::string& why) {
    std::vector<int> seen;
    std::vector<uint64_t> seenU;
    for (const auto& S : app.series) {
        if (S.members.empty()) { why = "series '" + S.name + "' has no members"; return false; }
        bool haveBatch = false;
        for (const auto& b : app.batches) if (b.id == S.batchId) haveBatch = true;
        if (!haveBatch) { why = "series '" + S.name + "' points at a dead batch"; return false; }
        for (const auto& m : S.members) {
            // a member is EXACTLY one of stack / frame (§3.3) - both or neither
            // is a corrupt membership, not a new kind
            if ((m.seqId != 0) == (m.frameUid != 0)) {
                why = "series '" + S.name + "' has a member that is " +
                      (m.seqId ? "both a stack and a frame" : "neither stack nor frame");
                return false;
            }
            if (m.frameUid != 0) {
                const ImageDoc* d = frameMemberDoc(m.frameUid);
                if (!d) {
                    why = "series '" + S.name + "' frame member uid " +
                          std::to_string((unsigned long long)m.frameUid) + " does not exist";
                    return false;
                }
                if (d->batchId != S.batchId) {
                    why = "series '" + S.name + "' frame member \"" + d->name +
                          "\" is in batch " + std::to_string(d->batchId) +
                          " (series batch " + std::to_string(S.batchId) + ")";
                    return false;
                }
                for (uint64_t u : seenU)
                    if (u == m.frameUid) {
                        why = "frame " + d->name + " is in two series";
                        return false;
                    }
                seenU.push_back(m.frameUid);
                continue;
            }
            if (!seqInfo(m.seqId)) {
                why = "series '" + S.name + "' member seq " + std::to_string(m.seqId) +
                      " does not exist";
                return false;
            }
            for (const auto& d : app.images)
                if (d->seqId == m.seqId && d->batchId != S.batchId) {
                    why = "series '" + S.name + "' member seq " + std::to_string(m.seqId) +
                          " has a frame in batch " + std::to_string(d->batchId) +
                          " (series batch " + std::to_string(S.batchId) + ")";
                    return false;
                }
            for (int s : seen)
                if (s == m.seqId) {
                    why = "stack " + std::to_string(m.seqId) + " is in two series";
                    return false;
                }
            seen.push_back(m.seqId);
        }
    }
    why.clear();
    return true;
}

// Frame MEMBERS die with their docs (§3.3). Every close funnels through
// closeImages, so this is the one sweep that keeps a series from holding a
// member whose frame is gone - the frame twin of closeStack's
// removeFromSeries. The fit goes stale with the membership (the canon:
// editing the members discards the computed numbers); the close itself
// already spoke, so this only drops the bookkeeping.
static void pruneDeadFrameMembers() {
    bool dropped = false;
    for (auto& s : app.series)
        for (auto it = s.members.begin(); it != s.members.end();)
            if (it->frameUid != 0 && !frameMemberDoc(it->frameUid)) {
                it = s.members.erase(it);
                dropped = true;
            } else {
                ++it;
            }
    if (!dropped) return;
    app.imagesRev++;
    linFitStale();
    pruneEmptySeries();
}

// ---- AnalysisSet (docs/reader-analysisset.md, "ras"): the model --------------
// No UI here, the series-model rule: the invariants have to hold before
// anything can draw them. Sets are few and roles are a handful each, so every
// lookup is a plain walk.
static App::ASet* aSetById(int id) {
    for (auto& s : app.analysisSets) if (s.id == id) return &s;
    return nullptr;
}
// ras 1.4: the ONE prohibition is a same-name set in one batch - (batch, name)
// is the identity the session's adopt rule and provenance key on. Default-name
// twins from one call are numbered here (" (2)"), the series-default-name
// precedent: a starting value people rename, not an exemption from the rule.
static std::string aSetUniqueName(int batchId, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const auto& s : app.analysisSets)
            if (s.batchId == batchId && s.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; i++) {
        std::string n = base + " (" + std::to_string(i) + ")";
        if (!taken(n)) return n;
    }
}
// The Files row's text, pure so the selftest asserts the exact words (ras 6):
// `PRNU 10lx  (2/2 roles)`, or `(1/2 roles - dark unbound)` when one is not.
// NO byte count, ever: a binding is not ownership, and the unique-source sum
// gains nothing from a set row (reference-design.md 7's one-line lesson).
static std::string aSetRowText(const App::ASet& S) {
    int total = (int)S.roles.size(), bound = 0;
    std::string firstUnbound;
    for (const auto& r : S.roles) {
        if (r.bound()) bound++;
        else if (firstUnbound.empty()) firstUnbound = r.role;
    }
    char b[160];
    if (bound == total)
        snprintf(b, sizeof b, "(%d/%d roles)", bound, total);
    else
        snprintf(b, sizeof b, "(%d/%d roles - %s unbound)", bound, total,
                 firstUnbound.c_str());
    return S.name + "  " + b;
}
// One role row: what this role is bound TO (the member's home stays the
// member's own row - the tree is not duplicated). Unbound rows carry their
// one-line reason (ras 3.2).
static std::string aSetRoleText(const App::ASetRole& r) {
    if (!r.bound())
        return r.role + ": unbound (" +
               (r.reason.empty() ? std::string("no reason recorded") : r.reason) + ")";
    std::string target = "?";
    if (r.seqId != 0) {
        const App::SeqInfo* si = seqInfo(r.seqId);
        target = (si ? si->name : std::string("stack")) + " (" +
                 std::to_string((int)framesOfSeq(r.seqId).size()) + ")";
    } else if (r.frameUid != 0) {
        for (const auto& d : app.images)
            if (d->uid == r.frameUid) { target = d->name; break; }
    } else if (r.seriesId != 0) {
        for (const auto& s : app.series)
            if (s.id == r.seriesId) { target = s.name; break; }
    }
    std::string s = r.role + " -> " + target;
    if (!r.note.empty()) s += "  - " + r.note;   // e.g. "bound to the open copy ..."
    return s;
}
// The tooltip sentence the MEMBER side gets - and nothing else: no column, no
// mark on the member row (ras 6). One line per binding that names this stack /
// frame / series.
static std::string aSetBoundSentence(int seqId, uint64_t uid, int seriesId) {
    std::string s;
    for (const auto& S : app.analysisSets)
        for (const auto& r : S.roles) {
            bool mine = (seqId != 0 && r.seqId == seqId) ||
                        (uid != 0 && r.frameUid == uid) ||
                        (seriesId != 0 && r.seriesId == seriesId);
            if (!mine) continue;
            if (!s.empty()) s += "\n";
            s += "bound as \"" + r.role + "\" in \"" + S.name + "\"";
        }
    return s;
}
// "メンバを閉じたら束縛を失い、そう表示する (黙って空にしない)" - the canon's
// rule (analysis-layers.md 1.1), applied after every close: a role whose
// target is gone becomes UNBOUND with the reason, never a dangling id and
// never a silent blank.
static void aSetAuditBindings() {
    bool changed = false;
    for (auto& S : app.analysisSets)
        for (auto& r : S.roles) {
            if (r.seqId != 0 && !seqInfo(r.seqId)) {
                r.seqId = 0;
                r.reason = "the stack was closed while bound";
                changed = true;
            } else if (r.frameUid != 0) {
                bool alive = false;
                for (const auto& d : app.images)
                    if (d->uid == r.frameUid) { alive = true; break; }
                if (!alive) {
                    r.frameUid = 0;
                    r.reason = "the frame was closed while bound";
                    changed = true;
                }
            } else if (r.seriesId != 0) {
                bool alive = false;
                for (const auto& s : app.series)
                    if (s.id == r.seriesId) alive = true;
                if (!alive) {
                    r.seriesId = 0;
                    r.reason = "the series was closed or ungrouped while bound";
                    changed = true;
                }
            }
        }
    if (changed) app.imagesRev++;      // the Files rows read these
}
// Close on the SET node: the bindings are a record, and only the record goes -
// members are untouched, because a binding is not ownership (ras 6). The
// report counts, the closeStack survival notice's shape.
static void closeAnalysisSet(int setId) {
    for (auto it = app.analysisSets.begin(); it != app.analysisSets.end(); ++it)
        if (it->id == setId) {
            int n = 0;
            for (const auto& r : it->roles) if (r.bound()) n++;
            char m[320];
            snprintf(m, sizeof m, "closed set \"%s\" - %d binding(s) dropped, "
                                  "members untouched", it->name.c_str(), n);
            app.analysisSets.erase(it);
            for (auto p = app.aSetBindPending.begin(); p != app.aSetBindPending.end();)
                p = p->setId == setId ? app.aSetBindPending.erase(p) : std::next(p);
            app.imagesRev++;
            fprintf(stderr, "%s\n", m);
            toast(m);
            pruneEmptyBatches();       // the batch may have held only this set
            return;
        }
}
// Rename: allowed, session-saved (the canon's operations table). The one
// refusal is ras 1.4's prohibition itself - two same-name sets in one batch.
static bool renameAnalysisSet(int setId, const std::string& name, std::string* whyNot) {
    App::ASet* S = aSetById(setId);
    if (!S || name.empty()) return false;
    for (const auto& o : app.analysisSets)
        if (o.id != setId && o.batchId == S->batchId && o.name == name) {
            if (whyNot) *whyNot = "a set named \"" + name + "\" already exists in "
                                  "this batch - a set's identity is (batch, name)";
            return false;
        }
    S->name = name;
    app.imagesRev++;
    return true;
}

// Close a set of images by index: erase in descending order so the indices stay
// valid, one forget per cache that can name them, current re-picked at the end.
static void closeImages(std::vector<int> idxs) {
    if (idxs.empty()) return;
    std::sort(idxs.begin(), idxs.end());
    idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
    uint64_t curUid = cur() ? cur()->uid : 0;
    bool closedCur = false;
    for (int i = (int)idxs.size() - 1; i >= 0; i--) {
        int idx = idxs[i];
        if (idx < 0 || idx >= (int)app.images.size()) continue;
        ImageDoc* im = app.images[idx].get();
        if (im->uid == curUid) closedCur = true;
        if (im->uid == app.previewUid) app.previewUid = 0;
        forgetImage(im);
        if (im->tex) glDeleteTextures(1, &im->tex);
        app.images.erase(app.images.begin() + idx);
    }
    if (curUid) {                     // re-point current: same doc if it survived
        if (!closedCur) {
            app.current = -1;
            for (int i = 0; i < (int)app.images.size(); i++)
                if (app.images[i]->uid == curUid) { app.current = i; break; }
        }
        if (closedCur || app.current < 0) {
            app.current = app.images.empty() ? -1
                        : std::min(idxs.front(), (int)app.images.size() - 1);
            app.fitRequested = true;
        }
    } else if (app.current >= (int)app.images.size()) {
        app.current = (int)app.images.size() - 1;
    }
    app.imagesRev++;
    if (!resolveB()) {   // B went with them: a later same-name file must not
        app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
    }
    pruneDeadFrameMembers();   // a closed frame is not a series member anymore
    pruneEmptyBatches();
    aSetAuditBindings();  // a closed member's binding becomes unbound, said so
}

// What the last close SAID about pixels that survived it (§4's survival
// notice), as state a selftest can read - the panel is OpenGL this machine
// cannot screenshot, same pattern as g_filesBadgeProbe. Empty = the last
// close freed everything it held (and said nothing, as before sharing).
static std::string g_lastCloseNote;

// The survival scan behind that notice, over the WHOLE closing set of ONE
// user action: closeBatch/closeSeries hand every doc they are about to close
// here at once, so the sentence can never name a "survivor" the same action
// then frees (a per-member scan did exactly that for a batch holding a source
// stack and its derived stack). Counts are UNIQUE PIXELS, not memberships -
// source+derived closing together is N frames, not 2N. Sources adopted by an
// in-flight load but still queued in app.seqReady count as ALIVE (the loader
// holds the shared_ptr and lands them moments later), named by the stack they
// are loading into - unless that stack is itself being closed, in which case
// closeStack sweeps the queue. Speaks (stderr + toast + g_lastCloseNote) only
// when pixels survive; a close that frees everything stays silent, as before
// sharing.
static void sayCloseNotice(const std::vector<int>& closing,
                           const std::vector<int>& closingSeqIds,
                           const std::string& what) {
    g_lastCloseNote.clear();
    if (closing.empty()) return;
    std::vector<char> inSet(app.images.size(), 0);
    for (int idx : closing)
        if (idx >= 0 && idx < (int)app.images.size()) inSet[idx] = 1;
    std::unordered_map<uint64_t, const ImageDoc*> outside;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (!inSet[i]) outside.emplace(app.images[i]->src->srcId, app.images[i].get());
    std::unordered_set<uint64_t> queued;   // decoded and adopted, not landed yet
    const int loadingSeq = app.seqLoadingId;
    if (loadingSeq != 0 &&
        std::find(closingSeqIds.begin(), closingSeqIds.end(), loadingSeq) ==
            closingSeqIds.end()) {
        std::lock_guard<std::mutex> lk(app.seqMtx);
        for (const auto& r : app.seqReady)
            if (r.second && r.second->src) queued.insert(r.second->src->srcId);
    }
    std::unordered_set<uint64_t> seen;
    int total = 0, kept = 0;
    std::vector<std::string> keepers;      // distinct counterpart names
    auto keeper = [&](const std::string& nm) {
        if (std::find(keepers.begin(), keepers.end(), nm) == keepers.end())
            keepers.push_back(nm);
    };
    for (int idx : closing) {
        if (idx < 0 || idx >= (int)app.images.size()) continue;
        const ImageDoc& d = *app.images[idx];
        if (!seen.insert(d.src->srcId).second) continue;
        total++;
        auto it = outside.find(d.src->srcId);
        if (it != outside.end()) {
            kept++;
            const ImageDoc* o = it->second;
            const App::SeqInfo* osi = o->seqId ? seqInfo(o->seqId) : nullptr;
            keeper(osi ? osi->name : o->name);
        } else if (queued.count(d.src->srcId)) {
            kept++;
            const App::SeqInfo* lsi = seqInfo(loadingSeq);
            keeper((lsi ? lsi->name : std::string("a stack")) + " (loading)");
        }
    }
    if (kept == 0) return;                 // freed everything: silence, as ever
    std::string who = "\"" + keepers[0] + "\"";
    if (keepers.size() > 1)
        who += " (+" + std::to_string(keepers.size() - 1) + " more)";
    char m[512];
    snprintf(m, sizeof m, "closed %s - %d frame(s) freed, %d still referenced by %s",
             what.c_str(), total - kept, kept, who.c_str());
    fprintf(stderr, "%s\n", m);
    toast(m);
    g_lastCloseNote = m;
}

// Close every frame of a stack plus its SeqInfo, and stop everything that
// would quietly regrow it: the sequence loader (pumpSequence stamps
// seqLoadingId on frames as they land), the remote prefetch queue, the
// linearity row, the server temporal result. announce=false is for
// closeBatch/closeSeries, which speak ONCE for their whole set instead.
static void closeStack(int seqId, bool announce = true) {
    if (seqId == 0) return;
    if (app.seqLoadingId == seqId) { stopSequenceLoader(); app.seqLoadingId = 0; }
    {   // queued remote prefetches. The ONE job the worker may be running right
        // now cannot be removed here - pumpRemoteFetch drops its result on
        // arrival instead (seqInfo(d.seqId) == nullptr). Both are needed.
        std::lock_guard<std::mutex> lk(app.rfMtx);
        int removed = 0;
        size_t freed = 0;
        for (auto it = app.rfQueue.begin(); it != app.rfQueue.end();)
            if (it->seqId == seqId) { freed += it->bytes; it = app.rfQueue.erase(it); removed++; }
            else ++it;
        app.rfPending -= removed;
        app.rfBytesInFlight -= std::min(app.rfBytesInFlight.load(), freed);
        if (app.rfPending <= 0) {
            app.rfPending = 0; app.rfTotal = 0; app.rfFetched = 0; app.rfBytesInFlight = 0;
        }
    }
    // §4: when another doc still references some of these pixels, the close
    // says so BEFORE the memberships go - counted in both directions (how many
    // die, how many live on), the same shape as derive's count promise. A
    // silent close would read as "freed" while a sharer keeps the pixels
    // alive. When this close is ONE MEMBER of a larger action, the caller has
    // already scanned and spoken over its whole set (announce=false):
    // announcing per member here named survivors the same action then freed.
    if (announce) {
        const App::SeqInfo* si = seqInfo(seqId);
        sayCloseNotice(framesOfSeq(seqId), { seqId },
                       "\"" + (si ? si->name : std::string("stack")) + "\"");
    }
    // rbOpenQueue entries carry no seqId yet, so a stack whose FOLDER is still
    // queued may open later regardless - accepted; closeBatch removes those by
    // batchId, which the queue does know.
    closeImages(framesOfSeq(seqId));
    for (auto it = app.seqs.begin(); it != app.seqs.end(); ++it)
        if (it->id == seqId) { app.seqs.erase(it); break; }
    // the stack is gone, so it is not a member of anything anymore; a series
    // that held nothing else goes with it (docs/series-plan.md invariant 3)
    removeFromSeries(seqId);
    pruneEmptySeries();
    for (auto it = app.lin.rows.begin(); it != app.lin.rows.end();)
        if (it->seqId == seqId) it = app.lin.rows.erase(it);
        else ++it;
    // ...and the fit that row was part of goes with it: dropping the dot while
    // the line and the "%d points" count still include it is worse than saying
    // "press Compute".
    linFitStale();
    app.temporal[0].seqId = app.temporal[1].seqId = -1;
    if (app.srvTemporal.seqId == seqId) app.srvTemporal = App::ServerTemporal{};
    if (app.srvTemporalB.seqId == seqId) app.srvTemporalB = App::ServerTemporal{};
    // the SeqInfo and any series this stack emptied are gone only now, so the
    // closeImages audit above could not see it - ask again
    aSetAuditBindings();
}

// Close a batch: every stack and loose frame in it, plus the queued opens that
// would resurrect it the moment the fetcher goes idle. A series lives in
// exactly one batch, so the batch's series go too - Close on a batch is the
// canon's "discard the contents", not "ungroup".
static void closeBatch(int batchId) {
    // The batch's SETS go with it - a set is a node of its batch (ras 1.4).
    // Counted, never silent: bindings are records, and records do not vanish
    // without a number attached. Members OUTSIDE this batch are untouched -
    // their pixels were never this set's to take.
    {
        int drop = 0, binds = 0;
        for (auto it = app.analysisSets.begin(); it != app.analysisSets.end();)
            if (it->batchId == batchId) {
                for (const auto& r : it->roles) if (r.bound()) binds++;
                drop++;
                it = app.analysisSets.erase(it);
            } else {
                ++it;
            }
        for (auto p = app.aSetBindPending.begin(); p != app.aSetBindPending.end();)
            p = p->batchId == batchId ? app.aSetBindPending.erase(p) : std::next(p);
        if (drop)
            fprintf(stderr, "close batch: %d set(s) dropped, %d binding(s) with "
                            "them - members outside the batch untouched\n",
                    drop, binds);
    }
    for (auto it = app.series.begin(); it != app.series.end();)
        if (it->batchId == batchId) {
            if (app.curSeriesId == it->id) app.curSeriesId = 0;
            it = app.series.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
    std::vector<int> seqIds;
    for (const auto& d : app.images)
        if (d->batchId == batchId && d->seqId != 0 &&
            std::find(seqIds.begin(), seqIds.end(), d->seqId) == seqIds.end())
            seqIds.push_back(d->seqId);
    // §4, scoped to THIS user action: ONE survival scan over everything the
    // batch close takes - member stacks AND loose frames - so the notice can
    // never name a survivor this same close then frees, and a loose member's
    // survivors are said too (closeImages itself carries no notice).
    {
        std::vector<int> closing;
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->batchId == batchId) closing.push_back(i);
        sayCloseNotice(closing, seqIds, "batch \"" + batchNameOf(batchId) + "\"");
    }
    for (int s : seqIds) closeStack(s, false);
    std::vector<int> loose;
    for (int i = 0; i < (int)app.images.size(); i++)
        if (app.images[i]->batchId == batchId) loose.push_back(i);
    closeImages(loose);
    for (auto it = app.rbOpenQueue.begin(); it != app.rbOpenQueue.end();)
        if (it->batchId == batchId) it = app.rbOpenQueue.erase(it);
        else ++it;
    for (auto it = app.seqQueue.begin(); it != app.seqQueue.end();)
        if (it->batchId == batchId) it = app.seqQueue.erase(it);
        else ++it;
    // ...and the sweeps the picker accepted INTO this batch, which are only a
    // NOTE until every load drains. closeAll purges them for exactly this
    // reason ("they named stacks being thrown away") and this function already
    // purges two queues by batchId - it just missed the third container. Batch
    // ids are never reused, so a surviving note resolves against nothing: a red
    // "sweep: 0 stack(s) ... N could not be matched" about an Open the user
    // deliberately discarded, and if the folder was reopened meanwhile the note
    // still points at the dead batch, so the new one gets no series either.
    for (auto it = app.seriesPending.begin(); it != app.seriesPending.end();)
        if (it->batchId == batchId) it = app.seriesPending.erase(it);
        else ++it;
    if (app.seriesPending.empty()) app.groupStacks.clear();
    if (app.loadBatchId == batchId) app.loadBatchId = 0;
    pruneEmptyBatches();       // the queue purge above may have freed this batch
}

// Ctrl+W: the stack when the frame is in one (the canon), the image otherwise.
// frameOnly = the Ctrl+Alt+W escape hatch: just this one frame.
static void closeCurrent(bool frameOnly = false) {
    ImageDoc* im = cur();
    if (!im) return;
    if (im->seqId != 0 && !frameOnly) { closeStack(im->seqId); return; }
    int seqId = im->seqId;
    // §4 for the two one-frame closes (Ctrl+W on a loose frame, Ctrl+Alt+W):
    // one membership goes here without passing closeStack, and pixels that
    // live on elsewhere must still be said to - a silent close reads as freed.
    // Said BEFORE closeImages: the notice reads the doc it is about.
    sayCloseNotice({ app.current }, {}, "\"" + im->name + "\"");
    closeImages({ app.current });
    // the escape hatch emptied the stack: drop the SeqInfo and its bookkeeping
    // too, or a zero-frame stack haunts the linearity table
    if (seqId != 0 && framesOfSeq(seqId).empty()) closeStack(seqId);
}

// Drop batches nothing references anymore: no image, no queued group, no
// pending remote open, not the load target. Called after every close and after
// every move - an empty Files heading is a lie about what is open. The preview
// pseudo-batch stays: its emptiness is its normal state between previews.
static void pruneEmptyBatches() {
    for (auto it = app.batches.begin(); it != app.batches.end();) {
        bool used = it->name == "preview" || app.loadBatchId == it->id;
        if (!used)
            for (const auto& d : app.images)
                if (d->batchId == it->id) { used = true; break; }
        if (!used)   // a set is a NODE of its batch (ras 1.4): the heading stays
            for (const auto& s : app.analysisSets)
                if (s.batchId == it->id) { used = true; break; }
        if (!used)
            for (const auto& q : app.seqQueue)
                if (q.batchId == it->id) { used = true; break; }
        if (!used)
            for (const auto& q : app.rbOpenQueue)
                if (q.batchId == it->id) { used = true; break; }
        if (!used) {
            it = app.batches.erase(it);
            app.imagesRev++;
        } else {
            ++it;
        }
    }
}

// Move a STACK between batches - the whole stack, per the canon's containment
// (frame ⊂ stack ⊂ batch): frames never move between batches one by one.
// A member moved out ALONE leaves its series: series ⊂ batch is strict, and the
// canon says say so rather than forbid it. Moving the series itself is a
// different operation (it carries every member and its own batchId along).
static void moveStackToBatch(int seqId, int batchId) {
    if (App::Series* S = seriesOfStack(seqId))
        if (S->batchId != batchId) {
            std::string sn = S->name;
            App::SeqInfo* si = seqInfo(seqId);
            removeFromSeries(seqId);
            pruneEmptySeries();
            // The membership just changed, so whatever was computed measured a
            // different set (docs/terminology.md: editing members / values /
            // unit / parameter discards the computed fit). The Linearity panel's
            // only freshness keys are the series id and fitValid, and a move
            // leaves the series alive - so both stayed true, the fit table and
            // the "%d points" count went on describing the old membership, and
            // both plots went on DRAWING the departed stack's point under a
            // member table that no longer listed it. Every sibling edit already
            // does this: closeStack, the seqctx join, the Files "leave series",
            // the modal's Save.
            //
            // moveSeriesToBatch sets S->batchId FIRST, so its per-member calls
            // never reach this branch: a series moved WHOLE keeps its fit, which
            // is right - nothing about its membership changed.
            linFitStale();
            toast((si ? si->name : std::string("stack")) + " left series \"" + sn +
                  "\" (moved to another batch)");
        }
    for (int idx : framesOfSeq(seqId)) app.images[idx]->batchId = batchId;
    app.imagesRev++;
    pruneEmptyBatches();
}
// ---- what the Files panel's series row does (docs/series-plan.md §4) --------
// Move a SERIES between batches: every member goes with it and so does the
// series itself, so strict containment holds at every instant. The series'
// batchId is set FIRST on purpose - moveStackToBatch drops a member whose
// series stays behind, which is the right rule for a lone stack and exactly
// the wrong one here.
static void moveSeriesToBatch(int seriesId, int batchId) {
    App::Series* S = seriesById(seriesId);
    if (!S || batchId == 0 || S->batchId == batchId) return;
    std::vector<int> ids;
    for (const auto& m : S->members) ids.push_back(m.seqId);
    S->batchId = batchId;
    for (int s : ids) moveStackToBatch(s, batchId);
    app.imagesRev++;
}
// UNGROUP (解散): take the fence away and leave everything standing. This is
// NOT Close - nothing is discarded, every stack stays open exactly where it is
// and merely stops being part of a sweep. The canon lists the two side by side
// because they are different operations, not two words for one.
static void ungroupSeries(int seriesId) {
    for (auto it = app.series.begin(); it != app.series.end(); ++it)
        if (it->id == seriesId) {
            std::string n = it->name;
            int members = (int)it->members.size();
            if (app.curSeriesId == seriesId) app.curSeriesId = 0;
            app.series.erase(it);
            app.imagesRev++;
            toast("ungrouped \"" + n + "\": " + std::to_string(members) +
                  " stack(s) stay open");
            break;
        }
    if (!app.curSeriesId && !app.series.empty()) app.curSeriesId = app.series.front().id;
    aSetAuditBindings();   // a role bound to the ungrouped series says so now
}
// Close a series: the canon's Close, which discards the CONTENTS. Each member's
// closeStack removes it from the series, and the last one takes the series with
// it (pruneEmptySeries) - the erase below is only for a series whose members
// were all gone already.
static void closeSeries(int seriesId) {
    App::Series* S = seriesById(seriesId);
    if (!S) return;
    std::string n = S->name;
    std::vector<int> ids;
    for (const auto& m : S->members) ids.push_back(m.seqId);
    {   // §4, scoped to THIS user action: one scan across every member stack,
        // so no member is called a survivor of a close that takes it too
        std::vector<int> closing;
        for (int s : ids)
            for (int idx : framesOfSeq(s)) closing.push_back(idx);
        sayCloseNotice(closing, ids, "series \"" + n + "\"");
    }
    for (int s : ids) closeStack(s, false);
    for (auto it = app.series.begin(); it != app.series.end(); ++it)
        if (it->id == seriesId) {
            if (app.curSeriesId == seriesId) app.curSeriesId = 0;
            app.series.erase(it);
            app.imagesRev++;
            break;
        }
    toast("closed series \"" + n + "\": " + std::to_string(ids.size()) + " stack(s)");
    aSetAuditBindings();
}
// "seqctx > Series > <name>": the one join that does not go through the modal.
// A function like every other command behind a Files row, so the selftest can
// press it. Returns false (and a reason) when strict containment refuses.
//
// The value comes from the stack's NAME and is said out loud - the menu item
// showed it before the click, and the toast repeats both the number and the text
// it was read out of. A name with nothing numeric in it joins UNSET, never at 0.
static bool seriesJoinFromMenu(int seriesId, int seqId, std::string* msgOut) {
    App::Series* S = seriesById(seriesId);
    App::SeqInfo* si = seqInfo(seqId);
    if (!S || !si) return false;
    std::string from;
    double v = extractLevelFromName(si->name, &from);
    // the series it is LEAVING: taking the last member out of one empties it
    const App::Series* was = seriesOfStack(seqId);
    int wasId = was && was->id != S->id ? was->id : 0;
    std::string wasName = wasId ? was->name : std::string();
    if (!addToSeries(S->id, seqId, v)) {
        if (msgOut)
            *msgOut = si->name + " is not in batch \"" + batchNameOf(S->batchId) +
                      "\" - Move to batch first";
        return false;
    }
    std::string msg = si->name + " joined series \"" + S->name + "\"";
    if (std::isfinite(v)) {
        char b[96];
        snprintf(b, sizeof b, " at %.6g%s%s", v, S->unit[0] ? " " : "", S->unit);
        msg += b;
        if (!from.empty()) msg += " (read from \"" + from + "\")";
    } else {
        msg += " with NO value yet - set it in Edit...";
    }
    // Every other caller of addToSeries prunes; this one did not, so joining the
    // sole member of one series into another left the first behind with zero
    // members - the state plan §1 invariant 3 forbids and seriesAudit rejects.
    pruneEmptySeries();
    if (wasId && !seriesById(wasId)) msg += "; series \"" + wasName + "\" is now empty and gone";
    linFitStale();          // the fit below was measured over the old membership
    if (msgOut) *msgOut = msg;
    return true;
}
// A standalone frame (no stack) hangs off the batch directly and moves alone.
// A frame MEMBER moved out alone leaves its series, exactly like a stack
// member (series ⊂ batch is strict, and the canon says say so, not forbid).
static void moveImageToBatch(int imageIdx, int batchId) {
    if (imageIdx < 0 || imageIdx >= (int)app.images.size()) return;
    ImageDoc* d = app.images[imageIdx].get();
    if (d->seqId == 0)
        if (App::Series* S = seriesOfFrame(d->uid))
            if (S->batchId != batchId) {
                std::string sn = S->name;
                removeFrameFromSeries(d->uid);
                pruneEmptySeries();
                linFitStale();   // the fit measured a membership that just changed
                toast(d->name + " left series \"" + sn + "\" (moved to another batch)");
            }
    d->batchId = batchId;
    app.imagesRev++;
    pruneEmptyBatches();
}
static void closeAll() {
    // FIRST: stop everything that produces images, or Close All does not close.
    // closeStack and closeBatch already know this - a queue the close leaves
    // behind reopens the closed content the moment the loader/fetcher goes
    // idle, stamped with a batchId app.batches no longer contains, so the
    // frames are resident, unnamed and unreachable by pruneEmptyBatches.
    // Measured before this line existed: Close All over 30 queued stacks left
    // running=1 / queued=28, and draining brought back 86 frames in 28 stacks
    // with batches=0. This is the one place that knows what a Close has to
    // stop; loadSession used to open-code half of it two lines above its own
    // closeAll() and inherits the whole rule now.
    stopSequenceLoader();
    app.seqLoadingId = 0;
    app.seqQueue.clear();
    app.seqRestore.clear();
    app.pendingAvg.clear();           // they name stacks being thrown away...
    app.avgRestore.clear();           // ...and paths that will not be open either
    app.dtRestore.clear();            // ...and the preprocessor recipes with them
    app.rbOpenQueue.clear();
    for (auto& bp : app.browsePanels) bp->pendingOpen.clear();
    app.ana.img = nullptr;
    for (int k = 0; k < 2; k++) {     // both A and B slots
        app.hist[k] = App::HistState{};
        app.proj[k] = App::ProjState{};
        app.temporal[k] = App::TemporalState{};
    }
    app.histExtra.clear();            // ...and every lettered one
    app.projExtra.clear();
    app.temporalExtra.clear();
    app.abSlot1Live = false;
    app.texLru.clear();
    app.imagesRev++;
    for (auto& d : app.images)
        if (d->tex) glDeleteTextures(1, &d->tex);
    app.images.clear();
    app.seqs.clear();
    // Series name stacks that no longer exist; they go with them (and with the
    // batches below - a series cannot outlive its batch). Pending restores name
    // stacks of the list being thrown away, so they go too.
    app.series.clear();
    app.curSeriesId = 0;
    app.seriesRestore.clear();
    app.seqLevelLegacy.clear();
    app.seriesPending.clear();                  // they named stacks being thrown away
    app.groupStacks.clear();                    // ...and those stacks are gone too
    // sets name members and batches of the list being thrown away; the pending
    // block entries name paths that will not be open either (ras 5.2)
    app.analysisSets.clear();
    app.aSetBindPending.clear();
    app.aSetRestore.clear();
    // The batches go with their contents: an empty batch that survives Close
    // All keeps its NAME reserved, so reopening the same folder came back as
    // "multi (2)" - the uniquifier colliding with a ghost.
    app.batches.clear();
    app.loadBatchId = 0;
    app.previewUid = 0;
    app.previewFiles.clear();
    app.previewFrames = 0;
    app.previewLabel.clear();
    // ...and the three this used to leave standing. dropPreview() clears all
    // seven; Close All cleared four, so the machine and port of a preview that
    // no longer exists survived a Close All and were handed to the next step.
    app.previewIndex = 0;
    app.previewHost.clear();
    app.previewPort = 0;
    app.current = -1;
    // compare state refers to docs that no longer exist; leaving it would let a
    // later file with the same name silently become B again
    app.compareMode = App::CmpOff;
    app.compareBUid = 0; app.compareB.clear(); app.compareBSeq = -1;
    app.cmpSlotRestore.clear();       // the same rule for the SLOT wants: a
                                      // stale want from a dead session would
                                      // graft a letter onto whichever open
                                      // next matches its path (every sibling
                                      // restore queue is cleared above)
    // in-flight remote fetches belong to the OLD list: bump the generation so
    // their results are dropped instead of grafting orphan frames onto the new one
    app.rfGen++;
    {
        std::lock_guard<std::mutex> lk(app.rfMtx);
        app.rfQueue.clear();
        app.rfDone.clear();
        app.rfPending = 0;
        app.rfTotal = 0;
        app.rfFetched = 0;
        app.rfBytesInFlight = 0;
    }
}
