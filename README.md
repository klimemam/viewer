# Viewer

Desktop image viewer with analytics, built with PySide6.

## Features

- **Folder browsing** — open a folder (or drag one onto the window), flip through images with ←/→, thumbnail gallery on the left.
- **Zoom & pan** — mouse-wheel zoom around the cursor, drag to pan, double-click or `F` to fit, `1` for 100%.
- **A/B compare** — press `A` to pin the current image as the reference, `C` to show it side-by-side with whatever you browse to next. Zoom and pan stay synced between the panes.
- **Diff** — press `D` in compare mode: MAE / RMSE / max delta / % differing pixels, plus an amplified difference heat image.
- **Stats panel** — per-channel histogram, mean/std/min/max, luminance stats, file info.
- **Pixel probe** — hover over the image to read exact pixel values (both A and current in compare mode).

## Setup

```
pip install -r requirements.txt
```

## Run

```
python main.py [folder-or-image]
```

## Shortcuts

| Key | Action |
| --- | --- |
| `Ctrl+O` | Open folder |
| `←` / `→` | Previous / next image |
| `F` / double-click | Fit to window |
| `1` | Zoom 100% |
| `A` | Pin current image as A |
| `C` | Toggle A/B compare |
| `D` | Diff A vs current (compare mode) |

## Notes

- Supported formats: PNG, JPEG, BMP, GIF, WebP, TIFF.
- Images in other modes (RGBA, palette, 16-bit) are converted to RGB for stats; alpha is not analyzed yet.
