// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ImageView
// One heading in the Files panel: a batch, the stacks under it, and the folder
// its first stack came from. Grouped by BATCH, not by folder - a batch is one
// open action, named by the user (the folder name is only the starting value),
// and two loads from the same folder are two batches, which is the point.
struct FileGroup {
    int batch = 0;
    std::string dir, label;
    std::vector<const std::vector<int>*> stacks;
};
// Extracted from drawFileList so the ORDER can be asserted without a frame: it
// is the thing "Move to batch" must not change.
//
// The order is app.batches', which is the order the batches were CREATED in.
// It used to be the order they were first met while walking the stacks - and
// that is a property of the stacks, not of the batches. Moving one stack into
// another batch moved that batch's whole heading to wherever the moved stack
// happened to sit, which for a move "upwards" is the top of the list, so every
// other heading shifted and the user had to find their work again. A move
// changes which heading a row sits UNDER. It must not reorder the headings.
static std::vector<FileGroup> buildFileGroups(const std::vector<std::vector<int>>& stacks) {
    std::vector<FileGroup> groups;
    for (const auto& stack : stacks) {
        const ImageDoc& head = *app.images[stack.front()];
        FileGroup* g = nullptr;
        for (auto& q : groups) if (q.batch == head.batchId) { g = &q; break; }
        if (!g) {
            const std::string& p = head.src->path;
            size_t slash = p.find_last_of("/\\");
            std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
            std::string label = "opened";
            for (const auto& b : app.batches) if (b.id == head.batchId) label = b.name;
            groups.push_back({ head.batchId, dir, label, {} });
            g = &groups.back();
        }
        g->stacks.push_back(&stack);
    }
    auto rank = [](int id) {
        for (size_t i = 0; i < app.batches.size(); i++)
            if (app.batches[i].id == id) return (int)i;
        return (int)app.batches.size();    // no batch names it: last, order kept
    };
    std::stable_sort(groups.begin(), groups.end(),
                     [&](const FileGroup& a, const FileGroup& b) {
                         return rank(a.batch) < rank(b.batch);
                     });
    // The transient preview is PINNED LAST and never reorders the rest.
    // It used to sort in wherever its batch was created, so glancing at a
    // file in the browser inserted a heading in the middle of the list and
    // pushed everything the user was working with down a row - the opposite
    // of what a throwaway look should cost.
    for (size_t i = 0; i + 1 < groups.size(); i++)
        if (groups[i].label == "preview") { std::rotate(groups.begin() + i,
                                                        groups.begin() + i + 1,
                                                        groups.end()); break; }
    return groups;
}

// Resolve a Series::Member to the ONE Files row that owns its identity. A
// standalone frame cannot go through the seqId lookup: its seqId is zero, as
// is every other loose frame's, so doing that repeats whichever loose frame
// happens to be first and never reaches the member's uid.
static const std::vector<int>* filesRowOfSeriesMember(
        const FileGroup& group, const App::Series::Member& member) {
    for (const auto* row : group.stacks) {
        if (!row || row->empty()) continue;
        const ImageDoc& head = *app.images[row->front()];
        if (member.seqId != 0) {
            if (head.seqId == member.seqId) return row;
        } else if (head.seqId == 0 && head.uid == member.frameUid) {
            return row;
        }
    }
    return nullptr;
}

// Resolve one member to the image row that navigation should select. Series
// roles use the first live member in declaration order; a standalone frame is
// identified by frameUid, never by framesOfSeq(0) (which denotes loose rows).
static int filesImageIndexOfSeriesMember(const App::Series::Member& member) {
    if (member.seqId != 0) {
        std::vector<int> frames = framesOfSeq(member.seqId);
        return frames.empty() ? -1 : frames.front();
    }
    if (member.frameUid != 0)
        for (int i = 0; i < (int)app.images.size(); i++)
            if (app.images[i]->uid == member.frameUid) return i;
    return -1;
}

static int filesImageIndexOfSeries(const App::Series& series) {
    for (const auto& member : series.members) {
        const int imageIdx = filesImageIndexOfSeriesMember(member);
        if (imageIdx >= 0) return imageIdx;
    }
    return -1;
}

static bool filesRowBelongsToSeries(const ImageDoc& head) {
    return head.seqId != 0 ? seriesOfStack(head.seqId) != nullptr
                           : seriesOfFrame(head.uid) != nullptr;
}

// What letters each Files row carried on the last draw, "name=letters;" per
// badged row. The panel is OpenGL this machine cannot screenshot, so the
// letters are asserted as state (--abstats-selftest S4), like g_tilePanesDrawn.
static std::string g_filesBadgeProbe;

// ---- WHAT ONE STACK's BLOCK DREW, and HOW TALL it was -----------------------
// The 2026-09-28 report 「reload で行が増えるのは嫌」: Watch's finding used to be
// drawn as an extra amber LINE under the stack's row, and a second one under
// that when an automatic reload had run - so a poll moved every stack below.
// The claim of the fix is that nothing is added, and the only honest way to
// assert "nothing is added" is to measure the block: `h` is the pixels this
// stack occupied, so a finding that puts a line back CHANGES it whatever the
// line says. `label` is what the row said, "##" and up cut off the way ImGui
// cuts it, so the TOKEN is asserted as drawn text rather than as a string a
// test built for itself. g_filesBadgeProbe's shape and its reason (the panel is
// OpenGL this machine cannot screenshot); --verify-selftest V26 reads it.
// `bar` is the amber left-edge bar, and it is in the probe for the reason the
// height is (review P2-6): watch-design §5 states the bar's behaviour as a
// RULE - "a red outline plus an amber bar = the generations are mixed AND the
// disk is still moving, and only one Reload in which every frame reads clears
// both" - and a rule nothing asserts is a rule that drifts. A colour is also
// the one thing a probe can see that a reader of the label cannot.
struct FilesStackRowProbe {
    int seqId = 0;
    std::string label;
    std::string tip;                   // what the row's tooltip was BUILT from
    float h = 0;
    bool bar = false;
};
static std::vector<FilesStackRowProbe> g_filesStackRowProbe;
// ...and the last row's tooltip text, set by rowWithMeta as it builds one. The
// stack row reads it back into its own probe entry (review P3-19).
static std::string g_filesRowTipProbe;
// ...and the panel's BUSY row while a reload is in flight ("reloading <stack>:
// 3 / 12 - 1.2 s"), recorded where it is drawn for the same reason. "" = no
// reload was running on the last draw, which is itself what V29 asserts once
// the job has landed.
static std::string g_filesBusyRowProbe;

// §4: the share mark. U+29C9 (two joined squares) when the atlas has it - the
// CJK fonts jpFontChoice picks vary here - else the plain word, which is longer
// but never a '?' box. Checked once: the font file does not change at runtime.
static const char* shareGlyph() {
    static int have = -1;
    if (have < 0) {
        ImFont* f = ImGui::GetFont();
        have = (f && f->FindGlyphNoFallback((ImWchar)0x29C9)) ? 1 : 0;
    }
    return have ? "\xE2\xA7\x89" : "";
}

// §4.1: the characters of a NAME this atlas cannot draw (板306 D).
//
// A code point the font does not hold renders as the fallback '?' - a half
// width question mark, not a tofu box, because none of the candidate fonts
// carry U+FFFD either and ImGui's FallbackChar is '?'. So 测试1.npy and
// 试测1.npy both read "??1.npy": two different files, identical on screen, in
// the Files panel and in every tooltip, message and export header built from
// the same name. Nothing in the program is wrong at that point - the bytes are
// right, the file opens, the pixels are the file's - and the one thing the user
// cannot do is tell the two rows apart.
//
// THIS NOTICE IS NEEDED EVEN THOUGH THE RANGES WERE WIDENED (板306 A), and the
// reason is that the two fix different things. Widening the ranges removes the
// characters this build forgot to ASK for. What is left over is the characters
// the FONT does not have - Meiryo has 1819 of the 2500 common simplified
// Chinese characters, measured - and every code point outside the BMP, which
// does not fit in ImWchar at all and so can never be asked for by this build of
// ImGui. Those cannot be removed by asking for more; they can only be declared.
//
// WHAT IS DELIBERATELY NOT DONE: the name on screen is never rewritten. An
// escaped row reading "测试1.npy" is a row nobody can match against
// their own folder, which trades one unreadable string for another. The name
// stays exactly as the file gave it and a COUNT is added beside it, in the two
// places a reader is already looking - the row's tooltip and one Messages line
// per document.
//
// Same shape as shareGlyph() above: it asks the BUILT ATLAS rather than the
// range table, because what the user sees is what was baked and nothing else.
static ImFont* bakedFont() {
    if (!ImGui::GetCurrentContext()) return nullptr;
    // ImGui::GetFont() is only set from NewFrame() onwards, so a windowless run
    // - every NOGL selftest - would see no font at all and could not tell "this
    // name cannot be drawn" from "nothing has been baked". The atlas itself is
    // built at start-up either way (fontAtlasBake), so ask it directly.
    if (ImFont* f = ImGui::GetFont()) return f;
    ImFontAtlas* a = ImGui::GetIO().Fonts;
    return (a && a->Fonts.Size) ? a->Fonts[0] : nullptr;
}
// THE CAUSES ARE COUNTED SEPARATELY (review P2-6), because one number forces
// one sentence and that sentence is false for all but one of them. (This used
// to open by counting the causes, and the count was stale; the field list below
// is the set, and C4 asserts that every field in it has a sentence - see the
// note on operator+=. A comment that counts the code is a second copy of the
// code, including in the sentence that says not to write one, which is where
// the last two went.) A different font fixes `noGlyph` and cannot touch the
// others: `tooHigh` is a
// property of THIS BUILD (ImWchar is 16-bit here, so a code point above U+FFFF
// cannot even be asked for, whatever font is installed), and `badBytes` is a
// property of the FILENAME ON DISK. Telling somebody to change their font
// because a file has an emoji in it is advice that cannot work.
// Do the bytes at `p` form a WELL-FORMED UTF-8 sequence for a code point above
// U+FFFF? Written out by hand because it is the one question ImGui's decoder
// cannot answer on this build: with a 16-bit ImWchar it reports every such
// sequence as an error, so "valid bytes this build cannot hold" and "invalid
// bytes" arrive identical. Only the 4-byte forms can exceed U+FFFF, and the
// overlong / surrogate / out-of-range rules are checked on the VALUE, because
// a byte pattern can be correct and still not be a character.
static bool utf8SequenceAbove16Bit(const char* p, const char* e) {
    const unsigned char* u = (const unsigned char*)p;
    if (e - p < 4) return false;
    if (u[0] < 0xF0 || u[0] > 0xF4) return false;
    for (int k = 1; k <= 3; k++)
        if (u[k] < 0x80 || u[k] > 0xBF) return false;
    const uint32_t cp = ((uint32_t)(u[0] & 0x07u) << 18) |
                        ((uint32_t)(u[1] & 0x3Fu) << 12) |
                        ((uint32_t)(u[2] & 0x3Fu) << 6) |
                        (uint32_t)(u[3] & 0x3Fu);
    return cp > 0xFFFF && cp <= 0x10FFFF;
}

struct UndrawableTally {
    int noGlyph = 0;        // ASKED FOR, and the font does not have it
    int notAsked = 0;       // in no range this build asks for (review P3)
    int tooHigh = 0;        // above U+FFFF: not representable in this build
    int badBytes = 0;       // the name is not valid UTF-8
    int total() const { return noGlyph + notAsked + tooHigh + badBytes; }
    // ADDING TWO TALLIES BELONGS HERE, beside the fields. It used to be written
    // out field by field at the one call site, and adding `notAsked` then left
    // that site summing every cause but the new one - so a name whose only
    // problem was
    // an unrequested script tallied to zero and the row said nothing. The same
    // shape as other defects on this PR: a set assembled in one place and
    // re-derived in another.
    //
    // A NEW CAUSE IS A FIELD AND EVERY PLACE THAT WALKS THE FIELDS, and this
    // comment used to put a number on that ("one function"), which was false -
    // `total()`, this operator and undrawableNoteOf's `add` column all walk
    // them, and forgetting the last one reproduces exactly the defect the
    // paragraph above describes. A comment cannot keep a count of the code
    // honest; a test can, and C4 has one that fails when a field is added
    // without a sentence for it.
    UndrawableTally& operator+=(const UndrawableTally& o) {
        noGlyph += o.noGlyph;
        notAsked += o.notAsked;
        tooHigh += o.tooHigh;
        badBytes += o.badBytes;
        return *this;
    }
};
// ---- WHAT IS COUNTED IS A SET OF CHARACTERS, not a list of strings --------
// (review round 5, the critical finding - and it is round 3's critical finding
// a second time, on the arm next door.)
//
// The contract is the one the manual states in §8c: THE SAME CHARACTER IS
// NEVER COUNTED TWICE. For two rounds that was implemented by trying to make
// the STRINGS handed in disjoint - splitting the path at its last separator so
// the directory part and the leaf shared no character, whole-string dedup for
// the rest. That is reasoning about one instance, and the instances never ran
// out: `d.name` is a strict SUPERSTRING of the path's leaf for an npz member
// ("試験.npz:温度", loader_npz.inc), for a remote frame ("frame.npy #3",
// open_dispatch.inc), for a decorated preview or stale row, for a series member
// and for a stack name - and no amount of splitting the path sees any of them.
// The npz shape said "8 character(s) here draw as '?'" about 4 characters.
//
// So the dedup moves DOWN to the unit, and the pieces are allowed to overlap
// freely. `seen` carries the units already counted for one row, so a character
// that appears in the row's label, in `d.name`, in the path and in the member
// name is counted once - and every superstring case above stops being a case.
//
// THE CONSTRAINT THE UNIT HAS TO MEET, which is not a unit (review round 7):
//
//     THE NUMBER MUST BE THE NUMBER OF '?' THE READER SEES FOR THIS ROW.
//
// The sentence promises it - "N character(s) here draw as '?'" - and the unit
// has now moved three times without that equation being written down, so it is
// written down here and a test holds it (C4's `questionMarks` oracle walks the
// pieces the way the renderer walks text and counts glyph positions, with
// nothing from this function).
//
// WHAT THE RENDERER DOES, because that is what fixes the unit: ImGui's text
// drawing decodes one step at a time and emits EXACTLY ONE GLYPH PER STEP,
// advancing by whatever ImTextCharFromUtf8 consumed. An ill-formed sequence is
// one step, one glyph, however many bytes it ate - so `\xF0\x80\x80\x80` is a
// single '?' on screen and takes the three bytes after the lead with it.
//
// So there are two kinds of thing to count, and they are counted differently
// because they ARE different:
//
//   * A CHARACTER (noGlyph / notAsked / tooHigh) is counted ONCE PER DISTINCT
//     CODE POINT. The sentence counts characters, so a name with the same
//     undrawable character twice has one character the reader cannot read. The
//     key is the bytes of the sequence, which for well-formed UTF-8 is the same
//     statement as the code point (the encoding is a bijection; overlong forms
//     are ill-formed). Keying on `cp` instead would be wrong where `cp` is not
//     available: on this 16-bit build a valid 4-byte emoji comes back as
//     U+FFFD, so every emoji in one name would collapse into one character.
//   * AN ILL-FORMED RUN (`badBytes`) IS NOT A CHARACTER, and is counted ONCE
//     PER DECODE STEP, never deduplicated. There is no "the same character" to
//     apply the rule to - that is the ruling, and it is also the end of two
//     rounds of trying: round 5 keyed it on the bytes the decoder consumed and
//     round 6 on one byte, and the second made the count say 4 where the screen
//     says 1. A step is what the screen shows, so a step is what is counted.
//     manual.md §8c says the same thing in the canon.
//
// WHY NOT ONE BYTE, since that was this round's predecessor: `used` was said to
// depend on what follows the bad byte. It does not. ImTextCharFromUtf8 takes
// the length from a table indexed by the LEAD BYTE alone, so a stray 0xFF is
// one step wherever it sits and a 0xF0 with three continuation bytes is one
// step of four; the worked example in the old comment here, "(4, FF, 'a', 'b')",
// cannot occur. Advancing by `used` is not just safe, it is the only advance
// that tracks the renderer - with one byte, the ASCII the renderer swallows
// after a bad lead byte came back as separately drawable characters.
//
// A CHARACTER's key: at most 4 bytes, because ImTextCharFromUtf8 never reports
// more on either path, so the key is those bytes and their count. Ill-formed
// runs do not come here - they have no identity to key on.
static uint64_t undrawableUnitKey(const char* p, int used) {
    const int n = used < 1 ? 1 : (used > 4 ? 4 : used);
    uint64_t k = (uint64_t)(unsigned)n << 32;
    for (int i = 0; i < n; i++) k |= (uint64_t)(unsigned char)p[i] << (i * 8);
    return k;
}
// ...and WHAT `seen` IS. A hash set, not a sorted or linear vector, because
// the number of units in one row is not bounded by anything: a path is as long
// as the filesystem allows and `d.note` is a sentence. A linear scan is O(n^2)
// in the length of the name, which is fine for the 40-odd units of an ordinary
// row and is not a bound anyone enforces (review round 6 P3 - the "<= 40" was
// an estimate in a report, which is not a guarantee in a loop).
static UndrawableTally undrawableTallyUnseen(const std::string& utf8Name,
                                             std::unordered_set<uint64_t>& seen) {
    UndrawableTally t;
    ImFont* f = bakedFont();
    if (!f) return t;                  // nothing baked: nothing to claim
    const char* p = utf8Name.c_str();
    const char* e = p + utf8Name.size();
    while (p < e) {
        unsigned int cp = 0;
        const int used = ImTextCharFromUtf8(&cp, p, e);
        if (used <= 0) break;                 // only at a NUL or the end
        // ONE UNIT, COUNTED ONCE for this row - the whole of the note above.
        // Asked at the classification, not before it, because the two kinds of
        // unit have two different lengths and only the classification knows
        // which one is in front of it. Every `t.<cause>++` below is guarded by
        // this and by nothing else.
        auto fresh = [&seen](uint64_t k) { return seen.insert(k).second; };
        // A DECODE THAT FAILED is a lost character EVEN WHERE THE FONT HAS A
        // GLYPH for what came out of it, and this is the case the Ubuntu runner
        // caught. ImTextCharFromUtf8 answers U+FFFD REPLACEMENT CHARACTER for
        // three different inputs - bytes that are not UTF-8, a truncated
        // sequence, and any code point above U+FFFF (this build's ImWchar is
        // 16-bit, so an emoji cannot be represented at all) - and ImGui's own
        // GetGlyphRangesJapanese ASKS for U+FFFD ("0xFFFD, 0xFFFD // Invalid"),
        // so a font that has the glyph draws a diamond and the atlas lookup
        // below would answer "nothing wrong with this name". DejaVu Sans has
        // it; Meiryo and YaHei do not - which is why this was invisible here
        // and a failure there.
        //
        // What separates a substitution from a name that really does contain
        // U+FFFD is the BYTES: the genuine article is exactly EF BF BD.
        const bool substituted =
            cp == (unsigned)IM_UNICODE_CODEPOINT_INVALID &&
            !(used == 3 && (unsigned char)p[0] == 0xEF &&
              (unsigned char)p[1] == 0xBF && (unsigned char)p[2] == 0xBD);
        // A FAILED DECODE IS ALWAYS `badBytes`, and `used` must not be asked
        // which kind it was. This was wrong, and ImGui's own source says why -
        // imgui.cpp, ImTextCharFromUtf8, on the error path:
        //
        //     wanted = ImMin(wanted, !!s[0] + !!s[1] + !!s[2] + !!s[3]);
        //     *out_char = IM_UNICODE_CODEPOINT_INVALID;
        //
        // so the return is the number of NON-ZERO BYTES it looked at, not 1,
        // and an ILL-FORMED sequence whose first byte is in the 4-byte range
        // returns 4 as readily as a well-formed one. The old code read that 4
        // as "a real code point above U+FFFF" and told the user about emoji
        // when the truth was broken bytes - and swallowed the following three
        // bytes, ASCII included, as one character. `*out_char` cannot separate
        // them either: on every error path it is U+FFFD.
        //
        // So `used` is not asked. The two are separated by READING THE BYTES
        // AGAIN, independently: if they form a well-formed UTF-8 sequence whose
        // value is above U+FFFF, the name is fine and this BUILD cannot hold
        // the character; anything else is a name that is not UTF-8.
        //
        // That extra decode is not zeal. On this build ImWchar is 16 bits, so
        // ImTextCharFromUtf8 reports a perfectly valid emoji as an ERROR - it
        // is out of ITS range - and a split that trusted the decoder alone
        // would tell a user with 🙂 in a filename that their bytes are broken
        // and offer them a fix that cannot apply. Both sentences have to be
        // true, so both need a fact the decoder does not carry.
        if (substituted) {
            // A WELL-FORMED SEQUENCE ABOVE U+FFFF is a character this build
            // cannot hold, so it keeps the character's rule: keyed on its four
            // bytes, which encode exactly one code point, and counted once.
            if (utf8SequenceAbove16Bit(p, e)) {
                if (fresh(undrawableUnitKey(p, 4))) t.tooHigh++;
                p += 4;
                continue;
            }
            // ...and anything else is NOT A CHARACTER. One step, one glyph on
            // screen, one count - and no key, because there is nothing for two
            // of these to be "the same" as. See the note above.
            t.badBytes++;
            p += used;
            continue;
        }
        p += used;
        // CONTROL CHARACTERS ARE NOT COUNTED, and this is not a nicety: ImGui
        // does not ASK the atlas for a glyph for U+000A, it breaks the line -
        // and the glyph ranges start at U+0020, so the atlas has none. Counting
        // one as "no glyph in the font" put a false "1 character here draws as
        // '?'" on EVERY row of the Files panel the moment this function was
        // handed a multi-line string. Something the renderer never looks up
        // cannot be something the renderer cannot draw.
        if (cp < 0x20 || cp == 0x7F) continue;
        const uint64_t key = undrawableUnitKey(p - used, used);
        if (cp > 0xFFFF) {                            // ImWchar32 builds land here
            if (fresh(key)) t.tooHigh++;
            continue;
        }
        if (f->FindGlyphNoFallback((ImWchar)cp)) continue;          // it drew
        // WAS IT EVEN ASKED FOR? "Point appearance.fontPath at another font"
        // is only true for a code point this build REQUESTED and the font did
        // not supply. For one that is in no range at all - Hangul, Thai,
        // Hebrew, Devanagari - no font on the machine can help, because the
        // atlas was never going to hold it whatever is installed. Offering a
        // font as the cure there is the same kind of false instruction as the
        // rest of this round (review P3).
        if (!fresh(key)) continue;
        if (cjkRangeHas(g_cjkRangesAsked, cp)) t.noGlyph++;
        else                                   t.notAsked++;
    }
    return t;
}
// ONE string on its own, with nothing counted before it.
//
// The set is REUSED rather than built per call: the Messages sweep asks this
// once per document and a stack is thousands of documents, so a fresh hash
// table per document is thousands of allocation pairs per sweep for a table
// that is thrown away immediately. One buffer, cleared at entry. Not
// re-entrant and not thread-safe, which it does not need to be - every caller
// is the UI thread or a selftest running in its place - and said here because
// a `static` in a leaf function is exactly the kind of thing a later caller
// breaks by accident.
static std::unordered_set<uint64_t>& undrawableSeenBuf() {
    static std::unordered_set<uint64_t> buf;
    buf.clear();
    return buf;
}
static UndrawableTally undrawableTally(const std::string& utf8Name) {
    return undrawableTallyUnseen(utf8Name, undrawableSeenBuf());
}
static int undrawableCount(const std::string& utf8Name) {
    return undrawableTally(utf8Name).total();
}
// The sentence the tally turns into. ONE function, so the tooltip and the
// Messages line cannot drift into two different claims about one name.
//
// Built cause by cause, and the WAY OUT is only offered for the cause it can
// actually fix (review P2-6): "point appearance.fontPath at another font" is
// true for a glyph the font is missing, and false for an emoji and for broken
// bytes - no font on earth changes either of those. (This function has never
// had a cache; the sentence claiming one was copied from undrawableNote, which
// no longer has one either - review round 6 P3.)
static std::string undrawableNoteOf(const UndrawableTally& t) {
    std::string s;
    if (t.total() > 0) {
        char b[256];
        std::string parts;
        auto add = [&](int n, const char* what) {
            if (n <= 0) return;
            if (!parts.empty()) parts += ", ";
            snprintf(b, sizeof b, "%d %s", n, what);
            parts += b;
        };
        add(t.noGlyph,  "with no glyph in this font");
        add(t.notAsked, "in a script this build does not load glyphs for");
        add(t.tooHigh,  "outside the range this build can draw at all "
                        "(emoji and the rarer CJK extensions)");
        add(t.badBytes, "whose bytes are not valid UTF-8");
        snprintf(b, sizeof b, "%d character(s) here draw as '?' - %s. ",
                 t.total(), parts.c_str());
        s = b;
        s += "Two different files can look identical like this. The name on "
             "disk is untouched.";
        // The only cause a different font can fix. notAsked
        // needs this build to request the script, tooHigh needs a wider
        // ImWchar, and badBytes is the name on disk.
        if (t.noGlyph > 0)
            s += " For the missing glyphs, \"appearance.fontPath\" "
                 "(settings.jsonc) points at another font; it takes effect at "
                 "the next start.";
    }
    return s;
}
// THE LINE THAT STANDS IN FOR THE PATH, in one place, because two things need
// the same answer: the tooltip draws it and undrawablePieces() counts it. A
// COMPUTED frame has no path at all - its provenance note takes the path's
// place, which is what the note is for - so "the path" is not the answer to
// "what does this row say it is".
//
// IT IS NOT "THE TOOLTIP'S FIRST LINE", which is what this was called for one
// round (review round 7 P3). The drawer puts things ABOVE it - a (stale)
// warning, Watch's finding, a reload note - and things below it, so on those
// rows the first line is one of those instead. A function made so that two
// surfaces cannot disagree should not itself be named after something it is
// not.
//
// The strings prefixed above it are deliberately NOT pieces: they are this
// program's own sentences, and the one of them that carries a name from
// somewhere else ("same picture as ...", another row's name) was raised in
// round 4 and dropped there as a finding. If that is revisited, this is the
// list to add it to - which is the whole point of the list.
static std::string filesRowPathLine(const ImageDoc& d) {
    return d.src->path.empty() && !d.note.empty() ? d.note : dispPath(d.src->path);
}
// ---- WHICH STRINGS A ROW SHOWS, in ONE place (review: sweep the shape) -----
// Found by sweeping "a caller counts a different set of strings from the other
// callers": the tooltip had been moved to an explicit list while the Messages
// sweep still counted only the row's name, so a row whose PATH carried
// undrawable characters got a tooltip note and no Messages line - the two
// surfaces saying different things about the same row, which is the state this
// list exists to prevent. Both callers now build their pieces here.
//
// The label is passed in because only the caller knows it: the tooltip has
// ImGui's "name##id" string, the Messages sweep has the stack's name.
// THE PIECES DO NOT HAVE TO BE DISJOINT, and TRYING TO MAKE THEM SO IS THE
// MISTAKE THIS LIST MADE TWICE (review round 5).
//
// The first version added both `d.name` and the whole path and counted the
// file's own name twice. The second split the path at its last separator so
// that the directory part and the leaf shared no character, and deduplicated
// whole strings for the rest - which is reasoning about the instances in front
// of it, and the instances did not run out: `d.name` is a strict SUPERSTRING
// of the leaf for an npz member, a remote frame, a decorated preview or stale
// row, a series member and a stack name, and whole-string dedup sees none of
// those. "試験.npz:温度" said 8 about 4 characters.
//
// So this list is now just THE STRINGS THIS ROW SHOWS, overlapping however
// they like. What makes the count right is one level down, in
// undrawableTallyUnseen(): the unit is counted once per row whatever string it
// came in. Add a new string here freely - that is the whole point of the
// change - and note that nothing here needs to know how the strings relate.
//
// "SHOWS" IS THE WHOLE TEST, and round 5 wrote "the strings this row CAN show,
// in full" while adding two strings that a row does not always show and
// missing one it does (review round 6):
//
//   * `d.name` is not a surface. It reaches the screen as a row's LABEL, and
//     the label is what the caller passes in - which for a stack row is the
//     STACK's name, not the head frame's. Adding `d.name` here made the row's
//     sentence say "N character(s) HERE draw as '?'" about a frame name the
//     row does not draw anywhere. Dropped: a loose row's label IS `d.name`, so
//     nothing is lost, and the frames of a stack are the member clause's
//     subject, counted there by their own names.
//   * `d.src->member` reaches the screen the same way and only that way (an
//     npz row's `d.name` is `<file>:<member>`), so it was a second copy of
//     part of the label. Dropped for the same reason.
//   * the path was added "in full" with the tooltip as the stated reason, and
//     the tooltip does not always show the path: a COMPUTED frame has none, and
//     its first line is `d.note` instead. So the piece is now the tooltip's
//     first line AS THE TOOLTIP BUILDS IT - one function, called by both, which
//     is the only arrangement in which this cannot drift again.
//
// Tooltip-shown and row-shown are both "shown", and that is why the directory
// part counts: a folder called C:/測定/ with a '?' in it is exactly as
// confusing as a file name with one, and it is on screen in the tooltip.
static std::vector<std::string> undrawablePieces(const std::string& shownLabel,
                                                 const ImageDoc& d) {
    std::vector<std::string> out;
    auto add = [&](const std::string& v) {
        if (v.empty()) return;
        // A cheap short-circuit, and NOT what makes the count right: equal
        // strings are the ordinary case and decoding one twice is waste. The
        // count would be the same with this line deleted.
        if (std::find(out.begin(), out.end(), v) != out.end()) return;
        out.push_back(v);
    };
    add(shownLabel);               // what is drawn ON the row
    add(filesRowPathLine(d));      // ...and the line that names the file
    // ...and the SERIES UNIT, which a member row's label carries and which no
    // caller passes: the Files panel builds that label with
    // filesSeriesMemberLead() and the Messages sweep has only the document, so
    // the tooltip was counting the unit and the Messages line was not - two
    // surfaces disagreeing about one row, which is the state this list exists
    // to prevent. Found here rather than passed in, so both callers get it.
    // app.series is a handful of entries; this is not the sweep's cost.
    // ...ONLY WHEN THE ROW DRAWS IT, which is the same test the rest of this
    // list passes (review round 7 P3). filesSeriesMemberLead() appends the unit
    // only for a member whose value is SET - "value unset" has no unit after it
    // - so a member with no value had the unit counted as being "here" when the
    // row shows no unit at all. That is the mistake `d.name` was dropped for,
    // one field over.
    for (const App::Series& S : app.series) {
        bool drawn = false;
        for (const App::Series::Member& m : S.members) {
            const bool mine = (d.seqId && m.seqId == d.seqId) ||
                              (!d.seqId && m.frameUid && m.frameUid == d.uid);
            if (!mine) continue;
            drawn = std::isfinite(m.value);
            break;
        }
        if (drawn) { add(S.unit); break; }
    }
    return out;
}
// ONE set of characters over ALL the pieces: `seen` is shared, so a character
// in two of them is one character. This is the line that closes the npz,
// remote-frame, stack-name and decorated-label shapes at once.
static UndrawableTally undrawableTallyOf(const std::vector<std::string>& pieces) {
    UndrawableTally t;
    std::unordered_set<uint64_t>& seen = undrawableSeenBuf();
    for (const std::string& v : pieces) t += undrawableTallyUnseen(v, seen);
    return t;
}

// ...and the same sentence for ONE string, which is what the selftests ask for
// when they are checking the sentence rather than a row.
//
// NOT CACHED, and not called from the panel. It used to be both: it was the
// Files panel's per-row path and the cache was what review P3-11 added to stop
// the panel decoding every name on every frame. The panel moved to an explicit
// piece list and its cache went with it (`rowCache`, at the tooltip), and this
// function was left behind with a comment still naming the caller it had lost
// (review round 6 P3). A cache for a handful of calls from a test is a cache
// whose reason cannot be written down truthfully, so it is gone.
static std::string undrawableNote(const std::string& text) {
    return undrawableNoteOf(undrawableTally(text));
}
// ...and the Messages line, once per document.
//
// Driven off app.imagesRev rather than hooked into an open: documents are
// pushed onto app.images from many separate places, and a notice added to one
// of those doors is a notice none of the others give. The revision counter is
// the one thing every one of them bumps. (How many doors there are is not
// stated: this sentence had a count in it and the count was wrong, twice. What
// matters is that no door is the hook - which is why the counter is.)
//
// NO NUMBER, deliberately: this said "six separate places" and named six, and
// grep finds nine pushes in seven files (review round 6). The claim worth
// making is that there is no single door, which is what makes a counter the
// hook; counting the doors in a comment just adds something else to be wrong.
//
// SAID ONCE PER ROW, and the key is the ROW's identity - `"s"+seqId` for a
// stack, `"u"+uid` for a loose file - held in `saidKey` below.
//
// This paragraph used to say "once per NAME" and the one after it used to say
// "each document is examined once, by uid", and both described machinery this
// PR removed: `said` (a name-keyed set) and `seenUid` (a uid-keyed memo) were
// real, and the per-BATCH rework that made this one line per OPEN deleted them
// and left the prose (review round 6 - text following code, not a finding
// closed by writing a comment).
//
// What the row key buys and what it costs, as the code now stands:
//   * a REOPEN of the same file is a NEW uid, so it is a new row and it is
//     reported again. That is the behaviour the second-open test in C4 relies
//     on, and it is the opposite of what "the same file reopened is the same
//     sentence" claimed.
//   * nothing is memoised per document, so every sweep walks all of
//     app.images and tallies every row that `saidKey` has not seen. During a
//     stack load the batch is still growing, nothing is in `saidKey` yet, and
//     the sweep repeats for each frame pushed. Whether that is worth a memo is
//     a measurement nobody has taken; it is not claimed here either way.
// ONE LINE PER OPEN, not one per document (review P2-5).
//
// The per-document version was a defect, not a verbosity preference: a folder
// of 400 numbered frames with a CJK name is ONE gesture that produced 400
// identical-shaped lines, and app.msgLog is capped at 300 - so the log that
// exists to diagnose this pushed out everything else in it, including the
// start-up font messages that say WHICH font is in use. A diagnostic that
// erases the diagnosis is worse than silence.
//
// The unit is the BATCH, because a batch is defined as one open action
// ("ONE batch per OPEN ACTION" - see newBatch in compare.inc), so it is the
// program's own name for the thing the user did. Within a batch, stacks are
// reported by stack, which is what the Files panel actually draws.
//
// WHEN: once nothing more can be pushed into the batch. A stack pushes members
// over many frames, so counting at the first sight of one would print "1 frame"
// for a load that ends with 400.
//
// AND THAT IS ASKED, NOT INFERRED FROM THE SIZE STANDING STILL (review round 7
// P2). The test used to be "the size is the same as it was last sweep", which
// only means "nothing arrived between two sweeps" - and that is the final
// answer only if a sweep can happen for no reason other than a push. Round 6
// wrote that as a universal, in this paragraph:
//
//     EVERY `imagesRev++` in the program happens WITH a push
//
// IT IS FALSE, and here is how to see that without taking a number on trust:
// `grep -n 'imagesRev++'` and read the ones that are not next to a push.
// compare.inc bumps it when the Files GROUPING is invalidated; series_model.inc
// bumps it on every member edit, rename and reorder; export.inc bumps it when a
// document is dropped from the caches; the derive and series modals bump it
// when they close. None of those pushes anything. Six lines below this one the
// file says so itself ("The line only appeared when something ELSE later moved
// the counter"), and so does `:1268`. One of those bumps landing mid-load makes
// two consecutive sweeps see the same size, so a 400-frame stack was reported
// as "N of 4" and never corrected - the exact failure the paragraph above says
// this delay exists to prevent.
//
// (This paragraph replaced a wrong COUNT with a wrong UNIVERSAL, and the
// universal was worse: a reader can check a number with grep and nobody checks
// an "every". So the universal that replaced it is checked - by the test below
// in C4, which moves the counter without pushing and asserts the silence.)
//
// WHY THIS SHAPE AND NOT A CORRECTION LINE. The ruling allowed either: ask the
// question directly, or say the number again when it turns out larger. Asking
// is better here because the answer EXISTS - a stack load records how many
// frames it is going to have (`SeqInfo::expectedFrames`), so "can more arrive
// for this row" is a fact the loader already wrote down, not an inference. A
// correction line would also have to be bounded (a load that grows in steps
// would correct itself in steps), and it would spend the one line per open that
// this whole mechanism exists to keep.
//
// WHAT IS LEFT, said out loud: `expectedFrames` is 0 when no loader recorded
// one - a hand-picked set of loose files, a plugin's product. There the
// size-stability delay is all there is, and it can still fire early. For loose
// rows that costs nothing for the per-row lines, because `saidKey` is keyed on
// the row and a row that arrives later gets its own line (C4 asserts exactly
// that); the one number that can still be early is the summary's denominator
// when more than five rows arrive in two bursts.
//
// So when a batch is still growing this asks to be run again: `wantAnother`
// gets it past the rev gate on the next frame, and app.wakeFrames guarantees
// there IS a next frame (the loop is idle-driven and would otherwise sit in
// glfwWaitEventsTimeout until the user moved the mouse). A frame with neither
// a rev change nor that flag still returns at once, so the cost this avoided -
// walking app.images every frame - stays avoided.
static void undrawableReportNewDocs() {
    static uint64_t seenRev = (uint64_t)-1;
    static bool wantAnother = false;
    // The batch is the SETTLE unit; the ROW is the "already said" unit. They
    // have to be different things (review P2). batchReuse() hands back an
    // EXISTING batch id whenever the same source directory is opened again -
    // that is deliberate, it is what makes "clicked three files in a row" one
    // batch - so a permanent per-batch flag silenced every later open from a
    // folder that had already been reported once. Keyed on the row instead, a
    // second open of the same folder reports its NEW rows and stays quiet
    // about the ones it already named.
    static std::unordered_map<int, int> batchSize;        // batchId -> size last sweep
    static std::unordered_map<std::string, char> saidKey; // row key -> already reported
    if (app.imagesRev == seenRev && !wantAnother) return;
    seenRev = app.imagesRev;
    wantAnother = false;
    if (!bakedFont()) return;

    // Group this sweep's documents into ROWS as the Files panel groups them:
    // one group per stack, one per loose document. `key` is what the panel's
    // own identity is - a stack is its seqId, a loose file is its uid - so a
    // 400-frame stack is one group and three files picked by hand are three.
    struct Group {
        std::string key, name;
        int seqId = 0, members = 0, affected = 0;
        const ImageDoc* rep = nullptr;    // for undrawablePieces(), as the row draws it
    };
    std::unordered_map<int, std::vector<Group>> byBatch;   // batchId -> its rows
    std::unordered_map<int, int> nowSize;
    for (const auto& d : app.images) {
        nowSize[d->batchId]++;
        // The name the PANEL DRAWS is the one that matters: a stack row draws
        // the STACK's name, not its head frame's (review P2-7).
        const App::SeqInfo* si = d->seqId ? seqInfo(d->seqId) : nullptr;
        const std::string shown = si ? si->name : d->name;
        if (shown.empty()) continue;
        const std::string key = d->seqId ? ("s" + std::to_string(d->seqId))
                                         : ("u" + std::to_string(d->uid));
        std::vector<Group>& gs = byBatch[d->batchId];
        Group* g = nullptr;
        for (Group& q : gs) if (q.key == key) { g = &q; break; }
        if (!g) { gs.push_back({ key, shown, d->seqId, 0, 0, d.get() }); g = &gs.back(); }
        g->members++;
        // A FRAME'S OWN NAME, because that is the subject of the only sentence
        // this number appears in: "N of M frame(s) in this stack have such
        // characters IN THEIR OWN NAMES" (review round 6 P2).
        //
        // It used to be the frame's whole piece list, path included. Two
        // things were wrong with that. The words say "their own names", and a
        // path is not a name; and the frames of a stack share a directory, so
        // one '?' in the folder made EVERY frame affected and the clause read
        // "N of N" while the row's own clause, one sentence earlier, had
        // already reported that same single character - one character counted
        // twice, with the frames blamed for it.
        //
        // (Round 6 argued this from a universal - "the sibling rule makes the
        // non-numeric segments of the leaves match, so counting leaves can only
        // give 0 or N". That is false for a MERGED load, whose members come
        // from different groups and legitimately have unrelated leaves. The
        // clause is still right and the two reasons above do not need it;
        // review round 7 P3 caught the reasoning, not the behaviour.)
        if (undrawableTally(d->name).total() > 0)
            g->affected++;
    }
    for (auto& [batchId, gs] : byBatch) {
        // CAN MORE STILL ARRIVE? Asked of the loader's own record first - a
        // stack knows how many frames it expects - and only then of the size,
        // which is a delay and not an answer. See the note above.
        bool filling = false;
        for (const Group& g : gs) {
            if (!g.seqId) continue;
            const App::SeqInfo* si = seqInfo(g.seqId);
            if (si && si->expectedFrames > 0 && g.members < si->expectedFrames)
                filling = true;
        }
        const int was = batchSize.count(batchId) ? batchSize[batchId] : -1;
        if (filling || was != nowSize[batchId]) {
            batchSize[batchId] = nowSize[batchId];
            // Come back even if nothing else moves the revision counter, and
            // make sure a frame happens at all. Without both of these the
            // batch just opened is never reported - see the comment above.
            wantAnother = true;
            app.wakeFrames = std::max(app.wakeFrames, 2);
            continue;
        }
        // The rows worth a line at all, and not yet said - and, counted at the
        // same time and in the same place, HOW MANY ROWS THIS OPEN ADDED. The
        // summary below is a sentence about rows "just opened", and the only
        // set that matches those words is the one this loop is already walking
        // past: the rows of this batch that saidKey has not seen. `gs` is not
        // it - it is built from every row in app.images carrying this batchId
        // (see the loop above), and batchReuse() hands back an existing batch
        // id when the same folder is opened again, so `gs` can hold rows from
        // several gestures. Using gs.size() said "6 of 400" on a second open of
        // a big folder, which is the thing the P3 before it asked to avoid.
        std::vector<const Group*> hits;
        int justOpened = 0;
        for (const Group& g : gs) {
            if (saidKey.count(g.key)) continue;
            justOpened++;
            const bool own = g.rep &&
                undrawableTallyOf(undrawablePieces(g.name, *g.rep)).total() > 0;
            if (own || g.affected > 0) hits.push_back(&g);
        }
        // Every row in this batch is accounted for now, said or not - otherwise
        // the clean ones would be re-examined on every later sweep of a batch
        // that keeps being reopened.
        for (const Group& g : gs) saidKey[g.key] = 1;
        if (hits.empty()) continue;
        // ...and a bound on the lines ONE BATCH can produce: at most five, or
        // one summary. A stack is already one line; this is for the case that
        // is not a stack - a hundred loose files picked at once.
        //
        // THE BOUND IS PER BATCH AND NOT PER GESTURE, which is what this said
        // before (review round 6 P3). They are the same thing only while one
        // open action makes one batch, and the picker's footer has a setting
        // that breaks that: with `app.pickBatchMode == 1` a selection spanning
        // K top-level folders becomes K batches, so one Load produces up to
        // K x 5 lines, or K summaries. Capping per gesture would need a gesture
        // id that nothing here has; what is true is written instead.
        if (hits.size() > 5) {
            // THE DENOMINATOR IS `justOpened`, built two loops up by the same
            // `saidKey.count(g.key)` test that decides what may be reported at
            // all - so the number and the words "just opened" name one set, and
            // hits.size() <= justOpened holds by construction. Neither
            // app.images.size() nor gs.size() is that set; both include rows
            // from earlier opens of the same folder (review round 5 P2).
            char b[192];
            snprintf(b, sizeof b, "%d of the %d item(s) just opened have "
                                  "characters that draw as '?' - hover a row to "
                                  "see which, and why.",
                     (int)hits.size(), justOpened);
            logMsg(b, true);
            continue;
        }
        for (const Group* g : hits) {
            std::string line = g->name + ": ";
            // `rep` is set when the group is created and never cleared, so
            // there is no second arm here. There used to be one -
            // `undrawableNote(g->name)` - and it was unreachable, which made
            // the cache behind it unreachable too and left two comments
            // justifying a cost that nothing was paying (review round 6 P3).
            const std::string own =
                undrawableNoteOf(undrawableTallyOf(undrawablePieces(g->name, *g->rep)));
            if (!own.empty()) line += own;
            // The MEMBER clause, for a stack whose frames carry it too (or
            // instead). Its own numbers, because they are about other strings.
            if (g->seqId && g->affected > 0) {
                char b[192];
                snprintf(b, sizeof b, "%s%d of %d frame(s) in this stack have "
                                      "such characters in their own names.",
                         own.empty() ? "" : " Also: ", g->affected, g->members);
                line += b;
            }
            logMsg(line, true);
        }
    }
    // Per RUN, and bounded: a session that opens and closes folders all day
    // would otherwise grow these without end, so they are capped the way
    // msgLog is. At worst one row is reported twice.
    if (saidKey.size() > 8192) { saidKey.clear(); batchSize.clear(); }
}
// The tooltip's counterpart sentence for a row whose pixels are shared (§4):
// `shared` = the row's frames whose source has another holder. Counterparts
// are counted PER STACK - `4 frame(s) shared - 2 with "D1", 2 with "D2"` -
// never the row-wide total pinned on whichever holder happens to sit first
// in app.images; a single frame names ALL its other holders. At most 3 are
// named, the rest are "(+N more)". The row itself only carries the mark;
// WHO ELSE has the pixels is said here, in reading order.
static std::string filesShareSentence(const std::vector<const ImageDoc*>& shared) {
    if (shared.empty()) return "";
    struct CP { const ImageDoc* rep; int frames; };
    std::vector<uint64_t> keys;            // stack seqId / loose uid, first-seen
    std::vector<CP> cps;
    for (const ImageDoc* f : shared) {
        std::vector<uint64_t> seenHere;    // one count per counterpart per frame
        for (const auto& e : app.images) {
            if (e.get() == f || e->src->srcId != f->src->srcId) continue;
            if (f->seqId && e->seqId == f->seqId) continue;   // the row itself
            uint64_t key = e->seqId ? ((uint64_t)1 << 63) | (uint64_t)e->seqId
                                    : e->uid;
            if (std::find(seenHere.begin(), seenHere.end(), key) != seenHere.end())
                continue;
            seenHere.push_back(key);
            size_t k = std::find(keys.begin(), keys.end(), key) - keys.begin();
            if (k == keys.size()) { keys.push_back(key); cps.push_back({ e.get(), 0 }); }
            cps[k].frames++;
        }
    }
    if (cps.empty()) return "";
    const int n = (int)shared.size();
    std::string s = *shareGlyph() ? std::string(shareGlyph()) + " " : "";
    char b[320];
    if (n == 1) {
        s += "shared - also ";
    } else {
        snprintf(b, sizeof b, "%d frame(s) shared - ", n);
        s += b;
    }
    const int named = std::min((int)cps.size(), 3);
    for (int k = 0; k < named; k++) {
        const ImageDoc* rep = cps[k].rep;
        const App::SeqInfo* osi = rep->seqId ? seqInfo(rep->seqId) : nullptr;
        const std::string where = osi ? osi->name : rep->name;
        if (k) s += ", ";
        if (n == 1 && rep->seqId)
            snprintf(b, sizeof b, "frame %d of \"%s\"", rep->seqIndex, where.c_str());
        else if (n == 1)
            snprintf(b, sizeof b, "open as \"%s\"", where.c_str());
        else
            snprintf(b, sizeof b, "%d with \"%s\"", cps[k].frames, where.c_str());
        s += b;
    }
    if ((int)cps.size() > named)
        s += " (+" + std::to_string((int)cps.size() - named) + " more)";
    return s;
}

// ---- how many BYTES a row is holding ----------------------------------------
// The sum over UNIQUE FrameSource, and never the stack x frame product. That is
// docs/reference-design.md §7's one-line lesson, turned into a decision by its §9
// 判断3 ("Files のバイト列: 追加する。unique source の和で"): two rows can point at
// ONE source - the same folder opened twice, a derived stack beside its
// original, one .npz member in two stacks - and pixels counted twice tell the
// reader a stack costs double what it does, in the panel whose whole job is to
// say what is open. The share mark (§4) already says THAT the pixels are shared;
// this is the same fact as a number.
//
// Dedupe by srcId, exactly as residentImageBytes does it and for its reason:
// srcId IS the frame's identity (§3.1), and it is the one field the accounting
// is allowed to read.
static size_t filesUniqueBytes(const std::vector<int>& rows) {
    size_t n = 0;
    std::unordered_set<uint64_t> seen;
    seen.reserve(rows.size());
    for (int i : rows) {
        if (i < 0 || i >= (int)app.images.size() || !app.images[i]) continue;
        const ImageDoc& d = *app.images[i];
        if (seen.insert(d.src->srcId).second) n += d.px().size() * sizeof(float);
    }
    return n;
}

// The ink for one layer of the canon, in whichever theme is on. Wrapped so the
// Files rows read as "this row is a series" rather than as a colour lookup.
static ImVec4 inkOf(int layer) { return ui_theme::layerInk(app.themeVariant, layer); }

// Which set row / role row is SELECTED (click = select; double-click = go to
// the bound member - the click/commit convention every list here follows).
// Selection of a role is a reading position, not app model state, so it lives
// with the panel.
static int g_aSetSelId = 0;      // ASet::id, 0 = none
static int g_aSetSelRole = -1;   // index into its roles, -1 = the set node itself

// One condition, printed to the format the WHOLE series shares, so the values
// read as a column. Formatting each on its own with %g gives 1, 2, 5, 10, 20
// four different widths and nothing lines up. The digits of the UI font are
// tabular, so a shared width and a shared number of decimals really do align -
// padding with spaces would not, the font being proportional.
//
// The decimals are the fewest that still say every member exactly: a sweep of
// whole milliseconds stays whole, and one member of 2.5 gives the column one
// decimal rather than giving 2.5 six.
static std::string conditionText(const App::Series& S, double v) {
    if (!std::isfinite(v)) return "value unset";
    int dec = 0;
    for (const auto& m : S.members) {
        if (!std::isfinite(m.value)) continue;
        int d = 0;
        for (; d < 6; d++) {
            char t[64];
            snprintf(t, sizeof t, "%.*f", d, m.value);
            double back = atof(t);
            if (std::fabs(back - m.value) <= 1e-9 * std::max(1.0, std::fabs(m.value))) break;
        }
        dec = std::max(dec, d);
    }
    int w = 0;
    for (const auto& m : S.members) {
        if (!std::isfinite(m.value)) continue;
        char t[64];
        snprintf(t, sizeof t, "%.*f", dec, m.value);
        w = std::max(w, (int)strlen(t));
    }
    char b[96];
    snprintf(b, sizeof b, "%*.*f", w, dec, v);
    return b;
}

// The stack name as a SERIES MEMBER row prints it.
//
// Opening a container names its stacks after their position inside it -
// "sweep.vnz:0", "sweep.vnz:1" (loadVnzNodes) - and that compound is why a
// member row could be read at all when the series it belongs to had scrolled
// off the top: the row had to say which sweep it came from, because nothing
// above it was still on screen to say so. The heading is held there now, and
// the row leads with its own [n], so the ":n" is the same fact printed twice.
// What is left is the file name, which goes to the END of the row - where a
// narrowing panel drops it first and the index and the condition survive.
//
// DISPLAY ONLY, and only here. The name itself is unchanged: it is what the
// Linearity table's stack column, the Series menu, "Move to batch" and every
// tooltip print, and in those places there is no [n] beside it and no pinned
// heading above it - three stacks all reading "sweep.vnz" would be a real loss
// of information. This is the one row that has somewhere else to put it.
//
// A DIGIT tail only. An .npz member builds the same shape out of the array's
// NAME (loadNpzMembers: "data.npz:arr_0"), and that is not a position - the
// row's [n] does not repeat it, so it stays.
static std::string filesMemberName(const std::string& name) {
    size_t c = name.find_last_of(':');
    if (c == std::string::npos || c == 0 || c + 1 >= name.size()) return name;
    for (size_t i = c + 1; i < name.size(); i++)
        if (name[i] < '0' || name[i] > '9') return name;
    return name.substr(0, c);
}

// The visible lead shared by stack and standalone-frame member rows. Keeping
// this in one function makes the member's [order] / condition / name contract
// independent of which physical layer supplies the row.
static std::string filesSeriesMemberLead(const App::Series& S,
                                         const App::Series::Member& member,
                                         int memberIndex,
                                         const std::string& name) {
    std::string value = conditionText(S, member.value);
    if (std::isfinite(member.value) && S.unit[0]) value += " " + std::string(S.unit);
    return "[" + std::to_string(memberIndex) + "]  " + value + "  " +
           filesMemberName(name);
}

// ---- what the Files panel holds at the top while it scrolls ------------------
// One level of the hierarchy and the rows it owns, in the list's own content
// coordinates (scroll-independent: ImGui::GetCursorPos, not GetCursorScreenPos).
// Recorded by the walk that draws the list and read at the top of the NEXT
// frame, which is where the band has to be submitted.
//
// Files needs a different mechanism from Browse and it is worth saying why,
// because they look like the same feature: Browse's rows are virtualised, so
// the only thing it can reason about is row INDICES, and it hands its band to
// the table's own frozen-row machinery. Files has no clipper - every row is
// submitted, every frame - so here the exact geometry is already known and the
// band is simply drawn above the scrolling region, which is the plainest way
// there is to make it occupy space rather than cover the first row.
struct FilesSpan {
    int batch = 0;             // the batch this level belongs to
    int series = 0;            // 0 = the batch's own heading, else the series'
    float y0 = 0;              // top of the heading row
    float hy = 0;              // ...and its bottom: above this it is still visible
    float y1 = 0;              // bottom of the last row under it
};
static std::vector<FilesSpan> g_filesSpans;
static float g_filesScrollY = 0;        // where the list was scrolled to, last frame
// What the band actually held, outermost first, ";"-joined - the same shape of
// probe as RbToolbarGeom::pinnedNames and read for the same reason: the claim
// is about what is on screen.
static std::string g_filesPinProbe;

static void drawFileList() {
    g_filesBadgeProbe.clear();
    g_filesStackRowProbe.clear();
    g_filesBusyRowProbe.clear();
    // No Open / Close buttons here (2026-08-03). Browse is how a file is found
    // and opened, and it reaches remote machines as well, which the OS dialog
    // never did. Close was the worse of the two: it silently escalated from the
    // frame to the whole stack, and it was the ONLY Close that did not name the
    // layer it closes -- the row menus say Close batch / Close stack / Close
    // series, and the File menu says Close Frame / Close Batch / Close All.
    if (app.seqRunning || !app.seqQueue.empty()) {
        int done = app.seqDone, total = app.seqTotal;
        if (app.seqRunning) ImGui::TextDisabled("loading %d/%d", done, total);
        else ImGui::TextDisabled("queued");
        if (!app.seqQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d stacks)", (int)app.seqQueue.size());
        }
        {   // keep Stop reachable: it is the only way to cancel a huge load
            const ImGuiStyle& st = ImGui::GetStyle();
            float stopW = ImGui::CalcTextSize("Stop").x + st.FramePadding.x * 2;
            float used = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x -
                         ImGui::GetCursorStartPos().x;
            if (ImGui::GetContentRegionAvail().x - used - st.ItemSpacing.x >= stopW) ImGui::SameLine();
        }
        if (ImGui::SmallButton("Stop")) stopLocalStackLoad();
    }
    if (app.rfPending > 0) {
        ImGui::SameLine();
        // the ONLY way to cancel a remote fetch used to be quitting the app
        if (ImGui::SmallButton("Stop##rf")) stopRemoteStackFetch();
    }
    if (app.rfPending > 0)
        ImGui::TextDisabled("remote: fetching %d/%d", app.rfFetched.load(), app.rfTotal.load());
    if (!app.rbOpenQueue.empty()) {   // stacks a folder scan has still to open
        ImGui::TextDisabled("remote: %d stack(s) queued", (int)app.rbOpenQueue.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##rq")) app.rbOpenQueue.clear();
    }
    // A FILE IS BEING READ (issue #232 stage 2). The same row as the reader's,
    // the stack loader's and the video export's, and the same vocabulary - which
    // is the point: "this is busy with something long, and here is the Stop" is
    // one idea and it is said in one place on screen.
    //
    // Elapsed seconds and the name of the phase; NO percentage. A read has a
    // denominator and a decode has none, and one bar carrying two units measures
    // neither honestly (openJobPhase says this at the source). A file small
    // enough to open in a frame never draws this at all.
    if (app.openJob) {
        ImGui::TextDisabled("opening %s: %s  %.1f s", baseName(app.openJob->path).c_str(),
                            openJobPhase(*app.openJob).c_str(),
                            nowSec() - app.openJob->startedAt);
        if (!app.openQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d file%s)", (int)app.openQueue.size(),
                                app.openQueue.size() == 1 ? "" : "s");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##open")) {
            app.openJob->cancel.store(true);
            app.openQueue.clear();   // Stop means stop - see the reader row's Stop
            toast("stopping...");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("stop as soon as the phase it is on finishes -\n"
                              "a stopped open leaves nothing behind");
    }
    // A FILE IS BEING RE-READ (the 2026-09-28 report 「reload を押すと応答なしに
    // なる」). The open row's vocabulary, one idea over: "this is busy with
    // something long, and here is the Stop". What differs is the NUMBER - a
    // reload has as many items as the stack has distinct sources, so it can give
    // a true fraction where an open can only name a phase (openJobPhase says
    // why) - and what Stop MEANS, which is the tooltip's whole subject.
    if (app.reloadJob) {
        // ...recorded as drawn, for the row tooltip's reason (review, item 5):
        // a test that called reloadJobRowText for itself would assert the
        // function and not the panel.
        g_filesBusyRowProbe = reloadJobRowText(*app.reloadJob);
        ImGui::TextDisabled("%s", g_filesBusyRowProbe.c_str());
        if (!app.reloadQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d stack%s)", (int)app.reloadQueue.size(),
                                app.reloadQueue.size() == 1 ? "" : "s");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##reload")) {
            app.reloadJob->cancel.store(true);
            app.reloadQueue.clear();     // Stop means stop - the open row's rule
            toast("stopping...");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Stop after the frame it is on.\n\n"
                              "The frames already re-read STAY re-read - they are\n"
                              "what the disk holds now. So the stack is left holding\n"
                              "two moments and says so on its row; reload it again\n"
                              "to finish.");
    }
    // A reader is running (issue #232). The Reader panel says this too, in more
    // detail and with what the reader has printed so far - but a MEMO REPLAY
    // opens no panel and produces no document until it is done, so without a
    // line here the ordinary case (a double-click, the command line) would spend
    // its whole run showing nothing at all. This list is the one place always on
    // screen, and it already carries the other two waits for the same reason.
    if (app.rdJob) {
        ImGui::TextDisabled("reader: %s  %.1f s",
                            baseName(readerFileOf(app.rdJob->spec)).c_str(),
                            nowSec() - app.rdJob->startedAt);
        if (!app.rdQueue.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(+%d file%s)", (int)app.rdQueue.size(),
                                app.rdQueue.size() == 1 ? "" : "s");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##rd")) {
            app.rdJob->cancel.store(true);
            app.rdQueue.clear();     // Stop means stop - see the panel's Stop
            toast("stopping the reader...");
        }
    }
    if (app.videoExport.active) {
        // The stack-to-video job reports where stack LOADING reports (#253):
        // one place on screen for "this stack is busy with something long", and
        // the same Stop beside it. Stop deletes the partial file - said here,
        // because a Stop that silently leaves a two-frame video behind is the
        // one outcome nobody could afterwards explain from the file.
        ImGui::TextDisabled("writing video: %s", videoExportProgress().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop##video")) app.videoExport.stop = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("stop, and delete the partial %s",
                              baseName(app.videoExport.path).c_str());
    }
    if (!app.seqNote.empty()) {          // why the stack is short, kept in view
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(app.seqNote.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("dismiss##seqnote")) app.seqNote.clear();
    }
    ImGui::Separator();
    // Context-menu closes and moves are DEFERRED to the end of the walk:
    // closing erases images mid-iteration while the row loop still holds
    // indices into the pre-close stacksCached() snapshot, and a move prunes
    // app.batches while the submenu is iterating it. Every series command is
    // deferred for the same reason squared - the walk is iterating app.series
    // itself, and one join would resize it under the loop drawing it.
    int pendingCloseSeq = 0, pendingCloseBatch = 0;
    int pendingMoveSeq = 0, pendingMoveImg = -1, pendingMoveTarget = 0;
    int pendingSerEdit = 0, pendingSerMove = 0, pendingSerMoveTo = 0;
    int pendingSerUngroup = 0, pendingSerClose = 0, pendingSerRename = 0;
    std::string pendingSerName;
    int pendingJoinSeq = 0, pendingJoinSer = 0, pendingLeaveSeq = 0, pendingNewSerBatch = 0;
    int pendingDeriveSeq = 0;   // opens the Derive stack dialog after the walk
    // Reload from disk, deferred like the rest: reloadSource's walk calls
    // forgetImage while the row loop still holds this frame's cached indices.
    int pendingReloadImg = -1, pendingReloadSeq = 0;
    int pendingAckAutoSeq = 0;  // §9: the x on an auto-reload note. Deferred too:
                                // the note is drawn inside the same row walk.
    int pendingRecomputeImg = -1;   // Recompute from stack (§10.1): forgetImage
                                    // again, on the reduced frame itself
    // Set commands, deferred for the same reason: the walk iterates
    // app.analysisSets, and a close would erase under the loop drawing it.
    int pendingCloseSet = 0, pendingSetRename = 0;
    std::string pendingSetName;
    int pendingAvgSeq = 0;      // ...and the frame average, for the same reason:
                                // it appends to app.images, which this walk is
                                // iterating over
    int pendingSumSeq = 0;      // the sum fold, deferred identically
    // The detrend stage's product (docs/features/analysis/flat-field-stats.md 判断6): a NEW stack,
    // so it appends to app.images and defers for the same reason as the two
    // above. The spec rides with the id - "which removal" is the whole content
    // of the request, and a product made with the wrong one is not fixable by
    // renaming it afterwards.
    int pendingDetrendSeq = 0;
    DetrendSpec pendingDetrendSpec;
    // "Move to batch" submenu, shared by the stack row (seqctx) and the
    // standalone frame row (imgctx). Returns the chosen batch id, 0 = none.
    // Batch names go through a ### suffix: user text must not become the ID.
    auto moveToBatchMenu = [&](int curBatch) -> int {
        int target = 0;
        if (ImGui::BeginMenu("Move to batch")) {
            for (const auto& b : app.batches) {
                if (b.id == curBatch) continue;           // already there
                if (b.name == "preview") continue;        // pseudo-batch
                char lb2[300];
                snprintf(lb2, sizeof lb2, "%s###mb%d", b.name.c_str(), b.id);
                if (ImGui::MenuItem(lb2)) target = b.id;
            }
            ImGui::Separator();
            static char nbBuf[256] = "";
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            bool go = ImGui::InputTextWithHint("##newbatch", "New batch...",
                                               nbBuf, sizeof nbBuf,
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            go |= ImGui::SmallButton("create");
            if (go && nbBuf[0]) {
                target = newBatch(uniqueBatchName(nbBuf));
                nbBuf[0] = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        return target;
    };
    // The A/B item both kinds of row carry, by IDENTITY rather than by name:
    // the B-image menu lists names, and two batches full of frame_001.npy made
    // that a coin toss. `pick` is the frame this row would hand to B.
    // The extra slots hang off the same row item. Shown as a plain entry rather
    // than behind Shift+right-click: a modifier that changes what a menu
    // contains is invisible until someone tells you about it, and the letter in
    // the label ("Add as compare slot D") says what will happen before it does.
    auto compareSlotItem = [](ImageDoc* pick) {
        // the current row gets it too, like every other; the decision of WHAT
        // to offer lives in slotRowItem, where a selftest can reach it
        std::string letter;
        SlotRowItem it = slotRowItem(pick, letter);
        if (it == SlotRowNone) return;
        if (it == SlotRowRemove) {
            if (ImGui::MenuItem(("Remove from compare slot " + letter).c_str()))
                removeCompareSlot(pick);
            return;
        }
        if (ImGui::MenuItem(("Add as compare slot " + letter).c_str()))
            addCompareSlot(pick);
        if (ImGui::IsItemHovered())
            // Split is NOT in the two-only list: it is precisely the mode that
            // tiles every letter (tileSlots engages there and only there), and
            // saying it "stays A against B" contradicted the sentence before it.
            ImGui::SetTooltip("slots beyond B show up in the statistics tables and,\n"
                              "in Split, as side-by-side panes. Wipe, difference and\n"
                              "blink stay A against B - those only mean two.");
    };
    // Returns whether it put anything in the menu, so a caller can skip the
    // separator that would otherwise lead an empty group. Every row with a
    // document contributes now, the current one included (see abRowItem); the
    // false return is left for a row that has no document at all.
    auto compareBItem = [&compareSlotItem](ImageDoc* pick) -> bool {
        if (abRowItem(pick) != AbSetB) return false;
        if (ImGui::MenuItem("Set as compare B")) {
            setCompareB(pick);
            if (app.compareMode == App::CmpOff) app.compareMode = App::CmpWipe;
        }
        compareSlotItem(pick);
        return true;
    };
    // Group by source folder. Opening a folder of folders gives one stack per
    // subfolder, which reads fine - but opening several leaf folders in a row
    // produced a flat list of bare filenames with nothing saying where each came
    // from. The folder is the only thing that distinguishes them.
    const auto& stacks = stacksCached();
    static std::vector<FileGroup> groups;
    // §4: srcId -> how many memberships hold it. A shared frame keeps one row
    // per referencing stack (the tree stays the membership tree); rows whose
    // pixels also live elsewhere carry the share mark and name the counterpart
    // in the tooltip. Cached with the groups: sharing changes exactly when the
    // membership list does (imagesRev - the crop CoW bumps it too).
    static std::unordered_map<uint64_t, int> srcOwners;
    static uint64_t groupsRev = 0;
    if (groupsRev != app.imagesRev) {
        groupsRev = app.imagesRev;
        groups = buildFileGroups(stacks);
        srcOwners.clear();
        for (const auto& d : app.images) srcOwners[d->src->srcId]++;
    }
    // The batch heading is ALWAYS drawn. It used to appear only once there was
    // more than one thing open, so opening a second folder made a heading
    // materialise above rows that had none - and a batch you can rename and
    // move things into cannot be a thing that exists only sometimes.
    const bool showHeaders = !groups.empty();
    // ---- one heading, drawn one way -----------------------------------------
    // `pinned` says only that this copy is the one being held above the
    // scrolling list. It is the same widget, with the same ink, the same
    // tooltip and the same context menu, and clicking it collapses the same
    // thing - a row that changed meaning because of where it happened to be
    // drawn would be worse than one that scrolled away.
    //
    // A pinned heading is OPEN by definition: what put it in the band is that
    // its rows are the ones on screen. So its state is forced rather than
    // stored, and a click on it is routed to the real node, which is drawn
    // later in the same frame - no second copy of the open/closed state exists
    // to fall out of step with the first.
    int collapseBatch = 0, collapseSeries = 0;      // written by the band, read by the walk
    auto drawBatchHead = [&](const FileGroup& group, bool pinned) -> bool {
        bool isPreview = group.label == "preview";
        // The batch ink, unless this is the preview batch - "transient" is
        // what that row has to say first, and dim already says it.
        ImGui::PushStyleColor(ImGuiCol_Text,
                              isPreview ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                        : inkOf(ui_theme::LayerBatch));
        if (pinned) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        else if (collapseBatch == group.batch) ImGui::SetNextItemOpen(false);
        bool open = isPreview
            ? ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                         ImGuiTreeNodeFlags_SpanAvailWidth,
                                "preview (transient)   (%d)", (int)group.stacks.size())
            : ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_DefaultOpen |
                                         ImGuiTreeNodeFlags_SpanAvailWidth,
                                "%s   (%d)", group.label.c_str(), (int)group.stacks.size());
        ImGui::PopStyleColor();
        if (pinned && ImGui::IsItemClicked()) collapseBatch = group.batch;
        if (ImGui::IsItemHovered() && !group.dir.empty())
            ImGui::SetTooltip("%s\n\n(right-click to rename this batch)",
                              dispPath(group.dir).c_str());
        // the batch is the user's grouping, so its name is theirs to change
        if (ImGui::BeginPopupContextItem("batchctx")) {
            static char nameBuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(nameBuf, sizeof nameBuf, "%s", group.label.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##bname", nameBuf, sizeof nameBuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && nameBuf[0]) {
                renameBatch(group.batch, nameBuf);   // uniquified: names restore sessions
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close batch"))    // the batch layer's Close
                pendingCloseBatch = group.batch;
            ImGui::EndPopup();
        }
        return open;
    };
    auto drawSeriesHead = [&](const App::Series& S, bool pinned) -> bool {
        const ImGuiStyle& st = ImGui::GetStyle();
        char smeta[96];
        // the unit is the series', and "not set" is a state to read, not a
        // blank to guess at (the panel says the same thing the same way)
        if (S.unit[0]) snprintf(smeta, sizeof smeta, "[%s]  (%d)", S.unit,
                                (int)S.members.size());
        else snprintf(smeta, sizeof smeta, "[unit not set]  (%d)", (int)S.members.size());
        float avail = ImGui::GetContentRegionAvail().x;
        float mw = ImGui::CalcTextSize(smeta).x;
        bool room = avail > mw + ImGui::GetFontSize() * 8.0f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        // same rule as the stack rows: ONE item spanning the row, the unit
        // and the count PAINTED on it. A second widget on the right is what
        // made a context menu unreachable on the name once already.
        if (room) ImGui::PushClipRect(p0, ImVec2(p0.x + avail - mw - st.ItemSpacing.x,
                                                 p0.y + ImGui::GetFrameHeight()), true);
        ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerSeries));
        if (pinned) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        else if (collapseSeries == S.id) ImGui::SetNextItemOpen(false);
        bool sopen = ImGui::TreeNodeEx("##ser", ImGuiTreeNodeFlags_DefaultOpen |
                                                ImGuiTreeNodeFlags_SpanAvailWidth,
                                       "%s", S.name.c_str());
        ImGui::PopStyleColor();
        if (room) ImGui::PopClipRect();
        if (pinned && ImGui::IsItemClicked()) collapseSeries = S.id;
        if (room) {
            ImVec2 m = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(m.x + avail - mw, m.y + (ImGui::GetItemRectSize().y -
                                                ImGui::GetTextLineHeight()) * 0.5f),
                S.unit[0] ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                          : IM_COL32(255, 184, 90, 255), smeta);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("series (系列): %d member(s) of one swept %s\n\n"
                              "(right-click: rename, edit, move, ungroup, close)",
                              (int)S.members.size(),
                              S.paramName.empty() ? "parameter" : S.paramName.c_str());
        if (ImGui::BeginPopupContextItem("serctx")) {
            static char sbuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(sbuf, sizeof sbuf, "%s", S.name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##sname", sbuf, sizeof sbuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && sbuf[0]) {
                pendingSerRename = S.id;
                pendingSerName = sbuf;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            // the same modal creation uses: members, values, order, unit
            if (ImGui::MenuItem("Edit series...")) {
                pendingSerEdit = S.id;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            {   // the SERIES moves - every member with it, so that its
                // same-batch invariant never stops being true on the way
                int t = moveToBatchMenu(S.batchId);
                if (t) { pendingSerMove = S.id; pendingSerMoveTo = t; }
                ImGui::TextDisabled("   (all %d member(s) move with it)",
                                    (int)S.members.size());
            }
            ImGui::Separator();
            // Two DIFFERENT operations, spelled out rather than one word
            // with a modifier: one keeps the data, the other destroys it.
            if (ImGui::MenuItem("Ungroup (keep the members)")) pendingSerUngroup = S.id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Removes the series only. All %d member(s) stay open,\n"
                                  "exactly where they are - they just stop being a sweep.",
                                  (int)S.members.size());
            if (ImGui::MenuItem("Close series (discard its members)"))
                pendingSerClose = S.id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Closes all %d member(s): each standalone frame, and\n"
                                  "every frame of each stack.",
                                  (int)S.members.size());
            ImGui::EndPopup();
        }
        return sopen;
    };
    // ---- the band: every level the list has scrolled past --------------------
    // Drawn ABOVE the scrolling region, so it takes its height out of the list
    // rather than painting over the list's first row. The levels it holds come
    // from the spans the walk recorded last frame against the scroll offset it
    // ended on, which is the only place either number exists: ImGui applies a
    // wheel event inside BeginChild, and the band has to be submitted before
    // that. One frame of lag while the wheel is actually turning, and none at
    // rest - the same settling every layout-dependent probe in this file has.
    g_filesPinProbe.clear();
    int pinPushed = 0;
    {
        // Its OWN ID scope, and not a detail. Without it the pinned copy and
        // the real heading are one ImGui id, which means one open/closed state
        // between them - and the copy forces that state open every frame,
        // because being open is what put it in the band. Collapsing the real
        // heading would be undone by its own reflection on the next frame.
        // Separate ids, one direction: the copy is forced open, a click on it
        // sets collapseBatch / collapseSeries, and the real node reads that.
        ImGui::PushID("filespin");
        std::vector<const FilesSpan*> pins;
        for (const auto& s : g_filesSpans)
            if (s.hy <= g_filesScrollY && g_filesScrollY < s.y1) pins.push_back(&s);
        // outermost first: a batch's span opens before the span of any series
        // inside it, so top-of-list order IS ancestor order
        std::sort(pins.begin(), pins.end(),
                  [](const FilesSpan* a, const FilesSpan* b) { return a->y0 < b->y0; });
        for (const FilesSpan* p : pins) {
            if (p->series == 0) {
                const FileGroup* g = nullptr;
                for (const auto& q : groups) if (q.batch == p->batch) g = &q;
                if (!g) continue;
                ImGui::PushID(g->batch);
                g_filesPinProbe += g->label + ";";
                // left pushed on purpose: TreeNodeEx indents what follows it,
                // and the band has to reproduce the indentation of the rows it
                // stands in for. Unwound below, innermost first.
                drawBatchHead(*g, true);
                pinPushed++;
            } else {
                const App::Series* S = nullptr;
                for (const auto& s : app.series) if (s.id == p->series) S = &s;
                if (!S) continue;
                ImGui::PushID(S->id);
                g_filesPinProbe += S->name + ";";
                drawSeriesHead(*S, true);
                pinPushed++;
            }
        }
        for (int i = 0; i < pinPushed; i++) { ImGui::TreePop(); ImGui::PopID(); }
        ImGui::PopID();
        if (pinPushed) ImGui::Separator();
    }
    // The list scrolls on its own now. It has to: a band that is part of the
    // scrolling content is a band that scrolls away, which is the entire
    // complaint. Everything above - the load progress, the stop buttons, the
    // note - stays outside it, where it was already behaving as a header.
    bool listOpen = ImGui::BeginChild("fileslist", ImVec2(0, 0), ImGuiChildFlags_None);
    if (listOpen) {
    g_filesSpans.clear();
    for (const auto& group : groups) {
      float gy0 = ImGui::GetCursorPos().y;
      ImGui::PushID(group.batch);
      bool open = true;
      float ghy = gy0;
      if (showHeaders) {
          open = drawBatchHead(group, false);
          ghy = ImGui::GetCursorPos().y;
      }
      if (open) {
        // name and format share one row: the dim/format part is right-aligned and dimmed.
        // `badge` is every compare letter this row's image (or stack) holds -
        // "B", "C", "BC", ... - drawn at the left edge in the B ink, one visual
        // weight for all of them: a slot the Files panel does not show is a
        // slot nobody can find again (defect 2026-07-30).
        auto rowWithMeta = [](const ImageDoc& d, const char* label, bool selected,
                              const char* extra = nullptr,
                              const std::string& badge = std::string(),
                              bool badgeDim = false,
                              const std::string& share = std::string()) -> bool {
            const ImGuiStyle& st = ImGui::GetStyle();
            char meta[112];
            // §4: the share mark leads the meta strip - painted onto the row,
            // so no new column and no wider row; the tooltip names the
            // counterpart (`share` is filesShareSentence's output).
            const char* mark = share.empty() ? ""
                             : *shareGlyph() ? "\xE2\xA7\x89  " : "shared  ";
            snprintf(meta, sizeof meta, "%s%dx%d %dch %s%s", mark, d.w, d.h, d.ch,
                     d.dtype.c_str(), extra ? extra : "");
            float avail = ImGui::GetContentRegionAvail().x;
            float metaW = ImGui::CalcTextSize(meta).x;
            // The row is ONE item spanning the full width, and the metadata is
            // painted onto it rather than being a second item. It used to be a
            // name-width Selectable followed by a TextDisabled, which made the
            // metadata the "last item" - so right-clicking a stack opened the
            // context menu only over that dim strip on the right, and never on
            // the name, where everyone aims.
            float nameW = std::max(avail - metaW - st.ItemSpacing.x, ImGui::GetFontSize() * 4.0f);
            bool room = avail > metaW + ImGui::GetFontSize() * 6.0f;   // else: name only
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::PushClipRect(p0, ImVec2(p0.x + (room ? nameW : avail),
                                           p0.y + ImGui::GetFrameHeight()), true);
            bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns,
                                             ImVec2(avail, 0));
            ImGui::PopClipRect();
            bool hov = ImGui::IsItemHovered();
            if (!badge.empty()) {   // which rows hold letters, without hunting a menu
                ImVec2 m = ImGui::GetItemRectMin();
                float h = ImGui::GetTextLineHeight();
                ImVec2 tp(m.x + 2 * app.uiScale,
                          m.y + (ImGui::GetItemRectSize().y - h) * 0.5f);
                // dim = "armed, not active": compare is off, the seats are
                // kept. Same ink, lower alpha - one lettering system.
                ImGui::GetWindowDrawList()->AddText(
                    tp, badgeDim ? IM_COL32(120, 190, 255, 110)
                                 : IM_COL32(120, 190, 255, 255), badge.c_str());
                // ...and as state the selftest can read (S4/S6) - the dim
                // state is part of what was drawn, so it is recorded, not
                // inferable from a colour the probe cannot see
                const char* p = label;
                while (*p == ' ') p++;
                std::string t(p);
                size_t hh = t.find("##");
                if (hh != std::string::npos) t.resize(hh);
                g_filesBadgeProbe += t + "=" + badge + (badgeDim ? "(dim);" : ";");
            }
            if (room) {   // draw-list, not an item: the row must stay the last item
                ImVec2 m = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(m.x + avail - metaW, m.y + (ImGui::GetItemRectSize().y -
                                                       ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), meta);
            }
            // A COMPUTED frame has no path, so this tooltip led with a blank
            // line and told the reader nothing at all - and the Files panel is
            // where a frame average sits beside the captures it will be
            // confused with. Its provenance note takes the path's place, which
            // is what the note is for. The share sentence (§4) follows either
            // lead: whose pixels these also are is true of computed frames too.
            // THE TOOLTIP, built whether or not the mouse is over this row.
            //
            // Built unconditionally, and recorded as the ONE string SetTooltip
            // is handed (review P3-19, and its second pass): a probe that
            // re-derives the text is a probe about the deriving, so a row that
            // stopped calling seqWatchTooltip would have gone on passing. A
            // probe that needs the mouse in the right place is a probe about
            // the mouse. So the row composes it once, the probe is that value,
            // and the hover only decides whether to SHOW it.
            {
                std::string lead = filesRowPathLine(d);
                // the generation statements, where the provenance is read
                // (§10.1 / §10.2): stale leads - it is the warning; "same
                // picture" trails - it is the reassurance two results of one
                // recipe can give when their recorded input tuples match.
                if (avgIsStale(&d))
                    lead = "(stale) the source stack was reloaded after this frame "
                           "was computed - the picture shows the OLD pixels. "
                           "Right-click > Recompute from stack.\n" + lead;
                else if (!d.avgOfPath.empty())
                    for (const auto& q : app.images)
                        if (avgSamePicture(&d, q.get()) && !avgIsStale(q.get())) {
                            lead += "\nsame picture as \"" + q->name +
                                    "\" (same recipe over the same source generation)";
                            break;
                        }
                // #56: what the last reload of THIS ROW'S STACK left behind,
                // and it leads even (stale) - a stale average is one Recompute
                // from being true again, a stack of two generations is not,
                // and this sentence is the one that explains the red outline
                // the row is wearing. (In practice the two never meet: a
                // reduced frame belongs to no stack and a stack head is never
                // a reduced frame. The order is stated anyway, because "it
                // cannot happen" is not a thing a reader can see here.)
                // ...and WATCH's finding, folded off its own amber line and onto
                // this row (watch-design §5, the 2026-09-28 report 「reload で行
                // が増えるのは嫌」). The row's token is a word; this is the
                // sentence it is short for, and it ends with the gesture that
                // acts on it, because there is no longer a button under the row
                // to point at. Under the #56 note for that note's own reason: a
                // stack of two generations is already wrong, and this is
                // something to do about the disk.
                if (std::string wt = seqWatchTooltip(d.seqId); !wt.empty())
                    lead = wt + "\n" + lead;
                if (std::string rn = seqReloadNote(d.seqId); !rn.empty())
                    lead = rn + "\n" + lead;
                std::string tip = lead + "\n" + meta;
                if (!share.empty()) tip += "\n\n" + share;
                // §4.1 (板306 D): and if this cannot be DRAWN, say so on the
                // row that cannot draw it. Last, because it is a statement
                // about the text above rather than about the pixels - and
                // because the row it lands on is the one the reader is already
                // trying to tell apart from its neighbour.
                //
                // Counted over an EXPLICIT LIST of the strings this row draws,
                // and not over `tip`.
                //
                // Handing it `tip` was worse than handing it d.name. `tip` is
                // multi-line by construction, the atlas has no glyph for
                // U+000A because the ranges begin at U+0020, and so every row
                // in the panel - including every plain ASCII one - grew a
                // "1 character here draws as '?'" that was simply false. (The
                // control-character rule in undrawableTally now closes that
                // too; both halves are needed, because a concatenation also
                // mixes in the separators this code adds itself.)
                //
                // And `tip` never contained the one string the review asked
                // for: a stack row DRAWS the stack's name, which is in `label`
                // and not in the tooltip's body at all (review P2-7). The list
                // names every string the row can show, so the stack name is
                // counted and the full path is counted (review P3-12); that no
                // CHARACTER is counted twice is undrawableTallyUnseen()'s job
                // and not this list's - see the note on the unit there.
                {
                    // The row's own label, with ImGui's "##id" suffix and the
                    // leading indent removed - what is left is what a reader
                    // sees on the row. The pieces themselves come from
                    // undrawablePieces(), which the Messages sweep also uses.
                    std::string shown = label ? label : "";
                    if (const size_t h = shown.find("##"); h != std::string::npos)
                        shown.resize(h);
                    while (!shown.empty() && shown.front() == ' ') shown.erase(0, 1);
                    // CACHED PER ROW, and this is the ONLY cache on this path.
                    // Without it every row decodes every string it shows on
                    // every frame - the cost review P3-11 removed, back by a
                    // different door. Keyed on the pieces themselves, because
                    // that is exactly what the answer depends on; the atlas
                    // cannot change after start-up.
                    //
                    // (This used to say "undrawableNote() has a cache but this
                    // path no longer goes through it". Round 6 removed that
                    // cache - for being unreachable - and left the sentence
                    // here pointing at it: review round 7 P3, type (d), in the
                    // same commit that deleted the thing it names.)
                    static std::unordered_map<std::string, std::string> rowCache;
                    const std::vector<std::string> pieces = undrawablePieces(shown, d);
                    std::string key;
                    for (const std::string& q : pieces) { key += q; key += '\x01'; }
                    std::string ud;
                    if (auto it = rowCache.find(key); it != rowCache.end()) {
                        ud = it->second;
                    } else {
                        ud = undrawableNoteOf(undrawableTallyOf(pieces));
                        if (rowCache.size() > 4096) rowCache.clear();
                        rowCache.emplace(key, ud);
                    }
                    if (!ud.empty()) tip += "\n\n" + ud;
                }
                g_filesRowTipProbe = tip;      // the value, not a second derivation
                if (hov) ImGui::SetTooltip("%s", tip.c_str());
            }
            return clicked;
        };
        // One row. mem != nullptr means the row is being drawn INSIDE a series
        // node: it is indented by the node and the member's VALUE leads the
        // label. Everything else about its physical layer stays identical: a
        // stack remains a stack and a standalone frame remains a frame.
        auto drawStackRow = [&](const std::vector<int>& stack, const App::Series* ser,
                                const App::Series::Member* mem, int memIdx) {
          const ImageDoc& head = *app.images[stack.front()];
          if (head.seqId == 0) {
            int i = stack.front();
            ImGui::PushID(i);
            // Two marks meet on this row and both stay: the compare letters
            // and the [preview] tag (the ONE transient slot must not read as a
            // registered file - a single click in Browse "adding a file to
            // Files" was reported as exactly that misreading).
            //
            // One lettering system, two kinds of letter, one asymmetric rule -
            // do NOT "simplify" the asymmetry away, it is the spec:
            //   - B/C/D... are SEATS (assignments). They show whenever
            //     assigned: full ink while a comparison runs, dimmed while
            //     compare is off ("armed, not active" - user report
            //     2026-07-30, 「B,Cは薄く表示」). B's seat is the PIN
            //     (bSeatUid), never cmpB(): the letter must not vanish while
            //     A stands on the pin (paused) or while compare is off.
            //   - A is the CURSOR, not a seat (docs/features/compare/compare-n.md §1). It
            //     marks the current row only while a comparison is ACTIVE:
            //     compare off means nothing is being compared, so no A.
            const bool cmpOn = app.compareMode != App::CmpOff;
            std::string badge;
            if (cmpOn && i == app.current) badge += "A";
            if (bSeatUid() && head.uid == bSeatUid()) badge += "B";
            badge += slotOf(&head);
            char lb[512];
            // the leading spaces reserve the badge's ground: two per letter,
            // and at least the two the rows have always had.
            // (stale) / (recomputed): the generation marks of a reduced frame
            // (§10.1 / §10.2) - stale means the picture shows pixels its stack
            // no longer holds; recomputed means a restore rebuilt it from
            // files newer than the session's record. Same row position as
            // [preview]: facts about THIS row's pixels, next to the name.
            const char* genMark = head.preview ? "   [preview]"
                                : avgIsStale(&head) ? "   (stale)"
                                : head.avgGenChanged ? "   (recomputed from newer files)"
                                : "";
            if (mem && ser) {
                std::string lead = filesSeriesMemberLead(*ser, *mem, memIdx, head.name);
                snprintf(lb, 512, "%*s%s%s##mf%llu", (int)(badge.size() * 2), "",
                         lead.c_str(), genMark, (unsigned long long)head.uid);
            } else {
                snprintf(lb, 512, "%*s%s%s##%d",
                         (int)std::max<size_t>(badge.size() * 2, 2),
                         "", head.name.c_str(), genMark, i);
            }
            std::string share;                 // §4: this frame's pixels, elsewhere
            {
                auto sh = srcOwners.find(head.src->srcId);
                if (sh != srcOwners.end() && sh->second > 1)
                    share = filesShareSentence({ &head });
                // ras §6: the member side gets a SENTENCE, never a mark - the
                // ⧉ tooltip precedent, one line per binding this frame holds
                std::string b2 = aSetBoundSentence(0, head.uid, 0);
                if (!b2.empty()) share += (share.empty() ? "" : "\n") + b2;
            }
            // A loose image is a frame. Preview keeps the dim ink: "transient"
            // outranks "which layer" on that one row.
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  head.preview ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]
                                               : inkOf(ui_theme::LayerFrame));
            bool rowHit = rowWithMeta(head, lb, i == app.current, nullptr, badge, !cmpOn,
                                      share);
            ImGui::PopStyleColor();
            if (head.preview && ImGui::IsItemHovered())
                ImGui::SetTooltip("a throwaway preview - the one slot the Browse panel\n"
                                  "reuses; the next click there replaces it. Double-click /\n"
                                  "Enter in Browse (or measuring it) registers it for real.");
            if (rowHit) {
                selectImage(i);
                if (app.fitOnSwitch) app.fitRequested = true;
            }
            // A standalone frame gets the move menu itself, whether it hangs
            // directly off the batch or appears as a series member (stacks
            // move via seqctx below).
            if (ImGui::BeginPopupContextItem("imgctx")) {
                if (compareBItem(app.images[i].get())) ImGui::Separator();
                // a second, independent viewer on this frame (item 28)
                if (ImGui::MenuItem("Open in new window"))
                    openInNewWindow(app.images[i].get());
                {   // manual reload (reference-design §3.2 / §8 row 5): re-read
                    // this frame's file and swap the pixels in place. WHY the
                    // item may be greyed is reloadSource's own answer, asked
                    // once (reloadUnavailable) rather than transcribed here -
                    // a second list of refusals is a list that goes stale
                    // behind the walk.
                    const std::string why = reloadUnavailable(*app.images[i]->src);
                    if (ImGui::MenuItem("Reload from disk", nullptr, false, why.empty()))
                        pendingReloadImg = i;
                    if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", why.c_str());
                }
                // Recompute (§10.1): the reduced frame's own "reload" - it has
                // no file, it has a stack, and the picture NEVER refreshes
                // itself. This item is the one way it gets new pixels.
                if (!head.avgOfPath.empty()) {
                    bool canRe = head.avgSeqId && seqInfo(head.avgSeqId);
                    if (!canRe)
                        for (const auto& q : app.images)
                            if (q->seqId != 0 && q->src->path == head.avgOfPath &&
                                q->src->member == head.avgOfMember) {
                                canRe = true; break;
                            }
                    if (ImGui::MenuItem("Recompute from stack", nullptr, false, canRe))
                        pendingRecomputeImg = i;
                    if (!canRe && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("the stack this frame was computed from is not "
                                          "open - there is nothing to recompute over.");
                }
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveImg = i; pendingMoveTarget = t; }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            return;
          }
          App::SeqInfo* si = seqInfo(head.seqId);
          // Switching A to another stack KEEPS the frame number, clipped to what
          // this stack has. Landing on frame 0 threw away the position every
          // time - and comparing two captures means looking at the same frame
          // of each, so the number is the thing worth preserving, not the
          // stack's own history. (Same rule B already follows: see
          // compareFollowFrame in resolveB.)
          int pos = 0;
          bool active = false;
          if (const ImageDoc* a = cur())
              if (a->seqId != 0)
                  pos = std::clamp(a->seqIndex, 0, (int)stack.size() - 1);
          for (int k = 0; k < (int)stack.size(); k++)
              if (stack[k] == app.current) { pos = k; active = true; }
          ImGui::PushID(head.seqId);
          const float probeY0 = ImGui::GetCursorPos().y;
          // Every letter this stack holds, in A-then-B-then-slot order - the
          // order the letters read everywhere else. B is a FRAME, and so is a
          // slot's pin: the stack is where they LIVE, so the stack row
          // carries them. Same asymmetric rule as the frame rows above (and
          // do not "simplify" it): the seats (B/C/D..., keyed on the PIN via
          // bSeatUid, never on cmpB()) show whenever assigned - dimmed while
          // compare is off; A is the cursor and marks the active stack only
          // while a comparison is running.
          const bool cmpOn = app.compareMode != App::CmpOff;
          std::string badge = cmpOn && active ? "A" : "";
          bool stackHasB = false;
          if (bSeatUid())
              for (int idx : stack)
                  if (app.images[idx]->uid == bSeatUid()) stackHasB = true;
          if (stackHasB) badge += "B";
          for (size_t k = 0; k < app.cmpExtra.size(); k++)
              for (int idx : stack)
                  if (app.images[idx]->uid == app.cmpExtra[k]) { badge += slotName(k); break; }
          char lb[600];
          const char* sname = si ? si->name.c_str() : "stack";
          // #56: the WORD beside the red outline below. A colour alone says
          // "something", never what - and a report quotes text, not a border -
          // so the row states the fact in the same place (stale) states its
          // own, and the outline is what makes it findable across a full
          // panel. Both, or neither is worth having.
          const char* rmark = seqReloadMark(head.seqId);
          // ...and Watch's, folded onto the same row (docs/features/watch/
          // watch-design.md §5, the 2026-09-28 report 「reload で行が増えるのは
          // 嫌」). It used to be an amber LINE under this row - two lines with
          // Auto-reload on - so a poll that found a changed file moved every
          // stack below it. The row that is already here says it instead.
          //
          // #56's token WINS when both are true, and that is a reading-order
          // decision rather than a space one: "the disk moved" is something to
          // act on, "this stack's frames are of two moments" is something
          // already wrong with the numbers on screen, and only one of them can
          // come first. The tooltip and the menu carry both.
          const std::string wmark = (*rmark ? std::string() : seqWatchMark(head.seqId)) +
                                    seqAutoReloadMark(head.seqId);
          if (mem) {
              // Reading order: [n], then the condition, then the file name.
              //
              // The value used to LEAD, because it is the reason the row is in
              // this series at all - and it still comes before the name, for
              // exactly that reason. What is new in front of it is the member's
              // position in the series, and what moved behind it is the file.
              //
              // The order IS the feature. A Files panel dragged narrow drops
              // what is on its right, so this row loses the file name first and
              // keeps the two things that say where the row sits in the sweep -
              // while the pinned heading above it, which no width can push off,
              // keeps saying which sweep that is. That is the whole reason there
              // is no setting for any of this: the columns already do the work
              // an option would have to be asked for.
              //
              // An unset value has to be readable as unset, not inferable from a
              // blank at the right edge - conditionText prints the words.
              //
              // The "##m<seqId>" suffix is the row's ID, so renaming the stack
              // does not reset its widget state. It does NOT protect the text:
              // ImGui cuts a label at the FIRST "##", so a stack the user
              // renames to "a##b" renders as "a" in either shape of row.
              std::string lead = filesSeriesMemberLead(*ser, *mem, memIdx, sname);
              snprintf(lb, sizeof lb, "%*s%s%s%s##m%d", (int)(badge.size() * 2), "",
                       lead.c_str(), rmark, wmark.c_str(), head.seqId);
          } else {
              // "##s<seqId>" is the row's ID, and it is here for review P3-18's
              // reason: without it the row's ID was its whole LABEL, so a mark
              // appearing or disappearing - which now happens on a Watch poll,
              // with nobody touching anything - reset this row's widget state
              // under the reader. The series-member row above has carried an
              // explicit ID since it was written; this one needed the same, and
              // ImGui cuts a label at the first "##" so nothing is drawn twice.
              snprintf(lb, sizeof lb, "%*s%s%s%s##s%d",
                       (int)std::max<size_t>(badge.size() * 2, 2),
                       "", sname, rmark, wmark.c_str(), head.seqId);
          }
          char frames[64];
          {   // "8f" for an 8-of-300 stack states nothing; say the ratio while
              // the total is known and the load is short of it
              const App::SeqInfo* fsi = seqInfo(head.seqId);
              int exp = fsi ? fsi->expectedFrames : 0;
              char cnt[32];
              if (exp > (int)stack.size())
                  snprintf(cnt, sizeof cnt, "%d/%df", (int)stack.size(), exp);
              else
                  snprintf(cnt, sizeof cnt, "%df", (int)stack.size());
              // ...and what those frames COST. Over unique sources, not frames
              // times one frame's size - see filesUniqueBytes.
              snprintf(frames, sizeof frames, "  %s  %s", cnt,
                       fmtBytesHuman(filesUniqueBytes(stack)).c_str());
          }
          std::string share;                   // §4: any member's pixels, elsewhere
          {
              std::vector<const ImageDoc*> sharedFrames;
              for (int fidx : stack) {
                  const ImageDoc& f = *app.images[fidx];
                  auto sh = srcOwners.find(f.src->srcId);
                  if (sh != srcOwners.end() && sh->second > 1)
                      sharedFrames.push_back(&f);
              }
              if (!sharedFrames.empty()) share = filesShareSentence(sharedFrames);
              // ras §6: `bound as "dark" in "PRNU 10lx"` - sentence, no mark
              std::string b2 = aSetBoundSentence(head.seqId, 0, 0);
              if (!b2.empty()) share += (share.empty() ? "" : "\n") + b2;
          }
          ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerStack));
          g_filesRowTipProbe.clear();
          bool stackHit = rowWithMeta(head, lb, active, frames, badge, !cmpOn, share);
          const std::string probeTip = g_filesRowTipProbe;
          ImGui::PopStyleColor();
          // #56, the user's own proposal: the red outline. Drawn straight onto
          // the row rather than as another item, exactly as the compare letters
          // and the meta strip are - the row must stay the LAST item, or the
          // right-click below opens over the wrong thing. Immediately after the
          // call for the same reason: no widget between it and GetItemRect*.
          if (*rmark) {
              ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
              ImGui::GetWindowDrawList()->AddRect(a0, a1, RELOAD_FAIL_RED32,
                                                 2.0f * app.uiScale, 0,
                                                 std::max(1.0f, 1.5f * app.uiScale));
          }
          // ...and Watch's own colour, kept: the amber that used to be a whole
          // line is a BAR down the row's left edge. A token alone is findable
          // only by reading every row, which is what the colour was for; a
          // second full outline beside #56's red one would be two frames
          // fighting over one rectangle. Draw-list and not a widget, for the
          // red outline's reason exactly - the row must stay the LAST item or
          // the right-click below opens over the wrong thing.
          const bool amberBar = !seqWatchMark(head.seqId).empty();
          if (amberBar) {
              ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
              const float wbar = std::max(2.0f, 2.5f * app.uiScale);
              ImGui::GetWindowDrawList()->AddRectFilled(
                  a0, ImVec2(a0.x + wbar, a1.y), WATCH_AMBER32, 1.0f * app.uiScale);
          }
          if (stackHit) {
              selectImage(stack[pos]);
              if (app.fitOnSwitch) app.fitRequested = true;
          }
          // The stack is the unit measurements attach to, so it earns a name of
          // its own: "the 25C dark set", not whatever the capture script called
          // the folder. F2 / right-click renames; the folder name is only the
          // starting value.
          if (si && ImGui::BeginPopupContextItem("seqctx")) {
            static char renameBuf[256];
            if (ImGui::IsWindowAppearing())
                snprintf(renameBuf, sizeof renameBuf, "%s", si->name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            bool done = ImGui::InputText("##rename", renameBuf, sizeof renameBuf,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            done |= ImGui::SmallButton("rename");
            if (done && renameBuf[0]) {
                si->name = renameBuf;
                app.lin.rev++;            // linearity rows show stack names
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            {   // B = the frame this stack is showing; with "B follows A's
                // frame number" on, it tracks A from there anyway. The stack A
                // stands in gets the group too - setting it as B is how you put
                // one document on both sides and see that they agree.
                ImageDoc* bpick = app.images[stack.front()].get();
                if (si->lastImageIdx >= 0 && si->lastImageIdx < (int)app.images.size() &&
                    app.images[si->lastImageIdx]->seqId == si->id)
                    bpick = app.images[si->lastImageIdx].get();
                if (compareBItem(bpick)) ImGui::Separator();
            }
            // the whole stack again, independently: the child regroups the
            // numbered siblings from the head file (item 28)
            if (ImGui::MenuItem("Open in new window"))
                openInNewWindow(app.images[stack.front()].get());
            // ---- Watch's finding, where the gesture that answers it is ------
            // (watch-design §5, the 2026-09-28 report.) A LINE, not a button:
            // "Reload from disk" is directly under it and is the button, and a
            // second one would be a second way to do one thing. TextDisabled
            // for the same reason the amber line was never a control - it is a
            // statement about the disk, and the row above already carries the
            // token that says so at a glance.
            // seqWatchMenuText, not seqWatchTooltip: this popup is already
            // open and "Reload from disk" is the item below, so the tooltip's
            // closing "right-click > Reload from disk" would be the menu
            // pointing at itself (review P2-4).
            if (std::string wt = seqWatchMenuText(si->id); !wt.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("%s", wt.c_str());
            }
            {   // manual reload (reference-design §3.2 / §8 row 5): every
                // member's file re-read, pixels swapped in place. ONE member
                // the walk can serve is enough to offer the item - the summary
                // counts the rest; when none can, the FIRST member's own reason
                // is the one shown, from the same predicate the frame menu asks
                // (reloadUnavailable, beside reloadSource).
                bool anyLocal = false;
                std::string why;
                for (int idx : stack) {
                    std::string w = reloadUnavailable(*app.images[idx]->src);
                    if (w.empty()) { anyLocal = true; break; }
                    if (why.empty()) why = std::move(w);
                }
                if (ImGui::MenuItem("Reload from disk", nullptr, false, anyLocal))
                    pendingReloadSeq = si->id;
                // WHAT IT WILL AND WILL NOT DO, stated where the click is. This
                // hover is where the old amber line's [Reload] carried §6 /
                // §11.4's answer to "a file appeared - does it join THIS
                // stack?", and folding the line onto the row took it off the
                // screen altogether (review P2-5). reloadGestureTip is the one
                // place those sentences live; --watch-selftest asserts them.
                if (anyLocal && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", reloadGestureTip(si->id).c_str());
                if (!anyLocal && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", why.c_str());
            }
            // §9: and the one thing the auto-reload note needed a control for.
            // It used to be a little `x` on the amber line under the row; the
            // line is gone, so the act of clearing it lives where every other
            // thing you can do to this stack lives. Only when there is one -
            // an item that is greyed out more often than not is noise.
            if (!si->autoReloadNote.empty() &&
                ImGui::MenuItem("Clear auto-reload note")) {
                pendingAckAutoSeq = si->id;
            }
            if (!si->autoReloadNote.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Clear the (auto-reloaded) token from this stack's row.\n\n"
                    "The reload it describes is not undone and is not forgotten:\n"
                    "the same sentence is in Messages, the frame count is on the\n"
                    "row, and a reload that refused a member leaves its own mark\n"
                    "there.");
            // The series (系列) this stack is in, or could be. Multi-select is
            // a later phase; until then this menu is how a stack joins a sweep
            // without going through the Linearity panel.
            if (ImGui::BeginMenu("Series")) {
                const App::Series* mine = seriesOfStack(si->id);
                // The value joining would give this stack, BEFORE the click. The
                // modal shows its proposal and the picker paints one per row;
                // this was the one place a number entered a measurement without
                // having been on screen first (docs/series-plan.md §2 = 確認).
                std::string jfrom;
                double jv = extractLevelFromName(si->name, &jfrom);
                if (std::isfinite(jv)) ImGui::TextDisabled("joins at %.6g (from \"%s\")",
                                                           jv, jfrom.c_str());
                else ImGui::TextDisabled("joins with NO value (nothing numeric in the name)");
                int listed = 0;
                for (const auto& s : app.series) {
                    bool same = s.batchId == head.batchId;
                    bool isMine = mine && mine->id == s.id;
                    char lb2[416];
                    if (std::isfinite(jv) && same && !isMine)
                        snprintf(lb2, sizeof lb2, "%s   -> %.6g %s###addser%d", s.name.c_str(),
                                 jv, s.unit[0] ? s.unit : "[unit not set]", s.id);
                    else
                        snprintf(lb2, sizeof lb2, "%s###addser%d", s.name.c_str(), s.id);
                    if (ImGui::MenuItem(lb2, nullptr, isMine, same && !isMine)) {
                        pendingJoinSeq = si->id;
                        pendingJoinSer = s.id;
                    }
                    // A series cannot widen to reach another batch: the answer
                    // is to move the stack first, and the menu says so instead
                    // of leaving a dead item with no reason.
                    if (!same && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("\"%s\" is in batch \"%s\".\n"
                                          "A series lives in ONE batch - use Move to batch\n"
                                          "first, then add it here.",
                                          s.name.c_str(), batchNameOf(s.batchId).c_str());
                    listed++;
                }
                if (!listed) ImGui::TextDisabled("(no series yet)");
                ImGui::Separator();
                if (ImGui::MenuItem("New series...")) pendingNewSerBatch = head.batchId;
                if (mine && ImGui::MenuItem("Remove from this series"))
                    pendingLeaveSeq = si->id;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            // A stack that is ALREADY open gets the same operation Browse
            // offers at open time - one item, one meaning, one code path
            // (requestStackAverage). Sitting beside Derive is deliberate: both
            // make a NEW thing out of this stack and leave this stack alone,
            // and this is the one that crosses a layer (stack -> frame).
            if (ImGui::MenuItem("Open frame average")) pendingAvgSeq = si->id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", AVG_TIP);
            // The fold is a CHOICE (docs/analysis-layers.md §3.2): mean and sum
            // are two items with two names, not a mode hidden in a dialog - the
            // item's name is the declaration of what the resulting pixels hold,
            // exactly as the result's own name will be.
            if (ImGui::MenuItem("Open frame sum")) pendingSumSeq = si->id;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", SUM_TIP);
            // ---- the AnalysisPreProcessor (docs/analysis-layers.md §5) ------
            // The detrend stage lives HERE and nowhere else (判断6): it makes a
            // PRODUCT, and every analyzer then applies to that product
            // unchanged. There is deliberately no detrend option on Histogram /
            // Projection / ROIs / Temporal - one would make the same column
            // name carry a different quantity.
            //
            // The items are named after what they DO, the frame average / sum
            // precedent: the settled default is one item that says it is the
            // default, and every other band is an item of its own rather than a
            // number in a dialog nobody re-reads. The choice ends up in the
            // product's name, note and cutoff, so what is chosen here is what a
            // report will cite.
            if (si->dt.made()) {
                ImGui::TextDisabled("already a detrend product");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("This stack IS the output of the stage:\n%s\n"
                                      "One stage, one cutoff - change the method on the "
                                      "source stack\ninstead of removing a surface twice.",
                                      detrendCutoffText(si->dt.spec).c_str());
            } else if (ImGui::BeginMenu("Detrend (remove shading)")) {
                auto item = [&](const char* label, const DetrendSpec& s) {
                    if (ImGui::MenuItem(label)) {
                        pendingDetrendSeq = si->id;
                        pendingDetrendSpec = s;
                    }
                };
                {   // 判断7: "ON にしたときの既定方式 = 多項式2次・減算・プレーン別"
                    DetrendSpec s; s.method = DTR_POLY; s.degree = 2;
                    item("polynomial degree 2, subtractive  (the settled default)", s);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", DETREND_TIP);
                if (ImGui::BeginMenu("other bands")) {
                    ImGui::TextDisabled("a higher degree removes more, and every");
                    ImGui::TextDisabled("statistic below it is a function of that band");
                    for (int deg = 1; deg <= 4; deg++) {
                        if (deg == 2) continue;
                        char lb[64];
                        snprintf(lb, sizeof lb, "polynomial degree %d, subtractive", deg);
                        DetrendSpec s; s.method = DTR_POLY; s.degree = deg;
                        item(lb, s);
                    }
                    ImGui::Separator();
                    for (int blk : { 32, 64, 128 }) {
                        char lb[64];
                        snprintf(lb, sizeof lb, "block median, %d px window, subtractive", blk);
                        DetrendSpec s; s.method = DTR_BLOCKMED; s.block = blk;
                        item(lb, s);
                    }
                    ImGui::Separator();
                    {   // 約束6: offered, and warned about where it is offered
                        DetrendSpec s; s.method = DTR_POLY; s.degree = 2; s.divide = true;
                        item("polynomial degree 2, DIVIDE (multiplicative shading)", s);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Dividing by the surface scales the noise by 1/s, so\n"
                                          "the spatial variance stops being uniform across the\n"
                                          "field - and the estimators downstream assume it is.\n"
                                          "Prefer subtraction for noise statistics; the product\n"
                                          "says which one it was made with either way.");
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            // A NEW stack from a subset of this one - the dialog offers the
            // matching rules (same names / numbers as another stack, a range,
            // hand-picked). References frames; this stack is not touched.
            if (ImGui::MenuItem("Derive stack (a subset, by reference)..."))
                pendingDeriveSeq = si->id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Make a NEW stack from a subset of this one - e.g. only\n"
                                  "the frames that also exist in another stack - so two\n"
                                  "stacks with mismatched membership can be compared\n"
                                  "frame by frame. Frames are SHARED by reference (no\n"
                                  "pixel copy); this stack keeps every frame it has.");
            // ...and the stack OUT of the tool, in one file (#253). The same
            // verb the File menu and the canvas carry, aimed at the stack this
            // row names rather than at whichever one is current - which is the
            // reason it is worth repeating here at all.
            if (ImGui::MenuItem("Export as video (displayed levels)...")) {
                askStackVideoExport(si->id);
                ImGui::CloseCurrentPopup();   // the fps/format modal opens at
                                              // root level and would evict this
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every frame of this stack, in order, through the SAME\n"
                                  "mapping the screen and the PNG export use. Lossless:\n"
                                  "FFV1/MKV where ffmpeg is on PATH, uncompressed AVI\n"
                                  "otherwise.");
            ImGui::Separator();
            {   // the STACK moves whole; its frames never split across batches
                if (const App::Series* mine = seriesOfStack(si->id))
                    ImGui::TextDisabled("in series \"%s\": moving it alone leaves it",
                                        mine->name.c_str());
                int t = moveToBatchMenu(head.batchId);
                if (t) { pendingMoveSeq = si->id; pendingMoveTarget = t; }
            }
            if (ImGui::MenuItem("Close stack"))     // the stack layer's Close
                pendingCloseSeq = si->id;
            ImGui::EndPopup();
          }
          // The only shortcut in the program that used to act on an UNFOCUSED
          // panel, with no text-input and no popup gate - so F2 while typing in
          // the Browse filter opened a rename popup, and F2 while a modal was up
          // REPLACED that modal (OpenPopup at root level evicts what is there).
          if (active && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
              !ImGui::GetIO().WantTextInput &&
              !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
              ImGui::IsKeyPressed(ImGuiKey_F2, false))
              ImGui::OpenPopup("seqctx");
          // ---- Watch: WHERE the finding went (docs/features/watch/watch-design.md §5)
          // It used to be drawn HERE: an amber line under this row with the
          // finding on it and a [Reload] beside it, and a second amber line
          // under that when an automatic reload had run. Both are gone (the
          // 2026-09-28 report 「reload で行が増えるのは嫌、もう少し控えめな表示で
          // 知らせたい」): the finding is a TOKEN on the row's own label and
          // an amber bar down its left edge, the sentence is in the row's
          // tooltip and in its right-click menu, and the little `x` that
          // cleared the auto-reload note is a menu item. Nothing is inserted,
          // so a poll no longer moves every stack below this one.
          //
          // (seqWatchMark / seqAutoReloadMark / seqWatchTooltip in watch.inc
          // are the three strings; this row draws the first two above and the
          // menu above draws the third.)
          // No frame slider here: it used to appear under the active row, and a
          // row that grows on selection makes the whole list jump - the scrub bar
          // lives at the bottom of the Image View now, where the frames are.
          {   // ...and what this block drew, for the test that holds the line
              // above to account (see FilesStackRowProbe)
              FilesStackRowProbe pr;
              pr.seqId = head.seqId;
              const char* pl = lb;
              while (*pl == ' ') pl++;
              pr.label = pl;
              const size_t cut = pr.label.find("##");
              if (cut != std::string::npos) pr.label.resize(cut);
              pr.h = ImGui::GetCursorPos().y - probeY0;
              pr.bar = amberBar;
              pr.tip = probeTip;         // what the tooltip was built from, as
                                         // DRAWN rather than as re-derived by a
                                         // test (review P3-19)
              g_filesStackRowProbe.push_back(std::move(pr));
          }
          ImGui::PopID();
        };
        // ---- the batch's series come FIRST, each with its members inside -----
        // Non-member stacks and frames keep their place and indentation below:
        // a sweep appearing must not push everything else down a level.
        for (const auto& S : app.series) {
            if (S.batchId != group.batch) continue;
            float sy0 = ImGui::GetCursorPos().y;
            ImGui::PushID(S.id);
            bool sopen = drawSeriesHead(S, false);
            float shy = ImGui::GetCursorPos().y;
            if (sopen) {
                // memIdx is the member's place in the series, and it is the
                // row's leading column - see the label built in drawStackRow.
                for (int mi = 0; mi < (int)S.members.size(); mi++)
                    if (const std::vector<int>* sp =
                            filesRowOfSeriesMember(group, S.members[mi]))
                        drawStackRow(*sp, &S, &S.members[mi], mi);
                ImGui::TreePop();
            }
            ImGui::PopID();
            g_filesSpans.push_back({ group.batch, S.id, sy0, shy, ImGui::GetCursorPos().y });
        }
        // ---- the batch's SETS: one node each, its role rows under it --------
        // (docs/features/adapters/reader-analysisset.md §6). Reading order inside the node: set
        // -> role -> member. A role row is the BINDING's display; the member's
        // home stays the member's own stack row - the tree is not duplicated,
        // so a role row jumps (double-click) rather than re-parents.
        for (const auto& AS : app.analysisSets) {
            if (AS.batchId != group.batch) continue;
            ImGui::PushID(-AS.id);        // negative: never collides with seqIds
            ImGui::PushStyleColor(ImGuiCol_Text, inkOf(ui_theme::LayerSet));
            bool sopen = ImGui::TreeNodeEx("##aset", ImGuiTreeNodeFlags_DefaultOpen |
                                                     ImGuiTreeNodeFlags_SpanAvailWidth,
                                           "%s", aSetRowText(AS).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked()) { g_aSetSelId = AS.id; g_aSetSelRole = -1; }
            if (ImGui::IsItemHovered()) {
                std::string tip = "analysis set: " + std::to_string((int)AS.roles.size()) +
                                  " role binding(s)";
                if (!AS.readerSpec.empty()) tip += "\nmade by reader " + AS.readerSpec;
                tip += "\n\n(right-click: rename, close - Close drops the bindings"
                       "\nonly; the members are untouched)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (ImGui::BeginPopupContextItem("asetctx")) {
                static char abuf[256];
                if (ImGui::IsWindowAppearing())
                    snprintf(abuf, sizeof abuf, "%s", AS.name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
                bool done = ImGui::InputText("##aname", abuf, sizeof abuf,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                done |= ImGui::SmallButton("rename");
                if (done && abuf[0]) {
                    pendingSetRename = AS.id;
                    pendingSetName = abuf;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close set (drop bindings, keep members)"))
                    pendingCloseSet = AS.id;
                ImGui::EndPopup();
            }
            if (sopen) {
                for (int ri = 0; ri < (int)AS.roles.size(); ri++) {
                    const App::ASetRole& r = AS.roles[ri];
                    const bool sel = g_aSetSelId == AS.id && g_aSetSelRole == ri;
                    ImGui::PushID(ri);
                    if (!r.bound()) {
                        // unbound: the reason is ON the row (ras §3.2), in the
                        // same warning ink the partial-load note uses
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.76f, 0.35f, 1));
                        if (ImGui::Selectable(aSetRoleText(r).c_str(), sel))
                            { g_aSetSelId = AS.id; g_aSetSelRole = ri; }
                        ImGui::PopStyleColor();
                    } else {
                        if (ImGui::Selectable(aSetRoleText(r).c_str(), sel,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            g_aSetSelId = AS.id;
                            g_aSetSelRole = ri;
                            if (ImGui::IsMouseDoubleClicked(0)) {
                                // double-click COMMITS: go to the bound member
                                int jump = -1;
                                if (r.seqId != 0) {
                                    const App::SeqInfo* si2 = seqInfo(r.seqId);
                                    if (si2 && si2->lastImageIdx >= 0 &&
                                        si2->lastImageIdx < (int)app.images.size() &&
                                        app.images[si2->lastImageIdx]->seqId == r.seqId)
                                        jump = si2->lastImageIdx;
                                    else {
                                        std::vector<int> fr = framesOfSeq(r.seqId);
                                        if (!fr.empty()) jump = fr.front();
                                    }
                                } else if (r.frameUid != 0) {
                                    for (int q = 0; q < (int)app.images.size(); q++)
                                        if (app.images[q]->uid == r.frameUid) { jump = q; break; }
                                } else if (r.seriesId != 0) {
                                    for (const auto& s2 : app.series)
                                        if (s2.id == r.seriesId && !s2.members.empty()) {
                                            jump = filesImageIndexOfSeries(s2);
                                            break;
                                        }
                                }
                                if (jump >= 0) {
                                    selectImage(jump);
                                    if (app.fitOnSwitch) app.fitRequested = true;
                                }
                            }
                        }
                        if (ImGui::IsItemHovered() && !r.refPath.empty())
                            ImGui::SetTooltip("Ref %s\nclick selects; double-click goes "
                                              "to the bound member", r.refPath.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        // ---- then everything this batch holds that is NOT in a series -------
        for (const auto& stackPtr : group.stacks) {
            const ImageDoc& head = *app.images[stackPtr->front()];
            if (filesRowBelongsToSeries(head)) continue;  // already drawn above
            drawStackRow(*stackPtr, nullptr, nullptr, -1);
        }
      }
      if (showHeaders && open) ImGui::TreePop();
      ImGui::PopID();
      if (showHeaders)
          g_filesSpans.push_back({ group.batch, 0, gy0, ghy, ImGui::GetCursorPos().y });
    }
    g_filesScrollY = ImGui::GetScrollY();
    }
    ImGui::EndChild();
    if (pendingCloseSeq) closeStack(pendingCloseSeq);
    if (pendingCloseBatch) closeBatch(pendingCloseBatch);
    if (pendingMoveTarget && pendingMoveSeq) moveStackToBatch(pendingMoveSeq, pendingMoveTarget);
    if (pendingMoveTarget && pendingMoveImg >= 0) moveImageToBatch(pendingMoveImg, pendingMoveTarget);
    if (pendingDeriveSeq) deriveModalOpen(pendingDeriveSeq);
    // ONE frame, re-read (reference-design §3.2). The whole of its sentence
    // lives in reloadOneFrameText beside the walk it describes - the shared
    // memberships, the stale reductions - because the asynchronous door's
    // landing reaches the same sentence some frames later (pumpReloadJob), and
    // a second wording of one event is the failure #71 is about.
    //
    // startFrameReload first, for the reason the stack's Reload takes the
    // worker: a 480 MB frame is the same 0.45 s of a window answering nothing
    // that issue #232 stage 2 moved off this thread for an OPEN of that very
    // file. False = it declined (a scripted run), and then this is the
    // synchronous door it always was.
    if (pendingReloadImg >= 0 && pendingReloadImg < (int)app.images.size() &&
        !startFrameReload(pendingReloadImg)) {
        ImageDoc* d = app.images[pendingReloadImg].get();
        std::string err, note;
        if (reloadSource(d->src, err, &note)) toast(reloadOneFrameText(*d, note));
        else toast("reload failed - " + baseName(d->src->path) + ": " + err, true);
    }
    // ONE reload path (watch-design §9): this is watchAutoDrain's own call with
    // automatic=false. The toast, the clearing of the Watch line and the record
    // it leaves all live in watchReloadNow, so the click and the program cannot
    // come to do two different things - which is the failure §9 is most exposed
    // to, because the second one is the one nobody watches.
    if (pendingReloadSeq) watchReloadNow(pendingReloadSeq, false);
    if (pendingAckAutoSeq) {
        // BOTH, always together - state.h declares that as the invariant of the
        // pair, and clearing only the note left autoReloadWhen holding a clock
        // for a note nobody can read (review P3-13).
        if (App::SeqInfo* si = seqInfo(pendingAckAutoSeq)) {
            si->autoReloadNote.clear();
            si->autoReloadWhen.clear();
        }
    }
    if (pendingRecomputeImg >= 0 && pendingRecomputeImg < (int)app.images.size()) {
        std::string err;   // success toasts inside (the reduction's own voice)
        if (!recomputeStackAverage(app.images[pendingRecomputeImg].get(), err))
            toast("recompute failed - " + err, true);
    }
    if (pendingAvgSeq) requestStackAverage(pendingAvgSeq);
    if (pendingSumSeq) requestStackAverage(pendingSumSeq, FOLD_SUM);
    if (pendingDetrendSeq) {
        // A detrend READS every member to compute its product, so a reload in
        // flight would fold two generations of the file into one shading
        // estimate (reloadInFlightFor says why, and says it in the Files row's
        // own vocabulary).
        if (const char* busy = reloadInFlightFor(pendingDetrendSeq, 0)) {
            toast(busy, true);
        } else {
            std::string err;   // the success toast is the product's own voice
            if (!detrendStackProduct(pendingDetrendSeq, pendingDetrendSpec, err))
                toast(err, true);
        }
    }
    if (pendingCloseSet) closeAnalysisSet(pendingCloseSet);
    if (pendingSetRename) {
        std::string why;
        if (!renameAnalysisSet(pendingSetRename, pendingSetName, &why) && !why.empty())
            toast(why, true);   // the one refusal: same name in one batch (ras 1.4)
    }
    // ---- the series commands, after the walk that drew them -----------------
    if (pendingSerRename)
        if (App::Series* S = seriesById(pendingSerRename)) {
            S->name = pendingSerName;
            app.imagesRev++;
            app.lin.rev++;                // the panel's selector shows this name
        }
    if (pendingSerEdit)
        if (App::Series* S = seriesById(pendingSerEdit)) openSeriesModal(S->batchId, S->id);
    if (pendingSerMove && pendingSerMoveTo) moveSeriesToBatch(pendingSerMove, pendingSerMoveTo);
    if (pendingSerUngroup) ungroupSeries(pendingSerUngroup);
    if (pendingSerClose) closeSeries(pendingSerClose);
    if (pendingNewSerBatch) openSeriesModal(pendingNewSerBatch, 0);
    if (pendingLeaveSeq) {
        App::Series* S = seriesOfStack(pendingLeaveSeq);
        App::SeqInfo* si = seqInfo(pendingLeaveSeq);
        if (S) {
            std::string n = S->name;
            removeFromSeries(pendingLeaveSeq);
            pruneEmptySeries();
            linFitStale();          // the fit below counted this point
            toast((si ? si->name : std::string("stack")) + " left series \"" + n + "\"");
        }
    }
    if (pendingJoinSeq && pendingJoinSer) {
        std::string msg;
        bool joined = seriesJoinFromMenu(pendingJoinSer, pendingJoinSeq, &msg);
        if (!msg.empty()) toast(msg, !joined);
    }
}

static void drawSequenceModal() {
    if (app.seqAskImage >= 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::OpenPopup("Load stack?");
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Load stack?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("%d files match %s", (int)app.seqAskFiles.size(),
                dispPath(app.seqAskPattern).c_str());
    ImGui::TextDisabled("Loading them as one stack enables temporal analysis.");
    ImGui::TextDisabled("Frames decode in the background; you can keep working.");
    ImGui::Separator();
    static bool remember = false;
    ImGui::Checkbox("remember my choice (File > Stack loading)", &remember);
    if (ImGui::Button("Load stack", ImVec2(150 * app.uiScale, 0))) {
        startSequenceLoad(app.seqAskImage, app.seqAskFiles, app.seqAskPattern);
        if (remember) app.seqLoadMode = 1;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("This file only", ImVec2(150 * app.uiScale, 0))) {
        if (remember) app.seqLoadMode = 2;
        app.seqAskImage = -1; app.seqAskFiles.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

