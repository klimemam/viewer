"""One colour image, however the axes were stored.

(H,W,3) is already what the viewer reads natively; the point of this reader is the
transposed case.  (3,H,W) on its own is three frames -- that is the rule, and it is
the right one, because three-frame stacks are far more common than colour images
stored channel-first.  When you know your file is channel-first, say so here by
putting the axes in order and leaving the fact in the note.

    python tools/import/run_adapter.py adapters/color_hwc.py:load shot.npy -o out.npz
"""

import numpy as np

from viewer_import import Frame

VERSION = 1


def load(path):
    arr = np.load(path)
    if arr.ndim != 3:
        raise ValueError("%s: shape %s is not one colour image -- expected (H,W,C) "
                         "or (C,H,W)" % (path, arr.shape))
    if arr.shape[-1] in (3, 4):
        return Frame(arr)
    if arr.shape[0] in (3, 4):
        return Frame(np.ascontiguousarray(np.transpose(arr, (1, 2, 0))),
                     note="CHW -> HWC")
    raise ValueError("%s: shape %s has no channel axis of 3 or 4 -- if these are "
                     "frames rather than channels, Stack(arr) is the reader you want"
                     % (path, arr.shape))
