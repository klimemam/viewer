"""An .npz holding the pixels under one key and the swept condition under another.

    data        (S, R, H, W)   S conditions, R repeats each
    exposure_ms (S,)           what was varied

That is a Series: the condition belongs to the series, and each condition's repeats
are a Stack of their own (so sigma_t stays meaningful inside a condition and is
never taken across the sweep).

    python tools/import/run_adapter.py adapters/npz_keys.py:load sweep.npz -o out.npz
"""

import numpy as np

from viewer_import import Series, Stack, Values

VERSION = 1

DATA_KEY = "data"               # edit these three lines for your files
COND_KEY = "exposure_ms"
COND_NAME, COND_UNIT = "exposure", "ms"     # the unit is stated, never read off the key


def load(path):
    z = np.load(path)
    for key in (DATA_KEY, COND_KEY):
        if key not in z.files:
            raise KeyError("%s has no member %r -- it holds: %s"
                           % (path, key, ", ".join(z.files)))
    data, cond = z[DATA_KEY], z[COND_KEY]
    if len(data) != len(cond):
        raise ValueError("%s: %d condition(s) but %d block(s) of pixels"
                         % (path, len(cond), len(data)))
    stacks = [Stack(data[i], name="%g %s" % (cond[i], COND_UNIT)) for i in range(len(cond))]
    return Series(stacks, conditions=Values(cond, COND_NAME, COND_UNIT))
