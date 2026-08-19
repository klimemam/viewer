#pragma once
// Watch: what "the source files changed on disk" MEANS (docs/features/watch/watch-design.md
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
//   ...and PER MEMBER is decided by the READING, not by preference. A local
//   scan stats every file, so a member is a file. A peer's LIST group row is
//   one reading of the whole set (groupObs below), so there the member IS the
//   stack and one file still growing does hold the rest back - which is a fact
//   about what an aggregated row can support, and is said out loud rather than
//   papered over.
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
    // ---- §1's REMOTE half, and the honesty constraint it carries -------------
    // A peer's LIST group row gives the SET's total bytes and its NEWEST
    // member's mtime - never a member's own state. So a remote finding can say
    // that this stack's source moved and CANNOT say which of its members did,
    // and `srcMoved` is the field that says exactly that much.
    //
    // It is a flag and not a number on purpose. `changed` is a COUNT, and a
    // count is precisely what an aggregate reading cannot produce: filling it
    // with N would print "5 file(s) changed on disk" about a row that never
    // claimed anything of the sort, and filling it with 1 would invent a file.
    // The two never both appear - remoteFinding() drops the count when it
    // raises the flag.
    bool srcMoved = false;
    // Whose disk this is ABOUT, as a person reads it (peerLabel's answer, so a
    // peer running here says "this machine" rather than nothing). "" = local,
    // and a local line names no machine because there is only one.
    std::string host;
    // How many files the row covers. The row DOES carry this - it is the member
    // list's length - and it is what makes "which of them is not knowable" a
    // bounded statement instead of a shrug.
    int members = 0;
    bool any() const { return changed || appeared || vanished || srcMoved; }
};
inline bool operator==(const Finding& a, const Finding& b) {
    return a.changed == b.changed && a.appeared == b.appeared &&
           a.vanished == b.vanished && a.firstChanged == b.firstChanged &&
           a.firstAppeared == b.firstAppeared && a.firstVanished == b.firstVanished &&
           a.srcMoved == b.srcMoved && a.host == b.host && a.members == b.members;
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

// ---- §1's remote half: ONE LIST group row is ONE reading of a whole stack ---
//
// The peer already answers this question. For a numbered sequence LIST returns a
// SYNTHETIC group row carrying the set's summed bytes, its newest member's mtime
// and the full member name list (core/serve.cpp putGroupEntryV3), so one request
// on the watched directory decides a whole stack and no new protocol is needed.
//
// Turning that row into an Obs is what makes the rule ONE rule: observe() folds
// it exactly as it folds a directory scan. What differs is what the numbers
// MEAN, and it is worth being blunt about:
//
//   THE NAMES ARE PER-MEMBER TRUTH. The row lists every member, so a file that
//   appeared and a file that is gone are named as precisely as a local scan
//   names them. That half needs no hedge and gets none.
//
//   THE (mtime, size) IS NOT. It is the SET's, so every member is handed the
//   SAME pair here - which is not a shortcut but the shape of the evidence.
//
//   §4 THEREFORE SETTLES THE STACK AS ONE UNIT. "Two readings that agree" is
//   answered for the aggregate rather than per file: one member still being
//   written on the peer holds back the whole stack's finding, where a local
//   stat releases the settled members beside it (--watch-selftest W9 against
//   --rwatch-selftest R7). A limit of one reading of N files, not a policy.
inline Obs groupObs(uint64_t bytes, int64_t mtime, std::vector<std::string> names) {
    Obs o;
    o.reserve(names.size());
    for (std::string& n : names) o.push_back({ std::move(n), mtime, bytes });
    normalize(o);
    return o;
}

// What a finding folded from an AGGREGATED reading may honestly say. `raw` is
// what observe() computed over a groupObs; the conversion drops the `changed`
// COUNT (see Finding::srcMoved) and keeps appeared/vanished exactly as they are,
// because those came off the member name list and are per-member facts.
//
// `members` is how many members the reading covered, and `host` is what to call
// the machine. Nothing to say converts to nothing to say: a Finding{} compares
// equal to the previous one and costs the UI no wake at all (§3).
inline Finding remoteFinding(const Finding& raw, const std::string& host, int members) {
    Finding f;
    if (!raw.any()) return f;
    f.srcMoved = raw.changed > 0;
    f.appeared = raw.appeared;  f.firstAppeared = raw.firstAppeared;
    f.vanished = raw.vanished;  f.firstVanished = raw.firstVanished;
    f.host = host;
    f.members = members;
    return f;
}

// The sentence §5 puts under the stack's header row, amber, and the ONE string
// both the panel and the selftests read (seqReloadNote's discipline: the guard
// asserts the wording that is drawn, not a second opinion beside it).
// "" = nothing to say.
//
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
    std::vector<std::string> parts;
    // §1's honesty constraint, spelled where the screen reads it. A local stack
    // is stat'ed per file, so its count is a real count and it names the file
    // when there is one. An aggregated remote reading has neither, and this
    // clause says so OUTRIGHT rather than leaving the reader to carry the local
    // precision across: the count it does have is how many files the row covers,
    // and "which of them" is the thing a peer listing does not answer.
    if (f.srcMoved)  parts.push_back("source changed");
    if (f.changed)   parts.push_back(countPhrase(f.changed, f.firstChanged,
                                                 "changed on disk"));
    if (f.appeared)  parts.push_back(countPhrase(f.appeared, f.firstAppeared,
                                                 "appeared in the folder"));
    if (f.vanished)  parts.push_back(countPhrase(f.vanished, f.firstVanished,
                                                 "no longer exist"));
    // WHERE, said ONCE and on the first clause: every clause of one finding is
    // about one stack, and a peer named after each of them reads as three
    // machines. A local finding names nothing - there is only one disk here.
    if (!f.host.empty()) parts[0] += " on " + f.host;
    if (f.srcMoved)
        parts[0] += " (" + std::to_string(f.members) +
                    " file(s); a peer listing cannot say which)";
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : ", ") + p;
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

// ---- §9: the line an AUTOMATIC reload leaves on the stack -------------------
//
// The same discipline as findingText and rebuildText - ONE spelling, read by the
// Files panel and by --watch-selftest - but the reason it exists is different
// and is worth stating, because it is the whole of what makes §9 safe:
//
//   NOBODY PRESSED ANYTHING. A manual Reload has a click behind it, so its toast
//   is a receipt for something the reader already knows happened. An automatic
//   one has no click, so the toast is the ONLY notice - and a toast expires.
//   The record therefore has to outlive it, and it carries the reload's WHOLE
//   summary verbatim rather than a shortened version of it: `summary` is what
//   reloadStackFromDisk returned, which already states the frames re-read, the
//   refusals with #56's mark, the membership names and the n-of-N. A second,
//   friendlier wording of that would be a second opinion about what happened.
//
//   WHEN, because "a number moved" is unanswerable without it. Local wall clock,
//   the same as SeqInfo::reloadWhen.
//
// `pinDropped` is the one thing the summary genuinely does not carry. A compare
// pin is a uid - a membership identity - so a rebuild that renumbers frames
// leaves it exactly where it was (§12.5); but a pin on a frame whose FILE IS GONE
// goes the way closing that frame takes it, and that is a comparison ending with
// nobody's click on it. It gets a clause of its own.
inline std::string autoReloadText(const std::string& when, const std::string& summary,
                                  bool pinDropped) {
    std::string s = "auto-reloaded " + when + " - " + summary;
    if (pinDropped) s += " - the compare pin was on a frame that is gone";
    return s;
}

}   // namespace watch
