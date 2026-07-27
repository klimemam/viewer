"""Modern Dear ImGui theme — "Aurora" design study.

Design goals
------------
* Calm layered backgrounds (never pure black / pure white) so the *image*
  being viewed stays the brightest thing on screen.
* Exactly one accent color, used only for "where am I / what is selected"
  (selection, checkmarks, sliders, active tab overline, docking preview).
* Soft rounding + generous padding for a contemporary, low-noise look.
* Everything is parameterized: variant (dark / light) and accent are
  arguments, so the theme doubles as a playground for design exploration.

The values are plain ImGui style values, so the same numbers can be ported
1:1 to a C++ code base later.

Usage:
    from imgui_theme import apply_modern_style
    apply_modern_style(variant="dark", accent=ACCENTS["Aurora Blue"])
"""
from imgui_bundle import imgui

# Accent presets (RGB 0-1). One accent at a time — pick your mood.
ACCENTS = {
    "Aurora Blue": (0.42, 0.55, 1.00),
    "Iris Violet": (0.61, 0.48, 1.00),
    "Mint Teal": (0.25, 0.82, 0.78),
    "Sunset Coral": (1.00, 0.48, 0.42),
    "Citrus Lime": (0.62, 0.85, 0.25),
}

DEFAULT_ACCENT = ACCENTS["Aurora Blue"]


def _v4(r, g, b, a=1.0):
    return imgui.ImVec4(r, g, b, a)


def _with_alpha(rgb, a):
    return imgui.ImVec4(rgb[0], rgb[1], rgb[2], a)


def _scale(rgb, k):
    """Lighten (k > 1) or darken (k < 1) an RGB tuple, clamped to [0, 1]."""
    return tuple(min(1.0, max(0.0, c * k)) for c in rgb)


def apply_shape_style(style=None):
    """Geometry only: rounding, padding, spacing. Shared by both variants."""
    style = style or imgui.get_style()

    style.window_rounding = 8.0
    style.child_rounding = 8.0
    style.frame_rounding = 5.0
    style.popup_rounding = 8.0
    style.scrollbar_rounding = 9.0
    style.grab_rounding = 5.0
    style.tab_rounding = 6.0

    style.window_border_size = 1.0
    style.child_border_size = 1.0
    style.popup_border_size = 1.0
    style.frame_border_size = 1.0
    style.tab_border_size = 0.0

    style.window_padding = imgui.ImVec2(14, 12)
    style.frame_padding = imgui.ImVec2(10, 6)
    style.cell_padding = imgui.ImVec2(8, 5)
    style.item_spacing = imgui.ImVec2(10, 8)
    style.item_inner_spacing = imgui.ImVec2(8, 6)
    style.indent_spacing = 20.0
    style.scrollbar_size = 12.0
    style.grab_min_size = 12.0

    style.window_title_align = imgui.ImVec2(0.5, 0.5)
    style.separator_text_border_size = 2.0
    style.separator_text_padding = imgui.ImVec2(18, 4)

    # Wide splitters between docked panels read as "gutters" between cards
    # instead of ImGui's 1px hairline splitters.
    style.docking_separator_size = 6.0

    style.anti_aliased_lines = True
    style.anti_aliased_fill = True
    style.circle_tessellation_max_error = 0.1


def _apply_dark_colors(style, accent):
    c = style.set_color_
    Col = imgui.Col_

    # Layered neutrals: app bg -> panel -> control. Slightly blue-shifted
    # so the grays feel "designed" instead of default.
    text = _v4(0.91, 0.91, 0.94)
    text_dim = _v4(0.54, 0.54, 0.60)
    bg0 = _v4(0.075, 0.075, 0.094)   # app / dock space
    bg1 = _v4(0.098, 0.098, 0.120)   # windows, menu bar
    bg2 = _v4(0.137, 0.137, 0.173)   # frames (inputs), tables header
    bg2_hover = _v4(0.168, 0.168, 0.220)
    bg2_active = _v4(0.196, 0.196, 0.259)
    hairline = _v4(1.0, 1.0, 1.0, 0.06)

    acc = _with_alpha(accent, 1.0)
    acc_hover = _with_alpha(_scale(accent, 1.12), 1.0)
    acc_down = _with_alpha(_scale(accent, 0.88), 1.0)

    c(Col.text, text)
    c(Col.text_disabled, text_dim)
    c(Col.text_link, acc)
    c(Col.text_selected_bg, _with_alpha(accent, 0.32))

    c(Col.window_bg, bg1)
    c(Col.child_bg, _v4(0, 0, 0, 0))
    c(Col.popup_bg, _v4(0.110, 0.110, 0.137, 0.98))
    c(Col.menu_bar_bg, bg1)
    c(Col.border, hairline)
    c(Col.border_shadow, _v4(0, 0, 0, 0))

    c(Col.frame_bg, bg2)
    c(Col.frame_bg_hovered, bg2_hover)
    c(Col.frame_bg_active, bg2_active)

    # Flat title bars: docked panels should read as one surface.
    c(Col.title_bg, bg1)
    c(Col.title_bg_active, bg1)
    c(Col.title_bg_collapsed, bg1)

    c(Col.scrollbar_bg, _v4(0, 0, 0, 0))
    c(Col.scrollbar_grab, _v4(1, 1, 1, 0.10))
    c(Col.scrollbar_grab_hovered, _v4(1, 1, 1, 0.18))
    c(Col.scrollbar_grab_active, _v4(1, 1, 1, 0.24))

    c(Col.check_mark, acc)
    c(Col.checkbox_selected_bg, _with_alpha(accent, 0.25))
    c(Col.slider_grab, acc)
    c(Col.slider_grab_active, acc_hover)
    c(Col.input_text_cursor, acc)

    c(Col.button, bg2)
    c(Col.button_hovered, bg2_hover)
    c(Col.button_active, _with_alpha(accent, 0.38))

    c(Col.header, _with_alpha(accent, 0.22))
    c(Col.header_hovered, _with_alpha(accent, 0.32))
    c(Col.header_active, _with_alpha(accent, 0.42))

    # Separators/gutters darker than the panels: dividers read as grooves,
    # and the wide docking separators look like gaps between surfaces.
    c(Col.separator, _v4(0.055, 0.055, 0.070, 1.0))
    c(Col.separator_hovered, _with_alpha(accent, 0.60))
    c(Col.separator_active, acc)

    c(Col.resize_grip, _v4(0, 0, 0, 0))
    c(Col.resize_grip_hovered, _with_alpha(accent, 0.50))
    c(Col.resize_grip_active, acc)

    # Tabs: quiet by default, selected tab lifts and gets an accent overline.
    c(Col.tab, bg1)
    c(Col.tab_hovered, bg2_hover)
    c(Col.tab_selected, bg2)
    c(Col.tab_selected_overline, acc)
    c(Col.tab_dimmed, bg1)
    c(Col.tab_dimmed_selected, bg2)
    c(Col.tab_dimmed_selected_overline, _with_alpha(accent, 0.35))

    c(Col.docking_preview, _with_alpha(accent, 0.35))
    c(Col.docking_empty_bg, bg0)

    c(Col.plot_lines, acc)
    c(Col.plot_lines_hovered, acc_hover)
    c(Col.plot_histogram, acc)
    c(Col.plot_histogram_hovered, acc_hover)

    c(Col.table_header_bg, bg2)
    c(Col.table_border_strong, _v4(1, 1, 1, 0.10))
    c(Col.table_border_light, hairline)
    c(Col.table_row_bg, _v4(0, 0, 0, 0))
    c(Col.table_row_bg_alt, _v4(1, 1, 1, 0.025))

    c(Col.drag_drop_target, acc)
    c(Col.drag_drop_target_bg, _with_alpha(accent, 0.20))
    c(Col.nav_cursor, acc)
    c(Col.nav_windowing_highlight, _with_alpha(accent, 0.70))
    c(Col.nav_windowing_dim_bg, _v4(0, 0, 0, 0.45))
    c(Col.modal_window_dim_bg, _v4(0, 0, 0, 0.55))

    return acc_down  # unused, kept for symmetry


def _apply_light_colors(style, accent):
    c = style.set_color_
    Col = imgui.Col_

    accent = _scale(accent, 0.82)  # darken accent for contrast on light bg
    text = _v4(0.13, 0.13, 0.17)
    text_dim = _v4(0.50, 0.50, 0.56)
    bg0 = _v4(0.918, 0.918, 0.937)
    bg1 = _v4(0.957, 0.957, 0.969)
    bg2 = _v4(1.0, 1.0, 1.0)
    bg2_hover = _v4(0.937, 0.937, 0.957)
    bg2_active = _v4(0.898, 0.898, 0.929)
    hairline = _v4(0.0, 0.0, 0.10, 0.10)

    acc = _with_alpha(accent, 1.0)
    acc_hover = _with_alpha(_scale(accent, 1.15), 1.0)

    c(Col.text, text)
    c(Col.text_disabled, text_dim)
    c(Col.text_link, acc)
    c(Col.text_selected_bg, _with_alpha(accent, 0.25))

    c(Col.window_bg, bg1)
    c(Col.child_bg, _v4(0, 0, 0, 0))
    c(Col.popup_bg, _v4(1.0, 1.0, 1.0, 0.99))
    c(Col.menu_bar_bg, bg1)
    c(Col.border, hairline)
    c(Col.border_shadow, _v4(0, 0, 0, 0))

    c(Col.frame_bg, bg2)
    c(Col.frame_bg_hovered, bg2_hover)
    c(Col.frame_bg_active, bg2_active)

    c(Col.title_bg, bg1)
    c(Col.title_bg_active, bg1)
    c(Col.title_bg_collapsed, bg1)

    c(Col.scrollbar_bg, _v4(0, 0, 0, 0))
    c(Col.scrollbar_grab, _v4(0, 0, 0, 0.16))
    c(Col.scrollbar_grab_hovered, _v4(0, 0, 0, 0.26))
    c(Col.scrollbar_grab_active, _v4(0, 0, 0, 0.34))

    c(Col.check_mark, acc)
    c(Col.checkbox_selected_bg, _with_alpha(accent, 0.18))
    c(Col.slider_grab, acc)
    c(Col.slider_grab_active, acc_hover)
    c(Col.input_text_cursor, acc)

    c(Col.button, bg2)
    c(Col.button_hovered, bg2_hover)
    c(Col.button_active, _with_alpha(accent, 0.30))

    c(Col.header, _with_alpha(accent, 0.16))
    c(Col.header_hovered, _with_alpha(accent, 0.26))
    c(Col.header_active, _with_alpha(accent, 0.36))

    c(Col.separator, _v4(0.878, 0.878, 0.902, 1.0))
    c(Col.separator_hovered, _with_alpha(accent, 0.60))
    c(Col.separator_active, acc)

    c(Col.resize_grip, _v4(0, 0, 0, 0))
    c(Col.resize_grip_hovered, _with_alpha(accent, 0.50))
    c(Col.resize_grip_active, acc)

    c(Col.tab, bg1)
    c(Col.tab_hovered, bg2_hover)
    c(Col.tab_selected, bg2)
    c(Col.tab_selected_overline, acc)
    c(Col.tab_dimmed, bg1)
    c(Col.tab_dimmed_selected, bg2)
    c(Col.tab_dimmed_selected_overline, _with_alpha(accent, 0.35))

    c(Col.docking_preview, _with_alpha(accent, 0.30))
    c(Col.docking_empty_bg, bg0)

    c(Col.plot_lines, acc)
    c(Col.plot_lines_hovered, acc_hover)
    c(Col.plot_histogram, acc)
    c(Col.plot_histogram_hovered, acc_hover)

    c(Col.table_header_bg, _v4(0.937, 0.937, 0.953))
    c(Col.table_border_strong, _v4(0, 0, 0, 0.14))
    c(Col.table_border_light, hairline)
    c(Col.table_row_bg, _v4(0, 0, 0, 0))
    c(Col.table_row_bg_alt, _v4(0, 0, 0, 0.02))

    c(Col.drag_drop_target, acc)
    c(Col.drag_drop_target_bg, _with_alpha(accent, 0.18))
    c(Col.nav_cursor, acc)
    c(Col.nav_windowing_highlight, _with_alpha(accent, 0.70))
    c(Col.nav_windowing_dim_bg, _v4(0.2, 0.2, 0.2, 0.35))
    c(Col.modal_window_dim_bg, _v4(0.2, 0.2, 0.2, 0.45))


def card_colors(variant="dark"):
    """(bg, border) for card-style child frames drawn on top of window_bg."""
    if variant == "light":
        return imgui.ImVec4(1.0, 1.0, 1.0, 1.0), imgui.ImVec4(0.0, 0.0, 0.1, 0.10)
    return imgui.ImVec4(0.122, 0.122, 0.153, 1.0), imgui.ImVec4(1.0, 1.0, 1.0, 0.05)


def apply_modern_style(variant="dark", accent=DEFAULT_ACCENT):
    """Apply the full theme (shapes + colors) to the current ImGui context."""
    style = imgui.get_style()
    apply_shape_style(style)
    if variant == "light":
        _apply_light_colors(style, accent)
    else:
        _apply_dark_colors(style, accent)
