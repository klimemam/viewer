// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 設定
// ---- File > Preferences... : the one place a setting can be changed --------
//
// Issue #50. The panel does NOT decide what a setting is - that argument was
// settled once, in docs/settings-inventory.md 判断19, and its result is the
// SETTING_KEYS table. This file draws that table's rows and nothing else, which
// is the whole reason it can be short: every question about which keys exist,
// what they are called and what they accept was answered upstream.
//
// The row model (settingsPrefRows) is built in core/app/settings.inc and
// touches no ImGui, so --settings-selftest W1..W4 assert what every row SAYS
// without a window. What is left here is the drawing, which is the part that
// cannot be wrong in an interesting way.
//
// TWO THINGS THIS PANEL DOES THAT A LIST OF TOGGLES WOULD NOT:
//
//   IT NAMES WHO DECIDES. Four layers can (defaults < prefs.txt <
//   settings.jsonc < CLI) and #50's actual symptom was that the operator
//   cannot see which one won - they change a thing, it works, they restart,
//   it is back. Every row carries its badge.
//
//   IT SAYS SO BEFORE THE CHANGE SPRINGS BACK. Changing a key the file decides
//   warns at that moment (settingsWarnIfSticky), not at the next start.

// The label a key wears. Derived from the path's leaf rather than typed per
// key: a second name for a setting is a second thing to keep in step, and the
// path is already the name the file, the template and the error messages use.
static std::string prefLabelOf(const char* path) {
    std::string p = path ? path : "";
    const size_t dot = p.find_last_of('.');
    std::string leaf = dot == std::string::npos ? p : p.substr(dot + 1);
    // camelCase -> "camel case", capitalised: the file's spelling stays the
    // authority and this is only how it reads on screen.
    std::string out;
    for (size_t i = 0; i < leaf.size(); i++) {
        const unsigned char c = (unsigned char)leaf[i];
        if (i && isupper(c) && !isupper((unsigned char)leaf[i - 1])) out += ' ';
        out += (char)(i == 0 ? toupper(c) : tolower(c) == c ? c : c);
    }
    if (!out.empty()) out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}

// The badge, dimmed except when something is actually deciding the value from
// outside this machine's prefs - those two are the ones a reader needs to spot
// while scanning, because they are the ones that explain a change not sticking.
static void prefBadge(const PrefRow& r) {
    const bool loud = r.badge.rfind("settings.jsonc:", 0) == 0 ||
                      r.badge.rfind("command line", 0) == 0;
    if (loud) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", r.badge.c_str());
    else      ImGui::TextDisabled("%s", r.badge.c_str());
    if (ImGui::IsItemHovered()) {
        if (r.badge == "default")
            ImGui::SetTooltip("No layer sets this: the value is the built-in default.");
        else if (r.badge == "this machine")
            ImGui::SetTooltip("prefs.txt on this machine holds it. Changing it here is\n"
                              "permanent for this user.");
        else if (loud && r.badge[0] == 's')
            ImGui::SetTooltip("settings.jsonc sets this key, and the file wins at every\n"
                              "start. A change here lasts until the next one.");
        else
            ImGui::SetTooltip("A command-line flag set it for this run only.");
    }
}

// One row's control. The kind comes from settingsWidgetFor(), which is beside
// the applier's own knowledge of what each key accepts - so a control can never
// offer a value the file itself would refuse.
static void prefWidget(const PrefRow& r) {
    ImGui::PushID((int)r.id);
    switch (r.widget) {
        case SW_Bool: {
            // The app field is reached through the same switch settings.inc
            // uses; a second mapping here would be the second place to keep in
            // step that this panel exists to avoid.
            bool* b = prefBoolField(r.id);
            if (b && ImGui::Checkbox("##v", b)) {
                prefAfterChange(r.id);
                settingsWarnIfSticky(r.id, r.path);
            }
            break;
        }
        case SW_Enum: {
            std::vector<const char*> names;
            int cur = 0;
            prefEnumChoices(r.id, names, cur);
            if (!names.empty() &&
                ImGui::Combo("##v", &cur, names.data(), (int)names.size())) {
                prefEnumSet(r.id, cur);
                prefAfterChange(r.id);
                settingsWarnIfSticky(r.id, r.path);
            }
            break;
        }
        case SW_Int: {
            int* v = prefIntField(r.id);
            if (v && ImGui::InputInt("##v", v)) {
                prefAfterChange(r.id);
                settingsWarnIfSticky(r.id, r.path);
            }
            break;
        }
        case SW_Text: {
            // series.defaultUnit lives in a fixed buffer (app.lin.unit) rather
            // than a std::string, so it gets the one branch of its own instead
            // of a pointer this accessor cannot give.
            if (r.id == SK_defaultUnit) {
                if (ImGui::InputText("##v", app.lin.unit, sizeof app.lin.unit)) {
                    prefAfterChange(r.id);
                    settingsWarnIfSticky(r.id, r.path);
                }
                break;
            }
            std::string* s = prefTextField(r.id);
            if (s) {
                char buf[512];
                snprintf(buf, sizeof buf, "%s", s->c_str());
                if (ImGui::InputText("##v", buf, sizeof buf)) {
                    *s = buf;
                    prefAfterChange(r.id);
                    settingsWarnIfSticky(r.id, r.path);
                }
            }
            break;
        }
        case SW_Link:
            // A list with a panel of its own. Editing an array of objects in a
            // one-line control is not something this panel can do honestly, so
            // it points at the place that can rather than pretending.
            if (ImGui::SmallButton("Open...")) prefOpenLink(r.id);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", prefLinkSummary(r.id).c_str());
            break;
        default:
            break;
    }
    ImGui::PopID();
}

static void drawPreferencesPanel() {
    if (!app.prefsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 54, ImGui::GetFontSize() * 30),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &app.prefsOpen)) { ImGui::End(); return; }

    ImGui::TextDisabled("Changes here are written to prefs.txt. settings.jsonc is yours to "
                        "edit; keys it sets win at every start.");
    // The migration, in two buttons. The app does not write settings.jsonc
    // (判断3 - a writer would eat the user's comments on the first toggle), so
    // "make this permanent" is "here is exactly what to paste", and the editor
    // chain opens the file for them to paste INTO.
    if (ImGui::SmallButton("Copy template")) {
        ImGui::SetClipboardText(settingsTemplateText().c_str());
        toast("settings.jsonc template copied - paste it into the file");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The whole file, with every key at its current value.\n"
                          "Keys at their default are written as comments (判断9:\n"
                          "an unset key and a key written at the default are\n"
                          "different the day the default moves).");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open settings.jsonc")) {
        const std::string sp = settingsPath();
        if (sp.empty()) toast("no config directory on this machine", true);
        else {
            std::string err;
            if (!adapter::openInEditor(sp, err)) toast(err, true);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("readers.editor, then $EDITOR, then the OS default.\n"
                          "The file may not exist yet - Copy template first.");
    static char filter[96] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##preffilter", "filter", filter, sizeof filter);
    ImGui::Separator();

    const std::vector<PrefRow> rows = settingsPrefRows();
    const std::string f = filter;
    auto matches = [&](const PrefRow& r) {
        if (f.empty()) return true;
        std::string hay = std::string(r.path ? r.path : "") + " " +
                          prefLabelOf(r.path) + " " + (r.note ? r.note : "");
        std::string lo = hay, lf = f;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        std::transform(lf.begin(), lf.end(), lf.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        return lo.find(lf) != std::string::npos;
    };

    // Sections in the table's own order, so the panel reads like the file the
    // user would write - and so a section added there appears here with no edit.
    for (size_t s = 0; s < SETTING_SECTION_COUNT; s++) {
        const std::string sec = std::string(SETTING_SECTIONS[s]) + ".";
        std::vector<const PrefRow*> mine;
        for (const PrefRow& r : rows)
            if (r.path && std::string(r.path).rfind(sec, 0) == 0 && matches(r))
                mine.push_back(&r);
        if (mine.empty()) continue;
        ImGui::SeparatorText(SETTING_SECTIONS[s]);
        if (!ImGui::BeginTable(SETTING_SECTIONS[s], 3,
                               ImGuiTableFlags_SizingStretchProp)) continue;
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.33f);
        ImGui::TableSetupColumn("origin", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        for (const PrefRow* r : mine) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(prefLabelOf(r->path).c_str());
            ImGui::TextDisabled("%s", r->path);
            ImGui::TableNextColumn();
            if (r->status == SS_Read) prefWidget(*r);
            // The row's own right-click. "Copy as JSONC" is how a change made
            // here becomes permanent, since this panel writes prefs.txt and the
            // file is the user's to write (判断3).
            if (r->status == SS_Read && ImGui::BeginPopupContextItem(r->path)) {
                if (ImGui::MenuItem("Copy as JSONC")) {
                    ImGui::SetClipboardText(settingsKeyAsJsonc(r->id).c_str());
                    toast(std::string(r->path) + " copied as JSONC");
                }
                ImGui::EndPopup();
            }
            else ImGui::TextDisabled("%s", r->note ? r->note : "");
            ImGui::TableNextColumn();
            if (r->status == SS_Read) prefBadge(*r);
            else ImGui::TextDisabled("%s", r->status == SS_Later ? "not read yet"
                                                                 : "not a key of this file");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
