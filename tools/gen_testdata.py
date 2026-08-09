"""Generate test data for the native viewer (npy / raw).

    python tools/gen_testdata.py [outdir] [--bench]

Every fixture the selftests need is written here, deterministically (fixed
rng seeds), so CI can regenerate the lot from a clean checkout - nothing under
tools/testdata is in git. `--bench` adds the large arrays the frame-time gate
and the A/B step-throttle measurement use; they are ~330 MB and no selftest
needs them, so they are opt-in.
"""
import base64
import shutil
import struct
import sys
import zlib
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

# shapes/ — one file per form the §3.1 rule has to decide, so the rule is
# testable at all.  The folder existed by hand and was therefore invisible to
# CI (tools/testdata/ is gitignored and only this script fills it).
#
# hw1_ and hw2_ are the two shapes issue #71 is about: the last axis is 1 or 2,
# where core/main.cpp (== 3 || == 4) and core/serve.cpp (<= 4) disagree today.
# They are FIXTURES ONLY - nothing asserts on them yet, deliberately: asserting
# would pick the side the issue exists to decide.  A mono capture saved as
# img[:, :, None] is an ordinary thing for numpy code to produce.
shp = out / "shapes"
shp.mkdir(exist_ok=True)
sh_h, sh_w = 64, 80
sbase = (yy[:sh_h, :sh_w] * sh_w + xx[:sh_h, :sh_w]).astype(np.float32)
np.save(shp / "hw_64x80.npy",        sbase)                              # (H,W)
np.save(shp / "hw1_64x80x1.npy",     sbase[:, :, None])                  # (H,W,1)  <- #71
np.save(shp / "hw2_64x80x2.npy",     np.stack([sbase, sbase / 2], -1))   # (H,W,2)  <- #71
np.save(shp / "hwc_64x80x3.npy",     np.stack([sbase] * 3, -1))          # (H,W,3)
np.save(shp / "chw_3x64x80.npy",     np.stack([sbase] * 3, 0))           # (C,H,W) = 3 frames
np.save(shp / "fhw_24x64x80.npy",    np.stack([sbase + k for k in range(24)], 0))
np.save(shp / "fhw1_12x64x80x1.npy", np.stack([sbase + k for k in range(12)], 0)[..., None])
np.save(shp / "fhwc_8x64x80x3.npy",
        np.stack([np.stack([sbase + k] * 3, -1) for k in range(8)], 0))

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

# unpadded/ - the ONE fixture where lexicographic and natural name order give
# different answers, which until now nothing generated here did: every other
# numbered set is zero-padded to a fixed width (frame_000, gain10_007, 0000),
# and for those two the two orders are the same sequence. So the listing could
# sort names as text while the stack it opened sorted them by value, and no
# test could see it (--browse-selftest, --browse-keys-selftest and
# --localbrowse-selftest all browse rb/).
#
#   lv1 / lv2 / lv10   as DIRECTORIES, because directories never collapse into
#                      a group: they are the rows themselves, so this is what
#                      the listing sort and the tree's per-level sort are
#                      asserted on. Text order says lv1, lv10, lv2.
#   frame_1 / frame_2 / frame_10  as .npy, so they DO collapse into one group -
#                      whose members (and whose displayed extent,
#                      "frame_1‥10.npy") are what the flat listing shows and
#                      what the stack is built from. Text order says
#                      frame_1, frame_10, frame_2.
#
# The name sorts after every other directory in rb/ on purpose: rb's rows are
# addressed by index in the --browse-keys action script, and inserting a folder
# earlier in the alphabet would renumber the ones already spoken for.
# The folders hold capture.npy and not frame_000.npy on purpose: --remote-selftest
# counts `**/frame_*.npy` under rb/ against a literal number, and only the three
# names this fixture is ABOUT should move it.
uset = rb / "unpadded"
uset.mkdir(exist_ok=True)
for k in (1, 2, 10):
    np.save(uset / f"frame_{k}.npy", (rng3.random((16, 16)) * 4095).astype(np.float32))
    d = uset / f"lv{k}"
    d.mkdir(exist_ok=True)
    np.save(d / "capture.npy", (rng3.random((8, 8)) * 4095).astype(np.float32))

for lv in ("10lx", "20lx", "40lx"):
    d = rb / "scanroot" / lv
    d.mkdir(parents=True, exist_ok=True)
    for k in range(8):
        np.save(d / f"frame_{k:03d}.npy", (rng3.random((32, 32)) * 4095).astype(np.float32))

# expset/zdeep: the one fixture that is DEEP and whose deepest level is also its
# LAST rows. Everything else under rb/ bottoms out in one or two levels and
# ends, at every depth, on files that sit at the top level - so scrolling to the
# end of any of them puts a top-level row at the top of the viewport, and the
# pinned-ancestor band correctly holds nothing. That is a true answer to the
# wrong question. Here the tail of the listing is sixty rows that are all three
# levels down, so scrolling anywhere near the bottom is guaranteed to leave the
# reader inside a/b/c whatever height the panel happens to have.
#
# Under EXPSET, and not at rb's root, which is where it was first written. The
# --browse-keys script walks rb's root by counting Down presses, and a listing
# sorts directories before files - so a seventh directory there moves every
# FILE row down one and quietly re-aims a dozen assertions that were written
# against row 9. expset is the one rb directory the script enters without
# asserting anything about the rows inside it (it clicks in, checks the
# history, and backs straight out), so a child here is invisible to every
# index that already exists.
#
# Three more constraints, each of them another test's:
#   - no file matches frame_*.npy - --remote-selftest counts those against a
#     literal 51 over the whole tree, and none of this may reach that number;
#   - nothing new at rb's root: --browse-selftest asserts rb lists exactly one
#     group and three plain files;
#   - the leaf names carry no digits, so the peer's numbered-run grouping folds
#     none of them together. Sixty rows that collapsed into one group row would
#     scroll nothing.
zd = eset / "zdeep" / "a" / "b" / "c"
zd.mkdir(parents=True, exist_ok=True)
for hi in "abcde":
    for lo in "abcdefghijkl":
        np.save(zd / f"shot_{hi}{lo}.npy", (rng3.random((8, 8)) * 4095).astype(np.float32))

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


# ---------------------------------------------------------------- media/ ----
# PNG / JPEG / TIFF for --media-selftest (core/imagefile.h).
#
# Written with zlib and struct, NOT with Pillow: CI installs numpy and nothing
# else (.github/workflows/build.yml), and a fixture that only exists on a
# machine with Pillow is a fixture CI has never seen - which is how the shapes/
# folder came to be checked on one laptop and nowhere else.
#
# Every PNG here is written by hand from its raw samples, so the file's declared
# bit depth and its values are both known EXACTLY on this side of the round
# trip. That is what makes "an 8-bit PNG is 0..255 of something, and the viewer
# must not silently rescale it" a test rather than an opinion.
media = out / "media"
media.mkdir(exist_ok=True)


def _png_chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path, arr, bit=8, ctype=0, palette=None):
    """arr: (H,W) or (H,W,C) of ints already in range. ctype: 0 grey, 2 RGB,
    3 palette, 4 grey+alpha, 6 RGBA. bit: 4, 8 or 16 (4 is greyscale only)."""
    a = np.asarray(arr)
    h, w = a.shape[0], a.shape[1]
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter type 0: none
        row = a[y]
        if bit == 16:
            raw += row.astype(">u2").tobytes()
        elif bit == 8:
            raw += row.astype("u1").tobytes()
        elif bit == 4:                      # two samples to a byte, high nibble first
            flat = row.astype("u1").ravel()
            if flat.size % 2:
                flat = np.append(flat, 0)
            raw += bytes(((flat[0::2] & 0xF) << 4) | (flat[1::2] & 0xF))
        else:
            raise ValueError("bit depth %r" % bit)
    body = b"\x89PNG\r\n\x1a\n" + _png_chunk(
        b"IHDR", struct.pack(">IIBBBBB", w, h, bit, ctype, 0, 0, 0))
    if palette is not None:
        body += _png_chunk(b"PLTE", np.asarray(palette, "u1").tobytes())
    body += _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    body += _png_chunk(b"IEND", b"")
    path.write_bytes(body)


mh, mw = 48, 64
myy, mxx = np.mgrid[0:mh, 0:mw]

# 16-bit greyscale: THE case this audience has. The values run past 255 on
# purpose - a viewer that read this through an 8-bit door, or divided by 65535,
# would show the same picture and the wrong numbers.
g16 = ((myy * mw + mxx) * 21 % 65536).astype(np.uint16)
write_png(media / "gray16.png", g16, bit=16, ctype=0)

# 8-bit greyscale: one frame, ONE channel (the last-axis rule, docs/
# input-adapters.md 3.1) - not one RGBA frame with three copies of it.
g8 = ((mxx * 4 + myy) % 256).astype(np.uint8)
write_png(media / "gray8.png", g8, bit=8, ctype=0)

# 8-bit RGB and RGBA
rgb8 = np.stack([mxx * 255 // (mw - 1), myy * 255 // (mh - 1),
                 np.full_like(mxx, 96)], -1).astype(np.uint8)
write_png(media / "rgb8.png", rgb8, bit=8, ctype=2)
rgba8 = np.concatenate([rgb8, np.full((mh, mw, 1), 200, np.uint8)], -1)
write_png(media / "rgba8.png", rgba8, bit=8, ctype=6)

# palette: 8-bit indices that come back as colour samples. The viewer has to say
# that happened - the numbers on screen are palette entries, not what was stored.
pal = np.arange(256 * 3, dtype=np.uint8).reshape(256, 3) % 251
write_png(media / "pal8.png", (mxx % 256).astype(np.uint8), bit=8, ctype=3,
          palette=pal)

# 4-bit greyscale: the one case where the decoder MULTIPLIES (stb expands 0..15
# to 0..255 by 17). The fixture exists so that the note saying so is asserted.
write_png(media / "gray4.png", (mxx % 16).astype(np.uint8), bit=4, ctype=0)

# A JPEG, and the reason it is a literal.
#
# There is no JPEG encoder in the standard library and CI installs only numpy,
# so this one is embedded rather than computed. It was produced ONCE, here, with
# Pillow 12.3 - the same ramp as rgb8 at 32x24, saved with quality=90,
# subsampling=0, progressive=False - and everything asserted about it is
# structural (dimensions, channels, corners within a tolerance), never
# pixel-exact: it is a LOSSY format, and a different decoder behind the same
# seam is allowed to differ by a few DN. 859 bytes.
JPEG_B64 = (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoM"
    "DAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsN"
    "FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAAR"
    "CAAYACADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
    "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkK"
    "FhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWG"
    "h4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl"
    "5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
    "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYk"
    "NOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOE"
    "hYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk"
    "5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD4a0nwJ0/d/pX6jWzHzPFyzNdtTtNI8Cfd"
    "/d/pXhVsx8z9YyzNdtTtdI8Cfd/d/pXg1sx8z9XyzNdtTtNJ8Cfd/d/pXg1sx8z9YyzNdtTM"
    "0jwH0/d/pXXWzHzP8lcszXbU7XSfAf3f3f6V4NbMfM/WMszXbU7TSfAf3f3f6V4VbMfM/WMs"
    "zXbU7XSPAf3f3f6V4NbMfM/WMszXbUzNJ8CdP3f6V11sx8z/ACUyzNdtTtNJ8Cfd/d/pXg1s"
    "x8z9YyzNdtTtdI8Cfd/d/pXg1sx8z9XyzNdtTtNJ8Cfd/d/pXhVsx8z9YyzNdtT/2Q==")
jpeg_bytes = base64.b64decode(JPEG_B64)
(media / "gradient.jpg").write_bytes(jpeg_bytes)

# ...and the same bytes under a .png name. The extension is a claim, the bytes
# are evidence: this one asserts which of the two the viewer believes, and that
# it says out loud that they disagreed.
(media / "mislabelled.png").write_bytes(jpeg_bytes)


# ---------------------------------------------------------------- TIFF -------
# REAL TIFFs, written here for the same reason the PNGs are: CI installs numpy
# and nothing else, so a fixture that needs Pillow is a fixture CI never sees.
#
# TIFF is the format the viewer reads that has the most ways to be written, and
# a reader that quietly guesses at one of them is the failure this project
# exists to avoid - so the fixtures are not "a TIFF" but ONE PER AXIS of that
# variety: byte order, bit depth, sample format, compression, predictor, strip
# count, page count, photometric. Half of them are files the reader must REFUSE,
# because "it refused the right thing, by name" is as much a behaviour as
# "it read the right numbers" and nothing else here can test it.
#
# Every one of them is generated from its samples on this side, so both what the
# file DECLARES and what it HOLDS are known exactly - which is what makes
# "16-bit comes back sample for sample" a test rather than a hope.

_TIFF_FMT = {1: "B", 3: "H", 4: "I", 11: "f"}      # BYTE / SHORT / LONG / FLOAT
_TIFF_SIZE = {1: 1, 3: 2, 4: 4, 11: 4}


def _packbits(data):
    """PackBits (TIFF compression 32773). Runs of 3+ equal bytes become a run."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += struct.pack("b", 1 - run) + bytes([data[i]])
            i += run
            continue
        lit = 1
        while i + lit < n and lit < 128:
            if (i + lit + 2 < n and data[i + lit] == data[i + lit + 1] ==
                    data[i + lit + 2]):
                break
            lit += 1
        out += struct.pack("b", lit - 1) + data[i:i + lit]
        i += lit
    return bytes(out)


def _lzw(data):
    """TIFF LZW (compression 5): MSB-first codes, 9..12 bits, early change.

    Written to match the decoder's contract exactly - clear at 256, EOI at 257,
    first free code 258, and the width steps UP one code early (at 511/1023/
    2047), which is the "early change" every TIFF encoder in the wild uses and
    the one detail an LZW implementation copied from a GIF gets wrong.
    """
    bits, acc, nacc = bytearray(), 0, 0

    def put(code, width):
        nonlocal acc, nacc
        acc = (acc << width) | code
        nacc += width
        while nacc >= 8:
            nacc -= 8
            bits.append((acc >> nacc) & 0xFF)

    table = {bytes([i]): i for i in range(256)}
    nxt, width = 258, 9
    put(256, width)
    omega = b""
    for ch in data:
        c = bytes([ch])
        if omega + c in table:
            omega += c
            continue
        put(table[omega], width)
        if nxt < 4093:
            table[omega + c] = nxt
            nxt += 1
            width = 12 if nxt >= 2047 else 11 if nxt >= 1023 else 10 if nxt >= 511 else 9
        else:                                  # table full: start over, as libtiff does
            put(256, width)
            table = {bytes([i]): i for i in range(256)}
            nxt, width = 258, 9
        omega = c
    if omega:
        put(table[omega], width)
    put(257, width)
    if nacc:
        bits.append((acc << (8 - nacc)) & 0xFF)
    return bytes(bits)


def _tiff_compress(data, code):
    if code == 1:
        return data
    if code in (8, 32946):
        return zlib.compress(data, 9)
    if code == 5:
        return _lzw(data)
    if code == 32773:
        return _packbits(data)
    raise ValueError("no encoder for TIFF compression %d" % code)


def _tiff_depth(a):
    if a.dtype == np.uint8:
        return 8, 1                                 # SampleFormat 1: unsigned
    if a.dtype == np.uint16:
        return 16, 1
    if a.dtype == np.float32:
        return 32, 3                                # SampleFormat 3: IEEE float
    raise ValueError("no TIFF fixture path for dtype %r" % a.dtype)


def _tiff_block(rows, bo, bits, sf, predictor, ch):
    """One strip or tile of pixels, (R,W,C) -> the bytes the file holds."""
    flat = rows.reshape(rows.shape[0], -1)
    if predictor == 2:
        if sf != 1:
            raise ValueError("predictor 2 is for integer samples")
        d = flat.astype(np.int64)
        d[:, ch:] -= flat[:, :-ch].astype(np.int64)
        flat = (d & ((1 << bits) - 1)).astype(flat.dtype)
    dt = {8: "u1", 16: "u2", 32: "f4"}[bits]
    return flat.astype(np.dtype(("|" if bits == 8 else bo) + dt)).tobytes()


def tiff_page(arr, photometric=None, compression=1, predictor=1,
              rows_per_strip=None, tile=None, subfiletype=None, extra=(),
              blocks=None):
    """One IFD's worth. `blocks` replaces the encoded pixels verbatim, which is
    how a fixture can hold a compression this file has no encoder for."""
    return dict(arr=arr, photometric=photometric, compression=compression,
                predictor=predictor, rows_per_strip=rows_per_strip, tile=tile,
                subfiletype=subfiletype, extra=list(extra), blocks=blocks)


def _tiff_parts(pg, bo):
    a = np.asarray(pg["arr"])
    if a.ndim == 2:
        a = a[:, :, None]
    h, w, ch = a.shape
    bits, sf = _tiff_depth(a)
    comp, pred = pg["compression"], pg["predictor"]
    photo = pg["photometric"]
    if photo is None:
        photo = 2 if ch >= 3 else 1                 # RGB / BlackIsZero
    entries = [(256, 4, [w]), (257, 4, [h]), (258, 3, [bits] * ch),
               (259, 3, [comp]), (262, 3, [photo]), (277, 3, [ch]),
               (284, 3, [1]), (339, 3, [sf] * ch)]
    if pred != 1:
        entries.append((317, 3, [pred]))
    if pg["subfiletype"] is not None:
        entries.append((254, 4, [pg["subfiletype"]]))
    entries += pg["extra"]
    blocks = []
    override = pg["blocks"] is not None
    if pg["tile"]:
        tw, th = pg["tile"]
        entries += [(322, 3, [tw]), (323, 3, [th])]
        offtag, cnttag = 324, 325
        for ty in range(0, h, th):
            for tx in range(0, w, tw):
                if override:
                    continue
                t = np.zeros((th, tw, ch), a.dtype)
                sub = a[ty:ty + th, tx:tx + tw]
                t[:sub.shape[0], :sub.shape[1]] = sub
                blocks.append(_tiff_compress(_tiff_block(t, bo, bits, sf, pred, ch), comp))
    else:
        rps = pg["rows_per_strip"] or h
        entries.append((278, 4, [rps]))
        offtag, cnttag = 273, 279
        for y in range(0, h, rps):
            if override:
                continue
            blocks.append(_tiff_compress(
                _tiff_block(a[y:y + rps], bo, bits, sf, pred, ch), comp))
    if override:
        blocks = list(pg["blocks"])
    return blocks, entries, offtag, cnttag


def write_tiff(path, pages, bo="<"):
    """pages: a list of tiff_page(). More than one = more than one IFD, which is
    what the viewer has to turn into a stack."""
    parts = [_tiff_parts(pg, bo) for pg in pages]

    def overflow(typ, count):                       # bytes that do not fit the 4-byte field
        b = _TIFF_SIZE[typ] * count
        return 0 if b <= 4 else b + (b & 1)

    sizes = []
    for blocks, entries, _, _ in parts:
        nent = len(entries) + 2                     # + offsets + byte counts
        ov = sum(overflow(t, len(v)) for (_, t, v) in entries) + 2 * overflow(4, len(blocks))
        sizes.append((sum(len(b) + (len(b) & 1) for b in blocks),
                      2 + 12 * nent + 4, ov))
    base, pos = [], 8
    for data, ifd, ov in sizes:
        base.append(pos)
        pos += data + ifd + ov
    ifd_at = [base[i] + sizes[i][0] for i in range(len(parts))]

    out = bytearray(struct.pack(bo + "HHI", 0x4949 if bo == "<" else 0x4D4D, 42, ifd_at[0]))
    for i, (blocks, entries, offtag, cnttag) in enumerate(parts):
        assert len(out) == base[i], (len(out), base[i])
        offs, cnts = [], []
        for b in blocks:
            offs.append(len(out))
            cnts.append(len(b))
            out += b
            if len(b) & 1:
                out += b"\0"
        assert len(out) == ifd_at[i], (len(out), ifd_at[i])
        full = sorted(entries + [(offtag, 4, offs), (cnttag, 4, cnts)])
        ovbase = ifd_at[i] + 2 + 12 * len(full) + 4
        ifd = bytearray(struct.pack(bo + "H", len(full)))
        ovdata = bytearray()
        for tag, typ, vals in full:
            payload = struct.pack(bo + _TIFF_FMT[typ] * len(vals), *vals)
            ifd += struct.pack(bo + "HHI", tag, typ, len(vals))
            if len(payload) <= 4:
                ifd += payload.ljust(4, b"\0")      # left-justified, both byte orders
            else:
                ifd += struct.pack(bo + "I", ovbase + len(ovdata))
                ovdata += payload
                if len(ovdata) & 1:
                    ovdata += b"\0"
        ifd += struct.pack(bo + "I", ifd_at[i + 1] if i + 1 < len(parts) else 0)
        out += ifd + ovdata
    path.write_bytes(bytes(out))


def write_bigtiff_gray8(path, arr):
    """A genuine BigTIFF (magic 43, 8-byte offsets, 20-byte IFD entries).

    The point of it is that sniffTiff ACCEPTS magic 43, so a BigTIFF is
    dispatched to the TIFF backend and must come back with a refusal that names
    BigTIFF - not with a parse error from a reader that walked a classic IFD
    over a structure that is not one.
    """
    a = np.asarray(arr, np.uint8)
    h, w = a.shape
    px = a.tobytes()
    ifd_at = 16 + len(px)
    entries = [(256, 4, 1, w), (257, 4, 1, h), (258, 3, 1, 8), (259, 3, 1, 1),
               (262, 3, 1, 1), (273, 16, 1, 16), (277, 3, 1, 1),
               (278, 4, 1, h), (279, 16, 1, len(px))]
    ifd = struct.pack("<Q", len(entries))
    for tag, typ, cnt, val in entries:
        ifd += struct.pack("<HHQ", tag, typ, cnt)
        ifd += (struct.pack("<HHHH", val, 0, 0, 0) if typ == 3 else
                struct.pack("<II", val, 0) if typ == 4 else struct.pack("<Q", val))
    ifd += struct.pack("<Q", 0)
    path.write_bytes(struct.pack("<HHHHQ", 0x4949, 43, 8, 0, ifd_at) + px + ifd)


# ---- what the reader must READ ---------------------------------------------
# gray8.tif is the same picture in the same layout as the build that REFUSED it
# (baseline, uncompressed, 8-bit grey, one strip): the file that was the honest
# refusal is the file that is now the round trip, so nothing about this fixture
# was chosen to flatter the new reader.
write_tiff(media / "gray8.tif", [tiff_page(g8)])

# 16-bit, and MULTI-STRIP with an uneven last strip (48 rows in 7s = 6 full + 6
# rows). Real 16-bit TIFFs are strip-per-few-rows, and a reader that assumed one
# strip reads the first rows and garbage after them - which looks like an image.
g16t = ((myy * mw + mxx) * 37 % 65536).astype(np.uint16)
write_tiff(media / "gray16.tif", [tiff_page(g16t, rows_per_strip=7)])

# The same samples through LZW + horizontal differencing, which is what a
# 16-bit TIFF off a real acquisition tool actually looks like. Asserted against
# gray16.tif sample for sample: two paths, one answer, or the compression is
# doing something to the measurement.
write_tiff(media / "gray16_lzw.tif",
           [tiff_page(g16t, compression=5, predictor=2, rows_per_strip=7)])

# RGB 8-bit, PackBits. Same ramp as rgb8.png so the channel rule is asserted
# against a known picture rather than against itself.
write_tiff(media / "rgb8.tif", [tiff_page(rgb8, compression=32773)])

# BIG-ENDIAN ("MM"), 16-bit RGB, Deflate + predictor. Byte order is a per-file
# fact in TIFF and half the cameras that write one write MM; a reader that
# assumed II returns byte-swapped samples, which are numbers, and wrong.
rgb16 = np.stack([mxx * 65535 // (mw - 1), myy * 65535 // (mh - 1),
                  np.full_like(mxx, 4096)], -1).astype(np.uint16)
write_tiff(media / "rgb16_be.tif",
           [tiff_page(rgb16, compression=8, predictor=2, rows_per_strip=16)], bo=">")

# 32-bit IEEE float: TIFF is the one of the three formats that carries a
# measurement straight, and this is the case that proves the viewer does not
# round it into an integer on the way in.
f32t = ((mxx - mw / 2) ** 2 + (myy - mh / 2) ** 2).astype(np.float32) / 100.0 - 3.5
write_tiff(media / "float32.tif", [tiff_page(f32t, rows_per_strip=13)])

# MULTI-PAGE = A STACK (core/imagefile.h). Three pages, each a constant plane at
# a different level, so "which frame is which" is readable in a failure.
pages3 = [np.full((mh, mw), v, np.uint16) for v in (1000, 2000, 3000)]
write_tiff(media / "stack3.tif", [tiff_page(p, rows_per_strip=11) for p in pages3])

# ...and the page that is NOT a frame. NewSubfileType bit 0 says "reduced
# resolution version of another image", i.e. a thumbnail: counting it as a frame
# of the stack would put a different picture in the middle of a measurement.
write_tiff(media / "thumb.tif",
           [tiff_page(g16t), tiff_page(g16t[::4, ::4].copy(), subfiletype=1)])

# Photometric 0 (WhiteIsZero): 0 means WHITE. The samples are still the samples,
# so they are NOT inverted - and that is exactly the kind of thing that has to
# be said out loud rather than left for a histogram to reveal.
write_tiff(media / "whitezero.tif", [tiff_page(g8, photometric=0)])

# ---- what the reader must REFUSE, by name ----------------------------------
# Tiled: a genuine tiled TIFF (16x16 tiles), not a strip file wearing tile tags.
write_tiff(media / "tiled.tif", [tiff_page(g8, tile=(16, 16))])

# JPEG-in-TIFF (compression 7). The strip really does hold the JPEG from above,
# so the refusal is about the compression scheme and not about a broken file.
write_tiff(media / "jpeginside.tif",
           [tiff_page(np.zeros((24, 32), np.uint8), compression=7, blocks=[jpeg_bytes])])

# Photometric 32803 = colour filter array. THE case core/imagefile.h rule 3 is
# about: the pattern must be read exactly or not at all, and this build reads it
# not at all - so it refuses instead of opening a plausible wrong picture.
write_tiff(media / "cfa.tif", [tiff_page(g8, photometric=32803,
                                         extra=[(33421, 3, [2, 2]),
                                                (33422, 1, [0, 1, 1, 2])])])

# Palette. PNG's palette IS expanded (stb does it, and the note says so), so the
# difference between the two formats has to be visible rather than surprising.
cmap = [(i * 257) % 65536 for i in range(256)] * 3
write_tiff(media / "palette.tif",
           [tiff_page((mxx % 256).astype(np.uint8), photometric=3,
                      extra=[(320, 3, cmap)])])

# Pages that are not one shape. A multi-page TIFF is a stack, and a stack's
# frames are one grid: this one says so instead of building a ragged stack.
write_tiff(media / "mixedpages.tif",
           [tiff_page(g8), tiff_page(g8[:12, :16].copy())])

write_bigtiff_gray8(media / "big.tif", g8)

# Two ways to be refused, both of which have to say something better than
# "unsupported": a PNG whose signature is right and whose body is not, and a
# file that is a .png in name only.
(media / "broken.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00" * 64)
(media / "notanimage.png").write_bytes(b"hello, this is not a picture\n")


# ------------------------------------------------------------------ y4m ----
# The one video container this build reads (docs/video-support.md). It is here
# and not in tools/mkexr.cpp - and not shelled out to ffmpeg, which CI does not
# have - because a y4m IS a text header plus raw planes: writing one from its
# own samples is twenty lines, and that is the same property that makes it the
# format whose numbers survive at all. If a fixture generator needed a codec,
# the format would not belong in this viewer.
#
# Frame f, pixel (x,y) = 1000 + 100f + 3x + y. The value names its own FRAME and
# its own POSITION, so "the frames are in presentation order" and "the samples
# are bit-exact" are one check rather than two - a reader that returned the
# frames reversed, or that swapped rows and columns, fails on the value.
Y4W, Y4H, Y4N = 16, 8, 5


def y4m_luma(f, bps):
    b = bytearray()
    for y in range(Y4H):
        for x in range(Y4W):
            v = 1000 + 100 * f + 3 * x + y
            b.append(v & 0xFF)                    # y4m is little-endian
            if bps == 2:
                b.append((v >> 8) & 0xFF)
    return bytes(b)


def y4m_bytes(header, bps, chroma_bytes=0, frames=Y4N):
    blob = header.encode("ascii")
    for f in range(frames):
        blob += b"FRAME\n" + y4m_luma(f, bps) + b"\x80" * chroma_bytes
    return blob


# 16-bit mono: the honest case. Bit-exact luma, so the values are DN.
mono16 = y4m_bytes("YUV4MPEG2 W16 H8 F30:1 Ip A1:1 Cmono16 XCOLORRANGE=FULL\n", 2)
(media / "mono16.y4m").write_bytes(mono16)

# One frame is not a stack - it is one picture, and the seam must not mint a
# SeqInfo for it.
(media / "one.y4m").write_bytes(
    y4m_bytes("YUV4MPEG2 W16 H8 F30:1 Ip A1:1 Cmono16\n", 2, frames=1))

# Subsampled chroma: present in the file, NOT read. Luma only, said in the note.
_sub = 2 * (Y4W // 2) * (Y4H // 2)
(media / "sub420.y4m").write_bytes(
    y4m_bytes("YUV4MPEG2 W16 H8 F30:1 Ip A1:1 C420jpeg\n", 1, _sub))

# C420paldv is an ordinary 8-bit file whose suffix begins with the same letter
# as the pNN bit-depth marker. Reading its depth as atoi("aldv") = 0 refused it
# as "a luma bit depth of 0", so the digit test in y4mread.cpp is pinned by a
# fixture rather than by eye.
(media / "paldv.y4m").write_bytes(
    y4m_bytes("YUV4MPEG2 W16 H8 F30:1 Ip A1:1 C420paldv\n", 1, _sub))

# Refused: a frame that is two fields is two instants, so sigma_t over such a
# stack is not a temporal noise.
(media / "fields.y4m").write_bytes(
    y4m_bytes("YUV4MPEG2 W16 H8 F30:1 It A1:1 Cmono\n", 1))

# Refused: a colour space this reader does not know. Named, not "unsupported".
(media / "badcsp.y4m").write_bytes(
    y4m_bytes("YUV4MPEG2 W16 H8 F30:1 Ip A1:1 C777\n", 1))

# Cut in half. N is ARITHMETIC from the byte count (the format declares no frame
# count anywhere), so the honest report is what the SURVIVING bytes imply:
# 56-byte header + 5 x 262-byte frames = 1366; half = 683; 627 body bytes =
# 2 x 262 + 103, so 2 whole frames and a third begun -> "2 of 3", never "2 of 5".
(media / "cut.y4m").write_bytes(mono16[: len(mono16) // 2])

# Not a y4m at all, and not a picture either: the refusal for a codec-bearing
# container is by NAME, so what is inside is irrelevant and this says so by
# being obviously nothing.
(media / "capture.mp4").write_bytes(b"\x00" * 4096)

print("wrote test data to", out)
for p in sorted(out.iterdir()):
    if p.is_dir():
        n = sum(1 for _ in p.rglob("*") if _.is_file())
        print(" ", p.name + "/", n, "files")
    else:
        print(" ", p.name, p.stat().st_size, "bytes")
