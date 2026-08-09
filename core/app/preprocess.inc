// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---------------------------------------------- AnalysisPreProcessor: the product
//
// docs/analysis-layers.md §5. Three of the four analyzer kinds have substance;
// this is the one that existed only as a declaration. What §5 asks of it:
//
//     AnalysisSet / layer data -> frame / stack / series / batch
//     "生成物は一級のデータで、§3 / §4 がそのまま掛かる - それがこの設計の
//      配当で、中間生成物のために特別な解析を書かない"
//
// So the product of the detrend stage is an ORDINARY STACK. Histogram,
// Projection, ROIs, Temporal, every Specific analyzer and every SetAnalyzer
// role apply to it because there is nothing here for them to special-case -
// which is exactly why 判断6 could forbid a detrend option on the General
// panels without taking anything away from anybody.
//
// -------- the invariant a computed picture owes (`0e4d4ec`, §5) --------------
// "計算で作った frame は撮ったものの顔をしてはならない。由来を名前と note の
// 両方に、ファイルは持たず、セッションにはレシピを書いて再計算する" - raised by
// §5 from the frame average's own rule to the rule for EVERY preprocessor
// product. All four halves are here, and the third one is load-bearing:
// src->path stays EMPTY on every frame of the product, so the session writes no
// `image` line for it and cannot hand back a capture under this name.
//
// -------- why the surface is fitted ONCE, on the temporal mean ---------------
// docs/flat-field-stats.md 約束3: "sigma_t には適用しない ... detrend は空間
// 統計にだけ掛かる段とする". A stack is detrended by fitting the surface on the
// stack's per-pixel temporal mean and removing THAT ONE SURFACE from every
// frame. Removing a per-pixel constant from every frame cannot change a
// per-pixel temporal variance, so sigma_t comes out of the product unchanged
// and the promise is kept by construction rather than by a warning in a
// tooltip. Fitting per frame is the obvious loop and would have put fit noise
// into sigma_t - a quantity this stage is not allowed to touch at all.
//
// The fold comes from computeStackStats, the accumulator every stack-level
// number in the program comes out of, so the mean this fits against is the
// mean the Temporal panel prints (docs/analysis-layers.md §6: "実装は 1-3 の
// 再利用").

// Where in the settled chain this stage sits, as the product states it. Stages
// 0 / 1a / 2b / 3b are not built: skipping an unbuilt stage is legal, and the
// value of the cutoff field is where that skip stops being silent (約束1).
static const char* kDetrendChainNote =
    "chain (docs/flat-field-stats.md \"順序\"): black level and saturation "
    "masking are not built, dark subtraction is a reader's today, this is "
    "stage 3, defect masking is not built, statistics follow";

// One tooltip for every door, the AVG_TIP precedent - the menu items must not
// describe the same operation three different ways.
static const char* DETREND_TIP =
    "Produce a DETRENDED STACK: fit a low-frequency surface per plane on\n"
    "this stack's temporal mean, remove it from every frame, and open the\n"
    "result as a stack of its own. The source stays open beside it.\n"
    "\n"
    "The surface is fitted ONCE and removed from every frame, so sigma_t is\n"
    "unchanged - the stage touches spatial statistics only. Each plane's\n"
    "mean level is preserved, so a ratio taken against the level still is.\n"
    "The result is computed, not captured, and says so in its name, in its\n"
    "note, and in the cutoff line of anything measured over it.";

// The one door. Returns false with `err` worded for a toast.
// `expect` = a session recipe being replayed (the name it was saved under).
static bool detrendStackProduct(int seqId, const DetrendSpec& sp, std::string& err,
                                const std::string& wantName = std::string()) {
    if (sp.method == DTR_NONE) {
        // Refusing a no-op product rather than making one: "raw" is a value of
        // the stage, and the data that carries it is the stack you already
        // have. A second identical stack whose only difference was a claim
        // would be the exact confusion 約束1 exists to prevent.
        err = "detrend: the method is \"raw\", which is what this stack already is";
        return false;
    }
    const App::SeqInfo* si = seqInfo(seqId);
    if (!si) { err = "detrend: that stack is not open"; return false; }
    if (si->dt.made()) {
        // One stage, one cutoff. Two removals stacked could not be stated
        // honestly in one cutoff string, and the recipe of the second could
        // not name the first (a product has no file path to be named by).
        err = "detrend: \"" + si->name + "\" is already a detrend product - "
              "change the method instead of removing a surface twice";
        return false;
    }
    const std::string srcName = si->name;
    const int expected = si->expectedFrames;
    const int srcRev = si->stackRev;
    std::vector<int> fr = framesOfSeq(seqId);
    const ImageDoc* ref = nullptr;
    std::vector<const ImageDoc*> use;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->src->remoteStep > 1 || d->px().empty()) continue;   // previews lie
        if (!ref) ref = d;
        if (d->w != ref->w || d->h != ref->h || d->ch != ref->ch) continue;
        use.push_back(d);
    }
    if (!ref) { err = "detrend: no frame of \"" + srcName + "\" is loaded yet"; return false; }
    const int W = ref->w, H = ref->h, C = ref->ch;
    // A product is a SECOND copy of the pixels - that is what makes it ordinary
    // data, and it is also what makes it expensive. The stack loader already
    // stops at a budget rather than at an allocation failure (seqMemBudget), so
    // this asks the same question before it starts rather than dying halfway
    // with half a stack minted. Refused out loud, with both numbers.
    {
        const size_t need = (size_t)W * H * C * sizeof(float) * use.size();
        const size_t room = seqMemBudget() - std::min(seqMemBudget(), claimedImageBytes());
        if (need > room) {
            char b[224];
            snprintf(b, sizeof b,
                     "detrend: the product would need %.1f GB and %.1f GB of the memory "
                     "budget is left - a product is a second copy of the pixels",
                     need / 1073741824.0, room / 1073741824.0);
            err = b;
            return false;
        }
    }
    const int refCfa = ref->cfa, refCfaPat = ref->cfaPattern, refLut = ref->displayLut;
    const bool refColorize = ref->cfaColorize;
    const float refBlack = ref->black, refWhite = ref->white;
    const int refBatch = ref->batchId;
    const std::string headPath = app.images[fr.front()]->src->path;
    const std::string headMember = app.images[fr.front()]->src->member;
    DetrendPlanes P = detrendPlanesOf(*ref);
    if (!P.err.empty()) { err = "detrend: " + P.err; return false; }

    // The fold: the SAME accumulator the Temporal panel and the frame average
    // use, so the surface is fitted against the mean the rest of the program
    // would print for this stack.
    std::vector<float> fold;
    StackStats st = computeStackStats(seqId, 0, 0, W, H, &fold, FOLD_MEAN);
    if (!st.valid) { err = "detrend: \"" + srcName + "\": " + st.err; return false; }
    DetrendFit F = detrendFitT(fold.data(), W, H, C, P.of.data(), P.nPl, sp);
    if (!F.ok) { err = "detrend: " + F.err; return false; }
    // The figure AFTER, measured on the same picture the figure before was
    // measured on (約束5: 除去前後の両方を出す). It is what says the removal did
    // what it claimed, and on the product it is what any later reader sees.
    std::vector<float> foldAfter = fold;
    detrendApply(F, P.of.data(), foldAfter.data(), foldAfter.size());
    DetrendReport after = detrendProbeT(foldAfter.data(), W, H, C, P.of.data(), P.nPl);

    char whenTs[32] = "";
    {
        time_t nowT = time(nullptr);
        if (struct tm* lt = localtime(&nowT))
            strftime(whenTs, sizeof whenTs, "%Y-%m-%d %H:%M:%S", lt);
    }
    const int n = st.frames;
    const int N = std::max(expected, n);
    const std::string ofN = n < N ? std::to_string(n) + " of " + std::to_string(N)
                                  : std::to_string(n);
    const std::string cutoff = detrendCutoffText(sp);
    const std::string shortSpec = detrendShortText(sp);
    // The NAME - what the Files panel, the title bar and every stack combo show.
    const std::string prodName = wantName.empty()
        ? srcName + "  detrended (" + shortSpec + ")"
        : wantName;
    // The NOTE - what the Inspector and the Files tooltip show. Every field a
    // number taken off this stack would need in a report, in words.
    std::string note = "DETREND PRODUCT of stack \"" + srcName + "\" - computed, not "
                       "captured. cutoff: " + cutoff +
                       ". The surface was fitted ONCE on the per-pixel temporal mean of " +
                       ofN + " frame(s) and removed from every frame, so sigma_t is "
                       "unchanged (docs/flat-field-stats.md 約束3); each plane's mean "
                       "level is added back, so mean_spatial is unchanged too. shading";
    for (int p = 0; p < P.nPl; p++) {
        char b[192];
        const DetrendPlaneReport& a = F.before.pl[p];
        const DetrendPlaneReport& z = after.pl[p];
        if (P.nPl == 4) { snprintf(b, sizeof b, " %s", CFA_CH_NAMES[p]); note += b; }
        else if (P.nPl > 1) { snprintf(b, sizeof b, " ch%d", p); note += b; }
        if (a.pctOk && z.pctOk)
            snprintf(b, sizeof b, " %.4g%% -> %.4g%% (p-p %.4g -> %.4g DN)%s",
                     a.pct, z.pct, a.ppDn, z.ppDn, p + 1 < P.nPl ? "," : "");
        else
            snprintf(b, sizeof b, " p-p %.4g -> %.4g DN (no level to take a ratio "
                                  "against)%s", a.ppDn, z.ppDn, p + 1 < P.nPl ? "," : "");
        note += b;
        if (F.degenerate[p])
            note += " [the fit was degenerate here and fell back to the plane mean]";
    }
    if (sp.divide)
        note += "; DIVIDED, so the noise is scaled by 1/s and the spatial variance is "
                "no longer uniform - the estimators downstream assume it is "
                "(docs/flat-field-stats.md 約束6)";
    if (st.nonFinite)
        note += "; " + std::to_string((unsigned long long)st.nonFinite) +
                " non-finite sample(s) were excluded from the fold the surface was "
                "fitted on, per pixel";
    if (n < N)
        note += "; the source stack is PARTLY LOADED - the surface comes from the " +
                std::to_string(n) + " frame(s) present, not from all " + std::to_string(N);
    if (headPath.empty())
        note += "; no file path behind the source stack, so a session cannot recompute "
                "this product";
    note += "; " + std::string(kDetrendChainNote);
    note += "; computed " + std::string(whenTs);

    // ---- mint the stack -----------------------------------------------------
    App::SeqInfo out;
    out.id = app.nextSeqId++;
    out.name = prodName;
    out.expectedFrames = (int)use.size();
    out.cfaType = refCfa;
    out.cfaPattern = refCfaPat;
    out.dt.spec = sp;
    out.dt.ofPath = headPath;
    out.dt.ofMember = headMember;
    out.dt.srcName = srcName;
    out.dt.when = whenTs;
    out.dt.srcSeqId = seqId;
    out.dt.srcRev = srcRev;
    out.dt.nPl = P.nPl;
    for (int p = 0; p < P.nPl; p++) {
        out.dt.ppBefore[p] = F.before.pl[p].ppDn;
        out.dt.ppAfter[p] = after.pl[p].ppDn;
        out.dt.pctBefore[p] = F.before.pl[p].pct;
        out.dt.pctAfter[p] = after.pl[p].pct;
        out.dt.pctOk[p] = F.before.pl[p].pctOk && after.pl[p].pctOk;
    }
    const int newSeq = out.id;
    app.seqs.push_back(out);            // RESEATS: no SeqInfo* survives this line

    int made = 0;
    for (size_t k = 0; k < use.size(); k++) {
        // Copy out everything read off `use[k]` BEFORE addImage: push_back can
        // reallocate app.images and every pointer into it dangles (the frame
        // average's own rule, and the one defect of this class we have had).
        const std::string baseNm = use[k]->name;
        const int idx = use[k]->seqIndex;
        std::vector<float> px = use[k]->px();
        auto d = std::make_unique<ImageDoc>();
        FrameSource& S = *d->src;
        S.w = W; S.h = H; S.ch = C;
        // NOT the source dtype: a surface has been subtracted, the values are
        // no longer integers, and calling them u16 would misdescribe every
        // number printed off this stack (the frame average's same call).
        S.dtype = "f32";
        S.data = std::move(px);
        detrendApply(F, P.of.data(), S.data.data(), S.data.size());
        // src->path STAYS EMPTY. This is the load-bearing line: give it the
        // source's path and isSavedLine writes an ordinary `image` line, and
        // the next session hands back a CAPTURE under this name with the note
        // that said otherwise gone.
        d->cfa = refCfa; d->cfaPattern = refCfaPat; d->cfaColorize = refColorize;
        d->displayLut = refLut;
        d->syncMirrors();          // addImage computes vmin/vmax and syncs again
        d->batchId = refBatch;
        d->name = baseNm + "  detrended";
        d->note = note;
        d->seqId = newSeq;
        d->seqIndex = idx;
        d->texDirty = true;
        addImage(std::move(d));
        ImageDoc* mk = app.images.back().get();
        // A detrended frame is the source frame minus a low-frequency surface
        // with the level added back, so it lives on the source's scale - an
        // auto-range here would stretch it over its own residue (the frame
        // average's reasoning, same fix).
        mk->black = refBlack; mk->white = refWhite;
        made++;
    }
    if (App::SeqInfo* nsi = seqInfo(newSeq)) nsi->expectedFrames = made;
    app.imagesRev++;
    fprintf(stderr, "detrend: \"%s\" -> \"%s\": %s, %d frame(s), %dx%d %dch, %d plane(s), "
                    "shading %.6g%% -> %.6g%% (p-p %.6g -> %.6g DN)\n",
            srcName.c_str(), prodName.c_str(), cutoff.c_str(), made, W, H, C, P.nPl,
            F.before.pl[0].pct, after.pl[0].pct, F.before.pl[0].ppDn, after.pl[0].ppDn);
    {
        char tb[320];
        if (F.before.pl[0].pctOk)
            snprintf(tb, sizeof tb, "detrend: \"%s\" - %s; shading %.4g%% -> %.4g%%",
                     srcName.c_str(), cutoff.c_str(), F.before.pl[0].pct, after.pl[0].pct);
        else
            snprintf(tb, sizeof tb, "detrend: \"%s\" - %s; shading p-p %.4g -> %.4g DN",
                     srcName.c_str(), cutoff.c_str(), F.before.pl[0].ppDn, after.pl[0].ppDn);
        toast(tb);
    }
    if (n < N)
        toast("detrend: " + std::to_string(n) + " of " + std::to_string(N) +
              " frames of \"" + srcName + "\" are loaded - the product says so", true);
    return true;
}

// The session's recipes, replayed behind the same "everything has landed" gate
// the frame averages use: a surface fitted while the stack is still filling
// would be a surface of however many frames had arrived by then.
static void pumpDetrendProducts() {
    if (app.dtRestore.empty()) return;
    if (!loadingSettled()) return;
    std::vector<App::DetrendRestore> todo;
    todo.swap(app.dtRestore);
    for (const auto& r : todo) {
        int found = 0;
        for (const auto& d : app.images)
            if (d->seqId != 0 && d->src->path == r.path &&
                (r.member.empty() || d->src->member == r.member)) {
                const App::SeqInfo* s = seqInfo(d->seqId);
                if (s && s->dt.made()) continue;      // never the product itself
                found = d->seqId;
                break;
            }
        if (!found) {
            fprintf(stderr, "session: a detrend product of \"%s\", which is not open - "
                            "not recomputed\n", r.path.c_str());
            continue;
        }
        std::string err;
        if (!detrendStackProduct(found, r.spec, err, r.name)) toast(err, true);
    }
}
