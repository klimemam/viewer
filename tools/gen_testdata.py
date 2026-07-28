"""Generate test data for the native viewer (npy / raw)."""
import sys
from pathlib import Path

import numpy as np

out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent / "testdata"
out.mkdir(exist_ok=True)

H, W = 480, 640
yy, xx = np.mgrid[0:H, 0:W]

# float32 zone plate (H, W)
r2 = (xx - W / 2) ** 2 + (yy - H / 2) ** 2
zone = (0.5 + 0.5 * np.cos(0.7 * np.pi * r2 / W)).astype(np.float32)
np.save(out / "zone_f32.npy", zone)

# u16 gradient (H, W)
np.save(out / "grad_u16.npy", (xx / (W - 1) * 65535).astype(np.uint16))

# RGB u8 (H, W, 3)
rgb = np.stack([xx * 255 // (W - 1), yy * 255 // (H - 1), np.full_like(xx, 128)], -1).astype(np.uint8)
np.save(out / "rgb_u8.npy", rgb)

# CHW float32 (3, H, W) — tests CHW->HWC path
np.save(out / "chw_f32.npy", (rgb.transpose(2, 0, 1) / 255.0).astype(np.float32))

# big-endian f4 — tests byteswap
np.save(out / "zone_be_f32.npy", zone.astype(">f4"))

# raw gray16 with size in filename
(out / f"noise_{W}x{H}_gray16.raw").write_bytes(
    (np.random.default_rng(0).integers(0, 65536, (H, W)).astype("<u2")).tobytes())

# raw rgb8
(out / f"bars_{W}x{H}_rgb8.bin").write_bytes(rgb.tobytes())

# Bayer RGGB 16bit raw (pattern in filename for auto-detect)
bay = np.zeros((H, W), np.uint16)
bay[0::2, 0::2] = (xx[0::2, 0::2] * 65535 // (W - 1)).astype(np.uint16)   # R: ramp
bay[0::2, 1::2] = 30000                                                    # Gr
bay[1::2, 0::2] = 30000                                                    # Gb
bay[1::2, 1::2] = (yy[1::2, 1::2] * 65535 // (H - 1)).astype(np.uint16)   # B: ramp
(out / f"cfa_rggb_{W}x{H}_bayer16.raw").write_bytes(bay.astype("<u2").tobytes())

# Quad Bayer RGGB 16bit raw (2x2 blocks per color)
qb = np.zeros((H, W), np.uint16)
qx, qy = (xx // 2) % 2, (yy // 2) % 2
qb[(qy == 0) & (qx == 0)] = 52000   # R blocks bright
qb[(qy == 0) & (qx == 1)] = 30000   # Gr
qb[(qy == 1) & (qx == 0)] = 30000   # Gb
qb[(qy == 1) & (qx == 1)] = 12000   # B blocks dark
(out / f"cfa_quad_rggb_{W}x{H}_bayer16.raw").write_bytes(qb.astype("<u2").tobytes())

# slanted edge (~7 deg, ~1.2px gaussian-ish transition) for iso12233/e-sfr
ang = np.deg2rad(7.0)
dist = (xx - W / 2) - np.tan(ang) * (yy - H / 2)
edge = (0.1 + 0.8 / (1.0 + np.exp(-dist / 1.2))).astype(np.float32)
np.save(out / "slant_edge_f32.npy", edge)

# numbered sequence: flat field + temporal noise + a fixed pattern, for the
# sequence loader and the built-in temporal analysis
seq = out / "seq"
seq.mkdir(exist_ok=True)
rng2 = np.random.default_rng(7)
fixed = rng2.normal(0, 40, (H, W))                      # fixed pattern (same every frame)
for k in range(24):
    level = 8000 + 300 * np.sin(k / 24 * 2 * np.pi)     # slow drift over time
    frame = level + fixed + rng2.normal(0, 120, (H, W))  # temporal noise
    frame = np.clip(frame, 0, 65535).astype("<u2")
    (seq / f"flat_{k:04d}_{W}x{H}_gray16.raw").write_bytes(frame.tobytes())

# remote-browser fixtures (--remote-selftest): one numbered .npy stack plus
# loose files in the same folder (LIST grouping: 1 group + 3 singles), and a
# root of per-illumination folders (SCAN / remote open-folder: 3 groups)
rb = out / "rb"
rb.mkdir(exist_ok=True)
rng3 = np.random.default_rng(11)
for k in range(24):
    np.save(rb / f"frame_{k:03d}.npy", (rng3.random((64, 64)) * 4095).astype(np.float32))
np.save(rb / "dark.npy", np.zeros((64, 64), np.float32))
np.save(rb / "flat.npy", np.full((64, 64), 1000.0, np.float32))
(rb / "notes.txt").write_text("not an image\n")
for lv in ("10lx", "20lx", "40lx"):
    d = rb / "scanroot" / lv
    d.mkdir(parents=True, exist_ok=True)
    for k in range(8):
        np.save(d / f"frame_{k:03d}.npy", (rng3.random((32, 32)) * 4095).astype(np.float32))

# picker batch-mode fixture (--picker-selftest UC5): three top-level condition
# folders, each a numbered stack plus a single representative average.npy -
# "one batch per top folder" must make three batches, stacks intact
bs = out / "batchset"
rng4 = np.random.default_rng(21)
for sub in ("00", "01", "02"):
    d = bs / sub
    d.mkdir(parents=True, exist_ok=True)
    for k in range(3):
        np.save(d / f"frame_{k:03d}.npy", (rng4.random((16, 16)) * 100).astype(np.float32))
    np.save(d / "average.npy", (rng4.random((16, 16)) * 100).astype(np.float32))

print("wrote test data to", out)
for p in sorted(out.iterdir()):
    print(" ", p.name, p.stat().st_size, "bytes")
