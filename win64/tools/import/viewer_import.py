"""viewer_import -- the types an input adapter returns.

An adapter is a function you write:

    from viewer_import import Frame, Stack, Series, Batch, Values

    def load(path):                      # path may be a file or a folder
        z = np.load(path)
        return Series([Stack(z["data"][i]) for i in range(len(z["exp"]))],
                      conditions=Values(z["exp"], "exposure", "ms"))

The return value names its layer -- frame / stack / series / batch -- and that
declaration is what decides which measurements are legal.  The same (F,H,W) is a
Stack when the frames are repeats of one condition (sigma_t means something) and a
Series when a condition was swept (sigma_t would report the sweep as noise).  No
other vocabulary is introduced; see docs/terminology.md.

The fifth layer of the canon is here too: AnalysisSet, a set of role bindings
({"image": Stack(arr), "dark": Ref("darks/")}), with Ref as the way to bind
data the viewer already has open without re-reading it.  Both are specified in
docs/features/adapters/reader-analysisset.md ("ras" in comments below).  A role value is Frame /
Stack / Series (inline: this call carries the pixels) or Ref (a path: the
viewer resolves it against what is open, 2.2).  A bare array is NOT a role
value (ras decision 2): the role dict is a declaration, and the one place that
must not fall back to shape guessing.  Nothing already here changed meaning,
so VERSION stays 1 (it moves when a FIELD changes meaning, not on additions).

Returning a bare array is also allowed and is read exactly as the viewer reads a
.npy natively -- (H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4).  The last axis is
channels when it is FOUR OR FEWER, so a bare (H,W,1) comes back as one mono
frame and (H,W,2) as one two-channel frame (issue #71); this file said "3|4"
while the viewer, run_adapter.py and core/serve.cpp all said "4 or fewer", and
an adapter author reading only this line would have expected a stack.  Naming a
type is what you do when you want to kill an ambiguity yourself.

Promises this file keeps (docs/features/adapters/input-adapters.md 4.10):

  * single file, standard library only.  It does NOT import numpy or torch.
    Arrays are carried, never inspected element by element: only `.shape` and
    `.dtype` are read.  Copy this file next to your adapter and it works with
    nothing installed.
  * every layer validates itself at construction, so a mistake is reported on the
    line that made it, with the layer and the shape named.
  * frozen: what you returned is what the viewer gets.

Python floor: 3.8.
"""

from dataclasses import dataclass, fields as _fields
from typing import Any, ClassVar, Dict, Optional

__all__ = ["Frame", "Stack", "Series", "Batch", "AnalysisSet", "Ref", "Values",
           "CFA_PATTERNS", "LAYOUTS", "VERSION"]

#: bumped when the meaning of a field changes; the viewer keys its cache on it
VERSION = 1

_BAYER = ("RGGB", "BGGR", "GRBG", "GBRG")
#: every accepted `cfa` spelling (4.3)
CFA_PATTERNS = tuple(list(_BAYER) + ["quad:" + p for p in _BAYER])
#: `layout` is the escape hatch for transposed arrays only (4.4); usually omitted
LAYOUTS = {"CHW": 3, "FCHW": 4}


# ------------------------------------------------------------------ inspection
# Everything below reads .shape / .dtype and nothing else.  Nested lists are
# supported so that this file is useful with no array library installed at all.

def _shape_of(obj):
    """Return a shape tuple, or None if the object does not have one."""
    shape = getattr(obj, "shape", None)
    if shape is not None:
        if callable(shape):                     # some frameworks expose shape()
            shape = shape()
        try:
            return tuple(int(d) for d in shape)
        except TypeError:
            return None
    if isinstance(obj, (list, tuple)):
        dims = []
        cur = obj
        while isinstance(cur, (list, tuple)):
            dims.append(len(cur))
            cur = cur[0] if len(cur) else None
        return tuple(dims)
    return None


def _dtype_name(obj):
    """A printable dtype name, or None when the object does not declare one."""
    dt = getattr(obj, "dtype", None)
    if dt is None:
        return None
    return getattr(dt, "name", None) or str(dt)


def _require_shape(layer, what, obj):
    shape = _shape_of(obj)
    if shape is None:
        raise TypeError(
            "%s: %s has no .shape -- pass a numpy array, a torch tensor, "
            "or a nested list" % (layer, what))
    return shape


def _require_numeric(layer, what, obj):
    """Refuse strings / objects / complex (4.8).  Only the dtype is looked at."""
    dt = getattr(obj, "dtype", None)
    if dt is None:
        return                                  # nested list: nothing to inspect
    name = _dtype_name(obj)
    kind = getattr(dt, "kind", None)
    if kind is not None:                        # numpy-style dtype
        if kind not in "biuf":
            raise ValueError("%s: dtype %s is not numeric -- %s must be integer "
                             "or float" % (layer, name, what))
        return
    low = str(name).lower()                     # torch-style dtype, duck typed
    for bad in ("complex", "str", "object", "bytes", "bool_"):
        if bad in low:
            raise ValueError("%s: dtype %s is not numeric -- %s must be integer "
                             "or float" % (layer, name, what))


def _check_cfa(layer, cfa):
    if not cfa:
        return
    if cfa not in CFA_PATTERNS:
        raise ValueError('%s: cfa "%s" is not a known pattern -- one of %s '
                         '(or quad:RGGB and friends)'
                         % (layer, cfa, ", ".join(_BAYER)))


def _check_layout(layer, layout, shape):
    if not layout:
        return
    if layout not in LAYOUTS:
        raise ValueError('%s: layout "%s" is not known -- layout is %s, and only '
                         'for arrays whose axes are transposed'
                         % (layer, layout, " or ".join(sorted(LAYOUTS))))
    want = LAYOUTS[layout]
    if len(shape) != want:
        raise ValueError('%s: layout "%s" needs a %d-D array but shape is %s'
                         % (layer, layout, want, shape))


def _check_meta(layer, meta):
    if meta is None:
        return {}
    if not isinstance(meta, dict):
        raise TypeError("%s: meta must be a dict of key -> value (facts about the "
                        "shot); prose belongs in note" % layer)
    for k in meta:
        if not isinstance(k, str):
            raise TypeError("%s: meta key %r is not a string" % (layer, k))
    return dict(meta)


def _check_range(layer, rng, per, per_word, inner=None):
    """`range` is (lo,hi), or one (lo,hi) per member of this layer (4.3.3).

    per      how many members this layer has (None when unknown)
    per_word "frame" or "stack" -- what those members are called
    inner    for a series: frames per stack, when it is known
    """
    if rng is None:
        return
    shape = _shape_of(rng)
    if shape is None:
        raise TypeError("%s: range must be (lo,hi) or an array of them" % layer)
    if per is None:
        forms = "(lo,hi)"
    elif per_word == "stack":                   # a series: one per stack, or per frame too
        forms = "(lo,hi), (S,2) or (S,R,2)"
    else:
        forms = "(lo,hi) or (F,2)"
    if not shape or shape[-1] != 2 or len(shape) > 3:
        raise ValueError("%s: range has shape %s -- range is %s"
                         % (layer, shape, forms))
    if len(shape) == 1:
        return                                              # uniform (lo,hi)
    if per is None:
        raise ValueError("%s: range has shape %s -- a %s takes range=(lo,hi)"
                         % (layer, shape, layer.lower()))
    if shape[0] != per:
        raise ValueError("%s: range has shape %s but %s %d %s(s) -- range is %s"
                         % (layer, shape,
                            "the stack has" if per_word == "frame" else "there are",
                            per, per_word, forms))
    if len(shape) == 3 and inner is not None and shape[1] != inner:
        raise ValueError("%s: range has shape %s but each stack has %d frame(s)"
                         % (layer, shape, inner))


def _check_values(layer, what, values, need_name):
    if not isinstance(values, Values):
        raise TypeError('%s: %s must be Values(...) -- e.g. Values(t, unit="s"); '
                        'the unit is never guessed' % (layer, what))
    if need_name and not values.name:
        raise ValueError('%s: %s has no name -- say what was varied, e.g. '
                         'Values(exposures, "exposure", "ms")' % (layer, what))


def _check_length(layer, what, values, n, word):
    if len(values) != n:
        raise ValueError("%s: %s has %d value(s) but there are %d %s(s)"
                         % (layer, what, len(values), n, word))


def _repr(obj, *extra):
    bits = []
    shape = getattr(obj, "shape", None)
    if shape is not None:
        bits.append("shape=%s" % (shape,))
    for name in ("name", "cfa", "note", "layout"):
        val = getattr(obj, name, "")
        if val:
            bits.append("%s=%r" % (name, val))
    bits.extend(extra)
    return "%s(%s)" % (type(obj).__name__, ", ".join(bits))


# ---------------------------------------------------------------------- Values

@dataclass(frozen=True, eq=False, repr=False)
class Values:
    """One number per member, with a unit.  `Values(values, name="", unit="")`.

    The same type carries timestamps (one per frame) and conditions (one per
    stack); no dedicated Times / Sweep types exist.  `unit` is always required:
    an axis handed over without one is not applied, exactly as a pasted column
    with no unit is not applied.  Values keep full precision -- they are plotted.
    """

    values: Any
    name: str = ""
    unit: str = ""

    def __post_init__(self):
        shape = _require_shape("Values", "values", self.values)
        if len(shape) != 1:
            raise ValueError("Values: shape %s -- values must be a 1-D vector, "
                             "one number per member" % (shape,))
        _require_numeric("Values", "values", self.values)
        if isinstance(self.values, (list, tuple)):
            for i, v in enumerate(self.values):
                if isinstance(v, bool) or not isinstance(v, (int, float)):
                    raise TypeError("Values: value %d is a %s -- values must be "
                                    "numbers" % (i, type(v).__name__))
        if not isinstance(self.name, str) or not isinstance(self.unit, str):
            raise TypeError("Values: name and unit must be strings")

    @property
    def applied(self):
        """False when no unit was given: the axis is carried but not applied."""
        return bool(self.unit.strip())

    @property
    def shape(self):
        return _shape_of(self.values)

    def __len__(self):
        return _shape_of(self.values)[0]

    def __repr__(self):
        return "Values(%d value(s), name=%r, unit=%r)" % (len(self), self.name, self.unit)

    def renamed(self, name):
        return Values(self.values, name, self.unit)


# ----------------------------------------------------------------------- Frame

@dataclass(frozen=True, eq=False, repr=False)
class Frame:
    """One image.  (H,W) is single channel, (H,W,C) is C channels.

    C is not restricted here: a frame has no frame axis, so there is nothing for
    it to be confused with.  (3,H,W) as a Frame means three channels only if you
    also say layout="CHW"; on its own it is three frames -- write Stack(arr).
    """

    pixels: Any
    cfa: str = ""
    name: str = ""
    note: str = ""
    meta: Optional[Dict[str, Any]] = None
    range: Any = None
    layout: str = ""

    LAYER: ClassVar[str] = "frame"

    def __post_init__(self):
        shape = _require_shape("Frame", "pixels", self.pixels)
        if len(shape) not in (2, 3):
            raise ValueError("Frame: shape %s is not a frame -- Frame takes (H,W) "
                             "or (H,W,C)" % (shape,))
        _require_numeric("Frame", "pixels", self.pixels)
        _check_cfa("Frame", self.cfa)
        _check_layout("Frame", self.layout, shape)
        _check_range("Frame", self.range, None, "frame")
        object.__setattr__(self, "meta", _check_meta("Frame", self.meta))

    @property
    def shape(self):
        return _shape_of(self.pixels)

    @property
    def channel_count(self):
        shape = self.shape
        if len(shape) == 2:
            return 1
        return shape[0] if self.layout == "CHW" else shape[2]

    def __repr__(self):
        return _repr(self, "channels=%d" % self.channel_count)


# ----------------------------------------------------------------------- Stack

@dataclass(frozen=True, eq=False, repr=False)
class Stack:
    """Frames taken under one condition.  (F,H,W) or (F,H,W,C).

    A stack has a time axis, so sigma_t / FPN separation and per-frame tables mean
    something here.  (3,H,W) is three frames -- if you meant a colour image, that
    is Frame(arr).  `timestamps` says when each frame was taken; it does not make
    the frames different measurements.  If a setting was changed between them, it
    is not a stack: build a Series and put the setting in `conditions`.
    """

    pixels: Any
    timestamps: Optional[Values] = None
    cfa: str = ""
    name: str = ""
    note: str = ""
    meta: Optional[Dict[str, Any]] = None
    range: Any = None
    layout: str = ""

    LAYER: ClassVar[str] = "stack"

    def __post_init__(self):
        shape = _require_shape("Stack", "pixels", self.pixels)
        if len(shape) not in (3, 4):
            raise ValueError("Stack: shape %s is not a stack -- Stack takes (F,H,W) "
                             "or (F,H,W,C); one image is Frame(...)" % (shape,))
        _require_numeric("Stack", "pixels", self.pixels)
        _check_cfa("Stack", self.cfa)
        _check_layout("Stack", self.layout, shape)
        _check_range("Stack", self.range, shape[0], "frame")
        object.__setattr__(self, "meta", _check_meta("Stack", self.meta))
        if self.timestamps is not None:
            _check_values("Stack", "timestamps", self.timestamps, need_name=False)
            _check_length("Stack", "timestamps", self.timestamps, shape[0], "frame")
            if not self.timestamps.name:            # 4.3.2: the default name
                object.__setattr__(self, "timestamps", self.timestamps.renamed("time"))

    @property
    def shape(self):
        return _shape_of(self.pixels)

    @property
    def frame_count(self):
        return self.shape[0]

    def __repr__(self):
        return _repr(self, "frames=%d" % self.frame_count)


# ---------------------------------------------------------------------- Series

@dataclass(frozen=True, eq=False, repr=False)
class Series:
    """Stacks (or frames) taken with one condition varied.  `conditions` is required.

    Pass a sequence of Stack / Frame, or one array:

        (S,H,W[,C])     S conditions, one frame each
        (S,R,H,W[,C])   S conditions, R repeats each

    The condition belongs to the series, not to the stacks inside it: that is what
    keeps a sweep from being averaged as if it were repeats.  Statistics that cross
    conditions (sigma_t above all) are refused for a series.
    """

    stacks: Any
    conditions: Values
    name: str = ""
    note: str = ""
    meta: Optional[Dict[str, Any]] = None
    range: Any = None
    layout: str = ""

    LAYER: ClassVar[str] = "series"

    def __post_init__(self):
        members = self.stacks
        count = None
        inner = None
        if isinstance(members, (list, tuple)):
            if not members:
                raise ValueError("Series: no stacks -- a series is a sequence of "
                                 "Stack / Frame, or one array")
            for i, m in enumerate(members):
                if not isinstance(m, (Stack, Frame)):
                    raise TypeError("Series: member %d is a %s -- a series holds "
                                    "Stack or Frame (or pass one array)"
                                    % (i, type(m).__name__))
            object.__setattr__(self, "stacks", tuple(members))
            count = len(members)
        else:
            shape = _require_shape("Series", "stacks", members)
            if len(shape) not in (3, 4, 5):
                raise ValueError("Series: shape %s is not a series -- Series takes "
                                 "(S,H,W), (S,R,H,W), (S,R,H,W,C), or a sequence "
                                 "of Stack / Frame" % (shape,))
            _require_numeric("Series", "stacks", members)
            count = shape[0]
            inner = 1 if len(shape) == 3 else shape[1]
        object.__setattr__(self, "_stack_count", count)
        object.__setattr__(self, "_frames_per_stack", inner)

        _check_layout("Series", self.layout, _shape_of(members) or ())
        _check_range("Series", self.range, count, "stack", inner)
        object.__setattr__(self, "meta", _check_meta("Series", self.meta))
        _check_values("Series", "conditions", self.conditions, need_name=True)
        _check_length("Series", "conditions", self.conditions, count, "stack")

    @property
    def shape(self):
        return _shape_of(self.stacks) if not isinstance(self.stacks, tuple) else None

    @property
    def stack_count(self):
        return self._stack_count

    @property
    def frames_per_stack(self):
        """Frames in each stack when the series was given as one array, else None."""
        return self._frames_per_stack

    def __repr__(self):
        return _repr(self, "stacks=%d" % self.stack_count,
                     "conditions=%r" % (self.conditions.name,))


# ----------------------------------------------------------------------- Batch

@dataclass(frozen=True, eq=False, repr=False)
class Batch:
    """Things to open together.  A batch claims no structure at all.

    If the members are one measurement with a condition varied across them, that
    is a Series, not a Batch.
    """

    members: Any
    name: str = ""

    LAYER: ClassVar[str] = "batch"

    def __post_init__(self):
        members = self.members
        if not isinstance(members, (list, tuple)) or not members:
            raise ValueError("Batch: no members -- a batch is a sequence of "
                             "Frame / Stack / Series / AnalysisSet")
        setNames = []
        for i, m in enumerate(members):
            if not isinstance(m, (Frame, Stack, Series, AnalysisSet)):
                raise TypeError("Batch: member %d is a %s -- a batch holds "
                                "Frame / Stack / Series / AnalysisSet (a batch "
                                "does not nest)" % (i, type(m).__name__))
            # ras 1.3/1.4: a set's identity is (batch, name), and every member
            # of this batch lands in ONE batch - so two sets under the same
            # EXPLICIT name fail here, on the constructing line.  Default
            # (schema-derived) names are exempt: numbering those is the
            # viewer's job, not a defect of this reader.
            if isinstance(m, AnalysisSet) and m.name_given:
                if m.name in setNames:
                    raise ValueError('Batch: two sets are both named "%s" -- a '
                                     "set's identity is (batch, name), so one "
                                     "call cannot return two (rename one; only "
                                     "explicit name= collides - default-named "
                                     "sets are numbered by the viewer)" % m.name)
                setNames.append(m.name)
        object.__setattr__(self, "members", tuple(members))

    def __repr__(self):
        return "Batch(%d member(s), name=%r)" % (len(self.members), self.name)


# ------------------------------------------------------------------------ Ref
# docs/features/adapters/reader-analysisset.md 2: "this role is THAT, over there" -- said by path,
# never by name (names are renameable and not unique, the session file's rule).

@dataclass(frozen=True, eq=False, repr=False)
class Ref:
    """A role bound to data the viewer resolves by path -- never re-read here.

    Ref(path, member="").  A file path means that one file read natively; a
    folder means the ONE numbered file group inside it (two or more groups are
    refused by name).  `member` names one array of an .npz container.  The
    viewer resolves it in ras 2.2 order: an open copy with the same identity is
    shared; an open copy whose file changed on disk is bound WITH a declaration;
    an unopened path is opened into the calling reader's batch.  A path that
    cannot be resolved leaves the role UNBOUND with the reason - it never fails
    the call (ras 3.1: a Ref speaks about the world, not about this reader).
    """

    path: str
    member: str = ""

    def __post_init__(self):
        if not isinstance(self.path, str) or not isinstance(self.member, str):
            raise TypeError("Ref: path and member must be strings")
        if not self.path.strip():
            raise ValueError("Ref: path is empty -- a reference names where "
                             "the data lives")

    def __repr__(self):
        return "Ref(%r%s)" % (self.path,
                              ", member=%r" % self.member if self.member else "")


def _is_role_name(name):
    """ras 1.2: role names are [A-Za-z0-9_]+ -- identifier-shaped, so session
    lines can be split on whitespace and the existing role vocabulary fits."""
    if not isinstance(name, str) or not name:
        return False
    for c in name:
        if not (c.isascii() and (c.isalnum() or c == "_")):
            return False
    return True


# ---------------------------------------------------------------- AnalysisSet

@dataclass(frozen=True, eq=False, repr=False)
class AnalysisSet:
    """A set of role bindings: {"image": Stack(arr), "dark": Ref("darks/")}.

    The first positional argument is the ROLE DICT (ras decision 1) - the
    canon's own role-schema notation become code, so a misspelling fails on the
    line that made it.  Role values are Frame / Stack / Series (inline) or Ref;
    a bare array is refused (decision 2), and neither Batch nor AnalysisSet can
    be a role value - a set does not nest.  A set carries no pixels of its own,
    so there is no `range`.  The default name is the role list ("{image, dark}"),
    a starting value in the tradition of the series default name; whether a
    name was explicitly given is kept (`name_given`) because only EXPLICIT
    same-name sets in one batch are a defect (ras 1.4).
    """

    roles: Any
    name: str = ""
    note: str = ""
    meta: Optional[Dict[str, Any]] = None

    LAYER: ClassVar[str] = "analysisset"

    def __post_init__(self):
        roles = self.roles
        if not isinstance(roles, dict):
            raise TypeError("AnalysisSet: roles must be a dict of {role: member}"
                            ' -- e.g. {"image": Stack(arr), "dark": Ref("darks/")}')
        if not roles:
            raise ValueError("AnalysisSet: no roles -- a set is a set of role "
                             "bindings")
        for k, v in roles.items():
            if not _is_role_name(k):
                raise ValueError("AnalysisSet: role name %r -- letters, digits "
                                 "and _ only (role names are written unquoted "
                                 "in session files)" % (k,))
            if isinstance(v, Ref):
                continue
            if isinstance(v, (Frame, Stack, Series)):
                continue
            if isinstance(v, (Batch, AnalysisSet)):
                raise TypeError('AnalysisSet: role "%s" is a %s -- a set does '
                                "not nest; a role holds Frame / Stack / Series "
                                "or Ref" % (k, type(v).__name__))
            raise TypeError('AnalysisSet: role "%s" is a %s -- a role holds '
                            "Frame / Stack / Series or Ref; a bare array does "
                            "not declare its layer (say Stack(arr), ras "
                            "decision 2)" % (k, type(v).__name__))
        object.__setattr__(self, "roles", dict(roles))
        if not isinstance(self.name, str) or not isinstance(self.note, str):
            raise TypeError("AnalysisSet: name and note must be strings")
        object.__setattr__(self, "meta", _check_meta("AnalysisSet", self.meta))
        # not a dataclass field on purpose: it is derived, and the transport
        # does not carry it - the harness reads it to tell an explicit-name
        # collision (a defect, ras 1.4) from a default-name one (numbered).
        object.__setattr__(self, "name_given", bool(self.name))
        if not self.name:
            object.__setattr__(self, "name", "{" + ", ".join(roles) + "}")

    def __repr__(self):
        return "AnalysisSet(%d role(s): %s, name=%r)" % (
            len(self.roles), ", ".join(self.roles), self.name)


#: the layer types an adapter may return, in containment order
LAYERS = (Frame, Stack, Series, AnalysisSet, Batch)


def describe(obj):
    """One line naming what an adapter returned.  Used by the harness summary."""
    layer = getattr(type(obj), "LAYER", None)
    if layer is None:
        return "%s %s" % (type(obj).__name__, _shape_of(obj) or "")
    return repr(obj)


def _self_check():
    """Cheap sanity check that the fields did not drift from the spec table (4.3)."""
    want = {
        Frame: ["pixels", "cfa", "name", "note", "meta", "range", "layout"],
        Stack: ["pixels", "timestamps", "cfa", "name", "note", "meta", "range", "layout"],
        Series: ["stacks", "conditions", "name", "note", "meta", "range", "layout"],
        Batch: ["members", "name"],
        AnalysisSet: ["roles", "name", "note", "meta"],   # no range: a set has no pixels
        Ref: ["path", "member"],
        Values: ["values", "name", "unit"],
    }
    for cls, names in want.items():
        got = [f.name for f in _fields(cls)]
        assert got == names, "%s fields drifted: %s" % (cls.__name__, got)
    return True


if __name__ == "__main__":
    _self_check()
    print("viewer_import VERSION %d: %s"
          % (VERSION, ", ".join(c.__name__ for c in LAYERS + (Values,))))
