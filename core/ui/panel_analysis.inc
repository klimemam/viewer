// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: パネル/解析
// Unit resolution for grid rows: every numeric row gets a unit column entry.
// Keys that can know their unit carry it in the name ("snr_db", "*_pct",
// "mtf50 (cy/px)" - the documented plugin convention); image-value rows go
// through abValueUnit, which is the ONE rule for what a pixel value is
// measured in. This function used to print the storage dtype instead ("f32",
// "f32^2") on the argument that a float .npy might hold anything - the
// position c418e31 retired for the histogram, projection and temporal axes.
// It is settled elsewhere too: serve.cpp emits "mean [DN]" / "sigma_t [DN]"
// regardless of source dtype, as does the per-frame TSV. And this one is not
// cosmetic - the unit rides into the clipboard through "Copy table (TSV)", so
// an exported measurement table carried "ch0.var  f32^2".
static std::string unitForAnalysisKey(const std::string& key, const std::string& dtype) {
    auto ends = [&](const char* s) {
        size_t n = strlen(s);
        return key.size() >= n && key.compare(key.size() - n, n, s) == 0;
    };
    std::string img = abValueUnit(dtype);              // the file's own value unit
    if (ends("snr_db"))                              return "dB";
    if (ends("_pct"))                                return "%";
    if (key.find("(cy/px)") != std::string::npos)    return "cy/px";
    if (key == "sfr@nyquist" || key == "finite ratio") return "ratio";
    if (ends("_deg"))                                return "deg";
    if (key == "lines_used")                         return "lines";
    if (key == "pixels")                             return "px";
    if (ends(".entropy"))                            return "bit";
    if (ends(".var"))                                return img + "^2";
    if (key == "varlap" || key == "tenengrad" || key == "grad_mean")
        return "a.u.";                               // relative-only metrics
    if (ends(".mean") || ends(".std") || ends(".noise") || ends(".min") ||
        ends(".max") || ends(".p1") || ends(".p50") || ends(".p99"))
        return img;
    return "";     // text rows (method / note) and keys we cannot vouch for
}

// The number the user came for, per analyzer: mtf50, prnu%, the noise floor.
// Host knowledge until an ABI v3 lets plugins declare it - same trade-off as
// the question groups in the Measure menu, and just as additive.
static const char* analyzerHeadlineSuffix(const std::string& name) {
    if (name == "noise/floor")         return ".noise";
    if (name == "uniformity/prnu-fpn") return ".prnu_pct";
    if (name == "sharpness/gradient")  return "tenengrad";
    if (name == "iso12233/e-sfr")      return "mtf50 (cy/px)";
    return nullptr;                    // stats/moments: all rows are peers
}

// The "Copy table (TSV)" document, built from the panel's cached state. A
// function rather than button-local code for the buildTemporalExport reason:
// what a report will paste has to be assertable by a machine, and
// --anaprov-selftest asserts on this exact string.
static std::string buildAnalysisTsv() {
    const auto& ana = app.ana;
    // provenance rides along as a # comment: a pasted grid must carry
    // the same evidence as a screenshot of the panel
    std::string tsv = "# " + ana.prov + "\n";
    tsv += "metric\tunit";
    for (const auto& cn : ana.cols) tsv += "\t" + cn;
    tsv += "\n";
    for (int k = 0; k < (int)ana.keys.size(); k++) {
        tsv += ana.keys[k] + "\t" + (k < (int)ana.units.size() ? ana.units[k] : "");
        for (int c = 0; c < (int)ana.cols.size(); c++)
            tsv += "\t" + (c < (int)ana.vals[k].size() ? ana.vals[k][c] : std::string());
        tsv += "\n";
    }
    return tsv;
}

static void drawPanelAnalysis() {
    ImageDoc* im = cur();
    if (im && !plugin_host::analyzers().empty()) {
        const auto& anas = plugin_host::analyzers();
        app.anaSel = std::clamp(app.anaSel, 0, (int)anas.size() - 1);
        std::string curCat, curItem;
        splitAnalyzerName(anas[app.anaSel].name, curCat, curItem);
        // reserve the measured tail (Run + auto + target note) before splitting
        // the rest between the two combos, otherwise the note is clipped away
        std::vector<const App::Ann*> rois;
        for (const auto& a : app.anns)
            if (a.type == 0 && a.visible) rois.push_back(&a);
        char tgt[48];
        if (rois.empty()) snprintf(tgt, sizeof tgt, "| whole image");
        else snprintf(tgt, sizeof tgt, "| %d ROI(s)", (int)rois.size());
        const ImGuiStyle& st = ImGui::GetStyle();
        float tailW = ImGui::CalcTextSize("Run").x + st.FramePadding.x * 2 + st.ItemSpacing.x
                    + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize("auto").x
                    + st.ItemSpacing.x + ImGui::CalcTextSize(tgt).x + st.ItemSpacing.x;
        float comboW = std::max(ImGui::GetFontSize() * 5.0f,
                                (ImGui::GetContentRegionAvail().x - tailW - st.ItemSpacing.x) * 0.5f);
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##anacat", curCat.c_str())) {   // level 1: category
            std::vector<std::string> seen;
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                bool dup = false;
                for (const auto& s : seen) if (s == c) { dup = true; break; }
                if (dup) continue;
                seen.push_back(c);
                if (ImGui::Selectable(c.c_str(), c == curCat) && c != curCat)
                    for (int j = 0; j < (int)anas.size(); j++) {
                        std::string c2, n2;
                        splitAnalyzerName(anas[j].name, c2, n2);
                        if (c2 == c) { app.anaSel = j; break; }
                    }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##anaitem", curItem.c_str())) { // level 2: measurement
            for (int i = 0; i < (int)anas.size(); i++) {
                std::string c, n;
                splitAnalyzerName(anas[i].name, c, n);
                if (c != curCat) continue;
                if (ImGui::Selectable(n.c_str(), i == app.anaSel)) app.anaSel = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool runClicked = ImGui::Button("Run") || app.anaRunRequest;
        app.anaRunRequest = false;
        ImGui::SameLine();
        ImGui::Checkbox("auto", &app.anaAuto);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("re-run when the image, ROI set, display range or CFA layout\n"
                              "changes. Off: results keep the run they came from and the\n"
                              "status line says so when the inputs have moved on.");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tgt);

        auto& ana = app.ana;
        // every input makeFrame() hands the plugin belongs in the key, or the
        // grid silently keeps showing numbers from the previous settings
        bool stale = ana.uid != im->uid || ana.plugin != app.anaSel || ana.rev != app.annRev ||
                     ana.dataRev != im->dataRev || ana.black != effBlack(*im) ||
                     ana.white != effWhite(*im) || ana.cfa != im->cfa ||
                     ana.cfaPattern != im->cfaPattern;
        bool ranNow = runClicked || (app.anaAuto && stale && !app.annBusy);
        if (ranNow) {
            double runT0 = nowSec();
            ana.cols.clear(); ana.keys.clear(); ana.vals.clear(); ana.series.clear(); ana.err.clear();
            ana.colColor.clear(); ana.units.clear(); ana.headline.clear();
            if (im->preview) promotePreview(im);   // measuring it = keeping it
            ana.img = im; ana.uid = im->uid; ana.plugin = app.anaSel; ana.rev = app.annRev;
            ana.dataRev = im->dataRev; ana.black = effBlack(*im); ana.white = effWhite(*im);
            ana.cfa = im->cfa; ana.cfaPattern = im->cfaPattern;
            auto runOne = [&](const psRect* roi, const std::string& colLabel, int colorIdx) {
                std::vector<std::pair<std::string, std::string>> rows;
                psFrame f = makeFrame(*im);
                char err[256] = { 0 };
                int32_t rc;
                if (anas[app.anaSel].isV2) {
                    AnaEmit2Ctx ectx = { &rows, &ana, (int)ana.cols.size(), colorIdx };
                    psAnalyzeSink2 sink = { &ectx, ana2Number, ana2Text, ana2Series, {} };
                    rc = anas[app.anaSel].v2.analyze(&f, roi, &sink, err, sizeof err);
                } else {
                    psAnalyzeSink sink = { &rows, anaEmitNumber, anaEmitText };
                    rc = anas[app.anaSel].v1.analyze(&f, roi, &sink, err, sizeof err);
                }
                if (rc != 0) {
                    ana.err += colLabel + ": " + (err[0] ? err : "failed") + "\n";
                    rows.clear();
                }
                ana.cols.push_back(colLabel);
                ana.colColor.push_back(colorIdx);   // header wears the ROI's color
                for (auto& r : ana.vals) r.resize(ana.cols.size());
                for (const auto& kv : rows) {
                    if (kv.first == "roi") continue;        // grid header already says which ROI
                    int rowIdx = -1;
                    for (int k = 0; k < (int)ana.keys.size(); k++)
                        if (ana.keys[k] == kv.first) { rowIdx = k; break; }
                    if (rowIdx < 0) {
                        ana.keys.push_back(kv.first);
                        ana.vals.emplace_back(ana.cols.size());
                        rowIdx = (int)ana.keys.size() - 1;
                    }
                    ana.vals[rowIdx][ana.cols.size() - 1] = kv.second;
                }
            };
            if (rois.empty()) {
                runOne(nullptr, "whole", -1);
            } else {
                for (const App::Ann* a : rois) {
                    // annotations are global across images: clamp to THIS image before
                    // handing the rect to a plugin (the ABI does not promise in-bounds)
                    int rx = std::clamp(a->x, 0, im->w);
                    int ry = std::clamp(a->y, 0, im->h);
                    int rw = std::clamp(a->w, 0, im->w - rx);
                    int rh = std::clamp(a->h, 0, im->h - ry);
                    if (rw < 1 || rh < 1) {
                        ana.cols.push_back(a->label + " (off)");
                        ana.colColor.push_back(a->color & 7);
                        for (auto& r : ana.vals) r.resize(ana.cols.size());
                        continue;
                    }
                    psRect rr = { (uint32_t)rx, (uint32_t)ry, (uint32_t)rw, (uint32_t)rh };
                    runOne(&rr, a->label, a->color & 7);
                }
            }
            ana.runMs = (float)((nowSec() - runT0) * 1000.0);
            // per-row presentation, derived once here so the draw loop below
            // only reads cached strings (no per-frame string building)
            {
                const char* hl = analyzerHeadlineSuffix(anas[app.anaSel].name);
                size_t hn = hl ? strlen(hl) : 0;
                for (const auto& k : ana.keys) {
                    ana.units.push_back(unitForAnalysisKey(k, im->dtype));
                    ana.headline.push_back(hl && k.size() >= hn &&
                                           k.compare(k.size() - hn, hn, hl) == 0);
                }
            }
            {   // Provenance, built once per run and shown verbatim until the
                // next one: where this ran, on what, over which target, when,
                // and how long. [local] is a promise, not decoration - numbers
                // in this grid are never quietly recomputed anywhere else.
                char ts[32] = "";
                time_t now = time(nullptr);
                if (struct tm* lt = localtime(&now))
                    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", lt);
                char fi[40] = "";
                if (im->seqId != 0) {
                    std::vector<int> fr = framesOfSeq(im->seqId);
                    int pos = 0;
                    for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
                    snprintf(fi, sizeof fi, "  frame %d/%d", pos + 1, (int)fr.size());
                }
                char pv[384];
                snprintf(pv, sizeof pv,
                         "[local] %s%s  %dx%d %s  |  %s  |  range %g..%g  |  %s  |  %.1f ms",
                         im->name.c_str(), fi, im->w, im->h, im->dtype.c_str(),
                         rois.empty() ? "whole image" : tgt + 2,   // tgt = "| N ROI(s)"
                         ana.black, ana.white, ts, ana.runMs);
                ana.prov = pv;
            }
        }
        // Run-state line: one line, every state, constant height - the grid
        // below must not jump when a result appears or goes stale.
        if (ana.img != im || ana.prov.empty()) {
            ImGui::TextDisabled("not measured yet  -  Run, M, or the Measure menu");
        } else {
            ImGui::TextDisabled("%s", ana.prov.c_str());
            if (stale && !ranNow && !app.anaAuto) {
                // amber, matching the temporal panel's partial-data warning:
                // old numbers are still shown (they were true when measured),
                // but they must not be read as describing the current inputs
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1),
                                   "|  inputs changed - Run (M)");
            }
        }
        // results toolbar: a constant row (disabled while empty, never absent,
        // so the grid below sits at a fixed height in every state)
        ImGui::BeginDisabled(ana.img != im || ana.keys.empty());
        if (ImGui::Button("Copy table (TSV)")) {
            ImGui::SetClipboardText(buildAnalysisTsv().c_str());
            toast("result grid copied as TSV (provenance included)");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ana.img != im || ana.series.empty() || app.csvDlg != nullptr);
        if (ImGui::Button("Export curves (CSV)...")) {
            // wide per series name - x, then one column per measured target -
            // so a spreadsheet plots it without reshaping. Column labels are
            // the same ROI labels the grid and canvas use; provenance rides
            // along as # comment rows. Built NOW: the results may change
            // while the OS dialog is open.
            std::string csv = "# " + ana.prov + "\n";
            std::vector<std::string> names;
            for (const auto& s : ana.series) {
                bool seen = false;
                for (const auto& n2 : names) if (n2 == s.name) { seen = true; break; }
                if (!seen) names.push_back(s.name);
            }
            char b[64];
            for (const auto& nm : names) {
                const App::AnalysisState::Series* first = nullptr;
                size_t maxN = 0;
                for (const auto& s : ana.series) {
                    if (s.name != nm) continue;
                    if (!first) first = &s;
                    maxN = std::max(maxN, s.ys.size());
                }
                if (!first) continue;
                csv += "# series: " + nm +
                       (first->yLabel.empty() ? "" : "  (y: " + first->yLabel + ")") + "\n";
                csv += first->xLabel.empty() ? "index" : first->xLabel;
                for (const auto& s : ana.series)
                    if (s.name == nm)
                        csv += "," + (s.col >= 0 && s.col < (int)ana.cols.size()
                                      ? ana.cols[s.col] : std::string("?"));
                csv += "\n";
                // x comes from the first curve: ROIs measured by one analyzer
                // share the sample grid; shorter curves leave trailing blanks
                for (size_t i = 0; i < maxN; i++) {
                    float xv = first->xs.empty() ? (float)i
                             : (i < first->xs.size() ? first->xs[i] : 0.0f);
                    snprintf(b, sizeof b, "%.6g", xv);
                    csv += b;
                    for (const auto& s : ana.series) {
                        if (s.name != nm) continue;
                        csv += ",";
                        if (i < s.ys.size()) { snprintf(b, sizeof b, "%.6g", s.ys[i]); csv += b; }
                    }
                    csv += "\n";
                }
            }
            if (!pfd::settings::available()) {
                toast("no file-dialog backend found (install zenity or kdialog)", true);
            } else {
                app.pendingCsv = std::move(csv);
                app.csvDlg = std::make_unique<pfd::save_file>("Export curves (CSV)",
                    "curves.csv", std::vector<std::string>{ "CSV (*.csv)", "*.csv" });
            }
        }
        ImGui::EndDisabled();
        // reference line: a spec limit the curves must clear reads instantly
        // off the plot; typing it beats remembering it
        ImGui::SameLine();
        ImGui::Checkbox("ref", &app.anaRefOn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("horizontal reference line on the plots below\n"
                              "(a spec value / pass line; saved with the session)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        ImGui::BeginDisabled(!app.anaRefOn);
        ImGui::DragFloat("##anaref", &app.anaRef, 0.005f, -FLT_MAX, FLT_MAX, "%.3g");
        ImGui::EndDisabled();
        if (ana.img == im) {
            if (!ana.err.empty()) {
                ImGui::PushTextWrapPos(0.0f);       // plugin errors carry paths
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", ana.err.c_str());
                ImGui::PopTextWrapPos();
            }
            int nCols = 2 + (int)ana.cols.size();       // metric + unit + one per target
            // SizingFixedFit + explicit inner width: with StretchProp, ScrollX never
            // scrolled and simply clipped the numbers (hence the old 16-column cap)
            float colW = ImGui::GetFontSize() * 7.0f;
            float unitW = ImGui::GetFontSize() * 3.0f;
            float pad = ImGui::GetStyle().CellPadding.x * 2;
            if (!ana.keys.empty() &&
                ImGui::BeginTable("anagrid", nCols,
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0, 0),
                                  (nCols - 1) * (colW + pad) + unitW + pad + nCols + 1)) {
                ImGui::TableSetupScrollFreeze(2, 1);   // metric + unit stay in view
                ImGui::TableSetupColumn("metric", ImGuiTableColumnFlags_WidthFixed, colW);
                ImGui::TableSetupColumn("unit", ImGuiTableColumnFlags_WidthFixed, unitW);
                for (const auto& cn : ana.cols)
                    ImGui::TableSetupColumn(cn.c_str(), ImGuiTableColumnFlags_WidthFixed, colW);
                // custom header row so each target column wears its ROI's
                // color: the frame on the canvas, the curve in the plot and
                // the column in the grid identify each other without a legend
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn(); ImGui::TableHeader("metric");
                ImGui::TableNextColumn(); ImGui::TableHeader("unit");
                for (int c = 0; c < (int)ana.cols.size(); c++) {
                    ImGui::TableNextColumn();
                    int ci = c < (int)ana.colColor.size() ? ana.colColor[c] : -1;
                    if (ci >= 0)
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(ANN_COLORS[ci & 7]));
                    ImGui::TableHeader(ana.cols[c].c_str());
                    if (ci >= 0) ImGui::PopStyleColor();
                }
                // headline rows (mtf50, prnu%, noise floor) wear the theme
                // accent + a tinted row: the grid keeps every number, the
                // emphasis says which one the analyzer exists to produce
                ImVec4 acc = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImU32 accBg = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.14f));
                for (int k = 0; k < (int)ana.keys.size(); k++) {
                    bool hl = k < (int)ana.headline.size() && ana.headline[k];
                    ImGui::TableNextRow();
                    if (hl) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, accBg);
                    ImGui::TableNextColumn();
                    if (hl) ImGui::TextUnformatted(ana.keys[k].c_str());
                    else    ImGui::TextDisabled("%s", ana.keys[k].c_str());
                    ImGui::TableNextColumn();
                    if (k < (int)ana.units.size() && !ana.units[k].empty())
                        ImGui::TextDisabled("%s", ana.units[k].c_str());
                    for (int c = 0; c < (int)ana.cols.size(); c++) {
                        ImGui::TableNextColumn();
                        if (c >= (int)ana.vals[k].size() || ana.vals[k][c].empty()) {
                            ImGui::TextDisabled("-");
                            continue;
                        }
                        const std::string& v = ana.vals[k][c];
                        // numbers right-aligned in their fixed column (textNum
                        // rule: digits keep their place across runs); text rows
                        // (method / note) stay left-aligned prose
                        bool num = v[0] == '-' || v[0] == '+' || v[0] == '.' ||
                                   (v[0] >= '0' && v[0] <= '9');
                        if (hl) ImGui::PushStyleColor(ImGuiCol_Text, acc);
                        if (num) textNumStr(v);
                        else     ImGui::TextUnformatted(v.c_str());
                        if (hl) ImGui::PopStyleColor();
                    }
                }
                ImGui::EndTable();
            }
            drawAnalysisPlots();   // curves from V2 analyzers (one plot per series name)
        }
    }
}

