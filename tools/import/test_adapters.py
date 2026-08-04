"""Tests for the input-adapter library (viewer_import.py) and harness (run_adapter.py).

    python tools/import/test_adapters.py            # one line per case, exit 1 on failure
    python tools/import/test_adapters.py -v         # + tracebacks for failures
    python tools/import/test_adapters.py --messages # + every validation message produced

No test framework: plain asserts and a main().  numpy is required (the harness
needs it anyway); nothing else.  Every case is written against docs/input-adapters.md
section 4 and is annotated with the paragraph it comes from.
"""

import ast
import os
import shutil
import subprocess
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

RUNNER = os.path.join(HERE, "run_adapter.py")
ADAPTERS = os.path.join(HERE, "adapters")

try:
    import numpy as np
except ImportError:                                     # pragma: no cover
    np = None

try:
    import viewer_import as vi
    VI_ERR = None
except Exception as _e:                                 # library not written yet
    vi = None
    VI_ERR = _e

CASES = []
MESSAGES = []       # every error message we provoked, for the report
TEMPDIRS = []


def case(fn):
    CASES.append(fn)
    return fn


# ---------------------------------------------------------------- helpers

def need_lib():
    if vi is None:
        raise AssertionError("cannot import viewer_import: %r" % (VI_ERR,))
    return vi


def need_numpy():
    if np is None:
        raise AssertionError("numpy is not installed")


def need_runner():
    if not os.path.exists(RUNNER):
        raise AssertionError("run_adapter.py does not exist yet: %s" % RUNNER)


def fails(fn, want, exc=Exception):
    """Run fn, require it to raise with `want` in the message.

    Prefix `want` with '=' to require the message to match exactly.
    """
    try:
        fn()
    except exc as e:
        got = str(e)
        MESSAGES.append("%s: %s" % (type(e).__name__, got))
        if want.startswith("="):
            assert got == want[1:], "message was\n  %r\nwanted exactly\n  %r" % (got, want[1:])
        else:
            assert want in got, "message was\n  %r\nwanted to contain\n  %r" % (got, want)
        return got
    raise AssertionError("expected an error containing %r, nothing was raised" % want)


def tempdir(tag="viewer_adapter_test_"):
    d = tempfile.mkdtemp(prefix=tag)
    TEMPDIRS.append(d)
    return d


def write(path, text):
    with open(path, "w") as f:
        f.write(text)
    return path


def run_adapter(source, func, path, adapter_name="adapter_under_test.py", expect_ok=True):
    """Write `source` as an adapter, run the harness on `path`, return (proc, npz_path)."""
    need_runner()
    d = tempdir()
    apath = write(os.path.join(d, adapter_name), source)
    out = os.path.join(d, "out.npz")
    proc = subprocess.run(
        [sys.executable, RUNNER, apath + ":" + func, path, "-o", out],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if expect_ok:
        assert proc.returncode == 0, "harness failed (%d)\n--- stderr ---\n%s" % (
            proc.returncode, proc.stderr)
        assert os.path.exists(out), "harness exited 0 but wrote no npz\n%s" % proc.stderr
    return proc, out


def members(npz_path):
    with np.load(npz_path, allow_pickle=False) as z:
        return dict((k, z[k]) for k in z.files)


def scalar(m, key):
    return m[key].item()


# ---------------------------------------------------------------- 4.4 shapes

@case
def frame_shapes():
    """4.4: Frame takes (H,W) or (H,W,C); C is unconstrained (a frame has no frame axis)."""
    need_lib(); need_numpy()
    vi.Frame(np.zeros((8, 9)))
    vi.Frame(np.zeros((8, 9, 3)))
    vi.Frame(np.zeros((8, 9, 5)))                       # C is not restricted to 3|4
    assert vi.Frame(np.zeros((8, 9, 3))).channel_count == 3
    assert vi.Frame(np.zeros((8, 9))).channel_count == 1
    fails(lambda: vi.Frame(np.zeros((8,))), "Frame: shape (8,)")
    fails(lambda: vi.Frame(np.zeros((2, 3, 4, 5))), "Frame: shape (2, 3, 4, 5)")


@case
def stack_shapes():
    """4.4/3.1: Stack takes (F,H,W) or (F,H,W,C); (3,H,W) is three frames, not colour."""
    need_lib(); need_numpy()
    assert vi.Stack(np.zeros((24, 8, 9))).frame_count == 24
    assert vi.Stack(np.zeros((24, 8, 9, 3))).frame_count == 24
    assert vi.Stack(np.zeros((3, 8, 9))).frame_count == 3
    fails(lambda: vi.Stack(np.zeros((8, 9))), "Stack: shape (8, 9)")
    fails(lambda: vi.Stack(np.zeros((2, 3, 4, 5, 6))), "Stack: shape (2, 3, 4, 5, 6)")


@case
def series_forms():
    """4.4/4.7: a series is a sequence of stacks, or (S,H,W), (S,R,H,W), (S,R,H,W,C)."""
    need_lib(); need_numpy()
    exp = vi.Values([1, 2, 5, 10], "exposure", "ms")

    s = vi.Series([vi.Stack(np.zeros((16, 8, 9))) for _ in range(4)], conditions=exp)
    assert s.stack_count == 4

    s = vi.Series(np.zeros((4, 8, 9)), conditions=exp)          # 4 conditions x 1 frame
    assert s.stack_count == 4 and s.frames_per_stack == 1

    s = vi.Series(np.zeros((4, 16, 8, 9)), conditions=exp)      # 4 conditions x 16 repeats
    assert s.stack_count == 4 and s.frames_per_stack == 16

    s = vi.Series(np.zeros((4, 16, 8, 9, 3)), conditions=exp)   # ... x 3 ch
    assert s.stack_count == 4 and s.frames_per_stack == 16

    s = vi.Series([vi.Frame(np.zeros((8, 9))) for _ in range(4)], conditions=exp)
    assert s.stack_count == 4

    fails(lambda: vi.Series(np.zeros((8, 9)), conditions=exp), "Series: shape (8, 9)")
    fails(lambda: vi.Series(np.zeros((4, 2, 2, 8, 9, 3)), conditions=exp),
          "Series: shape (4, 2, 2, 8, 9, 3)")
    fails(lambda: vi.Series([np.zeros((8, 9))], conditions=vi.Values([1], "exposure", "ms")),
          "Series: member 0")
    fails(lambda: vi.Series(np.zeros((4, 8, 9))), "conditions", TypeError)   # required


@case
def sigma_t_distinction():
    """4.5/4.6: (8,16,H,W) as a Series is 8 stacks of 16 and its conditions sit on the
    series, not on the stacks.  The same array as a Stack would be a 8-frame stack."""
    need_lib(); need_numpy()
    arr = np.zeros((8, 16, 4, 4))
    exp = vi.Values(list(range(8)), "exposure", "ms")
    s = vi.Series(arr, conditions=exp)
    assert s.stack_count == 8, s.stack_count
    assert s.frames_per_stack == 16, s.frames_per_stack
    assert s.conditions is exp
    assert not hasattr(s, "timestamps"), "a series has no timestamps field (4.3)"
    assert not hasattr(vi.Stack(np.zeros((4, 4, 4))), "conditions"), \
        "a stack has no conditions field (4.3)"


# ---------------------------------------------------------------- 4.3 fields

@case
def series_conditions_length():
    """4.10: the message quoted in the spec, word for word."""
    need_lib(); need_numpy()
    stacks = [vi.Stack(np.zeros((2, 4, 4))) for _ in range(16)]
    fails(lambda: vi.Series(stacks, conditions=vi.Values(list(range(8)), "exposure", "ms")),
          "=Series: conditions has 8 value(s) but there are 16 stack(s)")
    fails(lambda: vi.Series(np.zeros((16, 2, 4, 4)),
                            conditions=vi.Values(list(range(8)), "exposure", "ms")),
          "=Series: conditions has 8 value(s) but there are 16 stack(s)")


@case
def stack_timestamps():
    """4.3.2: timestamps length == frame count; name defaults to "time"; unit is required
    but an empty unit is not an error -- the axis is simply not applied."""
    need_lib(); need_numpy()
    st = vi.Stack(np.zeros((24, 4, 4)), timestamps=vi.Values(list(range(24)), unit="s"))
    assert st.timestamps.name == "time", st.timestamps.name
    assert st.timestamps.unit == "s"
    st = vi.Stack(np.zeros((24, 4, 4)),
                  timestamps=vi.Values(list(range(24)), "elapsed", "s"))
    assert st.timestamps.name == "elapsed"
    fails(lambda: vi.Stack(np.zeros((24, 4, 4)),
                           timestamps=vi.Values(list(range(8)), unit="s")),
          "=Stack: timestamps has 8 value(s) but there are 24 frame(s)")
    fails(lambda: vi.Stack(np.zeros((24, 4, 4)), timestamps=[0, 1, 2]),
          "Stack: timestamps must be Values")
    # empty unit constructs fine (it is dropped later, like a pasted column)
    st = vi.Stack(np.zeros((3, 4, 4)), timestamps=vi.Values([0, 1, 2]))
    assert st.timestamps.applied is False
    assert vi.Values([0, 1, 2], unit="s").applied is True


@case
def stack_empty_timestamps():
    """4.3.2: a 0-length stack accepts empty timestamps without length check failures."""
    need_lib(); need_numpy()
    st = vi.Stack(np.zeros((0, 4, 4)), timestamps=vi.Values([], unit="s"))
    assert len(st.timestamps) == 0
    assert st.shape == (0, 4, 4)
    fails(lambda: vi.Stack(np.zeros((1, 4, 4)), timestamps=vi.Values([], unit="s")),
          "=Stack: timestamps has 0 value(s) but there are 1 frame(s)")


@case
def values_rules():
    """4.3.2: Values(values, name="", unit=""); conditions must be named; values are 1-D
    and numeric (they get plotted)."""
    need_lib(); need_numpy()
    v = vi.Values([1, 2, 5, 10], "exposure", "ms")
    assert len(v) == 4 and v.name == "exposure" and v.unit == "ms"
    assert len(vi.Values(np.arange(7.0), "exposure", "ms")) == 7
    fails(lambda: vi.Series(np.zeros((4, 4, 4)),
                            conditions=vi.Values([1, 2, 3, 4], unit="ms")),
          "Series: conditions has no name")
    fails(lambda: vi.Values(np.zeros((2, 4)), "exposure", "ms"), "Values: shape (2, 4)")
    fails(lambda: vi.Values(np.array(["a", "b"]), "exposure", "ms"), "Values: dtype")
    fails(lambda: vi.Values(np.array([1 + 2j]), "exposure", "ms"), "Values: dtype")
    fails(lambda: vi.Values(["a", "b"], "exposure", "ms"), "Values: value 0")
    fails(lambda: vi.Values(3.0, "exposure", "ms"), "Values: values has no .shape")


@case
def range_shapes():
    """4.3.3: range is (lo,hi), or shaped like the layer it belongs to."""
    need_lib(); need_numpy()
    vi.Frame(np.zeros((4, 4)), range=(0, 1023))
    vi.Stack(np.zeros((6, 4, 4)), range=(0, 1023))
    vi.Stack(np.zeros((6, 4, 4)), range=np.zeros((6, 2)))
    exp = vi.Values(list(range(3)), "exposure", "ms")
    vi.Series(np.zeros((3, 5, 4, 4)), conditions=exp, range=(0, 1023))
    vi.Series(np.zeros((3, 5, 4, 4)), conditions=exp, range=np.zeros((3, 2)))
    vi.Series(np.zeros((3, 5, 4, 4)), conditions=exp, range=np.zeros((3, 5, 2)))
    fails(lambda: vi.Frame(np.zeros((4, 4)), range=np.zeros((4, 2))),
          "Frame: range has shape (4, 2)")
    fails(lambda: vi.Stack(np.zeros((6, 4, 4)), range=np.zeros((3, 2))),
          "Stack: range has shape (3, 2) but the stack has 6 frame(s)")
    fails(lambda: vi.Series(np.zeros((3, 5, 4, 4)), conditions=exp, range=np.zeros((4, 2))),
          "Series: range has shape (4, 2) but there are 3 stack(s)")
    fails(lambda: vi.Stack(np.zeros((6, 4, 4)), range=(0, 1, 2)), "Stack: range has shape (3,)")


@case
def cfa_known():
    """4.3: the four Bayer spellings and their quad: forms, nothing else."""
    need_lib(); need_numpy()
    for pat in ("RGGB", "BGGR", "GRBG", "GBRG"):
        vi.Frame(np.zeros((4, 4)), cfa=pat)
        vi.Stack(np.zeros((2, 4, 4)), cfa="quad:" + pat)
    fails(lambda: vi.Frame(np.zeros((4, 4)), cfa="RGGX"),
          '=Frame: cfa "RGGX" is not a known pattern -- one of RGGB, BGGR, GRBG, GBRG '
          '(or quad:RGGB and friends)')
    fails(lambda: vi.Frame(np.zeros((4, 4)), cfa="rggb"), 'Frame: cfa "rggb"')
    fails(lambda: vi.Stack(np.zeros((2, 4, 4)), cfa="quad:RGGX"), 'Stack: cfa "quad:RGGX"')


@case
def dtype_refused():
    """4.8: non-numeric (string / object / complex) is refused, with a reason."""
    need_lib(); need_numpy()
    for dt in ("uint8", "uint16", "int32", "float32", "float64", "bool"):
        vi.Stack(np.zeros((2, 4, 4), dtype=dt))
    fails(lambda: vi.Stack(np.zeros((2, 4, 4), dtype="complex64")), "Stack: dtype complex64")
    fails(lambda: vi.Frame(np.array([["a", "b"], ["c", "d"]])), "Frame: dtype")
    fails(lambda: vi.Frame(np.array([[object(), object()]], dtype=object)), "Frame: dtype object")


@case
def dtype_name_edge_cases():
    """Test _dtype_name with weird objects."""
    vi = need_lib()

    class NoDtype(object):
        pass

    class NamedDtype(object):
        @property
        def dtype(self):
            class DType(object):
                name = "my_named_dtype"
            return DType()

    class WeirdDtype(object):
        @property
        def dtype(self):
            class DType(object):
                def __str__(self): return "my_weird_dtype"
            return DType()

    assert vi._dtype_name(NoDtype()) is None
    assert vi._dtype_name(NamedDtype()) == "my_named_dtype"
    assert vi._dtype_name(WeirdDtype()) == "my_weird_dtype"


@case
def misspelled_field():
    """4.3: a misspelled field is a TypeError on the spot -- the point of naming a type."""
    need_lib(); need_numpy()
    fails(lambda: vi.Stack(np.zeros((2, 4, 4)), timestamp=vi.Values([0, 1], unit="s")),
          "timestamp", TypeError)
    fails(lambda: vi.Stack(np.zeros((2, 4, 4)), frames=2), "frames", TypeError)
    fails(lambda: vi.Frame(np.zeros((4, 4)), notes="hi"), "notes", TypeError)
    fails(lambda: vi.Series(np.zeros((2, 4, 4)),
                            conditions=vi.Values([0, 1], "exposure", "ms"), timestamps=None),
          "timestamps", TypeError)


@case
def frozen_and_meta():
    """4.10: frozen -- nobody rewrites what the adapter returned.  meta is a dict of facts."""
    need_lib(); need_numpy()
    st = vi.Stack(np.zeros((2, 4, 4)), meta={"sensor": "IMX999"})
    fails(lambda: setattr(st, "name", "x"), "")
    d = {"sensor": "IMX999"}
    st = vi.Stack(np.zeros((2, 4, 4)), meta=d)
    d["sensor"] = "changed"
    assert st.meta["sensor"] == "IMX999", "meta must be copied at construction"
    assert vi.Stack(np.zeros((2, 4, 4))).meta == {}
    fails(lambda: vi.Stack(np.zeros((2, 4, 4)), meta=[("a", 1)]), "Stack: meta must be a dict")
    fails(lambda: vi.Stack(np.zeros((2, 4, 4)), meta={3: "x"}), "Stack: meta key")


@case
def batch_rules():
    """4.3/4.7: a batch holds the layers above it and claims no structure of its own."""
    need_lib(); need_numpy()
    b = vi.Batch([vi.Stack(np.zeros((2, 4, 4)), name="dark"),
                  vi.Stack(np.zeros((2, 4, 4)), name="flat")])
    assert len(b.members) == 2
    assert b.members[0].name == "dark"
    vi.Batch([vi.Series(np.zeros((2, 4, 4)),
                        conditions=vi.Values([1, 2], "exposure", "ms"))], name="sweep")
    fails(lambda: vi.Batch([np.zeros((2, 4, 4))]), "Batch: member 0")
    fails(lambda: vi.Batch([vi.Batch([vi.Frame(np.zeros((4, 4)))])]), "Batch: member 0")
    fails(lambda: vi.Batch([]), "Batch: no members")
    fails(lambda: vi.Batch([vi.Frame(np.zeros((4, 4)))], cfa="RGGB"), "cfa", TypeError)


@case
def layout_field():
    """4.4: layout survives only as the escape hatch for transposed arrays."""
    need_lib(); need_numpy()
    vi.Frame(np.zeros((3, 8, 9)), layout="CHW")
    vi.Stack(np.zeros((4, 3, 8, 9)), layout="FCHW")
    fails(lambda: vi.Frame(np.zeros((3, 8, 9)), layout="HWC"), 'Frame: layout "HWC"')
    fails(lambda: vi.Frame(np.zeros((8, 9)), layout="CHW"), 'Frame: layout "CHW" needs a 3-D array')
    fails(lambda: vi.Stack(np.zeros((3, 8, 9)), layout="FCHW"), 'Stack: layout "FCHW" needs a 4-D array')


@case
def shape_of_edge_cases():
    """Missing shape edge cases in _shape_of.

    The issue rationale states: 'pass an irregular nested list to `_shape_of` and
    asserting it returns `None`'. This refers to the `TypeError` block where
    an object's `shape` attribute is an irregular nested list (e.g., `[1, [2]]`),
    causing `tuple(int(d) for d in shape)` to raise a TypeError. The list-parsing
    path of `_shape_of` always returns a tuple and never returns `None`.
    """
    need_lib()

    class BadShape:
        shape = [1, [2]]

    assert vi._shape_of(BadShape()) is None

    class CallableShape:
        def shape(self):
            return (1, 2)

    assert vi._shape_of(CallableShape()) == (1, 2)


@case
def plain_lists_work_without_numpy():
    """4.10: viewer_import must be copyable and work with nothing installed."""
    need_lib()
    px = [[[0, 1], [2, 3]], [[4, 5], [6, 7]]]           # (2,2,2) -> a 2-frame stack
    st = vi.Stack(px, timestamps=vi.Values([0.0, 0.5], unit="s"))
    assert st.frame_count == 2
    assert st.shape == (2, 2, 2)
    src = open(os.path.join(HERE, "viewer_import.py")).read()
    imports = set()
    for node in ast.walk(ast.parse(src)):           # real imports, not doc examples
        if isinstance(node, ast.Import):
            imports.update(a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom):
            imports.add((node.module or "").split(".")[0])
    assert "numpy" not in imports, "viewer_import must not import numpy (4.10): %s" % imports
    assert "torch" not in imports, "viewer_import must not import torch (4.10): %s" % imports
    assert imports <= {"dataclasses", "typing"}, "stdlib only (4.10): %s" % sorted(imports)


# ---------------------------------------------------------------- harness

BARE = """
import numpy as np
def load(path):
    return np.load(path)["data"]
"""

TYPED = """
import numpy as np
from viewer_import import Stack
def load(path):
    return Stack(np.load(path)["data"])
"""


def sample_npz(shape=(6, 4, 5), dtype="uint16"):
    d = tempdir()
    p = os.path.join(d, "sample.npz")
    a = np.arange(int(np.prod(shape)), dtype=dtype).reshape(shape)
    np.savez(p, data=a)
    return p, a


@case
def harness_bare_array_is_stack_arr():
    """4.2/4.6: a bare array is read exactly as native reads it.  arr and Stack(arr) must
    land in the same npz -- except for the note 4.6 requires on the bare form."""
    need_numpy(); need_runner()
    path, a = sample_npz()
    _, out_bare = run_adapter(BARE, "load", path)
    _, out_typed = run_adapter(TYPED, "load", path)
    mb, mt = members(out_bare), members(out_typed)
    assert set(mb) == set(mt), "member names differ: %s vs %s" % (sorted(mb), sorted(mt))
    differ = [k for k in mb if not np.array_equal(mb[k], mt[k])]
    assert differ == ["__note_0"], "bare and Stack(arr) differ in %s (expected only __note_0)" % differ
    assert scalar(mb, "__layer_0") == "stack"
    assert scalar(mt, "__layer_0") == "stack"
    assert np.array_equal(mb["__pixels_0"], a)
    assert scalar(mb, "__note_0") == "repeats (returned as a bare array)"
    assert scalar(mt, "__note_0") == ""


@case
def harness_bare_native_shapes():
    """3.1: (H,W) and (H,W,3|4) are one frame; (F,H,W) and (F,H,W,C) are a stack."""
    need_numpy(); need_runner()
    for shape, layer in (((4, 5), "frame"), ((4, 5, 3), "frame"), ((4, 5, 4), "frame"),
                         ((6, 4, 5), "stack"), ((6, 4, 5, 2), "stack"),
                         ((6, 4, 5, 7), "stack")):
        path, a = sample_npz(shape)
        _, out = run_adapter(BARE, "load", path)
        m = members(out)
        assert scalar(m, "__layer_0") == layer, "%s read as %s, wanted %s" % (
            shape, scalar(m, "__layer_0"), layer)
        assert np.array_equal(m["__pixels_0"], a)


@case
def harness_refuses_non_native_bare_shape():
    """3.2: say the shape and what native reads.  Never just 'could not open'."""
    need_numpy(); need_runner()
    path, _ = sample_npz((4, 8, 8, 8, 2))
    proc, _ = run_adapter(BARE, "load", path, expect_ok=False)
    assert proc.returncode != 0
    err = proc.stderr
    MESSAGES.append("harness: " + [l for l in err.splitlines() if "not a native form" in l][0]
                    if "not a native form" in err else "harness: " + err.strip()[-200:])
    assert "shape (4, 8, 8, 8, 2) is not a native form" in err, err
    assert "(H,W) / (H,W,3|4) / (F,H,W) / (F,H,W,C)" in err, err


@case
def harness_return_forms():
    """4.2: each of the four types, plus the bare array, comes back as its own layer."""
    need_numpy(); need_runner()
    path, _ = sample_npz((6, 4, 5))
    src = """
import numpy as np
from viewer_import import Frame, Stack, Series, Batch, Values
def frame(path):  return Frame(np.load(path)["data"][0])
def stack(path):  return Stack(np.load(path)["data"])
def series(path): return Series(np.load(path)["data"][:2],
                                conditions=Values([1.0, 2.0], "exposure", "ms"))
def batch(path):
    a = np.load(path)["data"]
    return Batch([Stack(a, name="dark"), Frame(a[0], name="flat")])
"""
    _, out = run_adapter(src, "frame", path)
    assert scalar(members(out), "__layer_0") == "frame"

    _, out = run_adapter(src, "stack", path)
    assert scalar(members(out), "__layer_0") == "stack"

    _, out = run_adapter(src, "series", path)
    m = members(out)
    assert scalar(m, "__layer_0") == "series"
    assert int(scalar(m, "__n")) == 3
    assert "__pixels_0" not in m, "a series node carries no pixels of its own"

    _, out = run_adapter(src, "batch", path)
    m = members(out)
    assert scalar(m, "__layer_0") == "batch"
    assert scalar(m, "__layer_1") == "stack" and scalar(m, "__name_1") == "dark"
    assert scalar(m, "__layer_2") == "frame" and scalar(m, "__name_2") == "flat"
    assert int(scalar(m, "__parent_1")) == 0 and int(scalar(m, "__parent_2")) == 0


@case
def harness_series_member_layout():
    """4.11 + 4.5: two stacks under one series; the conditions sit on the series node."""
    need_numpy(); need_runner()
    d = tempdir()
    path = os.path.join(d, "sweep.npz")
    np.savez(path, data=np.arange(2 * 16 * 4 * 5, dtype="uint16").reshape(2, 16, 4, 5),
             exp=np.array([10.0, 20.0]))
    src = """
import numpy as np
from viewer_import import Series, Values
def load(path):
    z = np.load(path)
    return Series(z["data"], conditions=Values(z["exp"], "exposure", "ms"),
                  name="sweep", note="two conditions")
"""
    _, out = run_adapter(src, "load", path)
    m = members(out)
    assert int(scalar(m, "__n")) == 3
    assert scalar(m, "__layer_0") == "series" and int(scalar(m, "__parent_0")) == -1
    assert scalar(m, "__name_0") == "sweep" and scalar(m, "__note_0") == "two conditions"
    assert np.array_equal(m["__conditions_values_0"], np.array([10.0, 20.0]))
    assert scalar(m, "__conditions_name_0") == "exposure"
    assert scalar(m, "__conditions_unit_0") == "ms"
    for i in (1, 2):
        assert scalar(m, "__layer_%d" % i) == "stack"
        assert int(scalar(m, "__parent_%d" % i)) == 0
        assert m["__pixels_%d" % i].shape == (16, 4, 5)
        assert "__conditions_values_%d" % i not in m, "conditions belong to the series (4.5)"
        assert "__timestamps_values_%d" % i not in m
    assert np.array_equal(m["__pixels_1"], np.arange(16 * 4 * 5, dtype="uint16").reshape(16, 4, 5))


@case
def harness_folder_adapter():
    """4.1: path may be a folder -- one condition per file is the natural series."""
    need_numpy(); need_runner()
    d = tempdir()
    folder = os.path.join(d, "sweep")
    os.makedirs(folder)
    for i, ms in enumerate((1.0, 2.0, 5.0)):
        np.save(os.path.join(folder, "exp_%02d.npy" % i),
                np.full((4, 4, 5), ms, dtype="float32"))
    src = """
import os
import numpy as np
from viewer_import import Series, Stack, Values
def load(path):
    assert os.path.isdir(path), "this reader takes a folder"
    names = sorted(f for f in os.listdir(path) if f.endswith(".npy"))
    stacks = [Stack(np.load(os.path.join(path, n)), name=n) for n in names]
    ms = [1.0, 2.0, 5.0]
    return Series(stacks, conditions=Values(ms, "exposure", "ms"), name=os.path.basename(path))
"""
    proc, out = run_adapter(src, "load", folder)
    m = members(out)
    assert int(scalar(m, "__n")) == 4
    assert scalar(m, "__layer_0") == "series" and scalar(m, "__name_0") == "sweep"
    assert scalar(m, "__name_1") == "exp_00.npy"
    assert m["__pixels_3"].shape == (4, 4, 5)
    assert float(m["__pixels_3"].flat[0]) == 5.0


@case
def harness_failure_message_survives():
    """4.9: the adapter's own message reaches the user unchanged."""
    need_numpy(); need_runner()
    path, _ = sample_npz()
    src = """
def load(path):
    raise ValueError("header says 12 planes but the file holds 9")
"""
    proc, _ = run_adapter(src, "load", path, expect_ok=False)
    assert proc.returncode != 0, "a failing adapter must not exit 0"
    assert "header says 12 planes but the file holds 9" in proc.stderr, proc.stderr
    assert "could not open" not in proc.stderr.lower()
    # a bare raise with no message must still say something useful
    src2 = """
def load(path):
    raise RuntimeError()
"""
    proc, _ = run_adapter(src2, "load", path, expect_ok=False)
    assert proc.returncode != 0
    assert "RuntimeError" in proc.stderr, proc.stderr
    assert "no message" in proc.stderr, proc.stderr


@case
def harness_summary_counts_both_directions():
    """4.11: one line on stderr, saying what was read and what was skipped."""
    need_numpy(); need_runner()
    d = tempdir()
    path = os.path.join(d, "sweep.npz")
    np.savez(path, data=np.zeros((2, 16, 4, 5), dtype="uint16"), exp=np.array([10.0, 20.0]))
    src = """
import numpy as np
from viewer_import import Series, Stack, Values
def load(path):
    z = np.load(path)
    stacks = [Stack(z["data"][i], timestamps=Values(list(range(16)))) for i in range(2)]
    return Series(stacks, conditions=Values(z["exp"], "exposure", "ms"))
"""
    proc, out = run_adapter(src, "load", path)
    lines = [l for l in proc.stderr.splitlines() if l.strip()]
    assert len(lines) == 1, "the summary must be one line, got:\n%s" % proc.stderr
    line = lines[0]
    MESSAGES.append("summary: " + line)
    assert "read" in line and "skipped" in line, line
    assert "1 series" in line and "2 stack" in line and "32 frame" in line, line
    assert "2" in line.split("skipped", 1)[1], "two unitless axes were dropped: %s" % line


@case
def harness_carries_unitless_values():
    """4.3.2: an axis with no unit is not applied -- but its values are NOT discarded.

    "not applied" and "not carried" are different things, and 4.3.2 is explicit
    about which one this is: the axis does not label anything until a unit
    exists, and the numbers travel anyway so the user can supply the unit in the
    UI instead of retyping measurements.  The empty unit is what says "not
    applied"; the summary still counts it as skipped.
    """
    need_numpy(); need_runner()
    path, _ = sample_npz((6, 4, 5))
    src = """
import numpy as np
from viewer_import import Stack, Values
def load(path):
    return Stack(np.load(path)["data"], timestamps=Values(list(range(6))))
"""
    proc, out = run_adapter(src, "load", path)
    m = members(out)
    assert np.array_equal(m["__timestamps_values_0"], np.arange(6.0)), \
        "4.3.2: the values must survive a missing unit"
    assert scalar(m, "__timestamps_unit_0") == "", \
        "an empty unit is what records that the axis is not applied"
    assert scalar(m, "__timestamps_name_0") == "time"
    assert "no unit" in scalar(m, "__note_0"), scalar(m, "__note_0")
    assert "skipped" in proc.stderr

    src2 = src.replace("Values(list(range(6)))", 'Values(list(range(6)), unit="s")')
    proc, out = run_adapter(src2, "load", path)
    m = members(out)
    assert np.array_equal(m["__timestamps_values_0"], np.arange(6.0))
    assert scalar(m, "__timestamps_name_0") == "time", "4.3.2: the default name"
    assert scalar(m, "__timestamps_unit_0") == "s"


@case
def harness_empty_timestamps():
    """4.3.2: the harness must serialize 0-length axes exactly as it does regular ones."""
    need_numpy(); need_runner()
    path, _ = sample_npz((0, 4, 5))
    src = """
import numpy as np
from viewer_import import Stack, Values
def load(path):
    return Stack(np.zeros((0, 4, 5)), timestamps=Values([], unit="s"))
"""
    proc, out = run_adapter(src, "load", path)
    m = members(out)
    assert m["__timestamps_values_0"].shape == (0,), \
        "__timestamps_values_0 shape was %s, wanted (0,)" % (m["__timestamps_values_0"].shape,)
    assert scalar(m, "__timestamps_unit_0") == "s"
    assert scalar(m, "__timestamps_name_0") == "time"


@case
def harness_torch_duck_typing():
    """4.10/4.11: no torch import anywhere; .detach()/.cpu()/.numpy() is duck typed, and
    bfloat16 becomes f32 with the conversion left in the note (4.8)."""
    need_numpy(); need_runner()
    path, _ = sample_npz((6, 4, 5))
    src = """
import numpy as np
from viewer_import import Stack

class FakeDtype(object):
    def __init__(self, s): self.s = s
    def __str__(self): return self.s
    def __repr__(self): return self.s

class FakeTensor(object):
    def __init__(self, a, dt): self._a = a; self.dtype = FakeDtype(dt)
    @property
    def shape(self): return self._a.shape
    def detach(self): return self
    def cpu(self): return self
    def float(self): return FakeTensor(self._a.astype("float32"), "torch.float32")
    def numpy(self): return self._a

def load(path):
    a = np.load(path)["data"].astype("float32")
    return Stack(FakeTensor(a, "torch.bfloat16"))
"""
    proc, out = run_adapter(src, "load", path)
    m = members(out)
    assert m["__pixels_0"].dtype == np.dtype("float32"), m["__pixels_0"].dtype
    assert m["__pixels_0"].shape == (6, 4, 5)
    assert "bfloat16" in scalar(m, "__note_0"), scalar(m, "__note_0")
    src_run = open(RUNNER).read()
    assert "import torch" not in src_run, "the harness must not import torch"


@case
def harness_normalises_endianness_and_contiguity():
    """4.11: big-endian and non-contiguous arrays are normalised, values untouched."""
    need_numpy(); need_runner()
    d = tempdir()
    path = os.path.join(d, "be.npy")
    a = np.arange(6 * 4 * 6, dtype=">u2").reshape(6, 4, 6)
    np.save(path, a)
    src = """
import numpy as np
from viewer_import import Stack
def load(path):
    return Stack(np.load(path)[:, :, ::2])       # big-endian and non-contiguous
"""
    _, out = run_adapter(src, "load", path)
    px = members(out)["__pixels_0"]
    assert px.flags["C_CONTIGUOUS"], "pixels must be contiguous"
    assert px.dtype.byteorder in ("=", "|"), "endianness must be native, got %r" % px.dtype.str
    assert px.shape == (6, 4, 3)
    assert np.array_equal(px, np.asarray(a[:, :, ::2], dtype="uint16"))


@case
def harness_range_and_meta():
    """4.3.1/4.3.3: meta rides along as facts; a per-frame range says so in the note."""
    need_numpy(); need_runner()
    path, _ = sample_npz((6, 4, 5))
    src = """
import numpy as np
from viewer_import import Stack
def load(path):
    a = np.load(path)["data"]
    r = np.tile(np.array([0.0, 1023.0]), (6, 1))
    return Stack(a, range=r, meta={"gain": {"value": 6.0, "unit": "dB"}, "sensor": "IMX999"})
"""
    _, out = run_adapter(src, "load", path)
    m = members(out)
    assert m["__range_0"].shape == (6, 2)
    assert "display range varies per frame" in scalar(m, "__note_0"), scalar(m, "__note_0")
    assert scalar(m, "__meta_sensor") == '"IMX999"'
    assert scalar(m, "__meta_gain") == '{"unit": "dB", "value": 6.0}'


@case
def meta_key_validation():
    """meta keys are validated against invalid characters."""
    import run_adapter
    for bad in ("/", "\\", ":", "\0"):
        fails(lambda: run_adapter.meta_key("", "hello" + bad + "world"),
              "cannot be written to an npz", run_adapter.AdapterError)


@case
def harness_bad_spec_and_missing_path():
    """4.9: refuse with a reason, never with a shrug."""
    need_numpy(); need_runner()
    path, _ = sample_npz()
    d = tempdir()
    apath = write(os.path.join(d, "a.py"), BARE)
    for args, want in (
            ([apath, path], "<file.py>:<function>"),
            ([apath + ":nosuch", path], "nosuch"),
            ([apath + ":load", os.path.join(d, "nope.npz")], "no such file or directory")):
        proc = subprocess.run([sys.executable, RUNNER] + args + ["-o", os.path.join(d, "o.npz")],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)
        assert proc.returncode != 0, args
        MESSAGES.append("harness: " + proc.stderr.strip().splitlines()[-1])
        assert want.lower() in proc.stderr.lower(), (args, proc.stderr)


@case
def harness_validates_lying_adapters():
    """4.11/6: the harness checks what it can -- lengths, shapes, dtype -- every time."""
    need_numpy(); need_runner()
    path, _ = sample_npz((6, 4, 5))
    src = """
def load(path):
    return "not an array"
"""
    proc, _ = run_adapter(src, "load", path, expect_ok=False)
    assert proc.returncode != 0
    MESSAGES.append("harness: " + proc.stderr.strip().splitlines()[-1])
    assert "str" in proc.stderr and "Frame" in proc.stderr, proc.stderr

    src = """
def load(path):
    return None
"""
    proc, _ = run_adapter(src, "load", path, expect_ok=False)
    assert proc.returncode != 0
    assert "returned nothing" in proc.stderr.lower() or "NoneType" in proc.stderr, proc.stderr


@case
def shipped_adapters_run():
    """The adapters under tools/import/adapters are documentation that runs."""
    need_numpy(); need_runner()
    assert os.path.isdir(ADAPTERS), "no adapters/ directory"
    for name in ("template.py", "npz_keys.py", "color_hwc.py"):
        assert os.path.exists(os.path.join(ADAPTERS, name)), name

    d = tempdir()
    p = os.path.join(d, "sweep.npz")
    np.savez(p, data=np.zeros((8, 16, 4, 5), dtype="uint16"),
             exposure_ms=np.arange(8.0) + 1.0)
    out = os.path.join(d, "a.npz")
    proc = subprocess.run([sys.executable, RUNNER,
                           os.path.join(ADAPTERS, "npz_keys.py") + ":load", p, "-o", out],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    assert proc.returncode == 0, proc.stderr
    m = members(out)
    assert scalar(m, "__layer_0") == "series"
    assert int(scalar(m, "__n")) == 9
    assert m["__pixels_1"].shape == (16, 4, 5)
    assert scalar(m, "__conditions_unit_0") == "ms"

    q = os.path.join(d, "color.npy")
    np.save(q, np.zeros((4, 5, 3), dtype="uint8"))
    out2 = os.path.join(d, "b.npz")
    proc = subprocess.run([sys.executable, RUNNER,
                           os.path.join(ADAPTERS, "color_hwc.py") + ":load", q, "-o", out2],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    assert proc.returncode == 0, proc.stderr
    m = members(out2)
    assert scalar(m, "__layer_0") == "frame"
    assert m["__pixels_0"].shape == (4, 5, 3)


# ---------------------------------------------------------------- main

def main(argv):
    verbose = "-v" in argv
    show_messages = "--messages" in argv
    failed = []
    for fn in CASES:
        name = fn.__name__
        try:
            fn()
        except Exception as e:
            first = str(e).splitlines()[0] if str(e).strip() else type(e).__name__
            print("FAIL %-42s %s" % (name, first))
            if verbose:
                traceback.print_exc()
            failed.append(name)
        else:
            print("ok   %s" % name)

    for d in TEMPDIRS:
        shutil.rmtree(d, ignore_errors=True)

    if show_messages:
        print("")
        print("--- messages produced (%d) ---" % len(MESSAGES))
        for m in MESSAGES:
            print("  " + m)

    print("")
    print("%d case(s), %d failed%s" % (len(CASES), len(failed),
                                       (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
