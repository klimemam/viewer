#!/usr/bin/env python3
# Transport benchmark for the "can a viewer plugin be written in Python?" study.
#
#   STUDY HARNESS ONLY - docs/python-plugins.md. NOT part of the viewer build,
#   NOT referenced by CMakeLists.txt, NOT imported by anything shipped.
#   Deleting tools/pyplugin-bench/ changes no shipped byte (verified: the study
#   records the sha256 of viewer.exe / viewer-serve.exe / plugins/*.dll with and
#   without this directory present).
#
# Run:
#   python tools/pyplugin-bench/bench_transport.py            # everything
#   python tools/pyplugin-bench/bench_transport.py --quick    # fewer reps
#   python tools/pyplugin-bench/bench_transport.py --json out.json
#
# What it measures, and why each one is here:
#   1 spawn      - process spawn + a trivial call. The floor for ANY per-call
#                  out-of-process design (candidate B).
#   2 oneshot    - moving one 12 Mpx float32 frame (48 MB) by npz / raw file /
#                  mmap, both directions. The adapter's transport today.
#   3 worker     - the same frame through a PERSISTENT worker: pipe, and shared
#                  memory both with and without a copy (candidate A).
#   4 compute    - numpy / torch doing analyzer-shaped work on a resident array.
#                  This is the transport-free floor, i.e. candidate C's cost,
#                  and the yardstick the overheads must be judged against.
#
# Every number is reported as median with min/p25/p75/max over N reps after
# warmups. This is a laptop; single numbers would be a lie.
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable

# 12 Mpx float32 = 48 MB, the size named in the study brief.
H, W = 3000, 4000
NBYTES = H * W * 4


def timeit(fn, reps, warmup=2):
    for _ in range(warmup):
        fn()
    ts = []
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        ts.append((time.perf_counter() - t0) * 1000.0)
    ts.sort()
    return {
        "n": reps,
        "median_ms": statistics.median(ts),
        "min_ms": ts[0],
        "max_ms": ts[-1],
        "p25_ms": ts[max(0, int(0.25 * len(ts)) - 0)],
        "p75_ms": ts[min(len(ts) - 1, int(0.75 * len(ts)))],
    }


def row(results, key, label, r, note=""):
    r["label"] = label
    if note:
        r["note"] = note
    results[key] = r
    print("  %-42s %9.2f ms  [%.2f .. %.2f]  n=%d %s"
          % (label, r["median_ms"], r["min_ms"], r["max_ms"], r["n"], note))
    sys.stdout.flush()


# --------------------------------------------------------------------------
def sec_spawn(res, reps):
    print("\n1. process spawn + trivial call (the per-call floor)")
    child = os.path.join(HERE, "child_trivial.py")

    def run(arg):
        def f():
            subprocess.run([PY, child, arg], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, check=True)
        return f

    row(res, "spawn_bare", "spawn python, print, exit", timeit(run("bare"), reps))
    row(res, "spawn_numpy", "spawn + import numpy", timeit(run("numpy"), reps))
    row(res, "spawn_harness", "spawn + numpy + json + shared_memory",
        timeit(run("harness"), reps))
    try:
        import torch          # noqa: F401
        row(res, "spawn_torch", "spawn + import torch", timeit(run("torch"), max(4, reps // 2)))
    except Exception as e:
        print("  spawn + import torch: SKIPPED (%s)" % e)
        res["spawn_torch"] = {"skipped": str(e)}


# --------------------------------------------------------------------------
def sec_oneshot(res, reps, tmp):
    print("\n2. one 48 MB frame across a file boundary (adapter's transport)")
    a = np.random.default_rng(7).random((H, W), dtype=np.float32)
    npz = os.path.join(tmp, "f.npz")
    raw = os.path.join(tmp, "f.raw")

    np.savez(npz, data=a)
    a.tofile(raw)

    def w_npz():
        np.savez(npz, data=a)

    def r_npz():
        with np.load(npz) as z:
            b = z["data"]
            assert b.shape == (H, W)

    def w_raw():
        a.tofile(raw)

    def w_raw_fsync():
        with open(raw, "wb") as f:
            f.write(a.tobytes())
            f.flush()
            os.fsync(f.fileno())

    def r_raw():
        b = np.fromfile(raw, dtype=np.float32)
        assert b.size == H * W

    def r_mmap():
        b = np.memmap(raw, dtype=np.float32, mode="r", shape=(H, W))
        # touch it the way an analyzer would: a full reduction
        float(b.mean())
        del b

    row(res, "npz_write", "np.savez 48 MB (write, page cache)", timeit(w_npz, reps))
    row(res, "npz_read", "np.load  48 MB (read, page cache)", timeit(r_npz, reps))
    row(res, "raw_write", "tofile   48 MB (write, page cache)", timeit(w_raw, reps))
    row(res, "raw_write_fsync", "write + fsync 48 MB (to the platter)",
        timeit(w_raw_fsync, max(4, reps // 3)))
    row(res, "raw_read", "fromfile 48 MB (read, page cache)", timeit(r_raw, reps))
    row(res, "mmap_read_mean", "memmap 48 MB + full mean()", timeit(r_mmap, reps))

    # compressed npz, because someone will reach for savez_compressed
    def w_npzc():
        np.savez_compressed(os.path.join(tmp, "fc.npz"), data=a)
    row(res, "npz_write_compressed", "np.savez_compressed 48 MB",
        timeit(w_npzc, max(3, reps // 5)))

    def memcpy():
        a.copy()
    row(res, "memcpy48", "numpy copy of 48 MB (in-process)", timeit(memcpy, reps))


# --------------------------------------------------------------------------
class Worker:
    """Spawn once, talk many times. This is candidate A's steady state."""

    def __init__(self):
        self.p = subprocess.Popen(
            [PY, "-u", os.path.join(HERE, "worker.py")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)

    def call(self, req, payload=None):
        self.p.stdin.write((json.dumps(req) + "\n").encode())
        if payload is not None:
            self.p.stdin.write(payload)
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("worker died")
        return json.loads(line)

    def close(self):
        try:
            self.p.stdin.write(b'{"cmd":"quit"}\n')
            self.p.stdin.flush()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def sec_worker(res, reps, tmp):
    print("\n3. persistent worker (spawn once, then call) - candidate A")
    from multiprocessing import shared_memory

    t0 = time.perf_counter()
    w = Worker()
    assert w.call({"cmd": "ping"})["ok"] == 1
    res["worker_first_ping_ms"] = (time.perf_counter() - t0) * 1000.0
    print("  worker spawn + first ping (once per session): %.1f ms"
          % res["worker_first_ping_ms"])

    row(res, "worker_ping", "round trip, no pixels (IPC floor)",
        timeit(lambda: w.call({"cmd": "ping"}), reps * 10))

    a = np.random.default_rng(7).random((H, W), dtype=np.float32)

    shm = shared_memory.SharedMemory(create=True, size=NBYTES)
    try:
        view = np.ndarray((H, W), dtype=np.float32, buffer=shm.buf)
        view[:] = a
        rep = w.call({"cmd": "attach", "name": shm.name, "shape": [H, W]})
        assert rep.get("nbytes") == NBYTES, rep

        def shm_touch():
            w.call({"cmd": "shm_touch"})

        def shm_copy_touch():
            view[:] = a                       # host writes the frame into shm
            w.call({"cmd": "shm_touch"})

        def shm_stats():
            w.call({"cmd": "shm_stats"})

        def shm_copy_stats():
            view[:] = a
            w.call({"cmd": "shm_stats"})

        def shm_noise():
            w.call({"cmd": "shm_noise"})

        row(res, "worker_shm_touch", "shm, frame already there, no real work",
            timeit(shm_touch, reps), "= pure call overhead")
        row(res, "worker_shm_copy_touch", "shm, host copies 48 MB in, no work",
            timeit(shm_copy_touch, reps), "= copy + overhead")
        row(res, "worker_shm_stats", "shm, no copy, mean+std (stats-shaped)",
            timeit(shm_stats, reps))
        row(res, "worker_shm_copy_stats", "shm, copy 48 MB in, mean+std",
            timeit(shm_copy_stats, reps))
        row(res, "worker_shm_noise", "shm, no copy, 16x16 tile-std median",
            timeit(shm_noise, max(5, reps // 2)), "(noise-shaped)")

        payload = a.tobytes()

        def pipe_stats():
            w.call({"cmd": "pipe", "nbytes": NBYTES, "shape": [H, W]}, payload)

        row(res, "worker_pipe_stats", "pipe 48 MB in + mean+std",
            timeit(pipe_stats, max(5, reps // 2)))

        # crash containment probe: does one bad call kill the session?
        bad = w.call({"cmd": "raise"})
        alive = w.call({"cmd": "ping"}).get("ok") == 1
        res["worker_exception"] = {"reply": bad, "worker_alive_after": alive}
        print("  exception in the plugin -> %r ; worker alive after: %s"
              % (bad.get("err"), alive))
    finally:
        w.close()
        del view
        shm.close()
        shm.unlink()


# --------------------------------------------------------------------------
def sec_interactive(res, reps):
    """The question the study actually turns on: while the user drags an ROI,
    how often can a Python analyzer answer? Frame stays in shared memory; only
    the rectangle changes. Reported as ms and as the rate it sustains."""
    print("\n5. interactive: ROI sweep through a warm worker (frame in shm)")
    from multiprocessing import shared_memory

    a = np.random.default_rng(7).random((H, W), dtype=np.float32)
    shm = shared_memory.SharedMemory(create=True, size=NBYTES)
    w = Worker()
    try:
        view = np.ndarray((H, W), dtype=np.float32, buffer=shm.buf)
        view[:] = a
        w.call({"cmd": "attach", "name": shm.name, "shape": [H, W]})
        tinit = w.call({"cmd": "torch_init"})
        res["worker_torch_init"] = tinit
        print("  worker torch_init -> %r" % tinit)

        sizes = [256, 512, 1024, 2048, 0]      # 0 = whole frame
        res["interactive"] = {}
        for kind in ("stats", "statspct", "noise", "torch"):
            for s in sizes:
                if s == 0:
                    roi = [0, 0, W, H]
                    tag = "whole 4000x3000"
                else:
                    roi = [(W - s) // 2, (H - s) // 2, s, s]
                    tag = "%dx%d" % (s, s)
                if kind == "torch":
                    req = {"cmd": "torch_roi", "roi": roi}
                else:
                    req = {"cmd": "shm_roi", "roi": roi, "kind": kind}
                probe = w.call(req)
                if "err" in probe:
                    print("  %-10s %-16s ERROR %s" % (kind, tag, probe["err"]))
                    continue
                n = reps if s and s <= 1024 else max(5, reps // 2)
                r = timeit(lambda: w.call(req), n)
                hz = 1000.0 / r["median_ms"] if r["median_ms"] > 0 else float("inf")
                r["hz"] = hz
                res["interactive"]["%s_%s" % (kind, tag.split()[0])] = r
                print("  %-10s %-16s %8.2f ms  [%.2f .. %.2f]  = %7.1f /s"
                      % (kind, tag, r["median_ms"], r["min_ms"], r["max_ms"], hz))
                sys.stdout.flush()
    finally:
        w.close()
        del view
        shm.close()
        shm.unlink()


# --------------------------------------------------------------------------
def sec_compute(res, reps):
    print("\n4. the work itself, on a resident array (transport-free floor)")
    a = np.random.default_rng(7).random((H, W), dtype=np.float32)

    def np_stats():
        float(a.mean()); float(a.std()); float(a.min()); float(a.max())

    def np_stats_pct():
        float(a.mean()); float(a.std())
        np.percentile(a, [1, 50, 99])

    def np_noise():
        t = a[:H // 16 * 16, :W // 16 * 16].reshape(H // 16, 16, W // 16, 16)
        t = t.transpose(0, 2, 1, 3).reshape(-1, 256)
        float(np.median(t.std(axis=1)))

    row(res, "numpy_stats", "numpy mean+std+min+max", timeit(np_stats, reps))
    row(res, "numpy_stats_pct", "numpy mean+std+percentile(1,50,99)",
        timeit(np_stats_pct, max(5, reps // 2)))
    row(res, "numpy_noise", "numpy 16x16 tile-std median", timeit(np_noise, max(5, reps // 2)))

    try:
        import torch
        res["torch_version"] = torch.__version__
        res["torch_cuda_available"] = bool(torch.cuda.is_available())
        t = torch.from_numpy(a)                    # zero-copy view of the numpy array

        def t_stats():
            float(t.mean()); float(t.std())

        def t_from_numpy():
            torch.from_numpy(a)

        row(res, "torch_cpu_stats", "torch CPU mean+std (zero-copy view)",
            timeit(t_stats, reps))
        row(res, "torch_from_numpy", "torch.from_numpy (wrap, no copy)",
            timeit(t_from_numpy, reps * 10))
        if torch.cuda.is_available():
            def h2d():
                t.cuda(); torch.cuda.synchronize()
            row(res, "torch_h2d", "torch host->device 48 MB", timeit(h2d, reps))
        else:
            print("  torch CUDA: NOT AVAILABLE on this machine "
                  "(%s) - device transfer UNMEASURED" % torch.__version__)
    except Exception as e:
        print("  torch: SKIPPED (%s)" % e)
        res["torch"] = {"skipped": str(e)}


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--json", default=os.path.join(HERE, "bench_results.json"))
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    reps = 5 if args.quick else 15

    res = {
        "python": sys.version,
        "numpy": np.__version__,
        "executable": PY,
        "frame": {"h": H, "w": W, "bytes": NBYTES, "dtype": "float32"},
        "reps": reps,
        "when": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    print("viewer python-plugin transport benchmark")
    print("  python %s" % sys.version.split()[0])
    print("  numpy  %s" % np.__version__)
    print("  frame  %dx%d float32 = %.1f MB" % (W, H, NBYTES / 1e6))
    print("  reps   %d (medians and full range reported)" % reps)

    want = set(args.only.split(",")) if args.only else None
    tmp = tempfile.mkdtemp(prefix="pybench_")
    try:
        if not want or "spawn" in want:
            sec_spawn(res, max(4, reps // 2))
        if not want or "oneshot" in want:
            sec_oneshot(res, reps, tmp)
        if not want or "worker" in want:
            sec_worker(res, reps, tmp)
        if not want or "compute" in want:
            sec_compute(res, reps)
        if not want or "interactive" in want:
            sec_interactive(res, reps)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    with open(args.json, "w") as f:
        json.dump(res, f, indent=1)
    print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
