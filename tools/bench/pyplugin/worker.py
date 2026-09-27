#!/usr/bin/env python3
# Persistent-worker side of the Python-plugin transport benchmark.
#
#   STUDY HARNESS ONLY - docs/python-plugins.md. Not part of the viewer build,
#   not imported by anything the viewer ships. Deleting this directory changes
#   no shipped byte.
#
# Protocol (deliberately the cheapest thing that could work, so the numbers are
# a FLOOR for any real design, not an upper bound):
#   parent -> child : one JSON object per line on stdin
#                     {"cmd": ...}, and for cmd="pipe" the line is followed by
#                     exactly nbytes of raw little-endian float32 pixels
#   child  -> parent: one JSON object per line on stdout
#
# Commands
#   ping                      round-trip floor, no pixels
#   attach   {name,shape}     open a named shared-memory block, keep the view
#   shm_stats                 mean/std over the attached shm view (no copy)
#   shm_noise                 16x16 tile-std median over the attached view
#   shm_touch                 sum of one row - "reply overhead with the compute
#                             taken out", so transport and work can be separated
#   shm_roi  {roi,kind}       the INTERACTIVE case: measure a sub-rectangle of
#                             the attached view. kind = stats|statspct|noise
#   torch_roi {roi}           the same through torch on a zero-copy tensor
#   pipe     {nbytes,shape}   read nbytes off stdin, then mean/std
#   raise                     raise an exception (crash-containment probe)
#   quit
import json
import struct
import sys

import numpy as np

try:
    from multiprocessing import shared_memory
except Exception:                                    # pragma: no cover
    shared_memory = None

_shm = None
_view = None


def tile_std_median(a, tile=16):
    """Same shape of work as plugins/analyzer_noise.c: std of every 16x16 tile,
    then the median of those. Not bit-identical - it is a cost model."""
    h, w = a.shape[0] // tile * tile, a.shape[1] // tile * tile
    t = a[:h, :w].reshape(h // tile, tile, w // tile, tile)
    t = t.transpose(0, 2, 1, 3).reshape(-1, tile * tile)
    return float(np.median(t.std(axis=1)))


def main():
    global _shm, _view
    out = sys.stdout
    inb = sys.stdin.buffer
    while True:
        line = inb.readline()
        if not line:
            return 0
        try:
            req = json.loads(line)
        except Exception as e:
            out.write(json.dumps({"err": "bad request: %s" % e}) + "\n")
            out.flush()
            continue
        cmd = req.get("cmd", "")
        try:
            if cmd == "quit":
                return 0
            elif cmd == "ping":
                rep = {"ok": 1}
            elif cmd == "attach":
                _shm = shared_memory.SharedMemory(name=req["name"])
                _view = np.ndarray(tuple(req["shape"]), dtype=np.float32,
                                   buffer=_shm.buf)
                rep = {"ok": 1, "nbytes": int(_view.nbytes)}
            elif cmd == "shm_stats":
                rep = {"mean": float(_view.mean()), "std": float(_view.std())}
            elif cmd == "shm_noise":
                rep = {"noise": tile_std_median(_view)}
            elif cmd == "shm_touch":
                rep = {"s": float(_view[0].sum())}
            elif cmd == "shm_roi":
                x, y, w, h = req["roi"]
                a = _view[y:y + h, x:x + w]
                kind = req.get("kind", "stats")
                if kind == "stats":
                    rep = {"mean": float(a.mean()), "std": float(a.std())}
                elif kind == "statspct":
                    p = np.percentile(a, [1, 50, 99])
                    rep = {"mean": float(a.mean()), "std": float(a.std()),
                           "p": [float(v) for v in p]}
                elif kind == "noise":
                    rep = {"noise": tile_std_median(a)}
                else:
                    rep = {"err": "unknown kind " + kind}
            elif cmd == "torch_init":
                import torch
                _t = torch.from_numpy(_view)        # zero-copy over the shm view
                globals()["_torch"] = torch
                globals()["_tensor"] = _t
                rep = {"ok": 1, "torch": torch.__version__,
                       "cuda": bool(torch.cuda.is_available())}
            elif cmd == "torch_roi":
                x, y, w, h = req["roi"]
                t = globals()["_tensor"][y:y + h, x:x + w]
                rep = {"mean": float(t.mean()), "std": float(t.std())}
            elif cmd == "pipe":
                n = int(req["nbytes"])
                buf = bytearray(n)
                mv = memoryview(buf)
                got = 0
                while got < n:
                    k = inb.readinto(mv[got:])
                    if not k:
                        raise EOFError("short read: %d of %d" % (got, n))
                    got += k
                a = np.frombuffer(buf, dtype=np.float32).reshape(tuple(req["shape"]))
                rep = {"mean": float(a.mean()), "std": float(a.std())}
            elif cmd == "raise":
                raise ValueError("deliberate failure from the plugin")
            else:
                rep = {"err": "unknown cmd " + cmd}
        except Exception as e:
            rep = {"err": "%s: %s" % (type(e).__name__, e)}
        out.write(json.dumps(rep) + "\n")
        out.flush()


if __name__ == "__main__":
    sys.exit(main())
