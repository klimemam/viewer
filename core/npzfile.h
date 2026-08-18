// The .npz, in the part BOTH binaries need (issue #217, docs/
// remote-reader-design.md §10).
//
// A .npz is a zip of .npy members. Reading one has always been core/app/
// loader_npz.inc's job, because a container becomes DOCUMENTS and only the
// client builds documents. What changed with #217 is that the peer now has to
// answer "what is in this file" over the link - and the two things that
// question needs are a zip directory walk and a .npy header peek, neither of
// which has anything to do with documents.
//
// WHAT IS SHARED AND WHAT IS NOT, deliberately - the same split core/vstream.h
// draws, and for #71's reason:
//
//   shared      the zip walk, the inflate, the .npy header peek, and the
//               FACTS a member states about itself (its name, its declared
//               size, the bytes of its header, and - when it is small enough
//               to be worth carrying - its values). A fact is what the side
//               holding the bytes can observe.
//   shared      the refusals those steps produce ("truncated zip member",
//               "inflate failed", "corrupt npy header"). A refusal the peer
//               sends is read by a person looking at this viewer's window, so
//               it has to be the sentence the local door would have used.
//   not shared  the CLASSIFICATION - which member is pixels, which is an axis
//               candidate, which is metadata, and the words a picker row shows
//               for each. That vocabulary (RImage / RAxis / RMeta / RAmbig /
//               RBad) is docs/npz-design.md §2.1's and it stays in ONE place,
//               core/app/loader_npz.inc, fed by these facts whether they came
//               out of a zip on this disk or out of a MSG_NPZ_SCAN reply. The
//               picker's rows therefore cannot read differently depending on
//               which end listed the file, by construction rather than by
//               promise (§10.2).
#pragma once
#include <stdint.h>
#include <stddef.h>                  // ptrdiff_t, SIZE_MAX
#include <stdlib.h>                  // strtoll, for a shape that must not throw
#include <string.h>                  // memcmp / memcpy for the .npy magic
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "miniz.h"                   // inflate; both binaries already link it

namespace nz {

// One member of the zip, as the central directory describes it.
struct Entry {
    std::string name;                // the zip member name, ".npy" included
    uint64_t localOff = 0, csize = 0, usize = 0;
    uint16_t method = 0;             // 0 stored, 8 deflate
};

// The .npy header, parsed ONCE.
//
// shape is the shape AS WRITTEN: a scalar is 0-D and stays 0-D here, because
// "() versus (1,) versus (1,1)" is the whole question the .npz classifier asks.
// esize 0 = a dtype this viewer cannot read as pixels (strings, object arrays);
// that is a fact about the member, not an error, so the peek still succeeds and
// the caller decides. The decoder and the classifier share this function so
// what the picker promises and what actually opens cannot drift apart.
struct Head {
    std::vector<int64_t> shape;
    std::string descr;                // raw, e.g. "<u2" / "<U8" / "|O"
    std::string code;                 // descr without the byte-order character
    std::string dtypeName;            // "u16" ... empty when esize == 0
    int esize = 0;
    bool fortran = false, be = false;
    size_t dataOff = 0;
};

// Return the end of a .npy header from the format's fixed prefix.  Version 1
// stores a u16 length at +8; later versions store a u32 length there.  Keeping
// the result in uint64_t matters on 32-bit builds: 12 + UINT32_MAX cannot be
// represented by size_t, and this value must be checked before it is cast for
// an allocation.
inline bool npyHeaderEnd(const std::vector<uint8_t>& pre, uint64_t& headerEnd,
                         std::string& err) {
    auto fail = [&](const char* m) { err = m; return false; };
    if (pre.size() < 10 || pre[0] != 0x93 || memcmp(&pre[1], "NUMPY", 5) != 0)
        return fail("not a .npy file (bad magic)");
    if (pre[6] >= 2) {
        if (pre.size() < 12) return fail("corrupt npy header");
        uint32_t v;
        memcpy(&v, &pre[8], 4);
        headerEnd = 12ull + (uint64_t)v;
    } else {
        uint16_t v;
        memcpy(&v, &pre[8], 2);
        headerEnd = 10ull + (uint64_t)v;
    }
    return true;
}

// Check both limits that stand between a 64-bit length from a file and a
// vector allocation on this build.  This is asked before resize/assign, never
// after a narrowing cast.
inline bool fitsAlloc(uint64_t n) {
    const std::vector<uint8_t> probe;
    const uint64_t cap = std::min<uint64_t>((uint64_t)probe.max_size(),
                                            (uint64_t)(size_t)-1);
    return n <= cap;
}

// miniz's deflate bound, expressed without the file-controlled multiply-add
// `csize * 1032 + 1024`.  The ceil form is exactly equivalent at the boundary
// and cannot overflow for any uint64_t input.
inline bool deflateSizePlausible(uint64_t usize, uint64_t csize) {
    const uint64_t excess = usize > 1024 ? usize - 1024 : 0;
    const uint64_t minCompressed =
        excess / 1032 + (excess % 1032 == 0 ? 0 : 1);
    return csize >= minCompressed;
}

inline bool peekHeader(const std::vector<uint8_t>& buf, Head& H, std::string& err) {
    auto fail = [&](const char* m) { err = m; return false; };
    if (buf.size() < 10 || buf[0] != 0x93 || memcmp(&buf[1], "NUMPY", 5) != 0)
        return fail("not a .npy file (bad magic)");
    int major = buf[6];
    uint64_t hlen = 0, hoff = 0;
    if (major >= 2) {
        if (buf.size() < 12) return fail("corrupt npy header");
        uint32_t v; memcpy(&v, &buf[8], 4); hlen = v; hoff = 12;
    } else {
        uint16_t v; memcpy(&v, &buf[8], 2); hlen = v; hoff = 10;
    }
    const uint64_t bsz = (uint64_t)buf.size();
    if (hoff > bsz || hlen > bsz - hoff) return fail("corrupt npy header");
    // data() + hoff, never &buf[hoff]: a 10-byte v1 file declaring hlen 0 passes
    // the check above with hoff == buf.size(), and the subscript is then one
    // past the end - undefined, and an abort under -D_GLIBCXX_ASSERTIONS. The
    // empty header is refused by "cannot parse descr" either way.
    std::string hdr((const char*)buf.data() + (size_t)hoff, (size_t)hlen);
    auto findQuoted = [&](const char* key) -> std::string {
        size_t k = hdr.find(key);
        if (k == std::string::npos) return {};
        size_t q1 = hdr.find('\'', hdr.find(':', k));
        if (q1 == std::string::npos) return {};
        size_t q2 = hdr.find('\'', q1 + 1);
        return hdr.substr(q1 + 1, q2 - q1 - 1);
    };
    H.descr = findQuoted("'descr'");
    if (H.descr.empty()) return fail("cannot parse descr");
    H.fortran = hdr.find("'fortran_order': True") != std::string::npos;
    size_t sp = hdr.find("'shape'");
    size_t p1 = hdr.find('(', sp), p2 = hdr.find(')', sp);
    if (p1 == std::string::npos || p2 == std::string::npos) return fail("cannot parse shape");
    H.shape.clear();
    {
        std::string s = hdr.substr(p1 + 1, p2 - p1 - 1);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t c = s.find(',', pos);
            std::string tok = s.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
            // strtoll rather than stoll: a shape this malformed is a fact about
            // the member and must not throw out of a peek the peer runs on
            // every file of a listing.
            if (tok.find_first_of("0123456789") != std::string::npos)
                H.shape.push_back((int64_t)strtoll(tok.c_str(), nullptr, 10));
            if (c == std::string::npos) break;
            pos = c + 1;
        }
    }
    char bo = '<';
    H.code = H.descr;
    if (!H.code.empty() && (H.code[0] == '<' || H.code[0] == '>' ||
                            H.code[0] == '|' || H.code[0] == '=')) {
        bo = H.code[0]; H.code = H.code.substr(1);
    }
    H.be = (bo == '>');
    H.esize = 0;
    H.dtypeName.clear();
    if      (H.code == "u1") { H.esize = 1; H.dtypeName = "u8"; }
    else if (H.code == "i1") { H.esize = 1; H.dtypeName = "i8"; }
    else if (H.code == "b1") { H.esize = 1; H.dtypeName = "bool"; }
    else if (H.code == "u2") { H.esize = 2; H.dtypeName = "u16"; }
    else if (H.code == "i2") { H.esize = 2; H.dtypeName = "i16"; }
    else if (H.code == "u4") { H.esize = 4; H.dtypeName = "u32"; }
    else if (H.code == "i4") { H.esize = 4; H.dtypeName = "i32"; }
    else if (H.code == "f4") { H.esize = 4; H.dtypeName = "f32"; }
    else if (H.code == "f8") { H.esize = 8; H.dtypeName = "f64"; }
    H.dataOff = (size_t)(hoff + hlen);       // bounded by buf.size(), above
    return true;
}

// One element of a numeric member as a DOUBLE, byte order INCLUDED.
//
// A double and not a float because an axis is the quantity data is plotted
// against, so it must not pass through float on the way in: the pixel path's
// getVal returns float and that is right for pixels, but not for
// 12345.678901234567.
//
// It lives HERE, beside peekHeader, because both machines decode the same
// bytes and there may only be one answer (#221 review). The peer read
// `__viewer`'s version with a host-order memcpy of its own, so a container
// written big-endian - `>i4`, which numpy writes the moment its author says so
// - was version 1 to this side and version 16777216 to the peer. The peer then
// carried that number to the client as the file's declared version and the
// client refused a file it opens perfectly well when the same bytes are local.
// A header field whose meaning depends on which machine looked is not a
// format; the descr already says which order the bytes are in, and now the
// only decoder that exists reads it.
inline double elem(const std::vector<uint8_t>& buf, const Head& H, size_t i) {
    if (H.esize <= 0) return 0;
    // Bounds are the DECODER's business, not each caller's: a member whose
    // header promises more than the file holds is a fact about the file, and
    // the peer meets those on input it did not write.
    const size_t es = (size_t)H.esize;
    if (i > (size_t)-1 / es) return 0;
    const size_t off = H.dataOff + i * es;
    if (off < H.dataOff || off > buf.size() || es > buf.size() - off) return 0;
    const uint8_t* p = buf.data() + off;
    auto bswap = [](uint64_t v, int n) {
        uint64_t r = 0;
        for (int k = 0; k < n; k++) r = (r << 8) | ((v >> (8 * k)) & 0xff);
        return r;
    };
    switch (H.esize) {
    case 1: return H.code == "i1" ? (double)*(const int8_t*)p : (double)*p;
    case 2: { uint16_t u; memcpy(&u, p, 2); if (H.be) u = (uint16_t)bswap(u, 2);
              return H.code == "i2" ? (double)(int16_t)u : (double)u; }
    case 4: { uint32_t u; memcpy(&u, p, 4); if (H.be) u = (uint32_t)bswap(u, 4);
              if (H.code == "f4") { float f; memcpy(&f, &u, 4); return (double)f; }
              return H.code == "i4" ? (double)(int32_t)u : (double)u; }
    case 8: { uint64_t u; memcpy(&u, p, 8); if (H.be) u = bswap(u, 8);
              double d; memcpy(&d, &u, 8); return d; }
    }
    return 0;
}

// A zip64 record that does not hold together, said with the MEMBER in it
// (#221 review). The peer runs this walk on a file the client named, so the
// refusal it sends is the only thing the person ever sees: "corrupt zip" with
// no name is a sentence about a container of forty arrays that says nothing
// about which one, and a peer that instead read past the buffer would not be
// sending a sentence at all.
inline std::string zip64Bad(const std::string& member, const std::string& why) {
    return "corrupt zip64 record for member \"" + member + "\": " + why;
}

// ...and the same sentence for the records that belong to the CONTAINER rather
// than to any one member (#180 codex supplement). The end record, its zip64
// locator and the central directory's own offset are read before a name exists,
// so "which member" has no answer - but "which record" does, and a person
// holding a file another program wrote needs that word to know who to ask.
inline std::string zipBad(const std::string& why) {
    return "corrupt zip container: " + why;
}

// ---- THE ONE BOUNDS RULE, and every read in this file goes through it -------
//
// #180 codex supplement. A zip64 record states its offsets as 64-bit numbers
// that a file - or an attacker - may set to anything, and the walk asked
// "does offset + needed fit?". Near UINT64_MAX that sum WRAPS: `z64 + 56 <=
// buf.size()` is true for z64 = 2^64 - 8, and the rd32() that follows reads at
// 2^64 - 8. The peer runs this on a file a client merely NAMED, so the wrap is
// an out-of-bounds read on the machine that holds the data, and a peer that
// reads out of bounds does not send a refusal - it dies, and takes the session.
//
// So no bound in this file is ever written as a sum. `off <= size` first, then
// `needed <= size - off`: both operands of every comparison are values that
// already exist, the subtraction cannot wrap because the first test established
// off <= size, and the rule reads the same at every call site. Everything is
// done in uint64_t and only converted to size_t AFTER it has been bounded, so a
// 32-bit build cannot truncate an offset into range either.
inline bool zipFits(uint64_t off, uint64_t needed, uint64_t size) {
    return off <= size && needed <= size - off;
}

// Minimal zip reader for npz: central-directory walk, stored (0) and deflate
// (8), with zip64 sizes. Inflate comes from miniz.
inline bool list(const std::vector<uint8_t>& buf, std::vector<Entry>& out, std::string& err) {
    auto rd16 = [&](size_t o) { return (uint16_t)(buf[o] | buf[o + 1] << 8); };
    auto rd32 = [&](size_t o) {
        return (uint32_t)buf[o] | (uint32_t)buf[o + 1] << 8 |
               (uint32_t)buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24;
    };
    auto rd64 = [&](size_t o) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= (uint64_t)buf[o + i] << (8 * i);
        return v;
    };
    out.clear();
    if (buf.size() < 22) { err = "not a zip file"; return false; }
    const uint64_t bsz = (uint64_t)buf.size();
    size_t eocd = SIZE_MAX;
    size_t start = buf.size() > 65557 ? buf.size() - 65557 : 0;
    for (size_t i = buf.size() - 22 + 1; i-- > start;)
        if (rd32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == SIZE_MAX) { err = "not a zip file (no end record)"; return false; }
    uint64_t count = rd16(eocd + 10);
    uint64_t cdSize = rd32(eocd + 12);
    uint64_t cdOff = rd32(eocd + 16);
    const bool needZip64 = cdOff == 0xffffffffu || cdSize == 0xffffffffu;
    // 0xffffffff in the end record is not a number: it is the file SAYING "the
    // real one is in the zip64 record". So a file that says it and then does
    // not hold a zip64 record that fits is refused HERE, by that name - the old
    // code fell through with cdOff still 0xffffffff and produced "zip contains
    // no entries", which is a sentence about the wrong thing.
    //
    // A count of 0xffff alone is NOT that claim (a plain zip may legitimately
    // hold exactly 65535 members), so it still only opts into the lookup.
    if (needZip64 || count == 0xffffu) {                    // zip64, when present
        bool got64 = false;
        if (eocd >= 20 && rd32(eocd - 20) == 0x07064b50) {
            const uint64_t z64 = rd64(eocd - 20 + 8);
            // A zip64 end record is [signature][u64 size][size bytes].  Merely
            // checking the first fixed 56 bytes is insufficient: a record can
            // claim a longer body that runs into its locator or past the file.
            // Validate the declared span by subtraction before reading fields.
            const uint64_t locator = (uint64_t)eocd - 20;
            if (zipFits(z64, 12, bsz) && z64 <= locator &&
                rd32((size_t)z64) == 0x06064b50) {
                const uint64_t recordSize = rd64((size_t)z64 + 4);
                const uint64_t room = locator - z64;
                if (recordSize >= 44 && room >= 12 && recordSize <= room - 12) {
                    count = rd64((size_t)z64 + 32);
                    cdSize = rd64((size_t)z64 + 40);
                    cdOff = rd64((size_t)z64 + 48);
                    got64 = true;
                }
            }
        }
        // A plain archive may really contain exactly 65535 entries, so the
        // count sentinel alone only asks us to use zip64 if it is present.
        // Offset and size sentinels, however, have no numeric meaning without
        // that record and must never fall through as ordinary values.
        if (!got64 && needZip64) {
            err = zipBad("its end record defers to a zip64 end record that is not "
                         "there, is incomplete, or starts past the end of the file");
            return false;
        }
    }
    // ...and the central directory's own offset, which after the block above may
    // be any 64-bit number the file chose. Refused by name rather than left to
    // wrap into range inside the loop condition.
    if (count && !zipFits(cdOff, 46, bsz)) {
        err = zipBad("its central directory starts past the end of the file");
        return false;
    }
    if (!zipFits(cdOff, cdSize, bsz)) {
        err = zipBad("its central directory runs past the end of the file");
        return false;
    }

    // Read exactly the count the end record declares.  A short or malformed
    // later record invalidates the container as a whole; returning the prefix
    // already walked would silently remove rows from the member picker.
    std::vector<Entry> listed;
    uint64_t p = cdOff, left = cdSize;
    for (uint64_t i = 0; i < count; i++) {
        auto shortDirectory = [&]() {
            err = zipBad("its end record declares " + std::to_string(count) +
                         " entries and its central directory holds " +
                         std::to_string(i));
            return false;
        };
        if (!zipFits(p, 46, bsz) || 46 > left) return shortDirectory();
        if (rd32((size_t)p) != 0x02014b50) {
            err = zipBad("entry " + std::to_string(i) + " of its central directory "
                         "does not begin with a central-directory signature");
            return false;
        }
        Entry e{};
        e.method = rd16((size_t)p + 10);
        e.csize = rd32((size_t)p + 20);
        e.usize = rd32((size_t)p + 24);
        const uint64_t nlen = rd16((size_t)p + 28), elen = rd16((size_t)p + 30),
                       clen = rd16((size_t)p + 32);
        e.localOff = rd32((size_t)p + 42);
        // The fixed header and all three variable fields form one record.  Its
        // entire span has to fit both the file and the declared directory size
        // before a name or extra field is read.
        const uint64_t span = 46 + nlen + elen + clen;
        if (!zipFits(p, span, bsz) || span > left) return shortDirectory();
        // data() + off again: a central-directory header that ends exactly at
        // the last byte with a zero-length name satisfies both bounds above and
        // then subscripts one past the end.
        e.name.assign((const char*)buf.data() + (size_t)(p + 46), (size_t)nlen);
        if (e.csize == 0xffffffffu || e.usize == 0xffffffffu || e.localOff == 0xffffffffu) {
            // ---- the zip64 extra field (APPNOTE 4.5.3) ----------------------
            //
            // #221 review. Every value below is EIGHT BYTES read at a length
            // the file itself declared, and the old code checked only that the
            // four-byte extra-field HEADER fit: an extra field with id 1 and a
            // size of 0 then had rd64() read up to 24 bytes past the end of
            // `buf`. On the peer that is a crash instead of an answer, from a
            // file a client merely named.
            //
            // So each read is checked against BOTH ends, immediately before it
            // happens: the declared end of the extra block (`fend`, which the
            // record promises) and the end of the buffer actually held
            // (`buf.size()`, which is the truth). They are different questions
            // - a record can promise more than the file contains - and the
            // arithmetic is done by SUBTRACTION so that no sum can wrap.
            const size_t ep0 = (size_t)(p + 46 + nlen);                // <= buf.size(), above
            if ((size_t)elen > buf.size() - ep0) {
                err = zip64Bad(e.name, "its extra field runs past the end of the file");
                return false;
            }
            const size_t eend = ep0 + elen;
            size_t ep = ep0;
            bool got = false;
            while (ep <= eend && eend - ep >= 4) {
                const uint16_t id = rd16(ep), sz = rd16(ep + 2);
                const size_t body = ep + 4;                   // <= eend <= buf.size()
                if ((size_t)sz > eend - body) {
                    err = zip64Bad(e.name,
                                   "an extra field claims more bytes than the record holds");
                    return false;
                }
                const size_t fend = body + sz;
                if (id != 1) { ep = fend; continue; }
                size_t q = body;
                // WHICH value was missing, by name. "usize / csize / localOff"
                // are three different truncations of the same field and a
                // person looking at the message is trying to find out which
                // writer produced the file.
                bool bad = false;
                auto take = [&](uint64_t& dst, const char* what) {
                    if (fend - q < 8 || buf.size() - q < 8) {
                        err = zip64Bad(e.name, std::string("its zip64 extra field stops "
                                                           "before the ") + what);
                        bad = true;
                        return;
                    }
                    dst = rd64(q);
                    q += 8;
                };
                if (e.usize == 0xffffffffu)    take(e.usize, "uncompressed size");
                if (!bad && e.csize == 0xffffffffu)    take(e.csize, "compressed size");
                if (!bad && e.localOff == 0xffffffffu) take(e.localOff, "local header offset");
                if (bad) return false;
                got = true;
                break;
            }
            if (!got) {
                err = zip64Bad(e.name, "it declares zip64 sizes and carries no zip64 "
                                       "extra field");
                return false;
            }
        }
        listed.push_back(std::move(e));
        p += span;                         // proved to fit, above
        left -= span;
    }

    // A central-directory digital signature is a legal record after the file
    // headers and is included in cdSize.  Apart from that one defined trailer,
    // the declared size must be consumed exactly by the declared records.
    if (left) {
        if (left >= 6 && rd32((size_t)p) == 0x05054b50) {
            const uint64_t sigSpan = 6 + (uint64_t)rd16((size_t)p + 4);
            if (sigSpan == left && zipFits(p, sigSpan, bsz)) {
                p += sigSpan;
                left = 0;
            }
        }
        if (left) {
            err = zipBad("its central-directory size does not match its complete records");
            return false;
        }
    }
    if (listed.empty()) { err = "zip contains no entries"; return false; }
    out.swap(listed);
    return true;
}

// Where a member's bytes begin in the zip, or 0 when the local header does not
// hold together. Its own function because the peer wants the same arithmetic
// without the extraction (it copies the compressed run out to a cache file).
inline bool dataStart(const std::vector<uint8_t>& zip, const Entry& e, size_t& at,
                      std::string& err) {
    // localOff and csize come out of the zip64 extra field, so both are numbers
    // the FILE chose and either may be near UINT64_MAX. `localOff + 30` and
    // `at + csize` both wrapped there and let a read through at an offset the
    // buffer never held (#180 codex supplement) - so every bound here is
    // zipFits, and the member is NAMED, because a person holding a container of
    // forty arrays needs to know which one their writer got wrong.
    const uint64_t zsz = (uint64_t)zip.size();
    if (!zipFits(e.localOff, 30, zsz)) {
        err = "corrupt local header for member \"" + e.name +
              "\": it starts past the end of the file";
        return false;
    }
    auto rd16 = [&](size_t o) { return (uint16_t)(zip[o] | zip[o + 1] << 8); };
    auto rd32 = [&](size_t o) {
        return (uint32_t)zip[o] | (uint32_t)zip[o + 1] << 8 |
               (uint32_t)zip[o + 2] << 16 | (uint32_t)zip[o + 3] << 24;
    };
    if (rd32((size_t)e.localOff) != 0x04034b50) {
        err = "corrupt local header for member \"" + e.name +
              "\": there is no local header where the directory says there is one";
        return false;
    }
    if (rd16((size_t)e.localOff + 8) != e.method) {
        err = "corrupt local header for member \"" + e.name +
              "\": it and the directory disagree about how the member is compressed";
        return false;
    }
    const uint64_t nlen = rd16((size_t)e.localOff + 26);
    const uint64_t elen = rd16((size_t)e.localOff + 28);
    // Check the complete local-header span before forming its end or comparing
    // the repeated member name.  nlen/elen are only 16-bit, but localOff is a
    // file-controlled zip64 value.
    const uint64_t span = 30 + nlen + elen;
    if (!zipFits(e.localOff, span, zsz)) {
        err = "corrupt local header for member \"" + e.name +
              "\": it runs past the end of the file";
        return false;
    }
    if (nlen != (uint64_t)e.name.size() ||
        (nlen && memcmp(zip.data() + (size_t)e.localOff + 30, e.name.data(),
                        (size_t)nlen) != 0)) {
        err = "corrupt zip: the directory entry for member \"" + e.name +
              "\" points at another member's bytes";
        return false;
    }
    const uint64_t start = e.localOff + span;       // proved to fit, above
    if (!zipFits(start, e.csize, zsz)) {
        err = "truncated zip member \"" + e.name + "\"";
        return false;
    }
    at = (size_t)start;
    return true;
}

inline bool extract(const std::vector<uint8_t>& zip, const Entry& e,
                    std::vector<uint8_t>& out, std::string& err) {
    size_t data = 0;
    if (!dataStart(zip, e, data, err)) return false;
    if (e.method == 0) {                                     // stored
        out.assign(zip.begin() + (ptrdiff_t)data, zip.begin() + (ptrdiff_t)(data + e.csize));
        return true;
    }
    if (e.method != 8) { err = "unsupported zip compression method"; return false; }
    // ...and the DECLARED uncompressed size is a 64-bit number out of the same
    // zip64 record, so it is bounded before it becomes an allocation. Deflate
    // cannot expand by more than 1032:1, so anything above that is a claim the
    // compressed run could not possibly redeem - refused as a fact about the
    // member instead of thrown as a bad_alloc from the middle of a walk.
    // Preserve the exact `usize > csize * 1032 + 1024` boundary without ever
    // forming that multiply-add.  The comparison is equivalent to asking
    // whether csize is smaller than ceil((usize - 1024) / 1032).
    if (!deflateSizePlausible(e.usize, e.csize)) {
        err = "corrupt zip member \"" + e.name +
              "\": it declares more uncompressed bytes than its compressed run can hold";
        return false;
    }
    if (!fitsAlloc(e.usize)) {
        err = "corrupt zip member \"" + e.name +
              "\": it declares more uncompressed bytes than this machine can hold";
        return false;
    }
    // zip members are RAW deflate (no zlib header), and the uncompressed size is
    // known from the directory, so decompress straight into the output buffer
    out.resize((size_t)e.usize);
    size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(),
                                             zip.data() + data, (size_t)e.csize, 0);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { err = "inflate failed"; return false; }
    out.resize(got);
    return true;
}

// Just enough bytes to read a .npy HEADER out of a member. The classifier looks
// at ~128 bytes of each member, and inflating every member in FULL to see them
// meant opening a multi-GB .npz decompressed the whole file on the UI thread
// before the dialog appeared - and a session restore paid it again, per member.
// Deflate is streamed into a fixed buffer and stopped when that buffer is full:
// HAS_MORE_OUTPUT is the expected outcome here, not a failure.
inline bool extractPrefix(const std::vector<uint8_t>& zip, const Entry& e,
                          size_t want, std::vector<uint8_t>& out, std::string& err) {
    size_t data = 0;
    if (!dataStart(zip, e, data, err)) return false;
    const uint64_t capWanted = std::min<uint64_t>((uint64_t)want, e.usize);
    if (!fitsAlloc(capWanted)) {
        err = "corrupt zip member \"" + e.name +
              "\": it declares more uncompressed bytes than this machine can hold";
        return false;
    }
    const size_t cap = (size_t)capWanted;
    if (e.method == 0) {                                     // stored
        size_t nCopy = (size_t)std::min<uint64_t>(cap, e.csize);
        out.assign(zip.begin() + (ptrdiff_t)data, zip.begin() + (ptrdiff_t)(data + nCopy));
        return true;
    }
    if (e.method != 8) { err = "unsupported zip compression method"; return false; }
    out.assign(cap, 0);
    if (cap == 0) return true;
    auto inf = std::make_unique<tinfl_decompressor>();   // ~11 KB: not a stack object
    tinfl_init(inf.get());
    size_t inBytes = (size_t)e.csize, outBytes = cap;
    tinfl_status st = tinfl_decompress(inf.get(), zip.data() + data, &inBytes,
                                       out.data(), out.data(), &outBytes,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE && st != TINFL_STATUS_HAS_MORE_OUTPUT) {
        err = "inflate failed";
        return false;
    }
    out.resize(outBytes);
    return true;
}

// ---- the facts a member states, and the rule for which VALUES ride along ----
//
// docs/remote-reader-design.md §10.2: "SCAN が返すのは事実 (name・shape・descr・
// usize・展開可否・小さい値) で、役割はその事実から client が判定する".
//
// `bytes` is the member's own .npy bytes - its HEADER always, and its values too
// when `whole`. Carrying the header rather than the fields parsed out of it is
// what makes the two doors agree about fortran order, byte order and item size
// without a field-by-field wire format that could omit one of them: the client
// runs peekHeader() on exactly the bytes the peer ran it on.
struct Fact {
    std::string name;          // array name, without the ".npy" the member has
    uint64_t usize = 0;        // what the zip directory says the member holds
    size_t entry = 0;          // index into the LISTING this fact came from. The
                               // peer issues it with the key and the client
                               // quotes it back as the node of a META / TILE -
                               // it is never computed on the client (§10.5).
    std::string err;           // why the header could not be read; "" = it was
    std::vector<uint8_t> bytes;
    bool whole = false;        // `bytes` is the entire member, values included
};

// A 1-D member longer than this cannot be any stack's frame axis (a served
// stack tops out at 2^20 frames - core/serve.cpp serveLayout), so its values
// are not worth carrying and not worth reading. THE LOCAL DOOR USED 2^24 and
// this tightens it, which docs/remote-reader-design.md §10.2 decides on purpose:
// the observable difference is the wording of members that were called axis
// candidates and could never have been promoted.
static const uint64_t INLINE_MAX_ELEMS = 1ull << 20;
// ...and the ceiling in BYTES, which is what actually has to fit in a message:
// 2^20 f8 values. A reserved member of a viewer container above it is left
// un-inlined rather than refused - the container reader then says which member
// it could not read, which is a better sentence than a size this file has no
// reason to know about.
static const uint64_t INLINE_MAX_BYTES = 8ull << 20;

// ...and the TOTAL, which is a different question and the one a wire has to ask
// (#221 review). Every member of a valid container can pass the per-member rule
// above and the file still be unlistable: 68 axes of 2^20 f8 elements are 68
// members of 8 MiB each - each of them legal, each of them wanted - and 544 MiB
// of reply, which core/remote.cpp refuses at 512 MiB as "oversized reply from
// the peer". So the sum has an owner too, and it is the side that fills the
// message.
//
// AND THE SUM IS THE WHOLE MESSAGE, not the values in it (#180 codex review).
// The first version of this counted `bytes` and nothing else, so a reply's
// NAMES were unbudgeted: a zip member name is a 16-bit length, 5,000 members of
// 60,000 characters each carry 300 MB of name alone, and a file whose values
// added up to 238 MiB - comfortably inside a 256 MiB ceiling - produced 524 MiB
// on the wire and came back to the person as "oversized reply from the peer".
// A ceiling that does not count everything the message carries is not a
// ceiling; it is an estimate with a failure mode.
//
// So a caller states, once, what a member costs BESIDES its three
// variable-length pieces (`perFact`) and pre-charges the message's own
// top-level fields into `used`. The three variable pieces - name, err, bytes -
// are counted here, where they are decided. This file still does not know the
// wire format; it knows that a wire has fixed overheads and that only the side
// filling the message can say what they are.
//
// `max` 0 means NO aggregate ceiling: that is the local door, which already
// holds the whole zip and is not copying anything onto a wire. A caller that
// sets one must refuse the WHOLE answer when `over` comes back non-empty - a
// scan that returned the members it managed to fit would be a listing that
// silently lost rows, and a picker missing a row is worse than a refusal that
// names the file.
struct InlineBudget {
    uint64_t max = 0;      // the ceiling, in bytes of the whole reply; 0 = none
    uint64_t used = 0;     // the reply so far, framing included. A caller sets
                           // this to what the message costs before its first
                           // member - the fields ahead of the member array.
    uint64_t perFact = 0;  // what one member costs beyond name + err + bytes
    std::string over;      // the member that crossed it; "" = it was not crossed
    uint64_t need = 0;     // ...and what THAT member wanted on top of `used`
};

// `__viewer` and nothing else decides which of the two readings an .npz gets
// (docs/input-adapters.md §4.11.1). Asked from the NAMES alone, so the peer can
// answer it before it inflates anything.
inline bool hasContainerMark(const std::vector<Entry>& entries) {
    for (const Entry& e : entries) if (e.name == "__viewer.npy") return true;
    return false;
}

// Is this member one whose VALUES the far side will need? Pixels never are -
// that is the whole point of the link - and everything else is small by
// construction (docs/remote-reader-design.md §10.2).
inline bool wantsValues(const std::string& name, const Head& H, bool container) {
    if (container) {
        // A container's reserved members ARE its declaration: layers, parents,
        // names, notes, cfa, ranges, timestamps, conditions, refs. All small,
        // all needed to build the tree, none of them pixels.
        return name.compare(0, 9, "__pixels_") != 0;
    }
    if (H.esize == 0) return true;              // strings, object arrays
    if (H.shape.empty()) return true;           // a 0-D scalar is a number
    if (H.shape.size() == 1) {
        uint64_t n = H.shape[0] < 0 ? 0 : (uint64_t)H.shape[0];
        return n <= INLINE_MAX_ELEMS;           // an axis candidate
    }
    return false;                               // 2-D and up: pixels, or nothing
}

// Every .npy member of a zip, as facts. ONE implementation, run by the local
// door on the file in front of it and by the peer on the file the link named.
inline std::vector<Fact> readFacts(const std::vector<uint8_t>& zip,
                                   const std::vector<Entry>& entries,
                                   InlineBudget* budget = nullptr) {
    const bool container = hasContainerMark(entries);
    std::vector<Fact> out;
    // RESERVE asks; COMMIT spends. They are two operations because the guard has
    // to happen before an inflate that may be refused (a ceiling that refuses
    // after paying for the decompression is the outage it exists to prevent),
    // while what is finally SPENT is the size of the buffer that actually ended
    // up on the fact - which can only be smaller. The comparison is a
    // subtraction: `used` never exceeds `max`, so `max - used` cannot wrap.
    // commit then forms `used + take` only after reserve proved that sum is at
    // most `max`. `over` non-empty means the caller must refuse the whole
    // answer.
    auto reserve = [&](uint64_t take, const std::string& who) {
        if (!budget || !budget->max) return true;
        if (take > budget->max - budget->used) {
            budget->over = who;
            budget->need = take;
            return false;
        }
        return true;
    };
    auto commit = [&](uint64_t take) { if (budget && budget->max) budget->used += take; };
    // A caller may pre-charge the message's own top-level fields into `used`.
    // If those alone do not fit there is nothing to list and saying so here
    // keeps the one refusal path.
    if (budget && budget->max && budget->used > budget->max) {
        budget->over = "(the reply's own fields)";
        budget->need = budget->used;
        return {};
    }
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".npy") != 0) continue;
        Fact f;
        f.entry = i;
        f.name = e.name.substr(0, e.name.size() - 4);
        f.usize = e.usize;
        // THE NAME AND THE FRAMING FIRST, before this member is even looked at.
        // They are known from the zip directory alone, they are on the wire
        // whatever else this member turns out to be, and 5,000 long names were
        // the reported way to pass a budget and blow the reply.
        {
            const uint64_t fixed =
                (budget ? budget->perFact : 0) + (uint64_t)f.name.size();
            if (!reserve(fixed, f.name)) return {};
            commit(fixed);
        }
        // Read the fixed prefix first, derive the header's declared end from
        // it, validate that length, then read exactly that prefix.  There is no
        // full extract in readFacts: a long or malformed v2 header must not
        // turn a header peek into an allocation of the member's entire declared
        // uncompressed size.
        std::vector<uint8_t> buf;
        std::string err;
        Head H;
        uint64_t headerEnd = 0;
        bool got = extractPrefix(zip, e, 12, buf, err) &&
                   npyHeaderEnd(buf, headerEnd, err);
        if (got && headerEnd > e.usize) {
            got = false;
            err = "corrupt npy header";
        }
        if (got && !fitsAlloc(headerEnd)) {
            got = false;
            err = "corrupt npy header: it is longer than this machine can hold";
        }
        // A successful header is carried on the wire, so its exact declared
        // size is also the allocation guard for the second prefix read.
        if (got && !reserve(headerEnd, f.name)) return {};
        if (got)
            got = extractPrefix(zip, e, (size_t)headerEnd, buf, err) &&
                  peekHeader(buf, H, err);
        if (!got) {
            f.err = err;
            // The error TRAVELS, so it is counted. It is one of this file's own
            // sentences and it is short, but "short" is not a bound and the
            // reply's size may not depend on a quantity nobody checked.
            if (!reserve((uint64_t)f.err.size(), f.name)) return {};
            commit((uint64_t)f.err.size());
            out.push_back(std::move(f));
            continue;
        }
        uint64_t payload = 0;
        bool sane = true;
        {
            uint64_t n = 1;
            for (int64_t d : H.shape) {
                if (d < 0) { sane = false; break; }
                if (d != 0 && n > UINT64_MAX / (uint64_t)d) { sane = false; break; }
                n *= (uint64_t)d;
            }
            if (sane && H.esize > 0) {
                if (n > (UINT64_MAX - H.dataOff) / (uint64_t)H.esize) sane = false;
                else payload = n * (uint64_t)H.esize;
            }
        }
        // How many bytes "the whole thing" IS. For a dtype this viewer sizes
        // that is the header plus the array; for one it does not - a string or
        // an object array, which is metadata and is READ rather than counted -
        // there is no element size to multiply by, so the member's own declared
        // length is the answer. Getting that second case wrong is not a
        // rounding error: it makes every `note` in every .npz come back blank.
        const uint64_t need = H.esize > 0 ? H.dataOff + payload : e.usize;
        const bool want = sane && wantsValues(f.name, H, container) &&
                          need <= INLINE_MAX_BYTES && need <= e.usize;
        // WHAT THIS MEMBER'S BYTES WILL COST AT MOST, decided from the header
        // and the zip directory - so it is known BEFORE the member is inflated
        // and long before anything is copied into a reply. A member that does
        // not fit is never decompressed: refusing after paying for the
        // decompression would be the outage the ceiling exists to prevent,
        // wearing a message. RESERVED, not spent: what is spent is the buffer
        // that actually ends up on the fact, a few lines down.
        if (!reserve(want ? need
                          : (buf.size() < H.dataOff ? (uint64_t)buf.size()
                                                    : (uint64_t)H.dataOff),
                     f.name))
            return {};
        if (want) {
            // Read only the bytes the parsed array needs.  Directory padding
            // after the value is neither sent nor allocated.
            if (buf.size() < need &&
                !extractPrefix(zip, e, (size_t)need, buf, err)) {
                f.err = err;
                if (!reserve((uint64_t)f.err.size(), f.name)) return {};
                commit((uint64_t)f.err.size());
                out.push_back(std::move(f));
                continue;
            }
            f.whole = buf.size() >= need;
            // ...and NO FURTHER. A member is its header and its array; bytes
            // past the array are not its value, and `need` is the figure the
            // budget above was told this member would cost. Without this a
            // member whose directory entry declares far more than its shape
            // does - the file says 1 GB, the header says 12 floats - would put
            // that gigabyte on the wire under a ceiling that had been shown a
            // much smaller number.
            if (f.whole && buf.size() > need) buf.resize((size_t)need);
        }
        // Trim to the header when the values are not wanted: what crosses the
        // link for a 4 GB image member is a few hundred bytes.
        if (!f.whole && buf.size() > H.dataOff) buf.resize(H.dataOff);
        // ...and THIS is the number that goes on the wire, so this is the one
        // that is spent. It is never larger than what was reserved above.
        commit((uint64_t)buf.size());
        f.bytes = std::move(buf);
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace nz
