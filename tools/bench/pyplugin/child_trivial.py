#!/usr/bin/env python3
# Spawn-floor target for the Python-plugin transport benchmark.
#
#   STUDY HARNESS ONLY - docs/python-plugins.md. Not part of the viewer build.
#
# Does the least a plugin process could possibly do: start, print one line,
# exit. Whatever this costs is the floor for ANY per-call out-of-process
# design, before a single pixel has moved.
import sys

what = sys.argv[1] if len(sys.argv) > 1 else "bare"
if what == "numpy":
    import numpy            # noqa: F401
elif what == "torch":
    import torch            # noqa: F401
elif what == "harness":
    # what a real adapter/plugin process actually pays: numpy + json + the
    # kind of stdlib a harness uses
    import json             # noqa: F401
    import numpy            # noqa: F401
    from multiprocessing import shared_memory   # noqa: F401
sys.stdout.write("ok\n")
