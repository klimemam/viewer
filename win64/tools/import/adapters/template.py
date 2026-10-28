"""Template for a new reader.  Copy this file, change load(), point the viewer at it.

    viewer:  Inspector > reader > this file, function `load`
    by hand: python tools/import/run_adapter.py adapters/mine.py:load DATA -o out.npz

`path` is whatever the viewer was pointed at, and may be a folder (one condition per
file is a common way to store a sweep).  Read only -- never write to the source.
Raise with a reason if you cannot read it; the message is what the user sees.
"""

import numpy as np

from viewer_import import Frame, Stack, Series, Batch, Values

VERSION = 1             # bump when the meaning of what you return changes


def load(path):
    arr = np.load(path)

    # Say which layer this is.  The type IS the declaration, and it decides which
    # measurements are legal -- a Stack has a time axis (sigma_t means something),
    # a Series has a parameter axis (sigma_t across it would be a lie).
    #
    #   return arr                                  # read like a plain .npy
    #   return Frame(arr)                           # (H,W) or (H,W,C), one image
    #   return Stack(arr)                           # (F,H,W), repeats of one condition
    #   return Stack(arr, timestamps=Values(t, unit="s"))
    #   return Series(arr, conditions=Values(exposures, "exposure", "ms"))
    #   return Batch([Stack(dark, name="dark"), Stack(flat, name="flat")])
    #
    # Units are always stated, never inferred from a key or a file name; an axis
    # handed over without one is carried but not applied.
    return Stack(arr, note="", meta={})
