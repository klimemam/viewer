// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: 設定
// ---- File > Preferences... : the one place a setting can be changed --------
//
// Issue #50. The panel does NOT decide what a setting is - that argument was
// settled once, in docs/features/settings/settings-inventory.md 判断19, and its result is the
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
//   IT NAMES WHO DECIDES. Normally four layers can (defaults < prefs.txt <
//   settings.jsonc < CLI); gamma/grid also have a fifth Session layer.
//   #50's actual symptom was that the operator
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
// outside this machine's prefs. File, CLI, and Session are the ones a reader
// needs to spot while scanning.
static void prefBadge(const PrefRow& r) {
    const bool session = r.badge == "session (.vsession)";
    const bool loud = session || r.badge.rfind("settings.jsonc:", 0) == 0 ||
                      r.badge.rfind("command line", 0) == 0;
    if (loud) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1), "%s", r.badge.c_str());
    else      ImGui::TextDisabled("%s", r.badge.c_str());
    if (ImGui::IsItemHovered()) {
        if (r.badge == "default")
            ImGui::SetTooltip("No layer sets this: the value is the built-in default.");
        else if (r.badge == "this machine")
            ImGui::SetTooltip("prefs.txt on this machine holds it. Changing it here is\n"
                              "permanent for this user.");
        else if (session)
            ImGui::SetTooltip("The loaded .vsession owns this display value. Changes here\n"
                              "remain session-owned and are included in save/autosave.");
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
                if (r.id == SK_pixelGrid)
                    settingsDisplayGuiChanged(r.id, r.path);
                else {
                    prefAfterChange(r.id);
                    settingsWarnIfSticky(r.id, r.path);
                }
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
        case SW_Gamma: {
            // Presets remain one-click actions, while Custom is an exact text
            // buffer so incomplete/invalid input never reaches 1/gamma. The
            // mode is derived from the value; there is no separate flag that
            // could drift from a .vsession value such as 1.25.
            const int preset = displayGammaPreset(app.dispGamma);
            bool changed = false, chosePreset = false;
            if (ImGui::RadioButton("1.0 (linear)", preset == 0) && preset != 0) {
                app.dispGamma = 1.0f; changed = chosePreset = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("2.2", preset == 1) && preset != 1) {
                app.dispGamma = 2.2f; changed = chosePreset = true;
            }

            static char custom[64] = {};
            static uint32_t sourceBits = 0;
            static bool editing = false;
            const uint32_t bits = displayGammaBits(app.dispGamma);
            if (!editing || chosePreset || sourceBits != bits) {
                snprintf(custom, sizeof custom, "%s", displayGammaText(app.dispGamma).c_str());
                sourceBits = bits;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Custom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            const bool submitted = ImGui::InputText(
                "##custom", custom, sizeof custom, ImGuiInputTextFlags_EnterReturnsTrue);
            const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
            const bool active = ImGui::IsItemActive();
            if (!chosePreset && (submitted || deactivated)) {
                float gamma = 0;
                if (parseDisplayGamma(custom, gamma)) {
                    if (gamma != app.dispGamma) {
                        app.dispGamma = gamma;
                        changed = true;
                    }
                    sourceBits = displayGammaBits(app.dispGamma);
                    snprintf(custom, sizeof custom, "%s",
                             displayGammaText(app.dispGamma).c_str());
                } else {
                    toast("Display gamma must be a positive finite number; "
                          "the value was not changed.", true);
                    snprintf(custom, sizeof custom, "%s",
                             displayGammaText(app.dispGamma).c_str());
                    sourceBits = displayGammaBits(app.dispGamma);
                }
            }
            editing = active && !chosePreset;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Enter a positive finite number. Press Enter or leave "
                                  "the field to apply it.");
            if (changed) {
                settingsDisplayGuiChanged(r.id, r.path);
            }
            break;
        }
        case SW_Int: {
            int* v = prefIntField(r.id);
            if (v && ImGui::InputInt("##v", v)) {
                prefAfterChange(r.id);
                settingsWarnIfSticky(r.id, r.path);
            }
            // Board row 297. The one thing the row's own sentence cannot say by
            // being a sentence: NOTHING HAS TO BE RESTARTED. Every other number
            // on this panel that takes effect later says so in a toast
            // (uiScale); this one takes effect at the next poll, so it says so
            // here, where the question is asked.
            if (r.id == SK_watchInterval && ImGui::IsItemHovered())
                ImGui::SetTooltip("Seconds between polls, 1 .. 3600 (default 5).\n"
                                  "The watcher reads this every round, so a change\n"
                                  "takes effect at the NEXT poll - no restart.\n"
                                  "A peer's folder is asked every 15 s, or every\n"
                                  "poll when this is slower than that.");
            break;
        }
        case SW_Float: {
            // The keys whose value is a REAL number: the UI multiplier (#257) and
            // the memory budget (phase④). Committed on Enter or on leaving the box
            // rather than per keystroke: typing "1.25" passes through 1 and 1.2,
            // and each of those would be a toast and a prefs.txt write for a value
            // nobody asked for.
            float* v = prefFloatField(r.id);
            if (v && ImGui::InputFloat("##v", v, 0.0f, 0.0f, "%.2f",
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
                prefAfterChange(r.id);
                settingsWarnIfSticky(r.id, r.path);
            }
            if (r.id == SK_uiScale && ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = work it out from the display.\n"
                                  "Otherwise 0.5 .. 4, applied at the next start.\n"
                                  "The startup line on stderr says what was decided.");
            if (r.id == SK_memBudget && ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = work it out from this machine.\n"
                                  "Otherwise 0.5 .. 4096 GB. Fewer frames fit in a\n"
                                  "smaller budget, and a short stack says \"n of N\".");
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
                    // 板306: InputText returns true ONCE PER CHARACTER, so a
                    // row whose change is worth a sentence would say it thirty
                    // times for one typed path. The value is committed here
                    // every keystroke, as before - nothing typed is lost if the
                    // panel closes - and the sentence waits below.
                    g_prefTextMidEdit = true;
                    prefAfterChange(r.id);
                    g_prefTextMidEdit = false;
                    settingsWarnIfSticky(r.id, r.path);
                }
                // ...and here it is, once, when the box is left. Only
                // appearance.fontPath has a sentence to say; the Reset menu
                // reaches the same words through prefAfterChange, which is why
                // they live there and not in this branch.
                if (r.id == SK_fontPath && ImGui::IsItemDeactivatedAfterEdit())
                    toast(fontPathNextStartText());
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
    // What the machine currently makes of a value that says "decide for me".
    // Under the control rather than inside it: 0 is the honest contents of the
    // box (writing the resolved figure back would turn an automatic choice into
    // a permanent one), and the resolved number is what the user came to see.
    if (!r.effective.empty()) ImGui::TextDisabled("%s", r.effective.c_str());
    ImGui::PopID();
}

// The row's own right-click, on BOTH of its halves. "Copy as JSONC" is how a
// change made here becomes permanent, since this panel writes prefs.txt and the
// file is the user's to write (判断3); "Reset to default" is 判断9's way back,
// which the panel was missing - deleting the key is the answer in the file and
// there was no equivalent for a value prefs.txt had saved.
//
// It is submitted from the name cell as well as the value cell because the value
// cell's last item is whatever the control happened to be: a checkbox is a few
// pixels wide and a link row's summary is disabled text, so on those rows the
// menu was effectively unreachable. The label is always there and always the same
// size, so every row has one entrance that does not depend on its widget.
static void prefRowMenu(const PrefRow& r, const char* popupId) {
    if (r.status != SS_Read) return;
    if (!ImGui::BeginPopupContextItem(popupId)) return;
    ImGui::TextDisabled("%s", r.path);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy as JSONC")) {
        ImGui::SetClipboardText(settingsKeyAsJsonc(r.id).c_str());
        toast(std::string(r.path) + " copied as JSONC");
    }
    // Greyed at the default rather than hidden: "this is already the default" is
    // an answer, and an item that comes and goes is one the user has to hunt for.
    const bool atDefault = r.value.empty();
    if (ImGui::MenuItem("Reset to default", nullptr, false, !atDefault)) {
        settingsResetGui(r.id, r.path);
        toast(std::string(r.path) + " is back to its built-in default");
    }
    if (atDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Already at the built-in default, so nothing is written\n"
                          "for it (an unset key and a key written at the default\n"
                          "are different the day the default moves).");
    ImGui::EndPopup();
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
        // A section with no rows is normally not drawn - but one that carries a
        // SENTENCE is (§3.2's 注1行). `panels` is the only one today: its keys are
        // not read until layout.ini and prefs settle which file owns a clean
        // start, and a section that is simply absent tells the reader nothing at
        // all, while a section that says why is the same discipline as an
        // SS_Later row. Filtered out with the rest when a filter is typed.
        const char* secNote = settingSectionNote(SETTING_SECTIONS[s]);
        if (mine.empty()) {
            if (!secNote) continue;
            std::string hay = std::string(SETTING_SECTIONS[s]) + " " + secNote;
            std::string lo = hay, lf = f;
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            std::transform(lf.begin(), lf.end(), lf.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (!f.empty() && lo.find(lf) == std::string::npos) continue;
            ImGui::SeparatorText(SETTING_SECTIONS[s]);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", secNote);
            ImGui::PopTextWrapPos();
            continue;
        }
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
            // The entrance every row has, whatever its control turned out to be.
            // The id carries the PATH: an ImGui popup id is derived from the id
            // stack, and a literal "name" would be the same id on every row of
            // the table - so right-clicking one row would raise another's menu.
            const std::string nameMenuId = std::string(r->path ? r->path : "") + "#name";
            prefRowMenu(*r, nameMenuId.c_str());
            ImGui::TableNextColumn();
            if (r->status == SS_Read) {
                prefWidget(*r);
                prefRowMenu(*r, r->path);
                // The row's own sentence, for the keys the design gave one (§3.2's
                // 帰結 column). Under the control, wrapped: it is the difference
                // between "a setting exists" and "you can tell what it does".
                if (r->note && *r->note) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextDisabled("%s", r->note);
                    ImGui::PopTextWrapPos();
                }
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
