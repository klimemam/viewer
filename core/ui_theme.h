// "Aurora" modern theme for the viewer (design study: docs/imgui_modern_design.md).
// Plain ImGui style values only — no extensions, works on vanilla ImGui 1.91+.
#pragma once
#include "imgui.h"

namespace ui_theme {

enum Variant { VariantDark = 0, VariantLight = 1 };

struct Accent {
    const char* name;
    ImVec4 color;                     // sRGB 0-1
};
const Accent* accents();              // preset list
int accentCount();

// Apply the full theme (shapes + colors) to the current ImGui context.
// Resets the style first, so it is safe to call again at runtime
// (e.g. from the View > Theme menu); uiScale is re-applied each time.
void apply(int variant, int accentIdx, float uiScale);

// Background behind everything (use for glClearColor) — matches the theme's
// darkest layer so letterboxing around the canvas looks intentional.
ImVec4 clearColor(int variant);

}  // namespace ui_theme
