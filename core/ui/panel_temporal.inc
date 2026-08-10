// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// ---- sigma_fpn, and the two things that make it readable -------------------
// #57 judgment 2 (docs/flat-field-stats.md (b)) made the sigma_fpn ROW ITSELF
// the corrected quantity. A corrected number that does not say what was taken
// off it cannot be checked, and a clamped one that prints a bare 0 is worse
// than useless - it reads as "this sensor has no fixed pattern" when what
// happened is "this stack cannot see one under its own temporal floor". So the
// amount and the clamp travel WITH the value, and this file says it once:
// three tables draw these rows (local, server, server-detached) and they had
// already drifted into two different NAMES for one quantity ("sigma_s" on
// screen, "sigma_fpn" in the export and on the server's own table).
static const char* FPN_TIP =
    "sigma_fpn = spatial sigma of the time-averaged frame, ddof=1, MINUS the\n"
    "temporal residual still in that mean:\n"
    "    sigma_fpn^2 = max(0, var_spatial(mean) - fpn_corr^2)\n"
    "    fpn_corr    = sqrt(mean(s_t,i^2 / n_i))\n"
    "Averaging n frames drops the temporal noise by sqrt(n) and no further;\n"
    "without the subtraction that leftover is read as fixed pattern (at n=16,\n"
    "sigma_t=10 DN a true 5 DN pattern reads 5.59 DN).\n"
    "Still includes scene detail unless the region is flat.\n"
    "docs/flat-field-stats.md (b) - #57 judgment 2";

// The four rows every Temporal table prints, with sigma_fpn's declaration.
// A clamped plane says so IN ITS CELL: the canon's guard rule is that a clamp
// is never a silent 0 (flat-field-stats.md (a)).
static void temporalNoiseRows(int nPl, const double* sigT, const double* fpn,
                              const double* corr, const bool* clamped,
                              const double* tot) {
    auto row = [&](const char* k, const double* v) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
        for (int p = 0; p < nPl; p++) { ImGui::TableNextColumn(); textNum("%.6g", v[p]); }
    };
    row("temporal noise (sigma_t)", sigT);
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("fixed pattern (sigma_fpn)");
    bool hov = ImGui::IsItemHovered();
    for (int p = 0; p < nPl; p++) {
        ImGui::TableNextColumn();
        if (clamped[p]) ImGui::TextColored(AB_AMBER, "0 (clamped)");
        else            textNum("%.6g", fpn[p]);
        hov |= ImGui::IsItemHovered();
    }
    if (hov) ImGui::SetTooltip("%s", FPN_TIP);
    row("  temporal residual subtracted (fpn_corr)", corr);
    row("total (quadrature)", tot);
}

// ...and the sentence a clamped plane owes the reader, under the table.
static void temporalClampNote(int nPl, const bool* clamped) {
    std::string pl;
    for (int p = 0; p < nPl; p++)
        if (clamped[p]) {
            if (!pl.empty()) pl += ", ";
            pl += nPl > 1 ? LIN_PLANES[p] : "value";
        }
    if (pl.empty()) return;
    ImGui::TextColored(AB_AMBER,
        "sigma_fpn clamped at 0 (%s): the temporal residual is not below the "
        "spatial spread,\nso this stack resolves no fixed pattern above its own "
        "temporal floor. The 0 is a\nstatement about the measurement, not a flat "
        "sensor - measure more frames or a flatter field.", pl.c_str());
}

// Server-computed temporal stats: shown for a remote stack under a policy that
// lets the server compute. The numbers come over the wire in seconds; they are
// never quietly recomputed locally (that would change a displayed measurement,
// and a partial local stack would make it WORSE). The tag says where and over
// how many frames, always.
static bool drawServerTemporal(const App::SeqInfo* si) {
    App::ServerTemporal& S = app.srvTemporal;
    if (!si || !serverComputes(*si) || S.seqId != si->id) return false;
    ImGui::Text("Temporal");
    ImGui::SameLine();
    if (S.pending) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s - measuring...]",
                           S.host.empty() ? PEER_HERE : S.host.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("computing on the server over all %d frames", si->expectedFrames);
        return true;
    }
    if (!S.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[server failed]");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", S.err.c_str());
        ImGui::TextDisabled("frames arrive locally in the background; the panel will\n"
                            "switch to local computation once enough are here.");
        return false;                 // fall through to the local path
    }
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s, %d frames, %s, %s]",
                       S.host.empty() ? PEER_HERE : S.host.c_str(), S.frames,
                       S.roiUsed ? "selected ROI" : "whole image",
                       S.nPl > 1 ? "CFA planes" : "plane=all (no CFA split)");
    ImGui::Separator();
    if (ImGui::BeginTable("srvtemporal", 1 + S.nPl,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("quantity [DN]", ImGuiTableColumnFlags_WidthStretch);
        for (int p = 0; p < S.nPl; p++)
            ImGui::TableSetupColumn(S.nPl > 1 ? LIN_PLANES[p] : "value",
                                    ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        temporalNoiseRows(S.nPl, S.tempNoise, S.fixedPattern, S.fpnCorr,
                          S.fpnClamped, S.totalNoise);
        ImGui::EndTable();
    }
    temporalClampNote(S.nPl, S.fpnClamped);
    if (!S.frameMean.empty()) {
        // the custom per-frame x axis applies to the server's series too: the
        // mapping is by frame number, and the server's x IS the frame number
        FrameAxis ax = frameAxisOf(si->id);
        if (!ax.why.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", ax.why.c_str());
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (float v : S.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        float fx0 = FLT_MAX, fx1 = -FLT_MAX;
        for (float ix : S.idx) {
            float x = (float)ax.at(ix);
            fx0 = std::min(fx0, x); fx1 = std::max(fx1, x);
        }
        if (fx0 > fx1) { fx0 = 0; fx1 = 1; }
        float tAvail = ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        // the axis names the region that was actually measured. It said "ROI"
        // unconditionally while requestServerTemporal never puts a rect on the
        // wire - and the detached view one screen down already says "frame".
        PlotRect tp = beginPlot(ax.custom ? ax.label.c_str() : "frame number",
                                S.roiUsed ? "ROI mean value [DN]" : "frame mean value [DN]",
                                fx0, fx1, mn, mx, !ax.custom, false, std::max(tAvail, 70.0f * app.uiScale));
        if (tp.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            for (size_t i = 1; i < S.frameMean.size(); i++)
                dl->AddLine(tp.at((float)ax.at(S.idx[i - 1]), S.frameMean[i - 1]),
                            tp.at((float)ax.at(S.idx[i]), S.frameMean[i]),
                            IM_COL32(105, 180, 240, 255), 1.5f);
            if (ImageDoc* c2 = cur()) {
                float mxp = tp.at((float)ax.at((float)c2->seqIndex), tp.ymin).x;
                dl->AddLine(ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y), IM_COL32(255, 184, 77, 200));
            }
            dl->PopClipRect();
        }
    }
    return true;
}

// Server temporal for a stack that is NOT opened (fired from the browser's
// group row or multi-selection). Takes over the panel until dismissed (x) or
// superseded: the user just asked for it, and it has no current-image to hang
// off. "Open as stack" turns the measurement into a real open in one click -
// whose own server temporal then replaces this one.
static void drawBrowseTemporal() {
    App::ServerTemporal& S = app.srvTemporal;
    ImGui::Text("Temporal");
    ImGui::SameLine();
    if (S.pending) {
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s - measuring...]",
                           S.host.empty() ? PEER_HERE : S.host.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
        ImGui::Separator();
        ImGui::TextDisabled("not opened: %s", S.label.c_str());
        return;
    }
    if (!S.valid) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[server failed]");
        ImGui::SameLine();
        if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
        ImGui::Separator();
        ImGui::TextDisabled("not opened: %s", S.label.c_str());
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", S.err.c_str());
        return;
    }
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "[server %s, %d frames - not opened: %s]",
                       S.host.empty() ? PEER_HERE : S.host.c_str(), S.frames,
                       S.label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open as stack")) {
        // copy first: openRemoteStack fires its own server temporal, which
        // resets S while we are still reading it
        std::string host = S.host, label = S.label;
        std::vector<std::string> files = S.files;
        openRemoteStack(host, files, label);
        return;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("open %s for real (frames transfer in the background)",
                          S.label.c_str());
    ImGui::SameLine();
    // The server just measured this stack's mean as a NUMBER; this opens the
    // same mean as a PICTURE. Same copy-first rule as the button beside it -
    // openRemoteStack fires its own server temporal, which resets S under us.
    if (ImGui::SmallButton("Open as frame average")) {
        std::string host = S.host, label = S.label;
        std::vector<std::string> files = S.files;
        openStackForAverage(host, files, label);
        return;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AVG_TIP);
    ImGui::SameLine();
    if (ImGui::SmallButton("x##srvtdrop")) { S = App::ServerTemporal{}; return; }
    ImGui::Separator();
    // cfaType was 0 by design (no open file to read a mosaic from): say so,
    // or a Bayer stack's plane-blind sigma reads as a wrong number
    ImGui::TextDisabled("plane=all (file not opened - no CFA split)");
    if (ImGui::BeginTable("srvbtemporal", 1 + S.nPl,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("quantity [DN]", ImGuiTableColumnFlags_WidthStretch);
        for (int p = 0; p < S.nPl; p++)
            ImGui::TableSetupColumn(S.nPl > 1 ? LIN_PLANES[p] : "value",
                                    ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("mean");
            for (int p = 0; p < S.nPl; p++) { ImGui::TableNextColumn(); textNum("%.6g", S.mean[p]); }
        }
        temporalNoiseRows(S.nPl, S.tempNoise, S.fixedPattern, S.fpnCorr,
                          S.fpnClamped, S.totalNoise);
        ImGui::EndTable();
    }
    temporalClampNote(S.nPl, S.fpnClamped);
    if (!S.frameMean.empty()) {
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (float v : S.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        float fx0 = S.idx.empty() ? 0 : S.idx.front(), fx1 = S.idx.empty() ? 1 : S.idx.back();
        float tAvail = ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
        PlotRect tp = beginPlot("frame number", "frame mean value [DN]",
                                fx0, fx1, mn, mx, true, false, std::max(tAvail, 70.0f * app.uiScale));
        if (tp.ok) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            for (size_t i = 1; i < S.frameMean.size(); i++)
                dl->AddLine(tp.at(S.idx[i - 1], S.frameMean[i - 1]),
                            tp.at(S.idx[i], S.frameMean[i]), IM_COL32(105, 180, 240, 255), 1.5f);
            dl->PopClipRect();
        }
    }
}

// ---- per-frame x axis (Temporal chart / export) -----------------------------
// Parse a pasted list of numbers: comma, space, tab and newline all separate
// (the user pastes an Excel column or a script's output - both shapes must
// work). A non-numeric token is a POSITIONED error, never a silent skip and
// never a zero: unreadable is unset (docs/terminology.md).
static bool parseAxisValues(const std::string& text, std::vector<double>& out,
                            std::string& err) {
    out.clear();
    err.clear();
    auto sep = [](char c) {
        return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t i = 0;
    int tok = 0, nBad = 0;
    std::string bad;
    while (i < text.size()) {
        while (i < text.size() && sep(text[i])) i++;
        if (i >= text.size()) break;
        size_t j = i;
        while (j < text.size() && !sep(text[j])) j++;
        std::string t = text.substr(i, j - i);
        tok++;
        char* end = nullptr;
        double v = strtod(t.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v)) {
            nBad++;
            if (nBad <= 3) {
                char b[96];
                snprintf(b, sizeof b, "%svalue %d ('%.24s') is not a number",
                         bad.empty() ? "" : ";  ", tok, t.c_str());
                bad += b;
            }
        } else out.push_back(v);
        i = j;
    }
    if (nBad) {
        if (nBad > 3) bad += ";  ...";
        err = bad;
        out.clear();
        return false;
    }
    return !out.empty();
}

// Attach a per-frame x axis to a stack. All three parts are required (an axis
// is a quantity with a unit; the unit is never defaulted), and the value count
// must equal the stack's frame count - against the EXPECTED total for a
// partially loaded stack. A mismatch refuses with both numbers: a partial
// mapping silently truncating either side is the classic quiet lie.
// The axis itself, once the values exist. Both ways of giving them - pasting a
// column into the Temporal panel and ticking a 1-D key in the .npz picker -
// land HERE, so the two cannot drift into two different ideas of what an axis
// is (and a key read out of a file gets no relaxation of the rules a pasted
// column obeys: the unit is required of both).
static std::string trimAxisLabel(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool applyFrameAxisVals(int seqId, const std::string& nameIn, const std::string& unitIn,
                               std::vector<double> vals, std::string& err) {
    App::SeqInfo* si = seqInfo(seqId);
    if (!si) { err = "not a stack"; return false; }
    // The trim lives HERE, not in one caller: a unit of " " is not a unit, and
    // it was refused from the pasted path and accepted from the .npz picker,
    // which rendered "x axis in  " - one rule promised, two rules enforced.
    std::string name = trimAxisLabel(nameIn), unit = trimAxisLabel(unitIn);
    if (name.empty() || unit.empty()) {
        err = "the axis needs a NAME and a UNIT - a bare list of numbers "
              "cannot label an axis (the unit is never assumed)";
        return false;
    }
    int n = (int)framesOfSeq(seqId).size();
    int N = std::max(si->expectedFrames, n);
    if ((int)vals.size() != N) {
        char b[192];
        snprintf(b, sizeof b, "%d value(s) for %d frame(s)%s - not applied: value i "
                              "maps to frame i, and a partial mapping would silently "
                              "lie about the rest",
                 (int)vals.size(), N, si->expectedFrames > n ? " (expected total)" : "");
        err = b;
        return false;
    }
    si->axisName = name;
    si->axisUnit = unit;
    si->axisVals = std::move(vals);
    return true;
}

static bool applyFrameAxis(int seqId, const char* name, const char* unit,
                           const std::string& text, std::string& err) {
    App::SeqInfo* si = seqInfo(seqId);
    if (!si) { err = "not a stack"; return false; }
    std::string nm = trimAxisLabel(name ? name : ""), un = trimAxisLabel(unit ? unit : "");
    // name and unit are checked BEFORE the values are parsed: an unlabelled
    // paste is refused for what it is, not for the first bad token in it
    if (nm.empty() || un.empty()) {
        err = "the axis needs a NAME and a UNIT - a bare list of numbers "
              "cannot label an axis (the unit is never assumed)";
        return false;
    }
    std::vector<double> vals;
    if (!parseAxisValues(text, vals, err)) {
        if (err.empty()) err = "no values";
        return false;
    }
    return applyFrameAxisVals(seqId, nm, un, std::move(vals), err);
}

// ---- frame-wise linearity: MIGRATED (docs/analysis-layers.md §3.3) --------
// The interim "Linearity (frame axis)" section lived here while the series
// layer could not hold it. It can now: a swept capture is a series by
// definition, so the fit - window, OLS / rev4-style methods, settings in the
// session - lives in the Series Analysis panel, over frame | stack mixed
// members. The frame axis KEEPS its monitor role in this panel (the per-frame
// mean curve below), which is all §3.3 leaves at the stack layer.

// ---- Temporal export: one document, two sinks (docs/export-design.md) -------
// A CSV/TSV document under construction. ONE builder produces the cells; the
// delimiter is the only thing the two sinks disagree about. TSV: tabs (and
// newlines) inside a cell are squashed to spaces. CSV: RFC 4180 minimal - a
// cell containing the delimiter, a quote or a newline is quoted and its quotes
// doubled. Decimal point is always '.' (snprintf's C locale IS the contract),
// digits are %.9g: the display's decimals setting is about scanning columns,
// an export is about reproducing numbers.
static std::string expCell(std::string s, char delim) {
    if (delim != ',') {
        for (char& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    }
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += '"';
        q += c;
    }
    q += '"';
    return q;
}
struct ExportDoc {
    std::string out;
    char delim;
    explicit ExportDoc(char d) : delim(d) {}
    void comment(const std::string& s) { out += "# " + s + "\n"; }
    void row(const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); i++) {
            if (i) out += delim;
            out += expCell(cells[i], delim);
        }
        out += "\n";
    }
};
static std::string expNum(double v) {
    char b[40];
    snprintf(b, sizeof b, "%.9g", v);
    return b;
}

// Per-frame S1 statistics of ONE stack as one rectangle: a header row and a
// row per RESIDENT frame, with per-plane spatial mean/sigma and the H/V
// profile non-uniformity of that frame (sigma of the column-mean / row-mean
// profile as % of the plane mean - the same sigma_col/sigma_row vocabulary as
// the profile-statistics section). Everything in a row is computable from that
// frame alone - deliberately NO temporal columns: sigma_t is a property of
// the stack, not of a frame (a per-frame "temporal noise" would be a category
// error - see docs/stats-taxonomy.md 3). Full-pixel scan of the region, so an
// independent numpy implementation can reproduce every number
// (--framestats-selftest prints exactly this block).
struct PerFrameBlock {
    int rows = 0, skipped = 0;
    bool roiUsed = false;
    int rx = 0, ry = 0, rw = 0, rh = 0;   // the region the rows describe
};
static PerFrameBlock exportPerFrameBlock(int seqId, ExportDoc& doc) {
    PerFrameBlock res;
    // the ROI convention every other measurement uses: selected rect, else full
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (App::Ann* a = findAnn(app.selectedAnn))
        if (a->type == 0) { rx = a->x; ry = a->y; rw = a->w; rh = a->h; res.roiUsed = true; }
    std::vector<int> fr = framesOfSeq(seqId);
    // the custom per-frame x axis rides along as its own column, name and unit
    // in the header - the frame column stays: the mapping is BY frame number,
    // and a row that dropped it could not state what its axis value belongs to
    const FrameAxis ax = frameAxisOf(seqId);
    int nPl = 0;
    bool cfa = false;
    for (int idx : fr) {
        const ImageDoc* d = app.images[idx].get();
        if (d->px().empty() || d->src->remoteStep > 1) { res.skipped++; continue; }   // previews lie
        int W = d->w, H = d->h, C = d->ch;
        int x0 = std::clamp(rx, 0, W - 1), y0 = std::clamp(ry, 0, H - 1);
        int ww = rw <= 0 ? W - x0 : std::min(rw, W - x0);
        int hh = rh <= 0 ? H - y0 : std::min(rh, H - y0);
        if (!nPl) {                                    // header from the first frame
            cfa = d->cfa != 0;
            nPl = cfa ? 4 : std::min(C, 4);
            res.rx = x0; res.ry = y0; res.rw = ww; res.rh = hh;
            std::vector<std::string> h = { "frame", "file" };
            if (ax.custom) h.insert(h.begin() + 1, ax.name + " [" + ax.unit + "]");
            for (int p = 0; p < nPl; p++) {
                static const char* CHN[4] = { "ch0", "ch1", "ch2", "ch3" };
                const char* pn = cfa ? CFA_CH_NAMES[p] : (C > 1 ? CHN[p] : "");
                std::string sfx = *pn ? std::string("_") + pn : std::string();
                h.push_back("mean" + sfx + " [DN]");
                h.push_back("sigma" + sfx + " [DN]");
                h.push_back("sigma_col" + sfx + " [%]");
                h.push_back("sigma_row" + sfx + " [%]");
            }
            doc.row(h);
        }
        double sum[4] = {}, sum2[4] = {};
        size_t cnt[4] = {};
        // projection profiles per plane; mean per column/row of that plane only
        std::vector<double> colS((size_t)ww * nPl, 0.0), rowS((size_t)hh * nPl, 0.0);
        std::vector<uint32_t> colN((size_t)ww * nPl, 0), rowN((size_t)hh * nPl, 0);
        for (int y = 0; y < hh; y++) {
            const float* row = &d->px()[((size_t)(y0 + y) * W + x0) * C];
            for (int x = 0; x < ww; x++)
                for (int c = 0; c < C; c++) {
                    int p = cfa ? cfaChannelAt(*d, x0 + x, y0 + y) : std::min(c, nPl - 1);
                    double v = row[(size_t)x * C + c];
                    if (!std::isfinite(v)) continue;
                    sum[p] += v; sum2[p] += v * v; cnt[p]++;
                    colS[(size_t)p * ww + x] += v; colN[(size_t)p * ww + x]++;
                    rowS[(size_t)p * hh + y] += v; rowN[(size_t)p * hh + y]++;
                }
        }
        std::vector<std::string> r = { std::to_string(d->seqIndex), d->name };
        if (ax.custom) r.insert(r.begin() + 1, expNum(ax.at(d->seqIndex)));
        for (int p = 0; p < nPl; p++) {
            double m = cnt[p] ? sum[p] / (double)cnt[p] : 0.0;
            double var = cnt[p] > 1
                ? std::max(0.0, sum2[p] / (double)cnt[p] - m * m) * (cnt[p] / (cnt[p] - 1.0)) : 0.0;
            auto profCv = [&](const std::vector<double>& S, const std::vector<uint32_t>& N,
                              size_t off, int n) {
                double ps = 0, ps2 = 0; int pc = 0;
                for (int i = 0; i < n; i++) {
                    if (!N[off + i]) continue;
                    double pm = S[off + i] / N[off + i];
                    ps += pm; ps2 += pm * pm; pc++;
                }
                if (pc < 2 || fabs(m) < 1e-12) return 0.0;
                double pmn = ps / pc;
                double pv = std::max(0.0, ps2 / pc - pmn * pmn) * (pc / (pc - 1.0));
                return sqrt(pv) / fabs(m) * 100.0;
            };
            r.push_back(expNum(m));
            r.push_back(expNum(sqrt(var)));
            r.push_back(expNum(profCv(colS, colN, (size_t)p * ww, ww)));
            r.push_back(expNum(profCv(rowS, rowN, (size_t)p * hh, hh)));
        }
        doc.row(r);
        res.rows++;
    }
    return res;
}

// The old per-frame clipboard surface, kept as the thin wrapper the
// --framestats-selftest contract rides on: the per-frame block, alone, as TSV.
// The Temporal panel's buttons now go through buildTemporalExport instead
// (docs/export-design.md), whose per-frame section is this very block.
static void copyPerFrameStats(int seqId) {
    ExportDoc doc('\t');
    PerFrameBlock r = exportPerFrameBlock(seqId, doc);
    if (!r.rows) { toast("no resident frames to tabulate", true); return; }
    ImGui::SetClipboardText(doc.out.c_str());
    char b[160];
    snprintf(b, sizeof b, "copied %d frame row(s) (%s)%s%s",
             r.rows, r.roiUsed ? "selected ROI" : "whole frame",
             r.skipped ? " - " : "",
             r.skipped ? (std::to_string(r.skipped) + " not resident").c_str() : "");
    toast(b, r.skipped != 0);
}

// One side's temporal numbers, wherever they came from. The A/B table takes
// both sides through this, so a server-measured A and a locally computed B land
// in the same rows under the same headers - and "there is no number here" is a
// stated reason instead of a blank.
struct AbTemporal {
    bool valid = false, isStack = false, fromServer = false;
    // the REGION this side measured. A delta between a whole-frame A and an
    // ROI-restricted B is a difference of two different measurements.
    bool roiUsed = false;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    int nPl = 1;                      // CFA planes are never mixed, on either side
    // sigS is sigma_fpn - the CORRECTED one (#57 judgment 2). sigCorr is what
    // was subtracted (fpn_corr, in DN) and clamped[] says the printed 0 is a
    // clamp. Both travel here so an A measured on a server and a B measured
    // locally declare their correction the same way in one table.
    double sigT[4] = {}, sigS[4] = {}, sigCorr[4] = {}, sigTot[4] = {};
    bool clamped[4] = {};
    int frames = 0, expected = 0;
    const std::vector<float>* idx = nullptr;
    const std::vector<float>* mean = nullptr;
    const char* note = "";
};
static AbTemporal abTemporalOf(const ImageDoc* d, const App::TemporalState& T,
                               const App::ServerTemporal& S) {
    AbTemporal o;
    if (!d || d->seqId == 0) { o.note = "not a stack"; return o; }
    o.isStack = true;
    if (const App::SeqInfo* si = seqInfo(d->seqId)) o.expected = si->expectedFrames;
    if (S.valid && S.seqId == d->seqId) {          // server-measured wins: it saw
        o.valid = true; o.fromServer = true;       // every frame, not the resident ones
        o.nPl = S.nPl;
        o.roiUsed = S.roiUsed;
        o.rx = S.rx; o.ry = S.ry; o.rw = S.rw; o.rh = S.rh;
        for (int p = 0; p < S.nPl; p++) {
            o.sigT[p] = S.tempNoise[p]; o.sigS[p] = S.fixedPattern[p];
            o.sigCorr[p] = S.fpnCorr[p]; o.clamped[p] = S.fpnClamped[p];
            o.sigTot[p] = S.totalNoise[p];
        }
        o.frames = S.frames;
        o.idx = &S.idx; o.mean = &S.frameMean;
        return o;
    }
    if (T.valid && T.seqId == d->seqId) {
        o.valid = true;
        o.nPl = T.nPl;
        o.roiUsed = T.roiUsed;
        o.rx = T.rx; o.ry = T.ry; o.rw = T.roiUsed ? T.rw : 0; o.rh = T.roiUsed ? T.rh : 0;
        for (int p = 0; p < T.nPl; p++) {
            o.sigT[p] = T.tempNoise[p]; o.sigS[p] = T.fixedPattern[p];
            o.sigCorr[p] = T.fpnCorr[p]; o.clamped[p] = T.fpnClamped[p];
            o.sigTot[p] = T.totalNoise[p];
        }
        o.frames = T.frames;
        o.idx = &T.idx; o.mean = &T.frameMean;
        return o;
    }
    o.note = S.pending && S.seqId == d->seqId ? "measuring on the server..."
                                              : "needs >= 2 loaded frames";
    return o;
}

// A difference only means something when both sides measured the same quantity.
// A 1-channel stack's sigma_t and a 3-channel one's are not the same number, so
// the delta columns stay empty rather than subtract two unlike things
// (docs/ab-stats-plan.md 5).
static bool abDeltaMeaningful(const ImageDoc* a, const ImageDoc* b) {
    return a && b && a->ch == b->ch;
}

// ---- the unified Temporal export (docs/export-design.md) --------------------
// Everything the Temporal question is answered with, in ONE document: the
// stack statistics (sigma_t / sigma_fpn / sigma_tot per plane), the H/V
// profile statistics of the same image/ROI (from the Projection state - the
// SAME structs that panel draws, never re-derived), and the per-frame S1
// series. Labelled '#' sections, each section its own rectangle: the three
// tables have different row axes, and one rectangle would either put sigma_t
// on frame rows (the category error stats-taxonomy.md 3 forbids) or pad
// itself unreadable. Every compare side rides along with a side column - an
// export that silently dropped B would not reproduce the screen.
struct TExpSide {
    std::string letter;
    ImageDoc* doc = nullptr;
    AbTemporal t;
    const App::ProjState* proj = nullptr;
    std::string srvHost;          // when t.fromServer: which machine measured
    uint64_t nonFinite = 0;       // local temporal: samples excluded, and said so
    size_t dropped = 0;           // local temporal: samples with < 2 valid frames
};
static std::vector<TExpSide> temporalExportSides() {
    std::vector<TExpSide> out;
    ImageDoc* im = cur();
    if (!im) return out;
    recomputeTemporalIfNeeded(im, app.temporal[0]);
    recomputeProjectionIfNeeded(im, app.proj[0]);
    {
        TExpSide A;
        A.letter = "A";
        A.doc = im;
        A.t = abTemporalOf(im, app.temporal[0], app.srvTemporal);
        A.proj = &app.proj[0];
        if (A.t.fromServer) A.srvHost = app.srvTemporal.host;
        else if (A.t.valid) { A.nonFinite = app.temporal[0].nonFinite; A.dropped = app.temporal[0].dropped; }
        out.push_back(std::move(A));
    }
    ImageDoc* Bim = abStatsB();
    if (Bim) {
        recomputeTemporalIfNeeded(Bim, app.temporal[1]);
        recomputeProjectionIfNeeded(Bim, app.proj[1]);
        TExpSide B;
        B.letter = "B";
        B.doc = Bim;
        B.t = abTemporalOf(Bim, app.temporal[1], app.srvTemporalB);
        B.proj = &app.proj[1];
        if (B.t.fromServer) B.srvHost = app.srvTemporalB.host;
        else if (B.t.valid) { B.nonFinite = app.temporal[1].nonFinite; B.dropped = app.temporal[1].dropped; }
        out.push_back(std::move(B));
    }
    // the extra compare slots, resolved exactly as the two panels resolve them
    app.temporalExtra.resize(app.cmpExtra.size());
    app.projExtra.resize(app.cmpExtra.size());
    static const App::ServerTemporal NO_SERVER;   // extras are local-only
    for (const ResolvedSlot& rs : resolveSlots()) {
        if (rs.idx >= app.temporalExtra.size()) continue;
        recomputeTemporalIfNeeded(rs.doc, app.temporalExtra[rs.idx]);
        recomputeProjectionIfNeeded(rs.doc, app.projExtra[rs.idx]);
        TExpSide X;
        X.letter = slotName(rs.idx);
        X.doc = rs.doc;
        X.t = abTemporalOf(rs.doc, app.temporalExtra[rs.idx], NO_SERVER);
        X.proj = &app.projExtra[rs.idx];
        if (X.t.valid) { X.nonFinite = app.temporalExtra[rs.idx].nonFinite; X.dropped = app.temporalExtra[rs.idx].dropped; }
        out.push_back(std::move(X));
    }
    return out;
}

// The region a side's TEMPORAL numbers describe, and the one its PROFILES do.
// They are stated separately because they can genuinely differ: the server
// aggregate never takes a ROI (whole frame), while the projection follows the
// selection - and the export must state both, not paper over it (the "server
// temporal is whole-frame but labels said ROI" history).
static std::string expRegionTemporal(const TExpSide& s) {
    if (!s.t.valid) return "-";
    char b[96];
    if (s.t.roiUsed) snprintf(b, sizeof b, "ROI (%d,%d) %dx%d", s.t.rx, s.t.ry, s.t.rw, s.t.rh);
    else snprintf(b, sizeof b, "whole image (%dx%d)", s.doc->w, s.doc->h);
    return b;
}
static std::string expRegionProfile(const TExpSide& s) {
    const App::ProjState& P = *s.proj;
    char b[96];
    if (P.roiUsed) snprintf(b, sizeof b, "ROI (%d,%d) %dx%d", P.rx, P.ry, P.rw, P.rh);
    else snprintf(b, sizeof b, "whole image (%dx%d)", P.rw, P.rh);
    return b;
}
static bool expRegionsMatch(const TExpSide& s) {
    if (!s.t.valid) return true;                 // nothing measured, nothing to clash
    const App::ProjState& P = *s.proj;
    if (s.t.roiUsed != P.roiUsed) return false;
    if (!s.t.roiUsed) return true;               // both whole image
    return s.t.rx == P.rx && s.t.ry == P.ry && s.t.rw == P.rw && s.t.rh == P.rh;
}

static std::string buildTemporalExport(char delim) {
    std::vector<TExpSide> sides = temporalExportSides();
    if (sides.empty()) return std::string();
    ExportDoc doc(delim);

    // ---- provenance: a number without it cannot go in a report ----
    doc.comment("temporal + H/V profile statistics");
    {
        char ts[32] = "";
        time_t now = time(nullptr);
        if (struct tm* lt = localtime(&now)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
        doc.comment(std::string("app: viewer ") + viewerVersion() + "  |  generated: " + ts);
    }
    {
        std::string s = "sides: ";
        for (size_t i = 0; i < sides.size(); i++) {
            if (i) s += ", ";
            s += sides[i].letter;
        }
        doc.comment(s);
    }
    const char* pmodes[3] = { "mean", "max", "min" };
    const char* reduce = pmodes[std::clamp(app.projMode, 0, 2)];
    for (const TExpSide& s : sides) {
        const ImageDoc* d = s.doc;
        char b[512];
        snprintf(b, sizeof b, "%s: %s%s%s  |  path: %s", s.letter.c_str(),
                 abDocLabel(d).c_str(), avgIsStale(d) ? " (stale)" : "",
                 seqReloadMark(d->seqId),
                 d->src->path.empty() ? "-" : d->src->path.c_str());
        doc.comment(b);
        // A computed frame's provenance is its note - there is no path to
        // cite, and the note carries the recipe, the count, WHEN it was
        // computed, and any generation statement. The export is the report's
        // doorstep, so the whole statement travels (§10).
        if (d->src->path.empty() && !d->note.empty())
            doc.comment(s.letter + ": " + d->note);
        if (avgIsStale(d))
            doc.comment("NOTE: side " + s.letter + ": (stale) - the source stack was "
                        "reloaded after this frame was computed; these numbers "
                        "describe the OLD pixels (Recompute to refresh)");
        // #56: sigma_t is THE number a mixed-generation stack corrupts, and
        // this document is where it is carried into a report. So the mark
        // travels with it - not as a colour, which a TSV cannot hold, but as
        // the same sentence the Inspector prints. An export that dropped it
        // would be the one surface that let the number out unqualified.
        if (std::string rn = seqReloadNote(d->seqId); !rn.empty())
            doc.comment("NOTE: side " + s.letter + ": " + rn);
        std::string cfa = d->cfa
            ? std::string(CFA_PATTERNS[d->cfaPattern & 3]) + (d->cfa == 2 ? " quad" : "") + " (4 planes)"
            : std::string("none");
        if (!s.t.isStack) {
            snprintf(b, sizeof b, "%s: %dx%d %s  |  CFA: %s  |  not a stack "
                                  "(single frame: no temporal rows)",
                     s.letter.c_str(), d->w, d->h, d->dtype.c_str(), cfa.c_str());
            doc.comment(b);
        } else {
            snprintf(b, sizeof b, "%s: %dx%d %s  |  CFA: %s  |  frames n=%d/%d  |  "
                                  "temporal source: %s%s%s",
                     s.letter.c_str(), d->w, d->h, d->dtype.c_str(), cfa.c_str(),
                     s.t.frames, std::max(s.t.expected, s.t.frames),
                     s.t.valid ? (s.t.fromServer ? "server" : "local") : "none (",
                     s.t.fromServer && !s.srvHost.empty() ? (" " + s.srvHost).c_str() : "",
                     s.t.valid ? "" : (std::string(s.t.note) + ")").c_str());
            doc.comment(b);
            snprintf(b, sizeof b, "%s temporal region: %s", s.letter.c_str(),
                     expRegionTemporal(s).c_str());
            doc.comment(b);
        }
        snprintf(b, sizeof b, "%s profile region: %s  |  reduce: %s", s.letter.c_str(),
                 expRegionProfile(s).c_str(), reduce);
        doc.comment(b);
        if (!expRegionsMatch(s)) {
            snprintf(b, sizeof b, "NOTE: side %s: temporal and profile statistics were "
                                  "measured over DIFFERENT regions (see above)",
                     s.letter.c_str());
            doc.comment(b);
        }
        // the swept parameter, when this stack is a series member: the one
        // piece of +meta the file does not carry (docs/stats-taxonomy.md)
        if (App::Series* S = seriesOfStack(d->seqId))
            for (const auto& m : S->members)
                if (m.seqId == d->seqId) {
                    std::string v = std::isfinite(m.value)
                        ? expNum(m.value) + (S->unit[0] ? std::string(" ") + S->unit : std::string())
                        : std::string("(value not set)");
                    snprintf(b, sizeof b, "%s series: %s  |  %s = %s", s.letter.c_str(),
                             S->name.c_str(),
                             S->paramName.empty() ? "value" : S->paramName.c_str(),
                             v.c_str());
                    doc.comment(b);
                    break;
                }
    }

    // ---- section 1: the stack statistics --------------------------------
    doc.comment("== temporal summary (stack statistics; sigma_t is a property of "
                "the stack, never of a frame) ==");
    // fpn_corr / fpn_clamped ride in the table rather than in a comment: they
    // are per side AND per plane, which is a row shape, and a report that
    // quotes sigma_fpn has to be able to quote what was taken off it from the
    // same line (#57 judgment 2). They go AFTER sigma_tot so the three
    // sigma_* columns stay adjacent - tools/verify/invariants.sh reads that
    // triple as one header token.
    doc.row({ "side", "ch", "n", "N", "sigma_t [DN]", "sigma_fpn [DN]",
              "sigma_tot [DN]", "fpn_corr [DN]", "fpn_clamped", "source" });
    bool anyLocal = false, anyClamp = false;
    for (const TExpSide& s : sides) {
        if (!s.t.valid) continue;
        anyLocal |= !s.t.fromServer;
        std::string src = s.t.fromServer
            ? (s.srvHost.empty() ? std::string("server") : "server " + s.srvHost)
            : std::string("local");
        for (int p = 0; p < s.t.nPl; p++) {
            anyClamp |= s.t.clamped[p];
            doc.row({ s.letter, s.t.nPl > 1 ? LIN_PLANES[p] : "all",
                      std::to_string(s.t.frames),
                      std::to_string(std::max(s.t.expected, s.t.frames)),
                      expNum(s.t.sigT[p]), expNum(s.t.sigS[p]), expNum(s.t.sigTot[p]),
                      expNum(s.t.sigCorr[p]), s.t.clamped[p] ? "yes" : "no",
                      src });
        }
    }
    // The estimator is named because this table mixes local and server rows in
    // one column (`src`), and until #57 item 3 the two sides used different
    // ddof - a reader comparing a local row with a server row was comparing two
    // quantities under one heading. They are one quantity now, so the sentence
    // that says which one belongs here - and since #57 item 2 that sentence has
    // to say that sigma_fpn is CORRECTED, because the same key named the
    // uncorrected upper bound in every export written before this one.
    doc.comment("sigma_t = sqrt(mean per-pixel temporal variance, ddof=1 - the "
                "same estimator local and server); sigma_fpn = "
                "sqrt(max(0, var_spatial(per-pixel temporal means, ddof=1) - "
                "fpn_corr^2)) with fpn_corr = sqrt(mean(s_t,i^2/n_i)) - the "
                "temporal residual C subtracted (docs/flat-field-stats.md (b), "
                "#57 item 2); still includes scene detail unless the region is "
                "flat; sigma_tot = quadrature sum");
    doc.comment("the uncorrected upper bound this used to print is recoverable "
                "per row as sqrt(sigma_fpn^2 + fpn_corr^2)");
    if (anyClamp)
        doc.comment("NOTE: sigma_fpn CLAMPED at 0 where fpn_clamped says yes - the "
                    "temporal residual is not below the spatial spread, so that "
                    "row resolves no fixed pattern above its own temporal floor. "
                    "The 0 is a statement about the measurement, not a flat sensor.");
    if (anyLocal)
        doc.comment("local values are computed on a sampled grid (up to 40000 "
                    "samples, whole CFA cells); the per-frame section scans every "
                    "pixel, so levels can differ slightly");
    for (const TExpSide& s : sides) {
        char b[256];
        if (s.t.isStack && !s.t.valid) {
            snprintf(b, sizeof b, "side %s: no temporal rows - %s", s.letter.c_str(),
                     s.t.note);
            doc.comment(b);
        }
        if (s.nonFinite || s.dropped) {
            snprintf(b, sizeof b, "side %s: non-finite samples (excluded) = %llu; "
                                  "%zu sample(s) had < 2 valid frames",
                     s.letter.c_str(), (unsigned long long)s.nonFinite, s.dropped);
            doc.comment(b);
        }
    }

    // ---- section 2: the H/V profile statistics --------------------------
    {
        char b[128];
        snprintf(b, sizeof b, "== H/V profile statistics (the Projection panel's "
                              "numbers; reduce = %s) ==", reduce);
        doc.comment(b);
    }
    doc.row({ "side", "ch", "mean [DN]", "sigma_frame [DN]", "sigma_row [DN]",
              "sigma_col [DN]", "sigma_frame [%]", "sigma_row [%]", "sigma_col [%]",
              "pp_frame [DN]", "pp_row [DN]", "pp_col [DN]" });
    bool anyAllRow = false;
    for (const TExpSide& s : sides) {
        const App::ProjState& P = *s.proj;
        // side-major, pooled "all" last of its side - the same walk the panel's
        // two layouts share (projForEachRow), spelled once more here because
        // that walk is a lambda inside the panel. The rule, not the code, is
        // what the two exports share (docs/export-design.md 7).
        auto prow = [&](int i) {
            const App::ProjState::Stats& f = P.fStat[i];
            const App::ProjState::Stats& vv = P.vStat[i];
            const App::ProjState::Stats& hh = P.hStat[i];
            if (!f.valid && !hh.valid && !vv.valid) return;
            auto n9 = [](const App::ProjState::Stats& st, double v) {
                return st.valid ? expNum(v) : std::string("-");
            };
            doc.row({ s.letter, P.seriesNames[i],
                      n9(f, f.mean), n9(f, f.sd), n9(vv, vv.sd), n9(hh, hh.sd),
                      n9(f, f.pct), n9(vv, vv.pct), n9(hh, hh.pct),
                      n9(f, f.pp), n9(vv, vv.pp), n9(hh, hh.pp) });
        };
        for (int i = 0; i < P.nSeries; i++) prow(i);
        if (P.allRow) { prow(4); anyAllRow = true; }
    }
    doc.comment("sigma_frame = the whole region's own sigma; sigma_row = sigma of "
                "the row means (horizontal banding); sigma_col = sigma of the "
                "column means (vertical striping); % is sigma / mean; pp is max - min");
    if (anyAllRow)
        doc.comment("the 'all' row pools every pixel regardless of plane - a "
                    "different measurement, never a fifth plane");

    // ---- section 3: the per-frame series (bulk last) ---------------------
    doc.comment("== per-frame statistics (S1 per frame - the drift / flicker "
                "material; resident frames only) ==");
    for (const TExpSide& s : sides) {
        if (!s.t.isStack) continue;
        ExportDoc sub(delim);
        PerFrameBlock r = exportPerFrameBlock(s.doc->seqId, sub);
        char reg[96];
        if (r.roiUsed) snprintf(reg, sizeof reg, "ROI (%d,%d) %dx%d", r.rx, r.ry, r.rw, r.rh);
        else snprintf(reg, sizeof reg, "whole frame (%dx%d)", r.rw, r.rh);
        int expected = std::max(s.t.expected, r.rows + r.skipped);
        char b[384];
        snprintf(b, sizeof b, "-- side %s: %s  |  region: %s  |  resident %d of %d "
                              "frame(s) --",
                 s.letter.c_str(), abDocLabel(s.doc).c_str(), reg, r.rows, expected);
        doc.comment(b);
        if (r.rows) doc.out += sub.out;
        else doc.comment("(no resident frames)");
    }

    // (the frame-linearity section rode here until §3.3 moved the analysis
    // to the Series Analysis panel; the per-frame rows above keep carrying
    // the frame axis, which is the monitor role the axis retains)
    return doc.out;
}

// The Temporal panel's export surface: one builder, two sinks. This replaced
// the per-frame-only "Copy per-frame stats": the numbers a report needs
// travel together (docs/export-design.md).
static void temporalExportButtons() {
    if (ImGui::SmallButton("Copy (TSV)")) {
        std::string s = buildTemporalExport('\t');
        if (s.empty()) toast("nothing to export", true);
        else {
            ImGui::SetClipboardText(s.c_str());
            toast("temporal + profile statistics copied (TSV)");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("temporal summary, H/V profile statistics and the\n"
                          "per-frame table as ONE tab-separated document with a\n"
                          "provenance header - every section a rectangle, for\n"
                          "Excel. Covers every compare side that is on screen.");
    ImGui::SameLine();
    ImGui::BeginDisabled(app.texportDlg != nullptr);
    if (ImGui::SmallButton("Save (CSV)...")) {
        std::string s = buildTemporalExport(',');
        if (s.empty()) toast("nothing to export", true);
        else if (!pfd::settings::available())
            toast("no file-dialog backend found (install zenity or kdialog)", true);
        else {
            // built NOW: the numbers may change while the OS dialog is open
            app.pendingTexport = std::move(s);
            app.texportDlg = std::make_unique<pfd::save_file>(
                "Save temporal statistics (CSV)", "temporal_stats.csv",
                std::vector<std::string>{ "CSV (*.csv)", "*.csv" });
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("the same document as Copy (TSV), comma-separated,\n"
                          "to a file - the content is built when you click,\n"
                          "exactly like Export curves.");
}

// The "x axis..." popup: paste a comma / space / tab separated list of numbers
// and the Temporal chart plots the per-frame curve against THAT instead of the
// frame index - frames of a stack usually correspond to a physical quantity
// (elapsed time, exposure, temperature), and with a real axis the drift chart
// becomes a measurement plot. Name + unit + values, applied per STACK
// (each stack has its own axis or none), session-saved.
static void frameAxisPopupButton(ImageDoc* im) {
    if (!im || im->seqId == 0) return;
    if (ImGui::SmallButton("x axis..."))
        ImGui::OpenPopup("temporal_xaxis");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("plot the per-frame curve against a real quantity\n"
                          "(elapsed time, exposure, temperature...): paste one\n"
                          "value per frame, comma / space / tab separated.\n"
                          "Applies to THIS stack; each stack has its own axis.");
    static int forSeq = -1;
    static char nm[64], un[32];
    static char vbuf[16384];
    static std::string err;
    if (ImGui::BeginPopup("temporal_xaxis")) {
        App::SeqInfo* si = seqInfo(im->seqId);
        if (forSeq != im->seqId) {          // (re)seed from the stack on open
            forSeq = im->seqId;
            err.clear();
            snprintf(nm, sizeof nm, "%s", si ? si->axisName.c_str() : "");
            snprintf(un, sizeof un, "%s", si ? si->axisUnit.c_str() : "");
            vbuf[0] = 0;
            if (si) {
                std::string s;
                for (size_t i = 0; i < si->axisVals.size(); i++) {
                    if (i) s += ", ";
                    s += fmtExact(si->axisVals[i]);
                }
                snprintf(vbuf, sizeof vbuf, "%s", s.c_str());
            }
        }
        int n = (int)framesOfSeq(im->seqId).size();
        int N = si ? std::max(si->expectedFrames, n) : n;
        ImGui::TextDisabled("x axis for %s: one value per frame (%d expected)",
                            abDocLabel(im).c_str(), N);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::InputText("name", nm, sizeof nm);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        ImGui::InputText("unit", un, sizeof un);
        ImGui::InputTextMultiline("##axisvals", vbuf, sizeof vbuf,
                                  ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 6));
        if (ImGui::Button("Apply")) {
            std::string e;
            if (applyFrameAxis(im->seqId, nm, un, vbuf, e)) {
                err.clear();
                forSeq = -1;
                toast("x axis applied: " + std::string(nm) + " (" + un + ")");
                ImGui::CloseCurrentPopup();
            } else err = e;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!si || si->axisVals.empty());
        if (ImGui::Button("Clear")) {
            if (si) { si->axisName.clear(); si->axisUnit.clear(); si->axisVals.clear(); }
            err.clear();
            forSeq = -1;
            toast("x axis cleared: frame index");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!err.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", err.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndPopup();
    } else if (forSeq == im->seqId) forSeq = -1;   // closed: reseed next open
}

// What the Temporal chart actually laid out, last frame. This machine cannot
// screenshot OpenGL content (CopyFromScreen/BitBlt/PrintWindow give a white
// client area), so "the chart is on screen" has to be an assertion on STATE:
// the panel records how many points it stroked per slot, how tall the plot was,
// and - the one the regression is really about - whether the plot rect ended up
// INSIDE the panel or below its bottom edge. A chart laid out past the window
// bottom is drawn, is reachable by scrolling, and is invisible: exactly the
// report this probe exists to catch.
struct TemporalChartProbe {
    bool ran = false;            // the chart block was reached at all
    bool plotted = false;        // beginPlot returned a usable rect
    bool sideBySide = false;
    float availBefore = 0;       // content height left when the chart block began
    float plotTop = 0, plotBot = 0, winBot = 0;
    int nSlots = 0;              // slots the panel considered (A, B, C, ...)
    char slot[16] = {};          // 'A', 'B', 'C', ...
    int pts[16] = {};            // points stroked for that slot (0 = no curve)
    int nNoAxis = 0;             // slots that have no time axis and SAID so
    char noAxisSlot[16] = {};
};
static TemporalChartProbe g_tchart;
// The chart fits when its bottom edge is inside the panel. One text line of
// slack: the caller reserves whole rows, and half a pixel of rounding must not
// read as "off screen".
static bool temporalChartVisible() {
    return g_tchart.plotted && g_tchart.plotBot <= g_tchart.winBot + 1.0f;
}

// The A/B temporal view: rows are quantities, columns are A | B | delta | delta%.
// (docs/ab-stats-plan.md 4.) The sign is A-B, matching the difference image.
static void drawTemporalAB(ImageDoc* im, ImageDoc* Bim) {
    recomputeTemporalIfNeeded(im, app.temporal[0]);
    // NOT throttled: temporal is keyed on the STACK and its ROI, so stepping
    // frames never invalidates it (see the A4 check in --abstats-selftest).
    recomputeTemporalIfNeeded(Bim, app.temporal[1]);
    AbTemporal A = abTemporalOf(im, app.temporal[0], app.srvTemporal);
    AbTemporal B = abTemporalOf(Bim, app.temporal[1], app.srvTemporalB);
    // The extra slots (C, D, E...). They ride on the CHART only: the table's
    // axis is A | B | delta, and a delta has exactly two operands - the numeric
    // side already gives every slot its own row in the Projection table, and
    // this table would have to grow a column pair per slot to say the same
    // thing. A curve costs one cached lookup and one polyline.
    struct TExtra { ImageDoc* doc; std::string letter; AbTemporal t; bool diverged; };
    std::vector<TExtra> XS;
    {
        // resolveSlots prunes closed slots and follows A's frame number the
        // way resolveB does; rs.idx IS the letter, and it survives the follow
        // (a uid lookup would not - the followed sibling is not the pinned uid)
        app.temporalExtra.resize(app.cmpExtra.size());
        static const App::ServerTemporal NO_SERVER;        // extras are local-only
        for (const ResolvedSlot& rs : resolveSlots()) {
            if (rs.idx >= app.temporalExtra.size()) continue;
            recomputeTemporalIfNeeded(rs.doc, app.temporalExtra[rs.idx]);
            XS.push_back({ rs.doc, slotName(rs.idx),
                           abTemporalOf(rs.doc, app.temporalExtra[rs.idx], NO_SERVER),
                           rs.diverged });
        }
    }

    const std::string uA = abValueUnit(im->dtype), uB = abValueUnit(Bim->dtype);
    // the UNIT is DN on both sides; the STORAGE can still differ, and a delta
    // between an f32 and a u16 stack is worth a warning even so
    const bool dtypeMix = im->dtype != Bim->dtype;
    const std::string unit = uA == uB ? uA : (uA + "/" + uB);
    // ...and a four-plane sigma_t minus a pooled one is not a difference of
    // anything, so the plane split joins the channel count in the test
    // ...and a four-plane sigma_t minus a pooled one, or a whole-frame A minus
    // an ROI-restricted B, is not a difference of anything: the plane split and
    // the REGION join the channel count in the test the panel already applies
    const bool sameRegion = A.roiUsed == B.roiUsed && A.rx == B.rx && A.ry == B.ry &&
                            A.rw == B.rw && A.rh == B.rh;
    const bool canDelta = abDeltaMeaningful(im, Bim) && A.nPl == B.nPl && sameRegion;

    auto regionOf = [](const AbTemporal& s) {
        return s.roiUsed ? "selected ROI" : "whole image";
    };
    ImGui::Text("Temporal");
    ImGui::SameLine();
    if (sameRegion)
        ImGui::TextDisabled("[%s, %s]", regionOf(A),
                            A.fromServer || B.fromServer ? "server + local" : "local");
    else
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[A: %s, B: %s - %s]",
                           regionOf(A), regionOf(B),
                           A.fromServer || B.fromServer ? "server + local" : "local");
    ImGui::SameLine();
    temporalExportButtons();   // covers every side on screen, not only A
    if (A.isStack) { ImGui::SameLine(); frameAxisPopupButton(im); }
    // B's server aggregate is never fired automatically - one explicit press,
    // one remote job (docs/ab-stats-plan.md 4).
    if (const App::SeqInfo* sb = seqInfo(Bim->seqId))
        if (serverComputes(*sb)) {
            ImGui::SameLine();
            bool busy = app.srvTemporalB.pending && app.srvTemporalB.seqId == Bim->seqId;
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(busy ? "Measuring B..." : "Measure B"))
                requestServerTemporal(Bim->seqId, app.srvTemporalB);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Run the server-side aggregate over B's stack.\n"
                                  "It is never started on its own: B follows A, and\n"
                                  "an automatic B measurement would double every\n"
                                  "remote job you did not ask for.");
        }
    ImGui::Separator();

    char hA[64], hB[64];
    auto frames = [](char* buf, size_t n, const char* side, const AbTemporal& s) {
        if (!s.isStack) snprintf(buf, n, "%s", side);
        else if (s.expected > s.frames) snprintf(buf, n, "%s (n=%d/%d)", side, s.frames, s.expected);
        else snprintf(buf, n, "%s (n=%d/%d)", side, s.frames, s.frames);
    };
    frames(hA, sizeof hA, "A", A);
    frames(hB, sizeof hB, "B", B);
    std::string cQ = "quantity [" + unit + "]";
    std::string cD = "delta A-B [" + unit + "]";

    // One row set per CFA plane, so the table's height is known before it is
    // opened - and it HAS to be. ImGuiTableFlags_ScrollX puts the table in a
    // CHILD WINDOW, and a child-window table given the default outer_size takes
    // the WHOLE of the panel's remaining height (imgui_tables.cpp BeginTableEx:
    // CalcItemSize(outer_size, avail.x, use_child_window ? avail.y : 0)). That
    // is the entire bug behind "compare shows numbers but no chart": the plain
    // one-sided table below has no ScrollX, keeps its natural height and leaves
    // the plot on screen, while the A/B table swallowed every pixel and the
    // chart was laid out past the bottom edge of the panel - drawn, scrollable
    // to, and invisible. The table now asks for exactly the rows it has, and
    // never for more than it can spare from the chart.
    const int nPlanes = std::max(A.nPl, B.nPl);
    const ImGuiStyle& gst = ImGui::GetStyle();
    const float rowH = ImGui::GetTextLineHeight() + gst.CellPadding.y * 2;
    // FOUR rows per plane since #57 judgment 2: sigma_t, sigma_fpn, the
    // fpn_corr that was subtracted from it, and the quadrature total. The
    // correction is not a footnote - it is the number that makes the corrected
    // one checkable - so it is reserved like any other row.
    const float tblWant = rowH * (4 * nPlanes + 1)      // + the header row
                        + gst.ScrollbarSize             // ScrollX reserves one
                        + gst.CellPadding.y * 2;
    // ---- custom per-frame x axes, one per slot's stack. ONE plot has ONE x
    // axis: the custom axis is used only when every slot that has a curve
    // carries the same QUANTITY (same name and unit - the values may differ,
    // that is the point of plotting them together); otherwise everything falls
    // back to the frame index and a notice says why, rather than plotting
    // incommensurables on one line. Decided here because that notice is one of
    // the lines the table must leave room for.
    auto axHasCurve = [](const AbTemporal& t) {
        return t.mean && t.idx && t.mean->size() >= 2 && t.idx->size() >= 2;
    };
    std::vector<const ImageDoc*> axDocs = { im, Bim };
    std::vector<bool> axHas = { axHasCurve(A), axHasCurve(B) };
    std::vector<std::string> axLetters = { "A", "B" };
    for (const TExtra& x : XS) {
        axDocs.push_back(x.doc);
        axHas.push_back(axHasCurve(x.t));
        axLetters.push_back(x.letter);
    }
    std::vector<FrameAxis> axs(axDocs.size());
    for (size_t i = 0; i < axDocs.size(); i++)
        if (axDocs[i] && axDocs[i]->seqId != 0)
            axs[i] = frameAxisOf(axDocs[i]->seqId);
    bool axAny = false, axUse = false;
    std::string axLabel, axBorrowed;
    {
        // Two different reasons a side can fail the one-axis rule, and they
        // deserve different treatment:
        //   conflict - two sides carry DIFFERENT custom axes. Incommensurable;
        //              the fallback to frame index stands.
        //   missing  - a side simply has no axis of its own. Starting an A/B
        //              compare against an axis-less B used to throw the whole
        //              chart back to frame index - the user read it as "A/B
        //              比較を始めるとX Axisがクリアされちゃう". Follow-frame
        //              already pairs sides BY FRAME NUMBER, so lending the
        //              reference axis to an axis-less side of the same length
        //              asserts nothing follow-frame has not already assumed.
        //              Lent, not silently: the notice names who borrowed.
        bool conflict = false;
        int ref = -1;
        for (size_t i = 0; i < axDocs.size(); i++) {
            if (!axHas[i] || !axs[i].custom) continue;
            axAny = true;
            if (ref < 0) { ref = (int)i; axLabel = axs[i].label; }
            else if (axs[i].name != axs[ref].name || axs[i].unit != axs[ref].unit)
                conflict = true;
        }
        if (axAny && !conflict) {
            bool lendOk = true;
            for (size_t i = 0; i < axDocs.size(); i++) {
                if (!axHas[i] || axs[i].custom) continue;
                // same length = value k exists for every frame k the side has;
                // anything else would invent positions
                size_t nSide = axDocs[i] && axDocs[i]->seqId != 0
                             ? framesOfSeq(axDocs[i]->seqId).size() : 0;
                if (axs[ref].vals && nSide == axs[ref].vals->size()) {
                    axs[i] = axs[ref];
                    if (!axBorrowed.empty()) axBorrowed += ", ";
                    axBorrowed += axLetters[i];
                } else lendOk = false;
            }
            axUse = lendOk;
        }
    }
    // every notice line that will follow the table, counted before it is drawn
    // (none of them wrap, so each is exactly one line)
    std::string slotDiv;    // slots whose follow-frame found no partner frame
    for (const TExtra& x : XS)
        if (x.diverged) { if (!slotDiv.empty()) slotDiv += ", "; slotDiv += x.letter; }
    // a clamped sigma_fpn owes the reader a line, per side that clamped, and
    // the chart below has to be told about it before the table takes its height
    auto anyClamp = [](const AbTemporal& t) {
        if (!t.valid) return false;
        for (int p = 0; p < t.nPl; p++) if (t.clamped[p]) return true;
        return false;
    };
    int notices = 0;
    if (anyClamp(A)) notices++;
    if (anyClamp(B)) notices++;
    if (axAny && !axUse) notices++;
    if (!axBorrowed.empty()) notices++;
    if (g_abFollowDiverged) notices++;
    if (!slotDiv.empty()) notices++;
    if (A.valid && B.valid && A.nPl != B.nPl) notices++;
    if (!A.valid || !B.valid) notices++;
    if (dtypeMix) notices++;
    if (!sameRegion) notices++;
    else if (!canDelta && im->ch != Bim->ch) notices++;
    if (A.fromServer || B.fromServer) notices++;
    // the chart's floor: plot + its three rows of axis chrome + one legend row
    const float chartMinH = 70.0f * app.uiScale + ImGui::GetFontSize() * 3
                          + 12 * app.uiScale + ImGui::GetTextLineHeightWithSpacing();
    const float tblRoom = ImGui::GetContentRegionAvail().y
                        - notices * ImGui::GetTextLineHeightWithSpacing() - chartMinH;
    const float tblH = std::min(tblWant, std::max(tblRoom, rowH * 3));
    ImGuiTableFlags tblFlags = ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX;
    // clamped short of its rows: they must stay REACHABLE, not clipped away
    if (tblH < tblWant - 0.5f) tblFlags |= ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("abtemporal", 5, tblFlags, ImVec2(0, tblH))) {
        ImGui::TableSetupColumn(cQ.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(hA, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(hB, ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn(cD.c_str(), ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableSetupColumn("delta [%]", ImGuiTableColumnFlags_WidthFixed, numColW());
        ImGui::TableHeadersRow();
        // isPct: the quantity is already a percentage, so a relative difference
        // of a percentage is meaningless - the absolute one, in points, is the
        // honest answer. Kept general; today's three rows are all in DN.
        auto row = [&](const char* k, double a, double b, bool isPct) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn();
            if (A.valid) textNum("%.6g", a);
            else textNumStr(A.isStack ? "-" : "- (not a stack)");
            ImGui::TableNextColumn();
            if (B.valid) textNum("%.6g", b);
            else textNumStr(B.isStack ? "-" : "- (not a stack)");
            ImGui::TableNextColumn();
            if (A.valid && B.valid && canDelta) textNum("%.6g", a - b); else textNumStr("");
            ImGui::TableNextColumn();
            if (!A.valid || !B.valid || !canDelta) textNumStr("");
            // a quantity that is ALREADY a percentage has no meaningful relative
            // difference: points are the honest answer. (Today's three rows are
            // all in DN; the rule is here so the next row cannot get it wrong.)
            else if (isPct) { char t[48]; snprintf(t, sizeof t, "%.4g pt", a - b); textNumStr(t); }
            else if (a == 0) textNumStr("-  (A = 0)");
            else textNum("%.4g", (a - b) / fabs(a) * 100.0);
        };
        // One row set per CFA plane: a stack whose planes are separated has
        // four sigma_t, and folding them into one row would put the iron rule
        // back (docs/terminology.md). When the two sides disagree about the
        // plane split, canDelta is already false and the columns stand alone.
        const int nPl = nPlanes;   // the height reserved above counted these rows
        for (int p = 0; p < nPl; p++) {
            char k1[80], k2[80], k3[80], k4[80];
            const char* sfx = nPl > 1 ? LIN_PLANES[p] : "";
            snprintf(k1, sizeof k1, "temporal noise (sigma_t) %s", sfx);
            // ONE name for one quantity: this table said sigma_s while the two
            // single-side tables above it said sigma_fpn (#57 judgment 2 -
            // docs/analysis-layers.md §3.2 leans on the names to keep the
            // corrected row and the ROIs column apart, which needs the names to
            // be the canon's).
            snprintf(k2, sizeof k2, "fixed pattern (sigma_fpn) %s", sfx);
            snprintf(k3, sizeof k3, "  temporal residual subtracted (fpn_corr) %s", sfx);
            snprintf(k4, sizeof k4, "total (quadrature) %s", sfx);
            row(k1, A.sigT[p], B.sigT[p], false);
            row(k2, A.sigS[p], B.sigS[p], false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", FPN_TIP);
            row(k3, A.sigCorr[p], B.sigCorr[p], false);
            row(k4, A.sigTot[p], B.sigTot[p], false);
        }
        ImGui::EndTable();
    }
    // A clamp is a property of ONE side's measurement, so it is named with its
    // side - a delta column beside a clamped 0 is a difference against a bound,
    // not against a fixed pattern, and the reader has to be told which side.
    {
        const AbTemporal* sides2[2] = { &A, &B };
        static const char* letters2[2] = { "A", "B" };
        for (int i = 0; i < 2; i++) {
            const AbTemporal& t = *sides2[i];
            if (!t.valid) continue;
            std::string pl;
            for (int p = 0; p < t.nPl; p++)
                if (t.clamped[p]) {
                    if (!pl.empty()) pl += ", ";
                    pl += t.nPl > 1 ? LIN_PLANES[p] : "value";
                }
            if (!pl.empty())
                ImGui::TextColored(AB_AMBER,
                    "%s: sigma_fpn clamped at 0 (%s) - the temporal residual is not "
                    "below the spatial spread, so no fixed pattern is resolved there",
                    letters2[i], pl.c_str());
        }
    }
    if (g_abFollowDiverged)
        ImGui::TextColored(ImVec4(0.98f, 0.76f, 0.35f, 1),
                           "follow-frame: B's stack has no frame %d - showing the "
                           "last one it had, so this is not a matched pair",
                           im->seqIndex);
    if (!slotDiv.empty())
        ImGui::TextColored(ImVec4(0.98f, 0.76f, 0.35f, 1),
                           "follow-frame: slot %s: no frame %d in that stack - its "
                           "curve is the pinned frame's stack, its marker is stale",
                           slotDiv.c_str(), im->seqIndex);
    if (A.valid && B.valid && A.nPl != B.nPl)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "plane split differs (A %s, B %s): no delta - the two "
                           "columns are not the same quantity",
                           A.nPl > 1 ? "CFA planes" : "plane=all",
                           B.nPl > 1 ? "CFA planes" : "plane=all");
    if (!A.valid || !B.valid)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "A: %s   |   B: %s",
                           A.valid ? "ok" : A.note, B.valid ? "ok" : B.note);
    if (dtypeMix)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "dtype mismatch (A %s, B %s): both sides are DN, but the "
                           "delta spans two storage classes", im->dtype.c_str(),
                           Bim->dtype.c_str());
    if (!sameRegion)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "regions differ (A %s, B %s): no delta - the two columns "
                           "are not the same quantity", regionOf(A), regionOf(B));
    else if (!canDelta && im->ch != Bim->ch)
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "channel counts differ (A %dch, B %dch): no delta - the two "
                           "columns are not the same quantity", im->ch, Bim->ch);
    if (A.fromServer || B.fromServer)
        ImGui::TextDisabled("source: A %s, B %s", A.fromServer ? "server" : "local",
                            B.fromServer ? "server" : "local");
    if (axAny && !axUse) {
        std::string what;
        for (size_t i = 0; i < axDocs.size(); i++) {
            if (!axHas[i]) continue;
            if (!what.empty()) what += "; ";
            what += axLetters[i] + ": " + (axs[i].custom ? axs[i].label : "frame index");
        }
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "custom x axes differ (%s) - one plot has one axis: "
                           "plotted on frame index", what.c_str());
    }
    if (!axBorrowed.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                           "%s: no x axis of its own - plotted on A's (paired by frame "
                           "number, as follow-frame already assumes)", axBorrowed.c_str());
    }

    // ---- per-frame mean over time: one curve per compare slot, one axis ----
    // What each slot puts on the chart and, when it puts nothing there, WHY.
    // The chart used to give up entirely the moment A had no series
    // ("if (!A.mean || A.mean->empty()) return;") - so a B with a perfectly
    // good time axis went unplotted because A did not have one, and the panel
    // said nothing about either. A missing curve is a fact about the data and
    // has to be stated; a blank space is a bug report waiting to be filed.
    struct TSlot {
        std::string letter, label, why;
        const AbTemporal* t = nullptr;
        const ImageDoc* doc = nullptr;
        ImU32 ink = 0;
        bool has = false;      // >= 2 measured frames: there is a time axis to plot
        bool stale = false;    // follow-frame diverged: not A's partner right now
    };
    std::vector<TSlot> slots;
    auto addSlot = [&](const std::string& letter, const AbTemporal& t,
                       const ImageDoc* d, size_t inkIdx, bool stale) {
        TSlot s;
        s.letter = letter; s.label = abDocLabel(d); s.t = &t; s.doc = d;
        s.ink = slotInk(inkIdx);
        s.stale = stale;
        s.has = t.mean && t.idx && t.mean->size() >= 2 && t.idx->size() >= 2;
        if (!s.has)
            s.why = !t.isStack ? "a single frame has no time axis"
                  : !t.valid   ? t.note
                  : t.fromServer ? "the server aggregate returned no per-frame means"
                                 : "fewer than 2 measured frames";
        slots.push_back(s);
    };
    addSlot("A", A, im, 0, false);
    addSlot("B", B, Bim, 1, g_abFollowDiverged);
    for (size_t i = 0; i < XS.size(); i++)
        addSlot(XS[i].letter, XS[i].t, XS[i].doc, 2 + i, XS[i].diverged);

    g_tchart = TemporalChartProbe{};
    g_tchart.ran = true;
    g_tchart.availBefore = ImGui::GetContentRegionAvail().y;
    g_tchart.winBot = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    g_tchart.nSlots = (int)std::min(slots.size(), sizeof g_tchart.slot);
    for (int i = 0; i < g_tchart.nSlots; i++) g_tchart.slot[i] = slots[i].letter[0];

    float mn = FLT_MAX, mx = -FLT_MAX, fx0 = FLT_MAX, fx1 = -FLT_MAX;
    int nHas = 0;
    bool anyMosaic = false;
    std::string noAxis;
    for (size_t i = 0; i < slots.size(); i++) {
        const TSlot& s = slots[i];
        if (s.t->nPl > 1) anyMosaic = true;
        if (!s.has) {
            if (!noAxis.empty()) noAxis += ";   ";
            noAxis += s.letter + " (" + elideFront(s.label, 22) + ") " + s.why;
            if (g_tchart.nNoAxis < (int)sizeof g_tchart.noAxisSlot)
                g_tchart.noAxisSlot[g_tchart.nNoAxis++] = s.letter[0];
            continue;
        }
        nHas++;
        for (float v : *s.t->mean) { mn = std::min(mn, v); mx = std::max(mx, v); }
        // min/max over the MAPPED positions, never front/back: a custom axis
        // (temperature up and down) is legitimately non-monotonic
        for (float ix : *s.t->idx) {
            float x = axUse ? (float)axs[i].at(ix) : ix;
            fx0 = std::min(fx0, x);
            fx1 = std::max(fx1, x);
        }
    }
    if (!nHas) {
        // Nothing to plot on ANY side. Say which side and why, rather than
        // leaving the space where a chart was.
        ImGui::TextColored(AB_AMBER, "no per-frame chart: %s", noAxis.c_str());
        ImGui::TextDisabled("a chart of frame index needs a STACK with at least two "
                            "measured frames on one side (sigma_t is a property of a "
                            "stack, never of a frame).");
        return;
    }
    if (fx0 > fx1) { fx0 = 0; fx1 = 1; }
    char yl[160];
    const char* rlab = sameRegion && !A.roiUsed ? "frame" : "ROI";
    // The per-frame mean is a LEVEL, not a noise statistic, so it is
    // legitimately pooled over the mosaic - but sitting under a table that is
    // split by plane it has to SAY it is pooled (docs/terminology.md).
    const char* plab = anyMosaic ? ", plane=all" : "";
    if (dtypeMix)
        snprintf(yl, sizeof yl, "%s mean value (%s%s)  -  A %s / B %s, DTYPE MISMATCH",
                 rlab, unit.c_str(), plab, im->dtype.c_str(), Bim->dtype.c_str());
    else snprintf(yl, sizeof yl, "%s mean value (%s%s)", rlab, unit.c_str(), plab);

    // Sharing one plot, every slot needs its own ink; alone in its own panel it
    // is stroked exactly like A, so the panels can be read against each other.
    // (Same rule as the projection profiles - a dash pattern reads as a broken
    // curve rather than as "this one is B".)
    auto curve = [&](const PlotRect& tp, size_t si, ImU32 ink) {
        const TSlot& s = slots[si];
        if (!tp.ok || !s.has) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::vector<ImVec2> pts;
        pts.reserve(s.t->mean->size());
        for (size_t i = 0; i < s.t->mean->size() && i < s.t->idx->size(); i++) {
            float x = axUse && si < axs.size() ? (float)axs[si].at((*s.t->idx)[i])
                                               : (*s.t->idx)[i];
            pts.push_back(tp.at(x, (*s.t->mean)[i]));
        }
        if (pts.size() < 2) return;
        dl->AddPolyline(pts.data(), (int)pts.size(), ink, 0, 1.5f);
        if (si < sizeof g_tchart.pts / sizeof g_tchart.pts[0])
            g_tchart.pts[si] = (int)pts.size();
    };
    // The current-frame position. DASHED, because it is a reference, not data
    // - drawn solid it read as a second curve ("frame 0 のところに黄色いやつ",
    // user report), which is exactly the confusion the dashed-is-reference
    // rule exists to prevent. It is also named in the legend now: an unlabelled
    // vertical line cannot be told from a series by looking.
    const ImU32 MARKER_INK = IM_COL32(255, 184, 77, 200);
    auto marker = [&](const PlotRect& tp, const ImageDoc* d, size_t si) {
        if (!tp.ok || !d) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = axUse && si < axs.size() ? (float)axs[si].at((float)d->seqIndex)
                                           : (float)d->seqIndex;
        float mxp = tp.at(x, tp.ymin).x;
        ImVec2 seg[2] = { ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y) };
        float dash = 5 * app.uiScale;
        addDashedPolyline(dl, seg, 2, MARKER_INK, 1.0f, dash, dash);
    };
    // the legend names only the curves that exist; the slots that have none are
    // named in their own line under the plot, with the reason
    std::vector<SlotLegendEntry> leg;
    for (const TSlot& s : slots) {
        if (!s.has) continue;
        std::string t = s.letter + ": " + elideFront(s.label, 22);
        // a partially loaded stack never reads as a complete one, here either
        if (s.t->expected > s.t->frames)
            t += "  n=" + std::to_string(s.t->frames) + "/" + std::to_string(s.t->expected);
        leg.push_back({ t, s.ink });
    }
    // ...and the one non-data mark on the plot is named like everything else
    leg.push_back({ "current frame", MARKER_INK });

    const ImGuiStyle& st = ImGui::GetStyle();
    const float minSide = AB_MIN_SIDE * app.uiScale;
    bool side = abSideBySide();
    // side by side generalises to however many slots have a curve; the fallback
    // threshold is per PANEL, which is what AB_MIN_SIDE has always measured
    float perPanel = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * (nHas - 1))
                   / (float)nHas;
    bool tooNarrow = side && perPanel < minSide;
    if (tooNarrow) side = false;
    float tAvail = ImGui::GetContentRegionAvail().y
                 - (ImGui::GetFontSize() * 3 + 12 * app.uiScale)
                 - (tooNarrow ? abNarrowNoteH() : 0.0f)
                 - (side ? 0.0f : slotLegendRow(leg, false))
                 - (noAxis.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
    if (side) tAvail -= ImGui::GetTextLineHeight() + 6 * app.uiScale;   // the name band
    float plotH = std::max(tAvail, 70.0f * app.uiScale);
    const char* xlab = axUse ? axLabel.c_str() : "frame number (index in stack)";
    // Box-zoom. Drag a rectangle on the chart to narrow both ranges to it;
    // double-click resets. The state is keyed to WHAT is on the axes (A's
    // document and the x label): a zoom in seconds must not survive a switch
    // to a different stack or axis, where the same numbers mean something
    // else. While zoomed the chart says so - a magnified chart that does not
    // announce itself reads as the whole signal.
    static bool tzOn = false;
    static float tzX0 = 0, tzX1 = 1, tzY0 = 0, tzY1 = 1;
    static std::string tzSig;
    {
        std::string sig = std::string(xlab) + "|" +
                          std::to_string(im ? (unsigned long long)im->uid : 0ULL);
        if (sig != tzSig) { tzSig = sig; tzOn = false; }
    }
    if (tzOn) { fx0 = tzX0; fx1 = tzX1; mn = tzY0; mx = tzY1; }
    static ImVec2 tzDragA;              // where the current drag began
    static bool tzDragging = false;
    auto zoomUI = [&](const PlotRect& tp) {
        if (!tp.ok) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool hov = ImGui::IsMouseHoveringRect(tp.p0, tp.p1) &&
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            tzOn = false; tzDragging = false;
        }
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            tzDragA = ImGui::GetMousePos(); tzDragging = true;
        }
        if (tzDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ImVec2 b2 = ImGui::GetMousePos();
            dl->AddRectFilled(tzDragA, b2, IM_COL32(120, 190, 255, 30));
            dl->AddRect(tzDragA, b2, IM_COL32(120, 190, 255, 160));
        }
        if (tzDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            tzDragging = false;
            float x0, x1, y0, y1;
            if (zoomRangeFromDrag(tp, tzDragA, ImGui::GetMousePos(), x0, x1, y0, y1)) {
                tzX0 = x0; tzX1 = x1; tzY0 = y0; tzY1 = y1; tzOn = true;
            }
        }
        if (tzOn) {
            const char* zt = "zoomed - double-click to reset";
            ImVec2 ts = ImGui::CalcTextSize(zt);
            dl->AddText(ImVec2(tp.p1.x - ts.x - 4 * app.uiScale, tp.p0.y + 2 * app.uiScale),
                        AB_AMBER32, zt);
        }
    };
    if (!side) {
        PlotRect tp = beginPlot(xlab, yl, fx0, fx1, mn, mx, !axUse, false, plotH);
        if (tp.ok) {
            g_tchart.plotted = true;
            g_tchart.plotTop = tp.p0.y; g_tchart.plotBot = tp.p1.y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(tp.p0, tp.p1, true);
            for (size_t i = 0; i < slots.size(); i++) curve(tp, i, slots[i].ink);
            // the "you are here" marker is A's frame index, and a lone frame
            // has none - a line at 0 would name a frame of somebody's stack
            if (A.isStack) marker(tp, im, 0);
            dl->PopClipRect();
            zoomUI(tp);
        }
        slotLegendRow(leg, true);
    } else {
        g_tchart.sideBySide = true;
        float childH = plotH + ImGui::GetFontSize() * 3 + 12 * app.uiScale
                     + ImGui::GetTextLineHeight() + 8 * app.uiScale;
        bool first = true;
        for (size_t i = 0; i < slots.size(); i++) {
            if (!slots[i].has) continue;
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::BeginChild(("##temp" + slots[i].letter).c_str(), ImVec2(perPanel, childH),
                              false, ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse);
            drawABBand(slots[i].letter.c_str(), slots[i].label, slots[i].stale);
            {   // same limits in every panel, by construction
                PlotRect tp = beginPlot(xlab, yl, fx0, fx1, mn, mx, !axUse, false, plotH);
                if (tp.ok) {
                    if (!g_tchart.plotted) { g_tchart.plotted = true; g_tchart.plotTop = tp.p0.y; }
                    g_tchart.plotBot = std::max(g_tchart.plotBot, tp.p1.y);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->PushClipRect(tp.p0, tp.p1, true);
                    curve(tp, i, slotInk(0));   // its own panel: A's stroke
                    marker(tp, slots[i].doc, i);
                    dl->PopClipRect();
                    zoomUI(tp);         // shared limits: a zoom in one panel zooms all
                }
            }
            ImGui::EndChild();
        }
    }
    // ...and the slots that contributed no curve, named, with the reason. A
    // chart that quietly holds fewer curves than there are compare slots is
    // indistinguishable from a broken one.
    if (!noAxis.empty()) ImGui::TextColored(AB_AMBER, "no curve for %s", noAxis.c_str());
    if (tooNarrow) abNarrowNote();
}

static void drawPanelTemporal() {
    // a browser-fired aggregate (files nobody opened) owns the panel until
    // dismissed - it is the freshest thing the user asked for and has no
    // current image to attach to
    g_tchart = TemporalChartProbe{};   // stale probe data is worse than none
    if (app.srvTemporal.seqId == -2) { drawBrowseTemporal(); return; }
    ImageDoc* im = cur();
    if (!im) { ImGui::TextDisabled("no image"); return; }
    // Compare on: the A|B|delta|delta% table and the shared chart. A side that
    // is not a stack still gets its column and its line under the chart, saying
    // exactly that - INCLUDING A. Handing this branch only stacks is how a
    // lone-frame A used to leave the whole panel blank while B, a full stack
    // with a perfectly good time axis, went unplotted.
    if (ImageDoc* Bim = abStatsB()) {
        drawTemporalAB(im, Bim);
        return;
    }
    if (im->seqId != 0) {
        App::SeqInfo* si = seqInfo(im->seqId);
        if (drawServerTemporal(si)) {         // remote stack, computed on the server
            // the export works here too: the summary carries the server's
            // numbers, the per-frame section covers the frames that are
            // RESIDENT locally and says how many that is
            temporalExportButtons();
            ImGui::SameLine();
            frameAxisPopupButton(im);
            return;
        }
        recomputeTemporalIfNeeded(im, app.temporal[0]);
        const App::TemporalState& T = app.temporal[0];
        ImGui::Text("Temporal");
        ImGui::SameLine();
        // for a remote stack whose server measure failed, be explicit that this
        // is a PARTIAL local computation, and of how many
        int expected = si ? si->expectedFrames : 0;
        if (expected > T.frames)
            ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "[local, %d of %d frames]",
                               T.frames, expected);
        else
            ImGui::TextDisabled("[local, %d frames, %s, %s]", T.frames,
                                T.roiUsed ? "selected ROI" : "whole image",
                                T.nPl > 1 ? "CFA planes" : "plane=all");
        ImGui::SameLine();
        temporalExportButtons();
        ImGui::SameLine();
        frameAxisPopupButton(im);
        ImGui::Separator();
        if (!T.valid) {
            ImGui::TextDisabled("need >= 2 loaded frames of equal size");
        } else {
            // one column per CFA plane, exactly as the linearity fit table
            // does - a single pooled column for a mosaiced stack would be the
            // inter-plane level spread wearing sigma_fpn's name
            if (ImGui::BeginTable("temporalstats", 1 + T.nPl,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("quantity [DN]", ImGuiTableColumnFlags_WidthStretch);
                for (int p = 0; p < T.nPl; p++)
                    ImGui::TableSetupColumn(T.nPl > 1 ? LIN_PLANES[p] : "value",
                                            ImGuiTableColumnFlags_WidthFixed, numColW());
                ImGui::TableHeadersRow();
                // The local table said "sigma_s" while the server table beside
                // it and the export below it both said "sigma_fpn" - one
                // quantity under two names, which is the confusion the amended
                // §3.2 asks the NAMES to prevent. One name, and it is the
                // canon's (docs/terminology.md).
                temporalNoiseRows(T.nPl, T.tempNoise, T.fixedPattern, T.fpnCorr,
                                  T.fpnClamped, T.totalNoise);
                ImGui::EndTable();
            }
            temporalClampNote(T.nPl, T.fpnClamped);
            // the same row the server emits: excluded, and how many. A silent
            // exclusion is only half of not folding them in.
            if (T.nonFinite || T.dropped)
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                                   "non-finite samples (excluded) = %llu"
                                   "%s%zu sample(s) had < 2 valid frames",
                                   (unsigned long long)T.nonFinite,
                                   T.dropped ? ";  " : "", T.dropped);
            // The frame-axis LINEARITY FIT moved to its series-layer home
            // (docs/analysis-layers.md §3.3): a swept capture is a series by
            // definition, so the fit - window, methods, session-saved settings
            // - lives in the Series Analysis panel now, over frame | stack
            // mixed members. A section that silently vanished would read as
            // "the feature is gone" (the user report from the section's own
            // day one), so this line says where it went; the axis keeps its
            // MONITOR role here, which is the curve below.
            FrameAxis axPeek = frameAxisOf(im->seqId);
            if (axPeek.custom)
                ImGui::TextDisabled("linearity fit: Series Analysis panel (a swept capture "
                                    "is a series; the axis keeps the monitor curve below)");
            // per-frame mean over time - against the custom x axis when this
            // stack carries one (a real quantity per frame), else frame index.
            // Non-monotonic values are fine: the polyline follows frame order,
            // which is why the range is min/max and never front/back.
            FrameAxis ax = frameAxisOf(im->seqId);
            if (!ax.why.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", ax.why.c_str());
            float mn = FLT_MAX, mx = -FLT_MAX;
            for (float v : T.frameMean) { mn = std::min(mn, v); mx = std::max(mx, v); }
            float fx0 = FLT_MAX, fx1 = -FLT_MAX;
            for (float ix : T.idx) {
                float x = (float)ax.at(ix);
                fx0 = std::min(fx0, x); fx1 = std::max(fx1, x);
            }
            if (fx0 > fx1) { fx0 = 0; fx1 = 1; }
            char yl[80];
            // the per-frame mean is a LEVEL, not a noise statistic, so it is
            // legitimately pooled - but it still has to say so next to a table
            // that is split by plane
            snprintf(yl, sizeof yl, "%s mean value (%s%s)", T.roiUsed ? "ROI" : "frame",
                     abValueUnit(im->dtype).c_str(), T.nPl > 1 ? ", plane=all" : "");
            float tAvail = ImGui::GetContentRegionAvail().y
                         - (ImGui::GetFontSize() * 3 + 12 * app.uiScale);
            g_tchart.ran = true;
            g_tchart.nSlots = 1;
            g_tchart.slot[0] = 'A';
            g_tchart.winBot = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
            PlotRect tp = beginPlot(ax.label.c_str(), yl,
                                    fx0, fx1, mn, mx, !ax.custom, false,
                                    std::max(tAvail, 70.0f * app.uiScale));
            if (tp.ok) {
                g_tchart.plotted = true;
                g_tchart.plotTop = tp.p0.y; g_tchart.plotBot = tp.p1.y;
                g_tchart.pts[0] = (int)T.frameMean.size();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(tp.p0, tp.p1, true);
                for (size_t i = 1; i < T.frameMean.size(); i++)
                    dl->AddLine(tp.at((float)ax.at(T.idx[i - 1]), T.frameMean[i - 1]),
                                tp.at((float)ax.at(T.idx[i]), T.frameMean[i]),
                                IM_COL32(105, 220, 130, 255), 1.5f);
                if (ImageDoc* c2 = cur()) {   // marker for the frame on screen
                    float mxp = tp.at((float)ax.at((float)c2->seqIndex), tp.ymin).x;
                    dl->AddLine(ImVec2(mxp, tp.p0.y), ImVec2(mxp, tp.p1.y),
                                IM_COL32(255, 184, 77, 200));
                }
                dl->PopClipRect();
            }
        }
        return;
    }
    // A lone frame, no compare: there is no time axis at all. Say so - the
    // panel used to draw literally nothing here, which reads as a broken panel
    // rather than as "this image is not a stack".
    ImGui::Text("Temporal");
    ImGui::Separator();
    ImGui::TextDisabled("%s is a single frame: no time axis.", im->name.c_str());
    ImGui::TextDisabled("Temporal noise (sigma_t) is a property of a STACK. Open one, "
                        "or set a stack as compare B.");
}

