"""run_adapter.py -- run one input adapter and write what it returned as a .npz.

    python run_adapter.py adapters/acme.py:load  DATA_PATH  [-o out.npz]

DATA_PATH is handed to the adapter untouched and may be a file or a folder (4.1).
The adapter's own directory goes on sys.path first, then this directory, so that
`from viewer_import import ...` works and a viewer_import.py copied next to the
adapter wins over the one shipped here.

What this harness does, so that adapter authors do not have to (4.11):

  * torch -> numpy, duck typed (`.detach().cpu().numpy()`); torch is never imported
  * bfloat16 -> f32, and says so in the note (4.8)
  * contiguity and endianness normalised; values are not touched otherwise
  * re-checks shapes, dtypes and the lengths of conditions / timestamps
  * writes ONE .npz with the reserved member names below
  * prints one line to stderr saying what was read and what was skipped

The npz is a flat list of nodes in depth-first order; node 0 is what the adapter
returned.  `__parent_<i>` is what makes it a tree (-1 at the root), which is how a
series says which stacks are its members without the stacks having to carry the
condition themselves (4.5).

    __n                        number of nodes
    __layer_<i>                "frame" | "stack" | "series" | "batch"
    __parent_<i>               parent node index, -1 for the root
    __pixels_<i>               pixels (frame and stack nodes only)
    __name_<i> __note_<i> __layout_<i>       always written, possibly ""
    __cfa_<i> __range_<i>                    written when given
    __timestamps_values_<i> __timestamps_name_<i> __timestamps_unit_<i>
    __conditions_values_<i> __conditions_name_<i> __conditions_unit_<i>
    __meta_<k>                 root node meta, JSON encoded value
    __meta_<i>_<k>             meta of node i > 0, JSON encoded value

An axis whose unit is empty is NOT written: a quantity without a unit is not
applied, the same rule a pasted column obeys (4.3.2).  It is counted as skipped
in the summary and left in the note.

Exit status: 0 wrote the npz, 2 anything else.  A failure always says why (4.9).
"""

import argparse
import importlib.util
import json
import os
import sys
import traceback

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROG = "run_adapter"

LAYERS = ("frame", "stack", "series", "batch")
NATIVE_FORMS = "(H,W) / (H,W,3|4) / (F,H,W) / (F,H,W,C)"
BARE_ARRAY_NOTE = "repeats (returned as a bare array)"      # 4.6


class AdapterError(Exception):
    """Something we can explain in one line; no traceback is printed for these."""


# ------------------------------------------------------------------ duck typing

def layer_of(obj):
    """"frame" / "stack" / "series" / "batch", or None if this is not a layer.

    Read through the class attribute rather than isinstance: the adapter may have
    imported its own copy of viewer_import.py, which is an entirely different
    class object (4.10 says the file is meant to be copied around).
    """
    layer = getattr(type(obj), "LAYER", None)
    if isinstance(layer, str) and layer in LAYERS:
        return layer
    name = type(obj).__name__.lower()
    if name in LAYERS and (hasattr(obj, "pixels") or hasattr(obj, "stacks")
                           or hasattr(obj, "members")):
        return name
    return None


def values_of(where, what, obj):
    """(values, name, unit) out of anything Values-shaped."""
    for attr in ("values", "name", "unit"):
        if not hasattr(obj, attr):
            raise AdapterError('%s: %s must be Values(...) -- e.g. Values(t, unit="s"); '
                               'the unit is never guessed' % (where, what))
    return obj.values, str(obj.name or ""), str(obj.unit or "")


def to_numpy(where, what, obj, notes):
    """Anything array-shaped -> a contiguous, native-endian numpy array."""
    if hasattr(obj, "detach") and hasattr(obj, "numpy"):            # torch-shaped
        if "bfloat16" in str(getattr(obj, "dtype", "")).lower() and hasattr(obj, "float"):
            obj = obj.float()
            notes.append("bfloat16 -> f32")
        obj = obj.detach()
        if hasattr(obj, "cpu"):
            obj = obj.cpu()
        obj = obj.numpy()
    elif not isinstance(obj, np.ndarray) and hasattr(obj, "numpy"):
        obj = obj.numpy()

    try:
        arr = np.asarray(obj)
    except Exception as e:
        raise AdapterError("%s: %s cannot be read as an array (%s)" % (where, what, e))

    if arr.dtype.kind not in "biuf":
        raise AdapterError("%s: dtype %s is not numeric -- %s must be integer or "
                           "float (strings, objects and complex are refused)"
                           % (where, arr.dtype.name, what))
    if not arr.dtype.isnative:
        arr = arr.astype(arr.dtype.newbyteorder("="))
    return np.ascontiguousarray(arr)


# ------------------------------------------------------------------ node tree

class Node(object):
    def __init__(self, layer, parent):
        self.layer = layer
        self.parent = parent
        self.pixels = None
        self.name = ""
        self.note = ""
        self.layout = ""
        self.cfa = ""
        self.range = None
        self.timestamps = None          # (values, name, unit)
        self.conditions = None
        self.meta = {}
        self.notes = []                 # what the harness had to add

    def full_note(self):
        parts = [p for p in ([self.note] + self.notes) if p]
        return ", ".join(parts)


class Build(object):
    def __init__(self):
        self.nodes = []
        self.skipped = []               # one line per thing not applied

    # -- common fields off any layer object ---------------------------------
    def common(self, node, obj, where):
        node.name = str(getattr(obj, "name", "") or "")
        node.note = str(getattr(obj, "note", "") or "")
        node.layout = str(getattr(obj, "layout", "") or "")
        node.cfa = str(getattr(obj, "cfa", "") or "")
        meta = getattr(obj, "meta", None)
        if meta:
            if not isinstance(meta, dict):
                raise AdapterError("%s: meta must be a dict of key -> value" % where)
            node.meta = dict(meta)
        rng = getattr(obj, "range", None)
        if rng is not None:
            node.range = to_numpy(where, "range", rng, node.notes).astype("float64")
            if node.range.ndim > 1:                                 # 4.3.3
                node.notes.append("display range varies per frame")

    def axis(self, node, obj, field, where, count, word):
        """Attach timestamps / conditions, or record why it was not applied."""
        raw = getattr(obj, field, None)
        if raw is None:
            return
        values, name, unit = values_of(where, field, raw)
        arr = to_numpy(where, field, values, node.notes).astype("float64")
        if arr.ndim != 1:
            raise AdapterError("%s: %s has shape %s -- one number per %s"
                               % (where, field, arr.shape, word))
        if arr.shape[0] != count:
            raise AdapterError("%s: %s has %d value(s) but there are %d %s(s)"
                               % (where, field, arr.shape[0], count, word))
        if field == "timestamps" and not name:
            name = "time"                                           # 4.3.2
        if not unit.strip():                                        # 4.3.2
            self.skipped.append("%s has no unit" % field)
            node.notes.append("%s dropped (no unit)" % field)
            return
        if field == "conditions" and not name:
            raise AdapterError("%s: conditions has no name -- say what was varied, "
                               'e.g. Values(exposures, "exposure", "ms")' % where)
        setattr(node, field, (arr, name, unit))

    # -- the walk -----------------------------------------------------------
    def add(self, layer, parent):
        node = Node(layer, parent)
        self.nodes.append(node)
        return len(self.nodes) - 1, node

    def walk(self, obj, parent, where):
        layer = layer_of(obj)
        if layer is None:
            return self.bare(obj, parent, where)
        if layer == "batch":
            return self.batch(obj, parent, where)
        if layer == "series":
            return self.series(obj, parent, where)
        return self.leaf(obj, layer, parent, where)

    def leaf(self, obj, layer, parent, where):
        idx, node = self.add(layer, parent)
        node.pixels = to_numpy(where, "pixels", getattr(obj, "pixels"), node.notes)
        rank = node.pixels.ndim
        if layer == "frame" and rank not in (2, 3):
            raise AdapterError("Frame: shape %s is not a frame -- Frame takes (H,W) "
                               "or (H,W,C)" % (node.pixels.shape,))
        if layer == "stack" and rank not in (3, 4):
            raise AdapterError("Stack: shape %s is not a stack -- Stack takes (F,H,W) "
                               "or (F,H,W,C)" % (node.pixels.shape,))
        self.common(node, obj, where)
        if layer == "stack":
            self.axis(node, obj, "timestamps", "Stack", node.pixels.shape[0], "frame")
        return idx

    def series(self, obj, parent, where):
        idx, node = self.add("series", parent)
        self.common(node, obj, where)
        stacks = getattr(obj, "stacks", None)
        if stacks is None:
            raise AdapterError("%s: a Series must carry stacks" % where)

        if isinstance(stacks, (list, tuple)):
            members = list(stacks)
            for i, m in enumerate(members):
                if layer_of(m) not in ("frame", "stack"):
                    raise AdapterError("Series: member %d is a %s -- a series holds "
                                       "Stack or Frame" % (i, type(m).__name__))
            count = len(members)
        else:
            arr = to_numpy("Series", "stacks", stacks, node.notes)
            if arr.ndim not in (3, 4, 5):
                raise AdapterError("Series: shape %s is not a series -- Series takes "
                                   "(S,H,W), (S,R,H,W), (S,R,H,W,C), or a sequence of "
                                   "Stack / Frame" % (arr.shape,))
            count = arr.shape[0]
            members = None

        self.axis(node, obj, "conditions", "Series", count, "stack")

        if members is not None:
            for m in members:
                self.walk(m, idx, where)
        else:
            # 4.4: (S,H,W) is one frame per condition; deeper is a stack per condition
            per_frame = arr.ndim == 3
            for i in range(count):
                _, child = self.add("frame" if per_frame else "stack", idx)
                child.pixels = np.ascontiguousarray(arr[i])
        return idx

    def batch(self, obj, parent, where):
        if parent != -1:
            raise AdapterError("Batch: a batch does not nest inside another layer")
        idx, node = self.add("batch", parent)
        node.name = str(getattr(obj, "name", "") or "")
        members = getattr(obj, "members", None)
        if not members:
            raise AdapterError("Batch: no members -- a batch is a sequence of "
                               "Frame / Stack / Series")
        for m in members:
            if layer_of(m) not in ("frame", "stack", "series"):
                raise AdapterError("Batch: member is a %s -- a batch holds "
                                   "Frame / Stack / Series" % type(m).__name__)
            self.walk(m, idx, where)
        return idx

    def bare(self, obj, parent, where):
        """A bare array is read exactly as native reads a .npy (3.1)."""
        if obj is None:
            raise AdapterError("%s returned nothing -- return an array, or "
                               "Frame / Stack / Series / Batch (4.9: never return "
                               "empty silently, say why instead)" % where)
        if not (hasattr(obj, "shape") or hasattr(obj, "__array__")
                or isinstance(obj, (list, tuple))):
            raise AdapterError("%s returned a %s -- return an array, or "
                               "Frame / Stack / Series / Batch"
                               % (where, type(obj).__name__))
        notes = []
        arr = to_numpy(where, "the returned array", obj, notes)
        shape = arr.shape
        if arr.ndim == 2:
            layer = "frame"
        elif arr.ndim == 3 and shape[2] in (3, 4):
            layer = "frame"                                         # 3.1's one exception
        elif arr.ndim in (3, 4):
            layer = "stack"
        else:
            raise AdapterError("shape %s is not a native form\n  native reads %s\n"
                               "  name the layer instead: Frame(...) / Stack(...) / "
                               "Series(..., conditions=...)" % (shape, NATIVE_FORMS))
        idx, node = self.add(layer, parent)
        node.pixels = arr
        node.notes.extend(notes)
        if layer == "stack":
            node.notes.append(BARE_ARRAY_NOTE)                      # 4.6
        return idx


# ------------------------------------------------------------------ npz output

def json_default(o):
    if hasattr(o, "item"):
        try:
            return o.item()
        except Exception:
            pass
    return str(o)


def meta_key(prefix, key):
    for bad in ("/", "\\", ":", "\0"):
        if bad in key:
            raise AdapterError('meta key "%s" cannot be written to an npz -- use '
                               "letters, digits, _ . -" % key)
    return prefix + key


def write_npz(path, nodes):
    out = {"__n": np.array(len(nodes), dtype="int32")}
    for i, nd in enumerate(nodes):
        out["__layer_%d" % i] = np.array(nd.layer)
        out["__parent_%d" % i] = np.array(nd.parent, dtype="int32")
        out["__name_%d" % i] = np.array(nd.name)
        out["__note_%d" % i] = np.array(nd.full_note())
        out["__layout_%d" % i] = np.array(nd.layout)
        if nd.pixels is not None:
            out["__pixels_%d" % i] = nd.pixels
        if nd.cfa:
            out["__cfa_%d" % i] = np.array(nd.cfa)
        if nd.range is not None:
            out["__range_%d" % i] = nd.range
        for field in ("timestamps", "conditions"):
            got = getattr(nd, field)
            if got is None:
                continue
            values, name, unit = got
            out["__%s_values_%d" % (field, i)] = values
            out["__%s_name_%d" % (field, i)] = np.array(name)
            out["__%s_unit_%d" % (field, i)] = np.array(unit)
        prefix = "__meta_" if i == 0 else "__meta_%d_" % i
        for k, v in nd.meta.items():
            out[meta_key(prefix, str(k))] = np.array(json.dumps(v, sort_keys=True,
                                                                default=json_default))
    np.savez(path, **out)
    return len(out)


def summary(spec, src, nodes, skipped, path, member_count):
    counts = {"batch": 0, "series": 0, "stack": 0, "frame": 0}
    frames = 0
    for nd in nodes:
        counts[nd.layer] += 1
        if nd.pixels is not None:
            frames += nd.pixels.shape[0] if nd.layer == "stack" else 1
    read = []
    for word in ("batch", "series", "stack"):
        n = counts[word]
        if n:
            read.append("%d %s%s" % (n, word, "" if word == "series" or n == 1 else "s"))
    read.append("%d frame%s" % (frames, "" if frames == 1 else "s"))
    why = ""
    if skipped:
        seen = []
        for s in skipped:
            if s not in seen:
                seen.append(s)
        why = " (%s)" % "; ".join(seen)
    size = os.path.getsize(path)
    return ("%s(%s): read %s; skipped %d%s; wrote %d member(s), %.1f kB to %s"
            % (spec, os.path.basename(src.rstrip("/\\")), ", ".join(read),
               len(skipped), why, member_count, size / 1024.0, path))


# ------------------------------------------------------------------ adapter load

def split_spec(spec):
    file_part, sep, func = spec.rpartition(":")
    if not sep or not func.isidentifier() or not file_part:
        raise AdapterError("adapter must be given as <file.py>:<function>, got %r "
                           "(the function name is the part after the last colon)" % spec)
    return file_part, func


def load_function(spec):
    file_part, func_name = split_spec(spec)
    path = os.path.abspath(file_part)
    if not os.path.isfile(path):
        raise AdapterError("no such adapter file: %s" % path)

    folder = os.path.dirname(path)
    for entry in (HERE, folder):                    # folder ends up first
        if entry not in sys.path:
            sys.path.insert(0, entry)
        elif entry == folder:
            sys.path.remove(entry)
            sys.path.insert(0, entry)

    mod_name = "viewer_adapter_" + os.path.splitext(os.path.basename(path))[0]
    loader = importlib.util.spec_from_file_location(mod_name, path)
    module = importlib.util.module_from_spec(loader)
    sys.modules[mod_name] = module
    loader.loader.exec_module(module)

    func = getattr(module, func_name, None)
    if func is None or not callable(func):
        defined = sorted(k for k, v in vars(module).items()
                         if callable(v) and not k.startswith("_"))
        raise AdapterError('%s has no function "%s"%s'
                           % (os.path.basename(path), func_name,
                              " -- it defines: " + ", ".join(defined) if defined else ""))
    return func, "%s:%s" % (os.path.basename(path), func_name)


def main(argv):
    ap = argparse.ArgumentParser(
        prog=PROG, description="Run an input adapter and write its result as one .npz.")
    ap.add_argument("adapter", help="<file.py>:<function>")
    ap.add_argument("path", help="what to hand the adapter; a file or a folder")
    ap.add_argument("-o", "--out", default=None, help="npz to write (default: a temp file)")
    args = ap.parse_args(argv)

    func, spec = load_function(args.adapter)

    if not os.path.exists(args.path):
        raise AdapterError("no such file or directory: %s" % args.path)

    try:
        returned = func(args.path)
    except Exception as exc:                                        # 4.9
        traceback.print_exc()
        if not str(exc).strip():
            sys.stderr.write("%s: %s raised %s with no message -- the reason has to "
                             "be in the exception\n" % (PROG, spec, type(exc).__name__))
        return 2

    build = Build()
    build.walk(returned, -1, spec)

    out = args.out
    if out is None:
        import tempfile
        fd, out = tempfile.mkstemp(prefix="viewer_adapter_", suffix=".npz")
        os.close(fd)
    member_count = write_npz(out, build.nodes)
    sys.stderr.write(summary(spec, args.path, build.nodes, build.skipped,
                             out, member_count) + "\n")
    if args.out is None:
        sys.stdout.write(out + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AdapterError as e:
        sys.stderr.write("%s: %s\n" % (PROG, e))
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(2)
