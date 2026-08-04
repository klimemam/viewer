"""Generate test data for the native viewer (npy / raw).

    python tools/gen_testdata.py [outdir] [--bench]

Every fixture the selftests need is written here, deterministically (fixed
rng seeds), so CI can regenerate the lot from a clean checkout - nothing under
tools/testdata is in git. `--bench` adds the large arrays the frame-time gate
and the A/B step-throttle measurement use; they are ~330 MB and no selftest
needs them, so they are opt-in.
"""
import shutil
import sys
from pathlib import Path

import numpy as np

_args = [a for a in sys.argv[1:] if not a.startswith("-")]
WANT_BENCH = "--bench" in sys.argv[1:]

out = Path(_args[0]) if _args else Path(__file__).parent / "testdata"
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

# BIG Bayer RGGB u16 npy (2064x1032 = 2.13 Mpx): large enough that the
# histogram's sample stride exceeds 1 (total/1M = 2) and the ROI-stats stride
# is 10 - exactly the sizes where a strided LINEAR index on an even width
# never lands on an odd column and silently empties the Gr and B planes
# (--verify-selftest V18). Planes are flat at distinct levels so a sampler
# that reads the wrong pixels cannot return the right per-plane means.
big = np.empty((1032, 2064), np.uint16)
big[0::2, 0::2] = 100   # R
big[0::2, 1::2] = 300   # Gr
big[1::2, 0::2] = 500   # Gb
big[1::2, 1::2] = 900   # B
np.save(out / "cfa_big_rggb_2064x1032_u16.npy", big)

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
# peer grouping stage-2 fixtures (--remote-selftest): gainset has TWO digit
# runs but only the trailing one is the frame axis (pattern must keep gain10
# literal: gain10_???.npy, never gain??_???.npy); expset's two varying runs
# must SPLIT into per-condition stacks instead of growing a second '?' run
gset = rb / "gainset"
gset.mkdir(exist_ok=True)
for gain in (10, 20):
    for k in range(8):
        np.save(gset / f"gain{gain}_{k:03d}.npy",
                (rng3.random((16, 16)) * 4095).astype(np.float32))
eset = rb / "expset"
eset.mkdir(exist_ok=True)
for g in range(2):
    for e in range(4):
        np.save(eset / f"g{g:02d}_e{e:02d}.npy",
                (rng3.random((16, 16)) * 4095).astype(np.float32))
# pattern-extent fixtures (--remote-selftest / --browse-selftest): an ALL-DIGIT
# folder groups as "????.npy", which is correct by the rule and says nothing -
# the displayed pattern must show the extent instead ("0000‥0003.npy"). padset
# has uneven zero padding (f_9 / f_10 / f_11), where the extent must be read by
# VALUE and not lexicographically ("f_9‥11.npy").
dset = rb / "digitset"
dset.mkdir(exist_ok=True)
for k in range(4):
    np.save(dset / f"{k:04d}.npy", (rng3.random((16, 16)) * 4095).astype(np.float32))
pset = rb / "padset"
pset.mkdir(exist_ok=True)
for k in (9, 10, 11):
    np.save(pset / f"f_{k}.npy", (rng3.random((16, 16)) * 4095).astype(np.float32))

for lv in ("10lx", "20lx", "40lx"):
    d = rb / "scanroot" / lv
    d.mkdir(parents=True, exist_ok=True)
    for k in range(8):
        np.save(d / f"frame_{k:03d}.npy", (rng3.random((32, 32)) * 4095).astype(np.float32))

# picker batch-mode fixture (--picker-selftest UC5): three top-level condition
# folders, each a numbered stack plus a single representative average.npy -
# "one batch per top folder" must make three batches, stacks intact
#
# 00/ is TWO levels deep. "one batch per top folder" reads the top folder off
# the group name by splitting on '/', so a group two levels down is the only
# input that can tell whether that name was built from '/' - and on Windows the
# relative path scanFolderGroups derives comes out of std::filesystem with '\'
# separators. A one-level tree cannot distinguish the two: it has no separator
# in it at all. Get it wrong and "00\rep" is a fourth top folder, so the batch
# count below is the assert that catches it.
bs = out / "batchset"
rng4 = np.random.default_rng(21)
for sub in ("00", "01", "02"):
    d = bs / sub
    d.mkdir(parents=True, exist_ok=True)
    for k in range(3):
        np.save(d / f"frame_{k:03d}.npy", (rng4.random((16, 16)) * 100).astype(np.float32))
    np.save(d / "average.npy", (rng4.random((16, 16)) * 100).astype(np.float32))
rep = bs / "00" / "rep"
rep.mkdir(parents=True, exist_ok=True)
for k in range(2):
    np.save(rep / f"shot_{k:03d}.npy", (rng4.random((16, 16)) * 100).astype(np.float32))

# ---------------------------------------------------------------------------
# Fixtures the selftests take as their <dir> argument. These were hand-made in
# a scratch directory until CI needed them; they are generated here now so
# `tools/run_selftests.sh` works on a clean checkout on any machine.
# ---------------------------------------------------------------------------

def fresh(p):
    """Rebuild a fixture tree from scratch.

    These four are counted, not just read: --lin asserts exactly 7 levels,
    --sweepfile exactly 7 folders, --close exactly 3 stacks, --picker exactly
    3 batches. A leftover directory from an older layout is a false FAIL that
    costs an hour to find, so the tree is removed before it is rewritten.
    """
    shutil.rmtree(p, ignore_errors=True)
    p.mkdir(parents=True, exist_ok=True)
    return p


# multi/ - three stacks of five identically-named frames, at three levels.
# The workhorse fixture: --range (2 stacks, 2+ frames each, same frame names),
# --abstats (2 stacks), --close and --verify (3 stacks), --batch (the picker
# must open on it), --export (a stack with 2+ frames), --tile (4+ images in one
# named batch) and --export-tsv (2+ stacks) all run against it.
# NOTE --close-selftest also browses <multi>/../rb/scanroot and --verify-selftest
# loads <multi>/../cfa_big_rggb_2064x1032_u16.npy, so multi must stay a direct
# child of the testdata root, next to those two.
multi = fresh(out / "multi")
rng5 = np.random.default_rng(31)
MH, MW = 64, 80
fixed_m = rng5.normal(0, 13.5, (MH, MW))       # fixed pattern, same every frame
for i, level in enumerate((1000.0, 1200.0, 1400.0)):
    d = multi / f"{i:02d}"
    d.mkdir(parents=True, exist_ok=True)
    for k in range(5):
        frame = level + fixed_m + rng5.normal(0, 28.0, (MH, MW))
        np.save(d / f"frame_{k:03d}.npy", frame.astype(np.float32))

# linset/ - a linearity sweep, root/<level>lx/frame_XXX.npy, with ground truth
# the fit must recover. --lin-selftest hard-codes this signature (7 levels
# including a 0 lx dark, 24 frames each, 4 CFA planes, unit "lx") and checks
# the recovered SNR curve against the injected model to 2% / 3%; --series and
# --scan run over the same tree, --framestats over linset/80lx alone.
#   per RGGB plane p:  mean = SENS[p]*level + OFFSET
#                      sigma_t^2 = K*(mean - OFFSET) + READ^2   (photon transfer)
LIN_LEVELS = [0, 10, 20, 40, 80, 160, 320]     # lx; the 0 is the measured dark
LIN_SENS = [8.0, 10.0, 10.0, 6.0]              # DN/lx per plane, RGGB
LIN_OFFSET, LIN_K, LIN_READ, LIN_NFR = 64.0, 2.0, 3.0, 24
LH = LW = 64
linset = fresh(out / "linset")
rng6 = np.random.default_rng(7)
lyy, lxx = np.mgrid[0:LH, 0:LW]
sens_map = np.array(LIN_SENS)[(lyy % 2) * 2 + (lxx % 2)]   # RGGB -> R Gr Gb B
for lv in LIN_LEVELS:
    d = linset / f"{lv}lx"
    d.mkdir(parents=True, exist_ok=True)
    signal = sens_map * lv                                  # DN above offset
    sigma = np.sqrt(LIN_K * signal + LIN_READ ** 2)         # DN, temporal
    for i in range(LIN_NFR):
        f = LIN_OFFSET + signal + rng6.normal(0.0, 1.0, (LH, LW)) * sigma
        np.save(d / f"frame_{i:03d}.npy", f.astype(np.float32))

# abset/ - A/B variants of the same numbering in ONE directory, for
# --picker-selftest: UC3 filters "_A" (must cut the file list), UC6 filters
# "00_A" (must cut exactly one group down to a single 2-D frame, which cannot
# join a sweep) and UC2 merges everything into one stack. Flat constant values
# make "which files actually loaded" readable in the failure output.
abset = fresh(out / "abset")
for k in range(3):
    np.save(abset / f"{k:02d}_A.npy", np.full((8, 8), float(k), np.float32))
    np.save(abset / f"{k:02d}_B.npy", np.full((8, 8), 100.0 + k, np.float32))

# levelfiles/ - the same sweep as linset laid out as ONE multi-frame .npy per
# level, which is the layout --sweepfile-selftest exists to cover. It asserts
# the folder names literally (lv000..lv320, lowercase) and re-derives the
# injected physics: sens 8 DN/lx, offset 64 DN, K 2 DN/e-, read 3 DN. Single
# plane here - no CFA, one axis at a time.
SWEEP_LEVELS = [0, 10, 20, 40, 80, 160, 320]
SW_SENS, SW_OFFSET, SW_K, SW_READ, SW_NFR = 8.0, 64.0, 2.0, 3.0, 8
SH = SW = 32
levelfiles = fresh(out / "levelfiles")
rng7 = np.random.default_rng(4242)
for lv in SWEEP_LEVELS:
    d = levelfiles / f"lv{lv:03d}"
    d.mkdir(parents=True, exist_ok=True)
    signal = SW_SENS * lv
    var = SW_K * signal + SW_READ ** 2
    frames = SW_OFFSET + signal + rng7.normal(0.0, np.sqrt(var), (SW_NFR, SH, SW))
    np.save(d / "capture.npy", frames.astype(np.float32))   # frames on axis 0

# bench_stack.npy - a real multi-frame stack over the wire for
# --remote-selftest (it asks the local peer for META/LIST and compares against
# the local decoder), and the second image of the --bench A/B pass.
np.save(out / "bench_stack.npy",
        (np.random.default_rng(1).random((120, 256, 320)) * 4095).astype(np.float32))

if WANT_BENCH:
    # only the frame-time gate needs this one, and it is 48 MB
    np.save(out / "bench_big.npy",
            (np.random.default_rng(0).random((3000, 4000)) * 4095).astype(np.float32))

    # bench_ab_a.npy / bench_ab_b.npy - the A/B step-throttle experiment of
    # docs/ab-stats-plan.md 1: TWO stacks of 12 Mpx u16 frames, opened together
    # with --compare split (+ follow-frame) and stepped by --bench-step, so
    # every benched frame is a real cache miss on BOTH slots. That is the only
    # shape that measures what the throttle removes; a single big frame keeps
    # every cache key hit, and bench_stack.npy is 0.08 Mpx.
    #
    # The original measurement (f424dae) used a hand-made pair that was never
    # committed - tools/testdata is not in git - so the number could not be
    # re-taken when 138da0d asked for it. Six frames a side is what that run
    # used, and it is the smallest stack that steps for a while without the
    # bounce at the ends dominating.
    #
    # u16, not f32: the histogram's bin path and the projection's accumulate
    # are dtype-dispatched, and u16 is what a sensor actually delivers.
    AB_F, AB_H, AB_W = 6, 3000, 4000                      # 12.0 Mpx a frame
    for name, seed, ped in (("bench_ab_a.npy", 20, 0), ("bench_ab_b.npy", 21, 700)):
        rng_ab = np.random.default_rng(seed)
        # a horizontal ramp (so the projection is a line and not a cloud) plus
        # per-frame noise (so no two frames share a histogram)
        ramp = np.arange(AB_W, dtype=np.int32) * 3 + ped
        stack = np.empty((AB_F, AB_H, AB_W), np.uint16)
        for k in range(AB_F):
            stack[k] = (ramp + rng_ab.integers(0, 512, (AB_H, AB_W), dtype=np.int32)
                        ).astype(np.uint16)
        np.save(out / name, stack)

print("wrote test data to", out)
for p in sorted(out.iterdir()):
    if p.is_dir():
        n = sum(1 for _ in p.rglob("*") if _.is_file())
        print(" ", p.name + "/", n, "files")
    else:
        print(" ", p.name, p.stat().st_size, "bytes")
