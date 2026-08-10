#pragma once
// Watch: what "the source files changed on disk" MEANS (docs/watch-design.md
// §1 and §4), decided with no clock, no filesystem and no thread.
//
// Everything in this header is a pure function of the readings it is handed.
// That is the whole point of it being its own file: the interval a real watcher
// polls at is seconds to low tens of seconds (§2), and a selftest that had to
// live through two of them would be a slow test that fails intermittently on a
// loaded machine. The caller decides WHEN a reading is taken; this decides WHAT
// is true of it, and --watch-selftest drives four polls in a microsecond.
// (The precedent is rbCursorFollow, extracted from the Browse panel for exactly
// this reason - a NOGL test calls the function the panel calls.)
//
// The two rules that are easy to get wrong, and that this file exists to state
// once:
//
//   NOT-EQUAL, never NEWER (§1). A file is changed when its (mtime, size) is
//   not equal to the baseline. A copy, a restore from backup and an rsync all
//   move mtime BACKWARDS, and a watcher that asks "is it newer?" reports
//   nothing at all for the most common way a capture folder is refilled.
//
//   TWO READINGS THAT AGREE (§4). A differing reading is a CANDIDATE, not a
//   finding. Only when the same (mtime, size) is read twice in a row is
//   anything said. A file being written grows between polls and therefore never
//   agrees with itself, so it is never grabbed - and nothing intermediate is
//   put on screen either ("writing..." would be a claim about a fact that has
//   not settled). The rule is applied PER MEMBER: one file still being written
//   must not hold back the finding about a file beside it that has settled.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace watch {

// One member of a watched set, as a directory listing (or a peer's LIST row)
// reports it. `name` is the member's name WITHIN the set - a basename for a
// folder stack - never a full path: the set is identified by its directory, and
// carrying the directory on every member would make two spellings of one folder
// two different sets.
struct Ent {
    std::string name;
    int64_t  mtime = 0;
    uint64_t size  = 0;
};
// The disk state of one member. NOT operator== on Ent: the name is identity,
// (mtime,size) is state, and conflating the two is how "did this file change?"
// turns into "is this the same file?".
inline bool sameState(const Ent& a, const Ent& b) {
    return a.mtime == b.mtime && a.size == b.size;
}

// One reading of a whole watched set. Sorted by name and unique - normalize()
// is what puts it that way, and every function below assumes it has run.
using Obs = std::vector<Ent>;

inline void normalize(Obs& o) {
    std::sort(o.begin(), o.end(),
              [](const Ent& a, const Ent& b) { return a.name < b.name; });
    o.erase(std::unique(o.begin(), o.end(),
                        [](const Ent& a, const Ent& b) { return a.name == b.name; }),
            o.end());
}
// Two readings of a set, member for member. Used to notice that the BASELINE
// moved - which happens exactly once per Reload, when the loader re-stats the
// files it just re-read (see watchPollRound's re-seed).
inline bool sameSet(const Obs& a, const Obs& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++)
        if (a[k].name != b[k].name || !sameState(a[k], b[k])) return false;
    return true;
}
inline const Ent* find(const Obs& o, const std::string& n) {
    auto it = std::lower_bound(o.begin(), o.end(), n,
                               [](const Ent& e, const std::string& s) { return e.name < s; });
    return (it != o.end() && it->name == n) ? &*it : nullptr;
}
inline Ent* findMut(Obs& o, const std::string& n) {
    return const_cast<Ent*>(find(const_cast<const Obs&>(o), n));
}

// What a watched set has to SAY once §4 has settled. Counts, because the line
// on screen states numbers ("3 file(s) changed on disk"), and one name per
// kind, because a finding about a single file that cannot name it is not worth
// a line. Three kinds and not one: a rewritten frame, a frame that appeared and
// a frame that is gone are three different things to do about it.
struct Finding {
    int changed = 0, appeared = 0, vanished = 0;
    std::string firstChanged, firstAppeared, firstVanished;
    bool any() const { return changed || appeared || vanished; }
};
inline bool operator==(const Finding& a, const Finding& b) {
    return a.changed == b.changed && a.appeared == b.appeared &&
           a.vanished == b.vanished && a.firstChanged == b.firstChanged &&
           a.firstAppeared == b.firstAppeared && a.firstVanished == b.firstVanished;
}
inline bool operator!=(const Finding& a, const Finding& b) { return !(a == b); }

// The per-set state machine. One of these per watched stack, owned by the
// worker that polls (never by SeqInfo: SeqTable reseats its storage on every
// add and remove, so a worker holding a SeqInfo* holds a dangling pointer by
// design - see App::SeqTable. What §5 DRAWS is copied onto the SeqInfo by the
// UI thread; the baseline and the candidate stay here).
struct SetState {
    // §1: the baseline. Seeded from FrameSource::mtime/fsize where the loader
    // recorded one; where it did not (a stack opened before Watch existed, a
    // source that never got a stat), the FIRST observation makes it - and that
    // first observation says nothing, because a stack whose baseline is its own
    // first poll has no evidence that anything moved. "Do not forge the state
    // at the moment it was opened" is the rule, and an empty baseline is how
    // that rule is spelled.
    bool haveBase = false;
    Obs  base;
    // §4: the previous reading, and what §4 has therefore CONFIRMED. Two
    // vectors and not one, because the rule needs both halves: `prev` is the
    // candidate ("this is what I read last time"), `conf` is the settled truth
    // ("this is what I have read twice").
    //
    // conf is what the finding is computed from, and that is what makes the
    // rule symmetric. Computing it from the raw reading instead makes the line
    // FLICKER on a file that is rewritten in place: the first poll of the
    // second write disagrees with the poll before it, the member drops out of
    // the reckoning for one round, and the finding it had already earned
    // vanishes and comes back. A member that has not settled keeps saying
    // whatever it last settled on - which for a member that has never settled
    // is the baseline, i.e. nothing.
    bool havePrev = false;
    Obs  prev, conf;
    // What is CONFIRMED right now. Compared against the next round's result, so
    // the UI is woken when the STATEMENT changes and never merely because a
    // poll happened (§3: polling must not cost an idle frame).
    Finding found;
};

// Adopt a baseline that is already known - FrameSource::mtime/fsize, recorded
// by the loader when it decoded these pixels (§1). Nothing has been read since,
// so what is confirmed IS the baseline: a set nobody has looked at has, by
// construction, nothing to report.
inline void seedBaseline(SetState& st, Obs base) {
    normalize(base);
    st.base = base;
    st.conf = std::move(base);
    st.haveBase = true;
    st.havePrev = false;
    st.prev.clear();
    st.found = Finding{};
}

// Fold one observation into st.
//
// Returns true exactly when what this set has to say CHANGED - a new finding, a
// finding that grew, or a finding that went away because the file was put back.
// That return value is the wake signal (§3), so "nothing moved" costs the UI
// thread nothing at all.
inline bool observe(SetState& st, Obs obs) {
    normalize(obs);
    if (!st.haveBase) {                 // §1: the first poll IS the baseline
        seedBaseline(st, obs);
        st.prev = std::move(obs);
        st.havePrev = true;
        return false;                   // ...and it announces nothing
    }
    if (!st.havePrev) {                 // a seeded baseline meeting its first
        st.prev = std::move(obs);       // reading: a candidate, never a fact
        st.havePrev = true;
        return false;
    }
    // §4, per member and in BOTH directions: a reading becomes confirmed when
    // the poll before it read the same thing. A file being written grows
    // between polls, so it never agrees with itself and never settles.
    bool grew = false;
    for (const Ent& e : obs) {
        const Ent* p = find(st.prev, e.name);
        if (!p || !sameState(*p, e)) continue;
        if (Ent* c = findMut(st.conf, e.name)) *c = e;
        else { st.conf.push_back(e); grew = true; }
    }
    if (grew) normalize(st.conf);
    // ...and confirmed ABSENCE, by the same rule. A file being replaced
    // (delete, then write) is missing for a single poll and must not be
    // announced as lost.
    for (auto it = st.conf.begin(); it != st.conf.end();) {
        const bool gone = !find(obs, it->name) && !find(st.prev, it->name);
        it = gone ? st.conf.erase(it) : std::next(it);
    }
    st.prev = std::move(obs);

    Finding f;
    for (const Ent& c : st.conf) {
        const Ent* b = find(st.base, c.name);
        if (!b) {
            f.appeared++;
            if (f.firstAppeared.empty()) f.firstAppeared = c.name;
        } else if (!sameState(*b, c)) {
            // NOT-EQUAL (§1). Deliberately not `c.mtime > b->mtime`: a copy, a
            // restore and an rsync all move mtime backwards.
            f.changed++;
            if (f.firstChanged.empty()) f.firstChanged = c.name;
        }
    }
    for (const Ent& b : st.base)
        if (!find(st.conf, b.name)) {
            f.vanished++;
            if (f.firstVanished.empty()) f.firstVanished = b.name;
        }
    if (f == st.found) return false;
    st.found = f;
    return true;
}

// The sentence §5 puts under the stack's header row, amber, and the ONE string
// both the panel and --watch-selftest read (seqReloadNote's discipline: the
// guard asserts the wording that is drawn, not a second opinion beside it).
// "" = nothing to say.
//
// Members are named because a LOCAL stack is stat'ed per file, so the count is
// a real count. The remote half of §1 cannot do this - a peer's LIST group row
// carries the set's total bytes and its newest mtime, never per-member state -
// and the sentence it will need is deliberately not written here yet: the
// remote origin is not watched in this stage, and an unused branch claiming to
// know how it reads would be the kind of second opinion this function exists to
// avoid.
// "3 file(s) changed on disk", "1 file(s) no longer exist (w_004.npy)". ONE
// spelling of this phrase, because §5's line and §6's Reload summary say the
// same things about the same files and two spellings of it would drift.
inline std::string countPhrase(int n, const std::string& first, const char* what) {
    std::string s = std::to_string(n) + " file(s) " + what;
    if (n == 1 && !first.empty()) s += " (" + first + ")";
    return s;
}

inline std::string findingText(const Finding& f) {
    if (!f.any()) return {};
    std::string s;
    if (f.changed)  s += countPhrase(f.changed,  f.firstChanged,  "changed on disk");
    if (f.appeared) s += (s.empty() ? "" : ", ") +
                         countPhrase(f.appeared, f.firstAppeared, "appeared in the folder");
    if (f.vanished) s += (s.empty() ? "" : ", ") +
                         countPhrase(f.vanished, f.firstVanished, "no longer exist");
    return s;
}

// ---- §6: the MEMBERSHIP a Reload rebuilds -----------------------------------
// Everything above answers "did the files move?". This answers "which files is
// this stack made of NOW?", and it is the same kind of thing: a decision about
// names, taken with no filesystem, no decode and no stack, so that the test
// which pins it needs neither a folder nor a poll interval.
//
// Three name lists, which are three different questions, and keeping them apart
// is the whole content of the function:
//
//   have    the stack's members, in the stack's own order
//   listed  what the directory holds RIGHT NOW (watchScanDir's names)
//   want    the stack's OWN rule, re-applied to that listing - for a folder
//           stack, siblingNamesAmong(head, listed), the very function the open
//           used (§11.3 split it out so both callers can have it)
//
// A member LEAVES only when its FILE IS GONE (absent from `listed`), never
// merely because `want` stopped naming it. Those are not the same statement:
// siblingNamesAmong picks the frame axis as the last digit field that VARIES,
// so one new file in a folder can move that choice and un-name members whose
// files are sitting right there. Dropping those would delete live frames from a
// measurement on the strength of a naming heuristic. A frame that is gone is not
// a frame the rule went off.
//
// A member JOINS when `want` names it and the stack has not got it. `want` is
// where §11.4 lives: a stack is not always its folder's whole group (a derived
// subset is not, and its membership was a decision made at derive time that §6
// says is never re-applied), and handing such a stack the folder's group here
// would re-admit exactly the frames its rule excluded. The CALLER decides what
// this stack's rule is; an empty `want` means "no rule to re-apply", and then
// the only thing that can happen is a departure.
struct Rebuild {
    std::vector<std::string> order;      // the membership AFTER, in stack order
    std::vector<std::string> added;      // ...the names that joined
    std::vector<std::string> dropped;    // ...and those whose file is gone
    bool any() const { return !added.empty() || !dropped.empty(); }
};

// `less` is the ONE comparator a stack is ever built with - rp::naturalLess, by
// way of sortFramesNumerically ("frame order is a fact, not a sort"). It is
// passed in rather than reimplemented here for the reason that function's own
// comment gives: a second opinion about frame order is a second measurement.
inline Rebuild planRebuild(const std::vector<std::string>& have,
                           const std::vector<std::string>& listed,
                           const std::vector<std::string>& want,
                           bool (*less)(const std::string&, const std::string&)) {
    Rebuild r;
    auto has = [](const std::vector<std::string>& v, const std::string& n) {
        return std::find(v.begin(), v.end(), n) != v.end();
    };
    for (const std::string& n : have) {
        if (has(listed, n)) r.order.push_back(n);
        else if (!has(r.dropped, n)) r.dropped.push_back(n);
    }
    for (const std::string& n : want) {
        // `listed` is checked even though `want` is derived from it: a caller
        // that builds `want` from anything else must not be able to graft a
        // name that is not on the disk.
        if (has(have, n) || has(r.added, n) || !has(listed, n)) continue;
        r.added.push_back(n);
        r.order.push_back(n);
    }
    if (less) {
        // The order is the OPEN's order, re-derived - not "the old members, then
        // the new ones". A file that appears into 000/001/003/004 is frame 2 and
        // has to be numbered 2; appending it would make it frame 4 and silently
        // re-label the two frames after it.
        std::sort(r.order.begin(), r.order.end(), less);
        std::sort(r.added.begin(), r.added.end(), less);
        std::sort(r.dropped.begin(), r.dropped.end(), less);
    }
    return r;
}

// The sentence the Reload summary carries, and THE guard for it (findingText's
// discipline: the panel and the selftest read one string).
//
// BOTH counts, always. A rebuild that changed the member list and did not say
// the new numbers leaves a stack quietly measuring a different set - and the
// count alone is not the fact either: one file gone and one arrived leaves the
// count where it was, so the names come first and the numbers after.
// `resident` of `expected` is the same n-of-N the stack's own header states.
inline std::string rebuildText(const Rebuild& r, int before, int resident, int expected) {
    if (!r.any()) return {};
    std::string s = "membership rebuilt: ";
    std::string parts;
    if (!r.added.empty())
        parts += countPhrase((int)r.added.size(), r.added.front(),
                             "appeared in the folder");
    if (!r.dropped.empty())
        parts += (parts.empty() ? "" : ", ") +
                 countPhrase((int)r.dropped.size(), r.dropped.front(), "no longer exist");
    return s + parts + " - " + std::to_string(resident) + " of " +
           std::to_string(expected) + " frame(s), was " + std::to_string(before);
}

}   // namespace watch
