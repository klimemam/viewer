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

print("wrote test data to", out)
for p in sorted(out.iterdir()):
    print(" ", p.name, p.stat().st_size, "bytes")
