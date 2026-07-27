"""Dear ImGui modern-design demo — a mock of the Viewer UI wearing the
"Aurora" theme from imgui_theme.py.

This is a *design study*, not a port: images, thumbnails and stats are
procedurally generated so the demo runs anywhere with zero assets, and the
layout mirrors main.py (gallery | viewer | stats, toolbar, status bar).
A "Design Lab" panel lets you switch dark/light and the accent color live.

On top of the theme, this file demonstrates the "de-ImGui" techniques:
a real font (Roboto) + icon toolbar, hidden dock tab bars with custom
panel headers, card-based stats, and a chrome-free plot.

Run:
    pip install imgui-bundle
    python imgui_demo.py

Headless screenshot (used for the docs):
    xvfb-run python imgui_demo.py --screenshot docs/img/imgui_dark.png
"""
import sys

import numpy as np
from imgui_bundle import (hello_imgui, imgui, immapp, immvision, implot,
                          icons_fontawesome_4 as fa)

from imgui_theme import ACCENTS, apply_modern_style, card_colors

THUMB_W, THUMB_H = 132, 88
NO_TAB_BAR = int(imgui.internal.DockNodeFlagsPrivate_.no_tab_bar)


# --- procedural sample "photos" -------------------------------------------
def _make_image(seed, w=960, h=640):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    u, v = xx / w, yy / h
    base = rng.uniform(0.30, 0.62, size=3)
    tilt = rng.uniform(-0.30, 0.30, size=3)
    img = np.stack([base[c] + tilt[c] * (u * rng.uniform(0.4, 1) + v * rng.uniform(0.4, 1))
                    for c in range(3)], axis=-1)
    for _ in range(rng.integers(2, 5)):  # soft "bokeh" circles
        cx, cy, r = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(60, 240)
        glow = np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * r * r)))
        img += glow[..., None] * rng.uniform(-0.16, 0.22, size=3)
    img += rng.normal(0, 0.012, size=img.shape)  # film grain
    return (np.clip(img, 0, 1) * 255).astype(np.uint8)


class State:
    def __init__(self):
        names = ["aurora", "harbor", "dune", "citrus", "nebula", "moss",
                 "ember", "glacier", "lagoon", "prism", "velvet", "zenith"]
        self.files = [f"{n}_{i + 1:02d}.png" for i, n in enumerate(names)]
        self.images = [_make_image(seed=101 + i) for i in range(len(names))]
        self.thumbs = [img[::6, ::6].copy() for img in self.images]
        self.index = 0
        self.pinned = None            # index pinned as "A"
        self.compare = False
        self.zoom = 1.0
        self.fit = True
        # design lab
        self.variant = "dark"
        self.accent_name = "Aurora Blue"
        self.accent = list(ACCENTS[self.accent_name])
        self.rounding = None   # None = theme default
        self.hist_cache = {}

    def current(self):
        return self.images[self.index]

    def histogram(self, index):
        if index not in self.hist_cache:
            img = self.images[index]
            self.hist_cache[index] = [
                np.histogram(img[..., c], bins=64, range=(0, 255))[0].astype(np.float32)
                for c in range(3)]
        return self.hist_cache[index]


st = State()


# hello_imgui re-applies its own tweaked theme after setup_imgui_style,
# so our theme is re-applied every frame from pre_new_frame (cheap: it only
# sets plain style values).
def _apply_theme():
    apply_modern_style(variant=st.variant, accent=tuple(st.accent))
    if st.rounding is not None:
        style = imgui.get_style()
        style.frame_rounding = st.rounding
        style.grab_rounding = st.rounding
        style.tab_rounding = min(st.rounding + 1, 9.0)


def _load_fonts():
    # A real UI font instead of ImGui's pixel-y default, FontAwesome merged
    # in for the icon toolbar. ImGui >= 1.92 loads glyphs dynamically, so no
    # glyph ranges are needed.
    hello_imgui.load_font("fonts/Roboto/Roboto-Regular.ttf", 16.0)
    merge = hello_imgui.FontLoadingParams()
    merge.merge_to_last_font = True
    hello_imgui.load_font("fonts/fontawesome-webfont.ttf", 14.0, merge)


# --- small UI helpers ------------------------------------------------------
def panel_header(title, right_text=""):
    """Small-caps dimmed header replacing the docked-window tab bar."""
    imgui.push_font(None, 12.0)
    imgui.text_disabled(title.upper())
    if right_text:
        w = imgui.calc_text_size(right_text).x
        imgui.same_line(imgui.get_content_region_avail().x - w)
        imgui.text_disabled(right_text)
    imgui.pop_font()
    imgui.spacing()


def icon_button(icon, tooltip, active=False, disabled=False):
    """Frameless icon button: transparent at rest, pill on hover,
    accent-tinted when the mode it toggles is active."""
    acc = imgui.get_style_color_vec4(imgui.Col_.check_mark)
    if active:
        bg = imgui.ImVec4(acc.x, acc.y, acc.z, 0.25)
        fg = imgui.ImVec4(acc.x, acc.y, acc.z, 1.0)
    else:
        bg = imgui.ImVec4(0, 0, 0, 0)
        fg = imgui.get_style_color_vec4(imgui.Col_.text)
    imgui.push_style_color(imgui.Col_.button, bg)
    imgui.push_style_color(imgui.Col_.text, fg)
    imgui.begin_disabled(disabled)
    clicked = imgui.button(f"{icon}##{tooltip}")
    imgui.end_disabled()
    imgui.pop_style_color(2)
    if not disabled:
        imgui.set_item_tooltip(tooltip)
    return clicked


def begin_card(cid):
    bg, border = card_colors(st.variant)
    imgui.push_style_color(imgui.Col_.child_bg, bg)
    imgui.push_style_color(imgui.Col_.border, border)
    imgui.push_style_var(imgui.StyleVar_.child_rounding, 10.0)
    imgui.push_style_var(imgui.StyleVar_.window_padding, imgui.ImVec2(14, 12))
    flags = (imgui.ChildFlags_.auto_resize_y | imgui.ChildFlags_.borders
             | imgui.ChildFlags_.always_use_window_padding)
    return imgui.begin_child(cid, imgui.ImVec2(0, 0), flags)


def end_card():
    imgui.end_child()
    imgui.pop_style_var(2)
    imgui.pop_style_color(2)
    imgui.spacing()


# --- panels ----------------------------------------------------------------
def gui_gallery():
    panel_header("Gallery", f"{len(st.files)}")
    avail = imgui.get_content_region_avail().x
    cols = max(1, int(avail // (THUMB_W + 16)))
    for i, name in enumerate(st.files):
        if i % cols != 0:
            imgui.same_line()
        imgui.begin_group()
        immvision.image_display(f"##thumb{i}", st.thumbs[i],
                                image_display_size=(THUMB_W, THUMB_H))
        clicked = imgui.is_item_clicked(0)
        hovered = imgui.is_item_hovered()
        rmin, rmax = imgui.get_item_rect_min(), imgui.get_item_rect_max()
        dl = imgui.get_window_draw_list()
        if i == st.index:  # accent ring around the selected thumbnail
            col = imgui.get_color_u32(imgui.Col_.check_mark)
            dl.add_rect(rmin, rmax, col, rounding=5.0, thickness=2.5)
        elif hovered:
            col = imgui.get_color_u32(imgui.Col_.check_mark, 0.45)
            dl.add_rect(rmin, rmax, col, rounding=5.0, thickness=1.5)
        label = name if i != st.pinned else f"{fa.ICON_FA_THUMBTACK} {name}"
        if len(label) > 19:
            label = label[:18] + "…"
        (imgui.text if i == st.index else imgui.text_disabled)(label)
        imgui.end_group()
        if clicked:
            st.index = i


def _toolbar():
    if icon_button(fa.ICON_FA_FOLDER_OPEN, "Open folder (Ctrl+O)"):
        pass
    imgui.same_line()
    if icon_button(fa.ICON_FA_CHEVRON_LEFT, "Previous (←)", disabled=st.index == 0):
        st.index -= 1
    imgui.same_line()
    if icon_button(fa.ICON_FA_CHEVRON_RIGHT, "Next (→)",
                   disabled=st.index >= len(st.files) - 1):
        st.index += 1
    imgui.same_line(0, 18)

    if icon_button(fa.ICON_FA_EXPAND, "Fit to window (F)", active=st.fit):
        st.fit = True
    imgui.same_line()
    if icon_button("1:1", "Zoom 100% (1)", active=(not st.fit and st.zoom == 1.0)):
        st.fit, st.zoom = False, 1.0
    imgui.same_line()
    imgui.set_next_item_width(150)
    changed, z = imgui.slider_float("##zoom", st.zoom, 0.1, 4.0, "%.2fx")
    if changed:
        st.fit, st.zoom = False, z
    imgui.same_line(0, 18)

    if icon_button(fa.ICON_FA_THUMBTACK, "Pin current image as A (A)",
                   active=st.pinned == st.index):
        st.pinned = st.index
    imgui.same_line()
    no_pin = st.pinned is None
    if icon_button(fa.ICON_FA_COLUMNS, "A/B compare (C)",
                   active=st.compare, disabled=no_pin):
        st.compare = not st.compare
    imgui.same_line()
    icon_button(fa.ICON_FA_ADJUST, "Diff A vs current (D)",
                disabled=no_pin or not st.compare)


def _fit_size(img, avail, zoom_to=None):
    h, w = img.shape[:2]
    k = min(avail.x / w, avail.y / h) if zoom_to is None else zoom_to
    return int(max(1, w * k)), int(max(1, h * k))


def _zoom_pill(dl, rmax):
    label = "Fit" if st.fit else f"{st.zoom * 100:.0f}%"
    imgui.push_font(None, 12.0)
    ts = imgui.calc_text_size(label)
    pad, margin = 7, 10
    p1 = imgui.ImVec2(rmax.x - ts.x - 2 * pad - margin, rmax.y - ts.y - 2 * pad - margin)
    p2 = imgui.ImVec2(rmax.x - margin, rmax.y - margin)
    dl.add_rect_filled(p1, p2,
                       imgui.color_convert_float4_to_u32(imgui.ImVec4(0, 0, 0, 0.55)),
                       rounding=(p2.y - p1.y) / 2)
    dl.add_text(imgui.ImVec2(p1.x + pad, p1.y + pad),
                imgui.color_convert_float4_to_u32(imgui.ImVec4(1, 1, 1, 0.9)), label)
    imgui.pop_font()


def gui_viewer():
    panel_header("Viewer", st.files[st.index])
    _toolbar()
    imgui.spacing()
    avail = imgui.get_content_region_avail()
    panes = ([("A", st.images[st.pinned]), ("B", st.current())]
             if (st.compare and st.pinned is not None) else
             [(None, st.current())])
    n = len(panes)
    header_h = 24 if panes[0][0] else 0
    pane_avail = imgui.ImVec2(avail.x / n - 8 * (n - 1), avail.y - header_h - 8)
    hairline = imgui.get_color_u32(imgui.Col_.border)
    for j, (tag, img) in enumerate(panes):
        if j:
            imgui.same_line()
        imgui.begin_group()
        if tag:
            name = st.files[st.pinned if tag == "A" else st.index]
            color = (imgui.get_style_color_vec4(imgui.Col_.check_mark) if tag == "A"
                     else imgui.get_style_color_vec4(imgui.Col_.text))
            imgui.text_colored(color, f"{tag}: {name}")
        size = _fit_size(img, pane_avail, None if st.fit else st.zoom)
        # center the image horizontally inside its pane
        pad_x = max(0, (pane_avail.x - size[0]) / 2)
        imgui.set_cursor_pos_x(imgui.get_cursor_pos_x() + pad_x)
        immvision.image_display(f"##view{j}", img, image_display_size=size)
        rmin, rmax = imgui.get_item_rect_min(), imgui.get_item_rect_max()
        dl = imgui.get_window_draw_list()
        dl.add_rect(rmin, rmax, hairline)
        if j == n - 1:
            _zoom_pill(dl, rmax)
        imgui.end_group()


CH_COLORS = [imgui.ImVec4(0.94, 0.42, 0.44, 1.0),
             imgui.ImVec4(0.38, 0.83, 0.55, 1.0),
             imgui.ImVec4(0.44, 0.62, 1.00, 1.0)]


def gui_stats():
    img = st.current()
    h, w = img.shape[:2]
    panel_header("Stats")

    if begin_card("card_hist"):
        imgui.push_font(None, 12.0)
        imgui.text_disabled("HISTOGRAM")
        imgui.pop_font()
        # chrome-free plot: no frame, no border, quiet legend
        implot.push_style_color(implot.Col_.frame_bg, imgui.ImVec4(0, 0, 0, 0))
        implot.push_style_color(implot.Col_.plot_bg, imgui.ImVec4(0, 0, 0, 0))
        implot.push_style_color(implot.Col_.plot_border, imgui.ImVec4(0, 0, 0, 0))
        implot.push_style_color(implot.Col_.legend_bg, imgui.ImVec4(0, 0, 0, 0))
        implot.push_style_color(implot.Col_.legend_border, imgui.ImVec4(0, 0, 0, 0))
        if implot.begin_plot("##hist", imgui.ImVec2(-1, 150),
                             implot.Flags_.no_menus):
            implot.setup_axes("", "",
                              implot.AxisFlags_.auto_fit,
                              implot.AxisFlags_.auto_fit | implot.AxisFlags_.no_tick_labels)
            implot.setup_legend(implot.Location_.north_east,
                                implot.LegendFlags_.horizontal)
            hist = st.histogram(st.index)
            xs = np.linspace(0, 255, 64, dtype=np.float32)
            for c, label in enumerate("RGB"):
                spec = implot.Spec(line_color=CH_COLORS[c], line_weight=2.0)
                implot.plot_line(label, xs, hist[c], spec)
            implot.end_plot()
        implot.pop_style_color(5)
        end_card()

    if begin_card("card_chan"):
        imgui.push_font(None, 12.0)
        imgui.text_disabled("CHANNELS")
        imgui.pop_font()
        imgui.push_style_var(imgui.StyleVar_.cell_padding, imgui.ImVec2(10, 6))
        flags = imgui.TableFlags_.row_bg | imgui.TableFlags_.borders_inner_h
        if imgui.begin_table("chan", 5, flags):
            for head in ("", "mean", "std", "min", "max"):
                imgui.table_setup_column(head)
            imgui.table_headers_row()
            for c, label in enumerate("RGB"):
                ch = img[..., c]
                imgui.table_next_row()
                imgui.table_next_column()
                imgui.text_colored(CH_COLORS[c], label)
                for val in (ch.mean(), ch.std(), ch.min(), ch.max()):
                    imgui.table_next_column()
                    imgui.text(f"{val:.1f}")
            imgui.end_table()
        imgui.pop_style_var()
        end_card()

    if begin_card("card_file"):
        imgui.push_font(None, 12.0)
        imgui.text_disabled("FILE")
        imgui.pop_font()
        for k, v in (("name", st.files[st.index]),
                     ("size", f"{w} x {h}  RGB"),
                     ("raw", f"{w * h * 3 / 1e6:.1f} MB")):
            imgui.text_disabled(k)
            imgui.same_line(80)
            imgui.text(v)
        end_card()


def gui_design_lab():
    panel_header("Design Lab")
    imgui.text_wrapped("Live theme playground - every control below restyles "
                       "the whole app instantly.")
    imgui.separator_text("Variant")
    for i, v in enumerate(("dark", "light")):
        if i:
            imgui.same_line()
        if imgui.radio_button(v, st.variant == v):
            st.variant = v
    imgui.separator_text("Accent")
    for name, rgb in ACCENTS.items():
        imgui.push_style_color(imgui.Col_.button, imgui.ImVec4(*rgb, 0.85))
        imgui.push_style_color(imgui.Col_.button_hovered, imgui.ImVec4(*rgb, 1.0))
        imgui.push_style_color(imgui.Col_.button_active, imgui.ImVec4(*rgb, 1.0))
        if imgui.button(f"  {name}  "):
            st.accent_name, st.accent = name, list(rgb)
        imgui.pop_style_color(3)
    changed, col = imgui.color_edit3("custom", st.accent)
    if changed:
        st.accent_name, st.accent = "custom", list(col)
    imgui.separator_text("Shape")
    cur = st.rounding if st.rounding is not None else imgui.get_style().frame_rounding
    changed, r = imgui.slider_float("rounding", cur, 0.0, 12.0, "%.0f px")
    if changed:
        st.rounding = r


def gui_status():
    imgui.text(f"{st.index + 1} / {len(st.files)}   {st.files[st.index]}")
    imgui.same_line(imgui.get_window_width() - 330)
    img = st.current()
    zoom = "fit" if st.fit else f"{st.zoom * 100:.0f}%"
    imgui.text_disabled(f"{img.shape[1]} x {img.shape[0]}   zoom {zoom}   "
                        f"{st.variant} / {st.accent_name}")


# --- app wiring ------------------------------------------------------------
def make_runner_params(screenshot_after=None):
    immvision.use_rgb_color_order()
    params = hello_imgui.RunnerParams()
    params.app_window_params.window_title = "Viewer - Dear ImGui design study"
    params.app_window_params.window_geometry.size = (1520, 940)
    # No menu bar: everything lives in the icon toolbar, like a real app.
    params.imgui_window_params.show_menu_bar = False
    params.imgui_window_params.show_status_bar = True
    params.imgui_window_params.show_status_fps = False
    # The theme is owned by imgui_theme.py / the Design Lab panel — do not
    # let hello_imgui re-apply a remembered theme from the ini file.
    params.imgui_window_params.remember_theme = False
    params.imgui_window_params.remember_status_bar_settings = False
    params.imgui_window_params.default_imgui_window_type = (
        hello_imgui.DefaultImGuiWindowType.provide_full_screen_dock_space)
    params.callbacks.setup_imgui_style = _apply_theme
    params.callbacks.load_additional_fonts = _load_fonts
    params.callbacks.show_status = gui_status

    # Panels are fixed surfaces with custom headers, not tabbed ImGui
    # windows: hide every dock tab bar.
    splits = []
    left = hello_imgui.DockingSplit()
    left.initial_dock, left.new_dock = "MainDockSpace", "LeftSpace"
    left.direction, left.ratio = imgui.Dir.left, 0.20
    left.node_flags = NO_TAB_BAR
    splits.append(left)
    right = hello_imgui.DockingSplit()
    right.initial_dock, right.new_dock = "MainDockSpace", "RightSpace"
    right.direction, right.ratio = imgui.Dir.right, 0.30
    right.node_flags = NO_TAB_BAR
    splits.append(right)
    lab = hello_imgui.DockingSplit()
    lab.initial_dock, lab.new_dock = "RightSpace", "LabSpace"
    lab.direction, lab.ratio = imgui.Dir.down, 0.36
    lab.node_flags = NO_TAB_BAR
    splits.append(lab)

    def window(label, space, fn):
        w = hello_imgui.DockableWindow()
        w.label, w.dock_space_name, w.gui_function = label, space, fn
        w.can_be_closed = False
        return w

    params.docking_params.docking_splits = splits
    params.docking_params.dockable_windows = [
        window("Gallery", "LeftSpace", gui_gallery),
        window("Viewer", "MainDockSpace", gui_viewer),
        window("Stats", "RightSpace", gui_stats),
        window("Design Lab", "LabSpace", gui_design_lab),
    ]
    params.docking_params.main_dock_space_node_flags = NO_TAB_BAR

    frame = {"n": 0}

    def pre_frame():
        _apply_theme()
        if screenshot_after is not None:
            frame["n"] += 1
            if frame["n"] >= screenshot_after:
                hello_imgui.get_runner_params().app_shall_exit = True

    params.callbacks.pre_new_frame = pre_frame
    if screenshot_after is not None:
        params.fps_idling.enable_idling = False
        params.ini_disable = True
    return params


def main():
    shot_path = None
    if "--screenshot" in sys.argv:
        shot_path = sys.argv[sys.argv.index("--screenshot") + 1]
        if "--light" in sys.argv:
            st.variant = "light"
        if "--compare" in sys.argv:
            st.pinned, st.compare = 4, True
    params = make_runner_params(screenshot_after=40 if shot_path else None)
    immapp.run(params, immapp.AddOnsParams(with_implot=True))
    if shot_path:
        from PIL import Image
        img = np.asarray(hello_imgui.final_app_window_screenshot())
        Image.fromarray(img).save(shot_path)
        print(f"saved {shot_path} {img.shape}")


if __name__ == "__main__":
    main()
