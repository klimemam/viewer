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

inline bool peekHeader(const std::vector<uint8_t>& buf, Head& H, std::string& err) {
    auto fail = [&](const char* m) { err = m; return false; };
    if (buf.size() < 10 || buf[0] != 0x93 || memcmp(&buf[1], "NUMPY", 5) != 0)
        return fail("not a .npy file (bad magic)");
    int major = buf[6];
    size_t hlen, hoff;
    if (major >= 2) {
        if (buf.size() < 12) return fail("corrupt npy header");
        uint32_t v; memcpy(&v, &buf[8], 4); hlen = v; hoff = 12;
    } else {
        uint16_t v; memcpy(&v, &buf[8], 2); hlen = v; hoff = 10;
    }
    if (hoff + hlen > buf.size()) return fail("corrupt npy header");
    // data() + hoff, never &buf[hoff]: a 10-byte v1 file declaring hlen 0 passes
    // the check above with hoff == buf.size(), and the subscript is then one
    // past the end - undefined, and an abort under -D_GLIBCXX_ASSERTIONS. The
    // empty header is refused by "cannot parse descr" either way.
    std::string hdr((const char*)buf.data() + hoff, hlen);
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
    H.dataOff = hoff + hlen;
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
    if (buf.size() < 22) { err = "not a zip file"; return false; }
    size_t eocd = SIZE_MAX;
    size_t start = buf.size() > 65557 ? buf.size() - 65557 : 0;
    for (size_t i = buf.size() - 22 + 1; i-- > start;)
        if (rd32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == SIZE_MAX) { err = "not a zip file (no end record)"; return false; }
    uint64_t count = rd16(eocd + 10);
    uint64_t cdOff = rd32(eocd + 16);
    if (cdOff == 0xffffffffu || count == 0xffffu) {          // zip64
        if (eocd >= 20 && rd32(eocd - 20) == 0x07064b50) {
            uint64_t z64 = rd64(eocd - 20 + 8);
            if (z64 + 56 <= buf.size() && rd32((size_t)z64) == 0x06064b50) {
                count = rd64((size_t)z64 + 32);
                cdOff = rd64((size_t)z64 + 48);
            }
        }
    }
    size_t p = (size_t)cdOff;
    for (uint64_t i = 0; i < count && p + 46 <= buf.size(); i++) {
        if (rd32(p) != 0x02014b50) break;
        Entry e{};
        e.method = rd16(p + 10);
        e.csize = rd32(p + 20);
        e.usize = rd32(p + 24);
        uint16_t nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
        e.localOff = rd32(p + 42);
        if (p + 46 + nlen > buf.size()) break;
        // data() + off again: a central-directory header that ends exactly at
        // the last byte with a zero-length name satisfies both bounds above and
        // then subscripts one past the end.
        e.name.assign((const char*)buf.data() + p + 46, nlen);
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
            const size_t ep0 = p + 46 + nlen;                 // <= buf.size(), above
            if ((size_t)elen > buf.size() - ep0) {
                err = zip64Bad(e.name, "its extra field runs past the end of the file");
                return false;
            }
            const size_t eend = ep0 + elen;
            size_t ep = ep0;
            bool got = false;
            while (ep + 4 <= eend) {
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
        out.push_back(std::move(e));
        p += 46 + nlen + elen + clen;
    }
    if (out.empty()) { err = "zip contains no entries"; return false; }
    return true;
}

// Where a member's bytes begin in the zip, or 0 when the local header does not
// hold together. Its own function because the peer wants the same arithmetic
// without the extraction (it copies the compressed run out to a cache file).
inline bool dataStart(const std::vector<uint8_t>& zip, const Entry& e, size_t& at,
                      std::string& err) {
    if (e.localOff + 30 > zip.size()) { err = "corrupt local header"; return false; }
    auto rd16 = [&](size_t o) { return (uint16_t)(zip[o] | zip[o + 1] << 8); };
    size_t nlen = rd16((size_t)e.localOff + 26), elen = rd16((size_t)e.localOff + 28);
    at = (size_t)e.localOff + 30 + nlen + elen;
    if (at + e.csize > zip.size()) { err = "truncated zip member"; return false; }
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
    size_t cap = std::min<uint64_t>(want, e.usize);
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
// above and the file still be unlistable: 65 one-million-element f8 axes are 65
// members of 8 MiB each - each of them legal, each of them wanted - and 520 MiB
// of reply, which core/remote.cpp refuses at 512 MiB as "oversized reply from
// the peer". So the sum has an owner too, and it is the side that fills the
// message.
//
// `max` 0 means NO aggregate ceiling: that is the local door, which already
// holds the whole zip and is not copying anything onto a wire. A caller that
// sets one must refuse the WHOLE answer when `over` comes back non-empty - a
// scan that returned the members it managed to fit would be a listing that
// silently lost rows, and a picker missing a row is worse than a refusal that
// names the file.
struct InlineBudget {
    uint64_t max = 0;      // the ceiling, in bytes of member payload; 0 = none
    uint64_t used = 0;     // what the members already listed add up to
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
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".npy") != 0) continue;
        Fact f;
        f.entry = i;
        f.name = e.name.substr(0, e.name.size() - 4);
        f.usize = e.usize;
        // Header only: an image member is never inflated here, however big it
        // is. 64 KiB covers every .npy header (v1 caps the length at 65535 and
        // the dict is padded to a 64-byte multiple); a v2 header longer than
        // that falls back to a full read rather than being called corrupt.
        std::vector<uint8_t> buf;
        std::string err;
        Head H;
        bool got = extractPrefix(zip, e, 64 * 1024, buf, err) && peekHeader(buf, H, err);
        if (!got && buf.size() < e.usize)
            got = extract(zip, e, buf, err) && peekHeader(buf, H, err);
        if (!got) {
            f.err = err;
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
        // WHAT THIS MEMBER WILL COST, decided from the header and the zip
        // directory - so it is known BEFORE the member is inflated and long
        // before anything is copied into a reply. A member that does not fit
        // is never decompressed: refusing after paying for the decompression
        // would be the outage the ceiling exists to prevent, wearing a message.
        if (budget && budget->max) {
            const uint64_t take = want ? need
                                       : (buf.size() < H.dataOff ? (uint64_t)buf.size()
                                                                 : H.dataOff);
            // Subtraction and not addition: `used` never exceeds `max` (this is
            // the only place it grows, and it grows only when it fit), so
            // `max - used` cannot wrap and `used + take` can never be formed.
            if (take > budget->max - budget->used) {
                budget->over = f.name;
                budget->need = take;
                return {};
            }
            budget->used += take;
        }
        if (want) {
            if (buf.size() < need && !extract(zip, e, buf, err)) {
                f.err = err;
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
        f.bytes = std::move(buf);
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace nz
