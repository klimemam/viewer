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

    __viewer                   container format version; its PRESENCE is what
                               distinguishes a viewer container from a plain npz.
                               1, or 2 when an analysisset is aboard (ONLY then:
                               set-free files stay 1, docs/reader-analysisset.md 4)
    __n                        number of nodes
    __layer_<i>                "frame" | "stack" | "series" | "batch" | "analysisset"
    __parent_<i>               parent node index, -1 for the root
    __pixels_<i>               pixels (frame and stack nodes only)
    __role_<i>                 the role a child of an analysisset plays (its parent
                               is the set; the member still LANDS in the batch -
                               binding is not containment)
    __refs_<i>                 on an analysisset node: JSON, declaration order.
                               {"dark": {"path": "...", "member": ""}} is a Ref the
                               viewer resolves; {"bias": {"node": 3}} binds a node
                               this container already carries - how one instance
                               placed in two roles (or in the batch members AND a
                               role) travels once (ras 1.2: same object, same node)
    __name_<i> __note_<i> __layout_<i>       always written, possibly ""
    __cfa_<i> __range_<i>                    written when given
    __timestamps_values_<i> __timestamps_name_<i> __timestamps_unit_<i>
    __conditions_values_<i> __conditions_name_<i> __conditions_unit_<i>
    __meta_<k>                 root node meta, JSON encoded value
    __meta_<i>_<k>             meta of node i > 0, JSON encoded value

An axis whose unit is empty is written WITH AN EMPTY UNIT: a quantity without a
unit is not applied, the same rule a pasted column obeys (4.3.2), but the values
are not thrown away -- the viewer carries them and the user supplies the unit.
It is counted as skipped in the summary and named in the note.

Exit status: 0 wrote the npz, 2 anything else.  A failure always says why (4.9).
"""

import argparse
import contextlib
import importlib.util
import json
import os
import sys
import traceback

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROG = "run_adapter"

LAYERS = ("frame", "stack", "series", "batch", "analysisset")
# 3.1: the last axis is channels when it is 4 or FEWER.  Byte-for-byte the same
# sentence core/main.cpp prints (NPY_NATIVE_FORMS) -- the viewer and this harness
# refusing the same array in two different wordings is how issue #71 stayed
# invisible.  ASCII "<=" because this is written to stderr through a pipe, where
# the encoding is the locale's (cp932 cannot represent U+2264 at all).
NATIVE_FORMS = "(H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)"
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
                           or hasattr(obj, "members") or hasattr(obj, "roles")):
        return name
    return None


def ref_of(obj):
    """(path, member) when this is Ref-shaped, else None.  Duck typed for the
    same reason layer_of is: the adapter may hold its own viewer_import copy."""
    if type(obj).__name__ != "Ref":
        return None
    if not hasattr(obj, "path") or not hasattr(obj, "member"):
        return None
    return str(obj.path), str(obj.member or "")


def is_role_name(name):
    """ras 1.2: [A-Za-z0-9_]+ -- identifier-shaped, session-line safe."""
    if not name:
        return False
    for c in name:
        if not ("a" <= c <= "z" or "A" <= c <= "Z" or "0" <= c <= "9" or c == "_"):
            return False
    return True


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
    arr = to_viewer_dtype(where, what, arr, notes)
    if not arr.dtype.isnative:
        arr = arr.astype(arr.dtype.newbyteorder("="))
    return np.ascontiguousarray(arr)


# The nine the viewer decodes.  numpy's DEFAULT integer is int64, so the very
# first thing a new reader writes -- np.array([[1, 2], [3, 4]]) -- is not one of
# them.  Until this check existed the container was written successfully and the
# viewer refused it at the far end, where the author cannot see the dtype and
# cannot act on it.  Convert here, where the reader is, or refuse here.
VIEWER_DTYPES = ("uint8", "int8", "bool", "uint16", "int16",
                 "uint32", "int32", "float32", "float64")


def to_viewer_dtype(where, what, arr, notes):
    if arr.dtype.name in VIEWER_DTYPES:
        return arr
    if arr.dtype.kind == "f":
        if arr.dtype.itemsize < 4:                  # float16 -> float32 is exact
            notes.append("%s -> float32" % arr.dtype.name)
            return arr.astype("float32")
        raise AdapterError(
            "%s: %s is %s, and the viewer reads float32 and float64. Narrowing a "
            "wider float here would change the numbers, so say which you want: "
            ".astype(np.float64)" % (where, what, arr.dtype.name))
    # Integer.  One step down to the widest type of the same signedness -- NOT
    # the smallest that fits, because the smallest changes the apparent depth of
    # the data.  If the values do not fit, that is the author's decision to make,
    # not ours to make silently.
    target = "uint32" if arr.dtype.kind == "u" else "int32"
    info = np.iinfo(target)
    lo = int(arr.min()) if arr.size else 0
    hi = int(arr.max()) if arr.size else 0
    if lo < info.min or hi > info.max:
        raise AdapterError(
            "%s: %s is %s holding %d..%d, which does not fit %s -- the viewer reads "
            "u1 i1 u2 i2 u4 i4 f4 f8. Scale or offset the values, or return them as "
            "float64." % (where, what, arr.dtype.name, lo, hi, target))
    notes.append("%s -> %s (values %d..%d fit)" % (arr.dtype.name, target, lo, hi))
    return arr.astype(target)


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
        self.role = ""                  # a set child: the role it plays (ras 4)
        self.refs = []                  # a set node: [(role, {...})], declaration order

    def full_note(self):
        parts = [p for p in ([self.note] + self.notes) if p]
        return ", ".join(parts)


class Build(object):
    def __init__(self):
        self.nodes = []
        self.skipped = []               # one line per thing not applied
        self.seen = {}                  # id(layer object) -> node index (ras 1.2)
        self.keep = []                  # holds those objects so id() stays unique

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
        if field == "conditions" and not name:
            # Checked BEFORE the unit: an unnamed sweep is an error whether or
            # not a unit came with it.  "what was varied" is not recoverable.
            raise AdapterError("%s: conditions has no name -- say what was varied, "
                               'e.g. Values(exposures, "exposure", "ms")' % where)
        if not unit.strip():                                        # 4.3.2
            # NOT APPLIED, but NOT DISCARDED.  "no unit" is not "no data": the
            # values are written with an empty unit and reach the viewer, which
            # holds exactly this state already (Series::unit empty, axis values
            # present) and lets the user type the unit in to complete it.
            # Dropping them here would make the user re-enter measurements by
            # hand, which is the one cost 4.3.2 names as unacceptable.  It is
            # still counted as skipped, so the summary says both directions.
            self.skipped.append("%s has no unit" % field)
            node.notes.append("%s has no unit (values kept, not applied)" % field)
            setattr(node, field, (arr, name, ""))
            return
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
        # ras 1.2: the same instance is the same node.  The pixels travel once;
        # whoever meets the object again gets the index of the node it already
        # is (the set handler turns that into a {"node": k} ref).
        key = id(obj)
        if key in self.seen:
            return self.seen[key]
        if layer == "batch":
            idx = self.batch(obj, parent, where)
        elif layer == "series":
            idx = self.series(obj, parent, where)
        elif layer == "analysisset":
            idx = self.analysisset(obj, parent, where)
        else:
            idx = self.leaf(obj, layer, parent, where)
        self.seen[key] = idx
        self.keep.append(obj)
        return idx

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
                               "Frame / Stack / Series / AnalysisSet")
        # ras 1.4: every member of this call lands in ONE batch, and a set's
        # identity is (batch, name) - so two sets under the same EXPLICIT name
        # are a defect of this reader's output and fail the call HERE, where the
        # type check can no longer be sidestepped.  Default (schema-derived)
        # names are exempt: the viewer numbers those.
        names = []
        for m in members:
            if layer_of(m) == "analysisset":
                nm = str(getattr(m, "name", "") or "")
                given = bool(getattr(m, "name_given", nm != ""))
                if given and nm:
                    if nm in names:
                        raise AdapterError(
                            'two sets in one call are both named "%s" -- a '
                            "set's identity is (batch, name); rename one "
                            "(ras 1.4: this is a defect of the reader's own "
                            "output, so the call fails)" % nm)
                    names.append(nm)
        for m in members:
            if layer_of(m) not in ("frame", "stack", "series", "analysisset"):
                raise AdapterError("Batch: member is a %s -- a batch holds "
                                   "Frame / Stack / Series / AnalysisSet"
                                   % type(m).__name__)
            self.walk(m, idx, where)
        return idx

    def analysisset(self, obj, parent, where):
        """ras 4: the set is a node; inline role members are its CHILDREN with
        __role, refs ride as one JSON member.  Landing (everything into the
        batch) is the viewer's job - the tree stays a tree here."""
        if parent != -1 and self.nodes[parent].layer != "batch":
            raise AdapterError("AnalysisSet: a set is a node of a batch (or the "
                               "whole return value) -- it cannot live inside a "
                               "%s" % self.nodes[parent].layer)
        idx, node = self.add("analysisset", parent)
        self.common(node, obj, where)
        roles = getattr(obj, "roles", None)
        if not isinstance(roles, dict) or not roles:
            raise AdapterError("AnalysisSet: roles must be a non-empty dict of "
                               "{role: member}")
        if not node.name:                       # ras 1.1: schema-derived default
            node.name = "{" + ", ".join(str(k) for k in roles) + "}"
        for role, val in roles.items():
            role = str(role)
            if not is_role_name(role):
                raise AdapterError("AnalysisSet: role name %r -- letters, digits "
                                   "and _ only" % role)
            ref = ref_of(val)
            if ref is not None:
                path, member = ref
                if not path.strip():
                    raise AdapterError('AnalysisSet: role "%s": Ref path is '
                                       "empty" % role)
                node.refs.append((role, {"path": path, "member": member}))
                continue
            sub = layer_of(val)
            if sub not in ("frame", "stack", "series"):
                raise AdapterError('AnalysisSet: role "%s" is a %s -- a role '
                                   "holds Frame / Stack / Series or Ref (a bare "
                                   "array does not declare its layer, ras "
                                   "decision 2)" % (role, type(val).__name__))
            if id(val) in self.seen:
                # 1.2: this object is already a node somewhere in the tree
                # (the batch members, another role, another set).  The pixels
                # travel once; the binding travels as a node ref.
                node.refs.append((role, {"node": self.seen[id(val)]}))
                continue
            cidx = self.walk(val, idx, where)
            self.nodes[cidx].role = role
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
        elif arr.ndim == 3 and shape[2] <= 4:
            layer = "frame"                     # 3.1's one exception: LAST axis, 4 or fewer
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


#: bumped when the MEANING of a reserved member changes; additions are compatible
CONTAINER_VERSION = 1

#: ras 4: ONLY a set-bearing container names version 2 - an old viewer then
#: refuses it whole instead of silently dropping the set; a set-free file
#: stays 1 and its compatibility does not move at all.
CONTAINER_VERSION_SET = 2


STREAM_VERSION = 1
STREAM_VERSION_SET = 2                  # same rule, same reason (ras 4)


def carries_set(nodes):
    for nd in nodes:
        if nd.layer == "analysisset":
            return True
    return False


def _stream_line(fh, text):
    fh.write(text.encode("utf-8") + b"\n")


def stream_out(fh, nodes):
    """The same tree as write_npz, handed over a pipe instead of a file.

    An npz cannot serve here: its central directory sits at the END, so reading
    one means holding all of it first -- which is the buffer this exists to
    avoid.  So: every small thing in a line-based header, then the pixel blobs
    raw, in the order their `pixels` lines appeared.  The header borrows the
    session format's rules (one key per line, the value is the rest of the line,
    unknown keys are skipped) so a viewer that predates a new key still reads
    what it does know.

    Text is fine for timestamps, conditions and range: they carry one number per
    frame or per member, which is nothing beside the pixels, and text is what the
    session already writes axis values as.
    """
    _stream_line(fh, "VIEWERSTREAM %d"
                 % (STREAM_VERSION_SET if carries_set(nodes) else STREAM_VERSION))
    _stream_line(fh, "n %d" % len(nodes))
    blobs = []
    for i, nd in enumerate(nodes):
        _stream_line(fh, "layer %d %s" % (i, nd.layer))
        _stream_line(fh, "parent %d %d" % (i, nd.parent))
        if nd.role:
            _stream_line(fh, "role %d %s" % (i, nd.role))
        if nd.refs:                     # one line: json.dumps emits no newlines
            _stream_line(fh, "refs %d %s" % (i, json.dumps(dict(nd.refs))))
        if nd.name:
            _stream_line(fh, "name %d %s" % (i, nd.name))
        note = nd.full_note()
        if note:
            _stream_line(fh, "note %d %s" % (i, note))
        if nd.layout:
            _stream_line(fh, "layout %d %s" % (i, nd.layout))
        if nd.cfa:
            _stream_line(fh, "cfa %d %s" % (i, nd.cfa))
        if nd.range is not None:
            r = np.asarray(nd.range).reshape(-1)
            _stream_line(fh, "range %d %d %s"
                         % (i, r.size, " ".join(repr(float(v)) for v in r)))
        for field in ("timestamps", "conditions"):
            got = getattr(nd, field)
            if got is None:
                continue
            values, name, unit = got
            v = np.asarray(values).reshape(-1)
            # name and unit on their own lines: both may contain spaces, and the
            # values line has to stay parseable by splitting on them
            _stream_line(fh, "%s_name %d %s" % (field, i, name))
            _stream_line(fh, "%s_unit %d %s" % (field, i, unit))
            _stream_line(fh, "%s %d %d %s"
                         % (field, i, v.size, " ".join(repr(float(x)) for x in v)))
        for k, val in nd.meta.items():
            _stream_line(fh, "meta %d %s %s"
                         % (i, meta_key("", str(k)),
                            json.dumps(val, sort_keys=True, default=json_default)))
        if nd.pixels is not None:
            a = np.ascontiguousarray(nd.pixels)
            _stream_line(fh, "pixels %d %s %d %s %d"
                         % (i, a.dtype.str.lstrip("<>|=").replace("f8", "f8"),
                            a.ndim, " ".join(str(d) for d in a.shape), a.nbytes))
            blobs.append(a)
    _stream_line(fh, "end")
    fh.flush()
    # The pixels, in the order they were announced. Written in slices so a
    # 755 MB stack does not need a 755 MB copy on the way out.
    total = 0
    for a in blobs:
        mv = memoryview(a.reshape(-1).view(np.uint8))
        step = 8 << 20
        for off in range(0, mv.nbytes, step):
            fh.write(mv[off:off + step])
        total += mv.nbytes
    fh.flush()
    return len(blobs), total


def write_npz(path, nodes):
    # __viewer is the discriminator (4.11.1): with it this npz is a viewer
    # container and the layer tree below is authoritative; without it the file is
    # an ordinary npz and the viewer classifies its members by shape
    # (docs/npz-design.md).  It is written FIRST and always, because "is this a
    # container?" must be answerable without understanding anything else here.
    out = {"__viewer": np.array(CONTAINER_VERSION_SET if carries_set(nodes)
                                else CONTAINER_VERSION, dtype="int32"),
           "__n": np.array(len(nodes), dtype="int32")}
    for i, nd in enumerate(nodes):
        out["__layer_%d" % i] = np.array(nd.layer)
        out["__parent_%d" % i] = np.array(nd.parent, dtype="int32")
        out["__name_%d" % i] = np.array(nd.name)
        out["__note_%d" % i] = np.array(nd.full_note())
        out["__layout_%d" % i] = np.array(nd.layout)
        if nd.role:
            out["__role_%d" % i] = np.array(nd.role)
        if nd.refs:                     # declaration order: dict keeps insertion,
            out["__refs_%d" % i] = np.array(json.dumps(dict(nd.refs)))   # no sort_keys
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
    counts = {"batch": 0, "series": 0, "stack": 0, "frame": 0, "analysisset": 0}
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
    if counts["analysisset"]:           # ras 4: sets are read too, so they are counted
        n = counts["analysisset"]
        read.append("%d set%s" % (n, "" if n == 1 else "s"))
    read.append("%d frame%s" % (frames, "" if frames == 1 else "s"))
    why = ""
    if skipped:
        seen = []
        for s in skipped:
            if s not in seen:
                seen.append(s)
        why = " (%s)" % "; ".join(seen)
    # `path` is a file on the npz route and a byte count on the streamed one:
    # there is nothing to stat when the result went down a pipe, and the line
    # still has to say how much was handed over.
    if isinstance(path, int):
        size, where = path, "the viewer"
    else:
        size, where = os.path.getsize(path), path
    return ("%s(%s): read %s; skipped %d%s; wrote %d member(s), %.1f kB to %s"
            % (spec, os.path.basename(src.rstrip("/\\")), ", ".join(read),
               len(skipped), why, member_count, size / 1024.0, where))


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
    ap.add_argument("--stream", action="store_true",
                    help="hand the result over stdout as a framed stream instead of "
                         "writing an npz (the viewer uses this; see docs)")
    args = ap.parse_args(argv)

    func, spec = load_function(args.adapter)

    if not os.path.exists(args.path):
        raise AdapterError("no such file or directory: %s" % args.path)

    try:
        if args.stream:
            # stdout is the wire now, so the reader's own print() has to go
            # somewhere else or it corrupts the payload. stderr is where the
            # summary already goes and where the viewer's panel already looks,
            # so a reader that prints its progress still shows it.
            with contextlib.redirect_stdout(sys.stderr):
                returned = func(args.path)
        else:
            returned = func(args.path)
    except Exception as exc:                                        # 4.9
        traceback.print_exc()
        if not str(exc).strip():
            sys.stderr.write("%s: %s raised %s with no message -- the reason has to "
                             "be in the exception\n" % (PROG, spec, type(exc).__name__))
        return 2

    build = Build()
    build.walk(returned, -1, spec)

    if args.stream:
        n_blobs, n_bytes = stream_out(sys.stdout.buffer, build.nodes)
        sys.stderr.write(summary(spec, args.path, build.nodes, build.skipped,
                                 n_bytes, n_blobs) + "\n")
        return 0

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
