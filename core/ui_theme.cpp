// "Aurora" theme — C++ port of the design study in docs/imgui_modern_design.md.
//
// Design rules (see the doc for the full rationale):
//   1. Layered neutrals (app bg -> panel -> control), never pure black/white,
//      slightly blue-shifted so the grays look designed rather than default.
//   2. Exactly one accent color, used only for "where am I / what is selected"
//      (selection, checkmarks, sliders, active tab overline).
//   3. Soft rounding + generous padding.
//   4. Hairline borders (low-alpha) instead of opaque frames.
#include "ui_theme.h"

#include <algorithm>

namespace ui_theme {

static const Accent ACCENTS[] = {
    {"Aurora Blue",  ImVec4(0.42f, 0.55f, 1.00f, 1.0f)},
    {"Iris Violet",  ImVec4(0.61f, 0.48f, 1.00f, 1.0f)},
    {"Mint Teal",    ImVec4(0.25f, 0.82f, 0.78f, 1.0f)},
    {"Sunset Coral", ImVec4(1.00f, 0.48f, 0.42f, 1.0f)},
    {"Citrus Lime",  ImVec4(0.62f, 0.85f, 0.25f, 1.0f)},
};

const Accent* accents() { return ACCENTS; }
int accentCount() { return (int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])); }

static ImVec4 withAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }
static ImVec4 scaleRgb(ImVec4 c, float k) {
    return ImVec4(std::min(1.0f, c.x * k), std::min(1.0f, c.y * k),
                  std::min(1.0f, c.z * k), c.w);
}

// Geometry: rounding, padding, spacing (shared by both variants).
// Values are unscaled; apply() calls ScaleAllSizes(uiScale) afterwards.
static void applyShapes(ImGuiStyle& s) {
    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 9.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 6.0f;

    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 1.0f;
    s.TabBorderSize    = 0.0f;

    s.WindowPadding    = ImVec2(12, 10);
    s.FramePadding     = ImVec2(9, 5);
    s.CellPadding      = ImVec2(8, 4);
    s.ItemSpacing      = ImVec2(9, 7);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.IndentSpacing    = 20.0f;
    s.ScrollbarSize    = 12.0f;
    s.GrabMinSize      = 12.0f;

    s.WindowTitleAlign        = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding    = ImVec2(18, 4);

    s.AntiAliasedLines = true;
    s.AntiAliasedFill  = true;
    s.CircleTessellationMaxError = 0.1f;
}

static void applyDark(ImGuiStyle& s, ImVec4 accent) {
    ImVec4* c = s.Colors;

    const ImVec4 text     = ImVec4(0.91f, 0.91f, 0.94f, 1.0f);
    const ImVec4 textDim  = ImVec4(0.54f, 0.54f, 0.60f, 1.0f);
    const ImVec4 bg0      = ImVec4(0.075f, 0.075f, 0.094f, 1.0f);  // app backdrop
    const ImVec4 bg1      = ImVec4(0.098f, 0.098f, 0.120f, 1.0f);  // panels, menu bar
    const ImVec4 bg2      = ImVec4(0.137f, 0.137f, 0.173f, 1.0f);  // inputs, buttons
    const ImVec4 bg2Hov   = ImVec4(0.168f, 0.168f, 0.220f, 1.0f);
    const ImVec4 bg2Act   = ImVec4(0.196f, 0.196f, 0.259f, 1.0f);
    const ImVec4 hairline = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);

    const ImVec4 acc    = withAlpha(accent, 1.0f);
    const ImVec4 accHov = withAlpha(scaleRgb(accent, 1.12f), 1.0f);

    c[ImGuiCol_Text]           = text;
    c[ImGuiCol_TextDisabled]   = textDim;
    c[ImGuiCol_TextLink]       = acc;
    c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.32f);

    c[ImGuiCol_WindowBg]     = bg1;
    c[ImGuiCol_ChildBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]      = ImVec4(0.110f, 0.110f, 0.137f, 0.98f);
    c[ImGuiCol_MenuBarBg]    = bg1;
    c[ImGuiCol_Border]       = hairline;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]        = bg2;
    c[ImGuiCol_FrameBgHovered] = bg2Hov;
    c[ImGuiCol_FrameBgActive]  = bg2Act;

    // Flat title bars: floating windows (help/about) read as one surface.
    c[ImGuiCol_TitleBg]          = bg1;
    c[ImGuiCol_TitleBgActive]    = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bg1;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.18f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1, 1, 1, 0.24f);

    c[ImGuiCol_CheckMark]        = acc;
    c[ImGuiCol_SliderGrab]       = acc;
    c[ImGuiCol_SliderGrabActive] = accHov;

    c[ImGuiCol_Button]        = bg2;
    c[ImGuiCol_ButtonHovered] = bg2Hov;
    c[ImGuiCol_ButtonActive]  = withAlpha(accent, 0.38f);

    c[ImGuiCol_Header]        = withAlpha(accent, 0.22f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.32f);
    c[ImGuiCol_HeaderActive]  = withAlpha(accent, 0.42f);

    // Separators darker than the panels: dividers read as grooves.
    c[ImGuiCol_Separator]        = ImVec4(0.055f, 0.055f, 0.070f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.60f);
    c[ImGuiCol_SeparatorActive]  = acc;

    c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.50f);
    c[ImGuiCol_ResizeGripActive]  = acc;

    // Tabs: quiet by default, selected tab lifts and gets an accent overline.
    c[ImGuiCol_Tab]                       = bg1;
    c[ImGuiCol_TabHovered]                = bg2Hov;
    c[ImGuiCol_TabSelected]               = bg2;
    c[ImGuiCol_TabSelectedOverline]       = acc;
    c[ImGuiCol_TabDimmed]                 = bg1;
    c[ImGuiCol_TabDimmedSelected]         = bg2;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.35f);

    c[ImGuiCol_PlotLines]            = acc;
    c[ImGuiCol_PlotLinesHovered]     = accHov;
    c[ImGuiCol_PlotHistogram]        = acc;
    c[ImGuiCol_PlotHistogramHovered] = accHov;

    c[ImGuiCol_TableHeaderBg]     = bg2;
    c[ImGuiCol_TableBorderStrong] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TableBorderLight]  = hairline;
    c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1, 1, 1, 0.025f);

    c[ImGuiCol_DragDropTarget]        = acc;
    c[ImGuiCol_NavCursor]             = acc;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(accent, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);

    (void)bg0;
}

static void applyLight(ImGuiStyle& s, ImVec4 accent) {
    ImVec4* c = s.Colors;

    accent = scaleRgb(accent, 0.82f);  // darken for contrast on light bg
    const ImVec4 text     = ImVec4(0.13f, 0.13f, 0.17f, 1.0f);
    const ImVec4 textDim  = ImVec4(0.50f, 0.50f, 0.56f, 1.0f);
    const ImVec4 bg1      = ImVec4(0.957f, 0.957f, 0.969f, 1.0f);
    const ImVec4 bg2      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    const ImVec4 bg2Hov   = ImVec4(0.937f, 0.937f, 0.957f, 1.0f);
    const ImVec4 bg2Act   = ImVec4(0.898f, 0.898f, 0.929f, 1.0f);
    const ImVec4 hairline = ImVec4(0.0f, 0.0f, 0.10f, 0.10f);

    const ImVec4 acc    = withAlpha(accent, 1.0f);
    const ImVec4 accHov = withAlpha(scaleRgb(accent, 1.15f), 1.0f);

    c[ImGuiCol_Text]           = text;
    c[ImGuiCol_TextDisabled]   = textDim;
    c[ImGuiCol_TextLink]       = acc;
    c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.25f);

    c[ImGuiCol_WindowBg]     = bg1;
    c[ImGuiCol_ChildBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]      = ImVec4(1.0f, 1.0f, 1.0f, 0.99f);
    c[ImGuiCol_MenuBarBg]    = bg1;
    c[ImGuiCol_Border]       = hairline;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]        = bg2;
    c[ImGuiCol_FrameBgHovered] = bg2Hov;
    c[ImGuiCol_FrameBgActive]  = bg2Act;

    c[ImGuiCol_TitleBg]          = bg1;
    c[ImGuiCol_TitleBgActive]    = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bg1;

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0, 0, 0, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0, 0, 0, 0.26f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0, 0, 0, 0.34f);

    c[ImGuiCol_CheckMark]        = acc;
    c[ImGuiCol_SliderGrab]       = acc;
    c[ImGuiCol_SliderGrabActive] = accHov;

    c[ImGuiCol_Button]        = bg2;
    c[ImGuiCol_ButtonHovered] = bg2Hov;
    c[ImGuiCol_ButtonActive]  = withAlpha(accent, 0.30f);

    c[ImGuiCol_Header]        = withAlpha(accent, 0.16f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.26f);
    c[ImGuiCol_HeaderActive]  = withAlpha(accent, 0.36f);

    c[ImGuiCol_Separator]        = ImVec4(0.878f, 0.878f, 0.902f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.60f);
    c[ImGuiCol_SeparatorActive]  = acc;

    c[ImGuiCol_ResizeGrip]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.50f);
    c[ImGuiCol_ResizeGripActive]  = acc;

    c[ImGuiCol_Tab]                       = bg1;
    c[ImGuiCol_TabHovered]                = bg2Hov;
    c[ImGuiCol_TabSelected]               = bg2;
    c[ImGuiCol_TabSelectedOverline]       = acc;
    c[ImGuiCol_TabDimmed]                 = bg1;
    c[ImGuiCol_TabDimmedSelected]         = bg2;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.35f);

    c[ImGuiCol_PlotLines]            = acc;
    c[ImGuiCol_PlotLinesHovered]     = accHov;
    c[ImGuiCol_PlotHistogram]        = acc;
    c[ImGuiCol_PlotHistogramHovered] = accHov;

    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.937f, 0.937f, 0.953f, 1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0, 0, 0, 0.14f);
    c[ImGuiCol_TableBorderLight]  = hairline;
    c[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(0, 0, 0, 0.02f);

    c[ImGuiCol_DragDropTarget]        = acc;
    c[ImGuiCol_NavCursor]             = acc;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(accent, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.2f, 0.2f, 0.2f, 0.45f);
}

void apply(int variant, int accentIdx, float uiScale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();                 // reset: safe to re-apply at runtime
    applyShapes(s);
    ImVec4 accent = ACCENTS[std::clamp(accentIdx, 0, accentCount() - 1)].color;
    if (variant == VariantLight) applyLight(s, accent);
    else                         applyDark(s, accent);
    s.ScaleAllSizes(uiScale);
}

ImVec4 clearColor(int variant) {
    return variant == VariantLight ? ImVec4(0.918f, 0.918f, 0.937f, 1.0f)
                                   : ImVec4(0.075f, 0.075f, 0.094f, 1.0f);
}

}  // namespace ui_theme
