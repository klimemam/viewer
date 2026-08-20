// "Aurora" modern theme for the viewer (design study: docs/features/theme/imgui_modern_design.md).
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
void apply(int variant, int accentIdx, float uiScale, bool compact = true);

// Background behind everything (use for glClearColor) — matches the theme's
// darkest layer so letterboxing around the canvas looks intentional.
ImVec4 clearColor(int variant);

// One ink per Files row kind: the data layers frame ≼ stack ≼ series, plus the
// batch management boundary. The tree says what a row is without reading its name.
//
// Deliberately LOW chroma. Rule 2 of this theme is that the accent — and only
// the accent — means "where am I / what is selected", and the accent is one of
// five the user picks. A layer tint at accent saturation would read as a
// selection, and would collide with whichever accent they chose. These sit near
// the body text in lightness and differ in hue alone: enough to tell four
// labels apart, not enough for any of them to shout.
//
// The hues walk in containment order (frame → stack → series → batch), so the
// sequence itself carries the nesting rather than being four arbitrary colors.
enum Layer { LayerFrame = 0, LayerStack = 1, LayerSeries = 2, LayerBatch = 3,
             LayerSet = 4 };   // AnalysisSet - the canon's 5th layer
ImVec4 layerInk(int variant, int layer);

}  // namespace ui_theme
