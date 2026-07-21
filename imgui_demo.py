"""Dear ImGui modern-design demo — a mock of the Viewer UI wearing the
"Aurora" theme from imgui_theme.py.

This is a *design study*, not a port: images, thumbnails and stats are
procedurally generated so the demo runs anywhere with zero assets, and the
layout mirrors main.py (gallery | viewer | stats, toolbar, status bar).
A "Design Lab" panel lets you switch dark/light and the accent color live.

Run:
    pip install imgui-bundle
    python imgui_demo.py

Headless screenshot (used for the docs):
    xvfb-run python imgui_demo.py --screenshot docs/img/imgui_dark.png
"""
import sys

import numpy as np
from imgui_bundle import hello_imgui, imgui, immapp, immvision, implot

from imgui_theme import ACCENTS, apply_modern_style

THUMB_W, THUMB_H = 132, 88


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


# --- panels ----------------------------------------------------------------
def gui_gallery():
    imgui.text_disabled(f"{len(st.files)} images")
    imgui.separator()
    avail = imgui.get_content_region_avail().x
    cols = max(1, int(avail // (THUMB_W + 16)))
    for i, name in enumerate(st.files):
        if i % cols != 0:
            imgui.same_line()
        imgui.begin_group()
        immvision.image_display(f"##thumb{i}", st.thumbs[i],
                                image_display_size=(THUMB_W, THUMB_H))
        clicked = imgui.is_item_clicked(0)
        rmin, rmax = imgui.get_item_rect_min(), imgui.get_item_rect_max()
        dl = imgui.get_window_draw_list()
        if i == st.index:  # accent frame around the selected thumbnail
            col = imgui.get_color_u32(imgui.Col_.check_mark)
            dl.add_rect(rmin, rmax, col, rounding=5.0, thickness=2.5)
        label = name if i != st.pinned else f"[A] {name}"
        imgui.push_text_wrap_pos(imgui.get_cursor_pos_x() + THUMB_W)
        (imgui.text if i == st.index else imgui.text_disabled)(label)
        imgui.pop_text_wrap_pos()
        imgui.end_group()
        if clicked:
            st.index = i
    return


def _toolbar():
    if imgui.button("  Fit  "):
        st.fit = True
    imgui.same_line()
    if imgui.button(" 100% "):
        st.fit, st.zoom = False, 1.0
    imgui.same_line()
    if imgui.button(" Pin as A "):
        st.pinned = st.index
    imgui.same_line()
    disabled = st.pinned is None
    imgui.begin_disabled(disabled)
    changed, st.compare = imgui.checkbox("Compare", st.compare)
    imgui.same_line()
    imgui.button(" Diff ")
    imgui.end_disabled()
    imgui.same_line()
    imgui.set_next_item_width(160)
    changed, z = imgui.slider_float("zoom", st.zoom, 0.1, 4.0, "%.2fx")
    if changed:
        st.fit, st.zoom = False, z


def _fit_size(img, avail, zoom_to=None):
    h, w = img.shape[:2]
    if zoom_to is None:  # fit
        k = min(avail.x / w, avail.y / h)
    else:
        k = zoom_to
    return int(max(1, w * k)), int(max(1, h * k))


def gui_viewer():
    _toolbar()
    imgui.separator()
    avail = imgui.get_content_region_avail()
    panes = ([("A", st.images[st.pinned]), ("B", st.current())]
             if (st.compare and st.pinned is not None) else
             [(None, st.current())])
    pane_avail = imgui.ImVec2(avail.x / len(panes) - 8 * (len(panes) - 1), avail.y - 24)
    for j, (tag, img) in enumerate(panes):
        if j:
            imgui.same_line()
        imgui.begin_group()
        if tag:
            name = st.files[st.pinned if tag == "A" else st.index]
            imgui.text_colored(imgui.get_style().color_(imgui.Col_.check_mark)
                               if tag == "A" else imgui.get_style().color_(imgui.Col_.text),
                               f"{tag}: {name}")
        size = _fit_size(img, pane_avail, None if st.fit else st.zoom)
        immvision.image_display(f"##view{j}", img, image_display_size=size)
        imgui.end_group()


CH_COLORS = [imgui.ImVec4(0.94, 0.42, 0.44, 1.0),
             imgui.ImVec4(0.38, 0.83, 0.55, 1.0),
             imgui.ImVec4(0.44, 0.62, 1.00, 1.0)]


def gui_stats():
    img = st.current()
    h, w = img.shape[:2]
    imgui.separator_text("Histogram")
    if implot.begin_plot("##hist", imgui.ImVec2(-1, 200)):
        implot.setup_axes("value", "count",
                          implot.AxisFlags_.auto_fit, implot.AxisFlags_.auto_fit)
        hist = st.histogram(st.index)
        xs = np.linspace(0, 255, 64, dtype=np.float32)
        for c, label in enumerate("RGB"):
            spec = implot.Spec(line_color=CH_COLORS[c], line_weight=2.0)
            implot.plot_line(label, xs, hist[c], spec)
        implot.end_plot()
    imgui.separator_text("Channels")
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
    imgui.separator_text("File")
    imgui.text_disabled("name")
    imgui.same_line(90)
    imgui.text(st.files[st.index])
    imgui.text_disabled("size")
    imgui.same_line(90)
    imgui.text(f"{w} x {h}  RGB  {w * h * 3 / 1e6:.1f} MB raw")


def gui_design_lab():
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
    imgui.spacing()
    imgui.text_disabled("Widget samples")
    imgui.button("Button")
    imgui.same_line()
    imgui.small_button("small")
    _, _ = imgui.checkbox("checkbox", True)
    imgui.set_next_item_width(-1)
    _, _ = imgui.slider_int("##s", 42, 0, 100, "slider %d")
    imgui.set_next_item_width(-1)
    _, _ = imgui.input_text_with_hint("##t", "input text...", "")


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
    params.imgui_window_params.show_menu_bar = True
    params.imgui_window_params.show_status_bar = True
    params.imgui_window_params.show_status_fps = False
    # The theme is owned by imgui_theme.py / the Design Lab panel — do not
    # let hello_imgui re-apply a remembered theme from the ini file.
    params.imgui_window_params.remember_theme = False
    params.imgui_window_params.remember_status_bar_settings = False
    params.imgui_window_params.default_imgui_window_type = (
        hello_imgui.DefaultImGuiWindowType.provide_full_screen_dock_space)
    params.callbacks.setup_imgui_style = _apply_theme
    params.callbacks.show_status = gui_status

    splits = []
    left = hello_imgui.DockingSplit()
    left.initial_dock, left.new_dock = "MainDockSpace", "LeftSpace"
    left.direction, left.ratio = imgui.Dir.left, 0.20
    splits.append(left)
    right = hello_imgui.DockingSplit()
    right.initial_dock, right.new_dock = "MainDockSpace", "RightSpace"
    right.direction, right.ratio = imgui.Dir.right, 0.30
    splits.append(right)

    def window(label, space, fn):
        w = hello_imgui.DockableWindow()
        w.label, w.dock_space_name, w.gui_function = label, space, fn
        return w

    params.docking_params.docking_splits = splits
    params.docking_params.dockable_windows = [
        window("Gallery", "LeftSpace", gui_gallery),
        window("Viewer", "MainDockSpace", gui_viewer),
        window("Stats", "RightSpace", gui_stats),
        window("Design Lab", "RightSpace", gui_design_lab),
    ]
    params.docking_params.main_dock_space_node_flags = (
        imgui.DockNodeFlags_.none)

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
