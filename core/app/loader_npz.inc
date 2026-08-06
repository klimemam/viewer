// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ローダ
// ---------------------------------------------------------------- .npz (zip)
// Minimal zip reader for npz: central-directory walk, stored (0) and deflate (8),
// with zip64 sizes. Inflate comes from miniz.
// npyRead: the recorded reading declaration (FrameSource::npyRead, §3.3) -
// reload must reproduce the interpretation the pixels were born under, which
// is why it is threaded through rather than read from any global.
static std::unique_ptr<ImageDoc> decodeNpyBuffer(const std::vector<uint8_t>& buf,
                                                 const std::string& path,
                                                 const std::string& displayName,
                                                 std::string& errOut, int frameIdx,
                                                 int& framesOut, int64_t& frameStrideOut,
                                                 int npyRead = 0 /*NR_NATIVE*/);
struct NpzEntry { std::string name; size_t localOff, csize, usize; uint16_t method; };

// ---- the .npy header, parsed ONCE -------------------------------------------
// shape is the shape AS WRITTEN: a scalar is 0-D and stays 0-D here, because
// "() versus (1,) versus (1,1)" is the whole question the .npz classifier asks.
// esize 0 = a dtype this viewer cannot read as pixels (strings, object arrays);
// that is a fact about the member, not an error, so the peek still succeeds and
// the caller decides. The decoder and the classifier share this function so
// what the picker promises and what actually opens cannot drift apart.
struct NpyHead {
    std::vector<int64_t> shape;
    std::string descr;                // raw, e.g. "<u2" / "<U8" / "|O"
    std::string code;                 // descr without the byte-order character
    std::string dtypeName;            // "u16" ... empty when esize == 0
    int esize = 0;
    bool fortran = false, be = false;
    size_t dataOff = 0;
};
static bool npyPeekHeader(const std::vector<uint8_t>& buf, NpyHead& H, std::string& err) {
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
    std::string hdr((char*)&buf[hoff], hlen);
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
            if (tok.find_first_of("0123456789") != std::string::npos)
                H.shape.push_back(std::stoll(tok));
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

// "(24, 480, 640)" / "scalar" - the shape a human recognises from the script
// that wrote it, printed from the header and never from what we made of it.
static std::string npyShapeText(const std::vector<int64_t>& shape) {
    if (shape.empty()) return "scalar";
    std::string s = "(";
    for (size_t i = 0; i < shape.size(); i++) {
        s += std::to_string(shape[i]);
        if (i + 1 < shape.size()) s += ", ";
        else if (shape.size() == 1) s += ",";
    }
    return s + ")";
}

// One .npy FILE on disk from a shape, a dtype and its bytes. v1.0 header, C
// order, padded to the 64-byte boundary numpy itself pads to.
//
// Shared rather than written twice: two selftests need fixtures of an exactly
// named shape, and both are testing the rule that reads the shape back - so a
// second copy of the header spelling would be a second thing that can be wrong
// in the direction that hides the fault. Returns the path it was handed.
static std::string npyWriteFile(const std::string& path, const char* descr,
                                const std::vector<int64_t>& shape,
                                const void* data, size_t nbytes) {
    std::string sh;
    if (shape.empty()) sh = "()";
    else if (shape.size() == 1) sh = "(" + std::to_string(shape[0]) + ",)";
    else {
        sh = "(";
        for (size_t i = 0; i < shape.size(); i++)
            sh += std::to_string(shape[i]) + (i + 1 < shape.size() ? ", " : "");
        sh += ")";
    }
    std::string hdr = std::string("{'descr': '") + descr +
                      "', 'fortran_order': False, 'shape': " + sh + ", }";
    hdr.append((64 - (10 + hdr.size() + 1) % 64) % 64, ' ');
    hdr += '\n';
    std::vector<uint8_t> out;
    const char magic[8] = { '\x93', 'N', 'U', 'M', 'P', 'Y', '\x01', '\x00' };
    out.insert(out.end(), magic, magic + 8);
    out.push_back((uint8_t)(hdr.size() & 0xff));
    out.push_back((uint8_t)(hdr.size() >> 8));
    out.insert(out.end(), hdr.begin(), hdr.end());
    const uint8_t* p = (const uint8_t*)data;
    out.insert(out.end(), p, p + nbytes);
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    f.write((const char*)out.data(), (std::streamsize)out.size());
    return path;
}

// ---- which axis is what -----------------------------------------------------
// docs/input-adapters.md §3: native reads FOUR shapes and refuses the rest by
// name. The decision is RANK and the LAST AXIS, and never the first axis's
// size. "A leading axis of 4 or less must be channels" is the guess that read
// (3,H,W) - three frames, an ordinary thing to save - as one 3-channel picture,
// and it could not be right in both of the two cases that actually occur. On
// the last axis the collision does not arise: an image three pixels wide is not
// a thing. THE EXCEPTION IS THE ONE WRITTEN DOWN, and there is only one.
//
// What a user cannot get from the shape, they SAY (§3.3, NpyRead below). That
// is a declaration, per file and visible on screen - which is what --npy-axis,
// one global flag for a whole run, could never be.

// A reading of an array. Never inferred beyond NR_NATIVE; the others exist only
// because someone chose them in the Inspector. VALUES ARE WRITTEN TO SESSIONS:
// append, never renumber.
enum NpyRead {
    NR_NATIVE = 0,   // no choice recorded: §3.1 decides from rank + last axis
    NR_STACK  = 1,   // leading axis is frames: (F,H,W) / (F,H,W,C)
    NR_HWC    = 2,   // 3-D as ONE frame, channels last:  (H,W,C), C = 1..4
    NR_CHW    = 3,   // 3-D as ONE frame, planes first:   (C,H,W), C = 1..4
};

// The four forms, spelled the way §3.2 spells them to a user.
//
// "C<=4", not "3|4": the rule is a CEILING (§3.1's table, and issue #71), and a
// refusal that still quotes 3|4 once (H,W,1) opens sends the next reader looking
// for a rule nobody implements. Spelled in ASCII on purpose - §4.13.0 draws it
// with U+2264, but this string is printed to a console whose codepage may be
// cp932 (which cannot encode that character at all) and is quoted back verbatim
// by tools/import/run_adapter.py through a pipe.
static const char* NPY_NATIVE_FORMS = "(H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)";

// Which axis is F/H/W/C under reading r, as indices into a shape of this rank.
// -1 = "this reading has no such axis". False = r does not apply to this rank,
// which is how impossible readings are kept off the menu (§3.3).
static bool npyReadAxes(size_t rank, int r, int& iF, int& iH, int& iW, int& iC) {
    iF = iH = iW = iC = -1;
    switch (r) {
    case NR_STACK:
        if (rank == 3) { iF = 0; iH = 1; iW = 2;           return true; }
        if (rank == 4) { iF = 0; iH = 1; iW = 2; iC = 3;   return true; }
        return false;
    case NR_HWC: if (rank == 3) { iH = 0; iW = 1; iC = 2; return true; } return false;
    case NR_CHW: if (rank == 3) { iC = 0; iH = 1; iW = 2; return true; } return false;
    }
    return false;
}

// §3.1, entire: rank, then the last axis, then stop. NR_NATIVE for a rank that
// has no reading at all (1-D, 5-D and up) - the caller refuses those by name.
//
// The last axis is CHANNELS when it is 4 OR FEWER - not "3 or 4" (issue #71).
// This line used to test ==3||==4 while core/serve.cpp tested <=4, so the same
// file read one way through File > Open and another way through a peer: a mono
// (480,640,1) capture - which is just what img[:, :, None] and np.expand_dims
// produce - opened locally as 480 frames of 1x640, and remotely as one 640x480
// picture. The stack reading is the dangerous one, because inventing a frame
// axis invents a TIME axis: Temporal reported sigma_t = 36.2 DN for a frame
// whose real read noise was 5.0 DN, and the number came from the scene's
// vertical structure rather than the sensor. docs/terminology.md is the canon
// that settles it ("a frame has no time axis", and "if the implementation
// disagrees with this table it is a bug"), and every downstream ceiling in this
// codebase is already 1..4 rather than {3,4}. What this costs is a stack of
// frames 1 to 4 pixels wide, which §3.1 wrote off as not a real thing, and
// which §3.3 can say again per file anyway.
static int npyNativeRead(const std::vector<int64_t>& shape) {
    if (shape.size() == 3) return shape[2] <= 4 ? NR_HWC : NR_STACK;
    if (shape.size() == 4) return NR_STACK;
    return NR_NATIVE;
}

// Registry identity (§6.2, declared above srcIdentityKey): the reading that
// actually decoded these pixels, with every spelling of "what native does"
// folded onto NR_NATIVE. A declaration that does not fit the shape decoded
// natively (npyLayout's fallback), so it keys natively too.
// External linkage since P6: state.h declares it and srcIdentityKey — inline
// there, callable from any TU — calls it, so a per-TU `static` no longer works.
// Still the one definition, in the main TU.
int npyKeyRead(const std::vector<int64_t>& shape, int npyRead) {
    if (npyRead == NR_NATIVE) return NR_NATIVE;
    int iF, iH, iW, iC;
    if (!npyReadAxes(shape.size(), npyRead, iF, iH, iW, iC)) return NR_NATIVE;
    return npyRead == npyNativeRead(shape) ? NR_NATIVE : npyRead;
}

// §3.2: name the shape that arrived, and name what native DOES read. A refusal
// that says only "cannot open" sends the reader nowhere, which is how a 1-D
// exposure vector stayed a one-pixel-tall image for as long as it did.
//
// §4.13, now attached: the third line of §3.2 is "choose a reader...", and THIS
// is the single place it hangs from - the same message that said no. The
// destination exists (openReaderPicker), and openPath raises it with this very
// text as the reason, so the refusal is the affordance rather than a dead end
// the user has to leave in order to act on.
static std::string npyNotNative(const std::vector<int64_t>& shape) {
    // npyShapeText says "scalar" for a 0-D array, and "shape scalar" is not a
    // sentence; everything else is a shape and is named as one.
    return (shape.empty() ? std::string("a scalar")
                          : "shape " + npyShapeText(shape)) +
           " is not a native form\n  native reads " + std::string(NPY_NATIVE_FORMS) +
           "\n  choose a reader to read it another way";
}

// The layout decision, lifted out of the decoder so the .npz picker can promise
// exactly what will happen. On success F/H/W/C and their strides describe the
// frame to read. npyRead is the user's declaration or NR_NATIVE.
static bool npyLayout(const std::vector<int64_t>& shape, const std::vector<int64_t>& strides,
                      int npyRead, int64_t& F, int64_t& sf,
                      int64_t& H, int64_t& W, int64_t& C,
                      int64_t& sh, int64_t& sw, int64_t& sc,
                      std::string& note, std::string& err) {
    F = 1; sf = 0; H = W = C = 1; sh = sw = sc = 0;
    const size_t rank = shape.size();
    auto addNote = [&](const std::string& s) { note = note.empty() ? s : note + ", " + s; };
    if (rank == 2) {                    // (H,W): one frame, one channel, no choice
        H = shape[0]; sh = strides[0];
        W = shape[1]; sw = strides[1];
    } else if (rank == 3 || rank == 4) {
        int iF, iH, iW, iC;
        int r = npyRead;
        // A remembered choice that no longer fits (the file was rewritten with a
        // different rank) falls back to native and SAYS it did - restoring a
        // session must not quietly apply a reading to an array it cannot mean.
        if (r != NR_NATIVE && !npyReadAxes(rank, r, iF, iH, iW, iC)) {
            addNote("re-read choice does not fit " + npyShapeText(shape) + "; read natively");
            r = NR_NATIVE;
        }
        if (r == NR_NATIVE) r = npyNativeRead(shape);
        npyReadAxes(rank, r, iF, iH, iW, iC);
        if (iF >= 0) { F = shape[iF]; sf = strides[iF]; }
        H = shape[iH]; sh = strides[iH];
        W = shape[iW]; sw = strides[iW];
        if (iC >= 0) { C = shape[iC]; sc = strides[iC]; }
        // The transposed reading is never native (§3.4 deleted the implicit
        // CHW->HWC), so when it happens it happened because someone asked.
        if (r == NR_CHW) addNote("planes first (C,H,W), as asked");
    } else {                            // 0-D, 1-D, 5-D and up: not a picture
        err = npyNotNative(shape);
        return false;
    }
    // EVERY axis has to be a real extent. W and H were bounded and F and C were
    // not, so (8,8,0) built an ImageDoc with ch=0 and (0,8,8,3) read H*W*C
    // values out of a payload holding none. A zero-length axis is not an image.
    if (F < 1 || H < 1 || W < 1 || C < 1) {
        err = "a zero-length axis in " + npyShapeText(shape) + ": no pixels in it";
        return false;
    }
    if (W > 32768 || H > 32768) { err = "unsupported image size"; return false; }
    // (F,H,W,C) is a native FORM; C > 4 is this viewer's own ceiling, so it is
    // said as a ceiling and not as "not a native form" - a different sentence
    // because it is a different refusal.
    if (C > 4) {
        err = npyShapeText(shape) + " would be " + std::to_string(C) +
              " channels: this viewer shows up to 4";
        return false;
    }
    if (F > 1) addNote("frame axis");
    return true;
}

// "3 frames x 1 ch  (F,H,W)" - what a reading MAKES of this shape, in the
// wording §3.3 uses. Computed, so the menu can never offer a reading that this
// array cannot support.
static std::string npyReadLabel(const std::vector<int64_t>& shape, int r) {
    std::vector<int64_t> st(shape.size(), 1);       // strides are irrelevant here
    int64_t F, sf, H, W, C, sh, sw, sc;
    std::string note, err;
    if (!npyLayout(shape, st, r, F, sf, H, W, C, sh, sw, sc, note, err)) return {};
    const char* form = r == NR_HWC ? "(H,W,C)" : r == NR_CHW ? "(C,H,W)"
                     : shape.size() == 4 ? "(F,H,W,C)" : shape.size() == 3 ? "(F,H,W)" : "(H,W)";
    return std::to_string((int)F) + (F == 1 ? " frame x " : " frames x ") +
           std::to_string((int)C) + " ch  " + form;
}

// Every reading this shape permits, native first. One entry means there is
// nothing to choose and the Inspector shows no control (§3.3: impossible
// readings are not offered, and neither is a menu of one).
static std::vector<int> npyReadOptions(const std::vector<int64_t>& shape) {
    std::vector<int> out;
    int nat = npyNativeRead(shape);
    if (nat == NR_NATIVE) return out;               // a refused shape reads no way
    for (int r : { nat, (int)NR_STACK, (int)NR_HWC, (int)NR_CHW }) {
        bool dup = false;
        for (int o : out) if (o == r) dup = true;
        if (dup || npyReadLabel(shape, r).empty()) continue;
        out.push_back(r);
    }
    return out;
}

static bool npzList(const std::vector<uint8_t>& buf, std::vector<NpzEntry>& out, std::string& err) {
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
        NpzEntry e{};
        e.method = rd16(p + 10);
        e.csize = rd32(p + 20);
        e.usize = rd32(p + 24);
        uint16_t nlen = rd16(p + 28), elen = rd16(p + 30), clen = rd16(p + 32);
        e.localOff = rd32(p + 42);
        if (p + 46 + nlen > buf.size()) break;
        e.name.assign((const char*)&buf[p + 46], nlen);
        if (e.csize == 0xffffffffu || e.usize == 0xffffffffu || e.localOff == 0xffffffffu) {
            size_t ep = p + 46 + nlen, eend = ep + elen;      // zip64 extra field
            while (ep + 4 <= eend && ep + 4 <= buf.size()) {
                uint16_t id = rd16(ep), sz = rd16(ep + 2);
                if (id == 1) {
                    size_t q = ep + 4;
                    if (e.usize == 0xffffffffu) { e.usize = (size_t)rd64(q); q += 8; }
                    if (e.csize == 0xffffffffu) { e.csize = (size_t)rd64(q); q += 8; }
                    if (e.localOff == 0xffffffffu) { e.localOff = (size_t)rd64(q); }
                    break;
                }
                ep += 4 + sz;
            }
        }
        out.push_back(std::move(e));
        p += 46 + nlen + elen + clen;
    }
    if (out.empty()) { err = "zip contains no entries"; return false; }
    return true;
}

static bool npzExtract(const std::vector<uint8_t>& zip, const NpzEntry& e,
                       std::vector<uint8_t>& out, std::string& err) {
    if (e.localOff + 30 > zip.size()) { err = "corrupt local header"; return false; }
    auto rd16 = [&](size_t o) { return (uint16_t)(zip[o] | zip[o + 1] << 8); };
    size_t nlen = rd16(e.localOff + 26), elen = rd16(e.localOff + 28);
    size_t data = e.localOff + 30 + nlen + elen;
    if (data + e.csize > zip.size()) { err = "truncated zip member"; return false; }
    if (e.method == 0) {                                     // stored
        out.assign(zip.begin() + data, zip.begin() + data + e.csize);
        return true;
    }
    if (e.method != 8) { err = "unsupported zip compression method"; return false; }
    // zip members are RAW deflate (no zlib header), and the uncompressed size is
    // known from the directory, so decompress straight into the output buffer
    out.resize(e.usize);
    size_t got = tinfl_decompress_mem_to_mem(out.data(), out.size(),
                                             zip.data() + data, e.csize, 0);
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
static bool npzExtractPrefix(const std::vector<uint8_t>& zip, const NpzEntry& e,
                             size_t want, std::vector<uint8_t>& out, std::string& err) {
    if (e.localOff + 30 > zip.size()) { err = "corrupt local header"; return false; }
    auto rd16 = [&](size_t o) { return (uint16_t)(zip[o] | zip[o + 1] << 8); };
    size_t nlen = rd16(e.localOff + 26), elen = rd16(e.localOff + 28);
    size_t data = e.localOff + 30 + nlen + elen;
    if (data + e.csize > zip.size()) { err = "truncated zip member"; return false; }
    size_t cap = std::min(want, e.usize);
    if (e.method == 0) {                                     // stored
        size_t nCopy = std::min(cap, e.csize);
        out.assign(zip.begin() + data, zip.begin() + data + nCopy);
        return true;
    }
    if (e.method != 8) { err = "unsupported zip compression method"; return false; }
    out.assign(cap, 0);
    if (cap == 0) return true;
    auto inf = std::make_unique<tinfl_decompressor>();   // ~11 KB: not a stack object
    tinfl_init(inf.get());
    size_t inBytes = e.csize, outBytes = cap;
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

// Load a .npy; an array with a frame axis becomes one stack (塊), which is what
// the temporal analysis operates on.
static std::unique_ptr<ImageDoc> decodeNpyFrame(const std::string& path, std::string& errOut,
                                                int frameIdx, int& framesOut,
                                                int64_t& frameStrideOut,
                                                int npyRead = 0 /*NR_NATIVE*/);   // defined below

// Shared by .npy files and .npz members: build a stack when the array has a
// frame axis, otherwise a single image. npyRead is the user's declaration
// (§3.3) and applies to EVERY frame of the stack, not just the one that opened.
static std::string loadNpyBuffer(const std::vector<uint8_t>& buf, const std::string& path,
                                 const std::string& displayName, int npyRead = 0 /*NR_NATIVE*/) {
    std::string err;
    int frames = 1;
    int64_t fstride = 0;
    auto first = decodeNpyBuffer(buf, path, displayName, err, 0, frames, fstride, npyRead);
    if (!first) return err.empty() ? "decode failed" : err;
    std::string label = first->name;
    if (frames <= 1) { addImage(std::move(first)); return {}; }

    App::SeqInfo info;
    info.id = app.nextSeqId++;
    info.name = label + "  (" + std::to_string(frames) + " frames)";
    app.seqs.push_back(info);
    first->seqId = info.id;
    first->seqIndex = 0;
    int firstIdx = (int)app.images.size();
    addImage(std::move(first));
    const ImageDoc* ref = app.images[firstIdx].get();
    for (int f = 1; f < frames; f++) {
        std::string e2;
        int fr = 1; int64_t fs = 0;
        auto doc = decodeNpyBuffer(buf, path, displayName, e2, f, fr, fs, npyRead);
        if (!doc) { toast(label + ": frame " + std::to_string(f) + ": " + e2, true); break; }
        doc->seqId = info.id;
        doc->seqIndex = f;
        computeMinMax(*doc);
        doc->black = ref->black; doc->white = ref->white;   // frames stay comparable
        // ...and the batch, which addImage assigns to the HEAD frame only: these
        // go straight into app.images, so a stack built inside one file used to
        // have frame 0 in a batch and frames 1..N-1 in none (the folder path
        // already does this, in pumpSequence). A stack is in ONE batch.
        doc->batchId = ref->batchId;
        doc->texDirty = true;
        doc->uid = app.nextUid++;
        app.imagesRev++;
        app.images.push_back(std::move(doc));
    }
    for (auto& s : app.seqs)
        if (s.id == info.id) s.lastImageIdx = firstIdx;
    int got = 0;
    for (const auto& d : app.images) if (d->seqId == info.id) got++;
    // n of N, both numbers, when the frame loop broke early: the stack's NAME
    // still says "(N frames)" and the stack holds fewer, and a partially loaded
    // stack that does not say so is the silent lie docs/terminology.md forbids.
    if (got < frames) {
        for (auto& s : app.seqs)
            if (s.id == info.id)
                s.name = label + "  (" + std::to_string(got) + " of " +
                         std::to_string(frames) + " frames)";
        toast(label + ": " + std::to_string(got) + " of " + std::to_string(frames) +
              " frames loaded - the rest could not be decoded", true);
    }
    fprintf(stderr, "npy stack: %s - %d of %d frames (%dx%d %dch)\n", label.c_str(), got,
            frames, ref->w, ref->h, ref->ch);
    return {};
}

// ---- classifying an .npz's members (docs/npz-design.md §2.1) ----------------
// One element as a DOUBLE. An axis is the quantity data is plotted against, so
// it never passes through float on the way in: the pixel path's getVal returns
// float and that is right for pixels, but it is not right for 12345.678901234567.
static double npyElem(const std::vector<uint8_t>& buf, const NpyHead& H, size_t i) {
    const uint8_t* p = buf.data() + H.dataOff + i * (size_t)H.esize;
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

// A string member's value, so a "note" key can be SHOWN and not merely counted.
// numpy writes <U as UCS-4 and |S as bytes. Anything else is left empty, which
// reads as "not shown" - never as a guess at what the bytes meant.
static std::string npzTextValue(const std::vector<uint8_t>& buf, const NpyHead& H) {
    if (H.code.empty() || H.dataOff >= buf.size()) return {};
    // ONE element wide, not "to the end of the member". "<U8" and "|S8" carry
    // their item size in the descr, and reading past it walked into the next
    // element - so a (5,) array of strings printed element 0 with its neighbour
    // glued on, presented as if it were the whole array.
    size_t item = 0;
    for (size_t i = 1; i < H.code.size(); i++)
        if (H.code[i] >= '0' && H.code[i] <= '9') item = item * 10 + (size_t)(H.code[i] - '0');
    if (H.code[0] == 'U') item *= 4;                  // UCS-4
    size_t avail = buf.size() - H.dataOff;
    if (item && item < avail) avail = item;
    const uint8_t* p = buf.data() + H.dataOff;
    std::string s;
    if (H.code[0] == 'S') {
        for (size_t i = 0; i < avail && p[i]; i++) s += (char)p[i];
    } else if (H.code[0] == 'U') {
        for (size_t i = 0; i + 4 <= avail; i += 4) {
            uint32_t c; memcpy(&c, p + i, 4);
            if (H.be) c = ((c & 0xffu) << 24) | ((c & 0xff00u) << 8) |
                          ((c >> 8) & 0xff00u) | (c >> 24);
            if (!c) break;
            if (c < 0x80) s += (char)c;                       // UTF-8, so a
            else if (c < 0x800) { s += (char)(0xc0 | (c >> 6));       // non-ASCII
                                  s += (char)(0x80 | (c & 0x3f)); }   // note is
            else if (c < 0x10000) { s += (char)(0xe0 | (c >> 12));    // readable
                                    s += (char)(0x80 | ((c >> 6) & 0x3f));
                                    s += (char)(0x80 | (c & 0x3f)); }
            else { s += (char)(0xf0 | (c >> 18));
                   s += (char)(0x80 | ((c >> 12) & 0x3f));
                   s += (char)(0x80 | ((c >> 6) & 0x3f));
                   s += (char)(0x80 | (c & 0x3f)); }
        }
    }
    if (s.size() > 120) s = s.substr(0, 117) + "...";
    return s;
}

// Every member, classified BY SHAPE, before anything is opened. This is the
// whole fix: the decision "is this pixels?" is made from the header, once, and
// a member that is not pixels can no longer become pixels by falling through
// into the image path.
static std::vector<NpzMember> npzScan(const std::vector<uint8_t>& zip,
                                      const std::vector<NpzEntry>& entries) {
    std::vector<NpzMember> out;
    for (size_t i = 0; i < entries.size(); i++) {
        const NpzEntry& e = entries[i];
        if (e.name.size() < 4 || e.name.compare(e.name.size() - 4, 4, ".npy") != 0) continue;
        NpzMember m;
        m.entry = i;
        m.name = e.name.substr(0, e.name.size() - 4);
        m.shapeText = "?";
        m.dtype = "?";
        // Header only: an image member is never inflated here, however big it is.
        // 64 KiB covers every .npy header (v1 caps the length at 65535 and the
        // dict is padded to a 64-byte multiple); a v2 header longer than that
        // falls back to a full read rather than being called corrupt.
        std::vector<uint8_t> buf;
        std::string err;
        NpyHead H;
        bool got = npzExtractPrefix(zip, e, 64 * 1024, buf, err) &&
                   npyPeekHeader(buf, H, err);
        if (!got && buf.size() < e.usize)
            got = npzExtract(zip, e, buf, err) && npyPeekHeader(buf, H, err);
        if (!got) {
            m.role = NpzMember::RBad;
            m.becomes = "not opened";
            m.why = err;
            out.push_back(std::move(m));
            continue;
        }
        m.shapeText = npyShapeText(H.shape);
        m.dtype = H.dtypeName.empty() ? H.descr : H.dtypeName;
        // The element COUNT is a product of numbers a header can declare freely.
        // (2305843009213693952,) wraps a size_t product to 0, which then passed
        // the "fits in the buffer" guard and reached reserve() as an uncaught
        // length_error. Every dim is checked, the product is checked, and the
        // result is checked against the bytes the ZIP DIRECTORY says are there -
        // so a truncated member is refused in the scan and never promised.
        size_t n = 1;
        bool sane = true;
        for (int64_t d : H.shape) {
            if (d < 0) { sane = false; break; }
            if (d != 0 && n > (size_t)-1 / (size_t)d) { sane = false; break; }
            n *= (size_t)d;
        }
        size_t payload = 0;
        if (sane && H.esize > 0) {
            if (n > ((size_t)-1 - H.dataOff) / (size_t)H.esize) sane = false;
            else payload = n * (size_t)H.esize;
        }
        if (!sane || H.dataOff + payload > e.usize) {
            m.role = NpzMember::RBad;
            m.becomes = "not an image: truncated or impossible";
            m.why = "shape " + m.shapeText + " needs more bytes than the member holds";
            out.push_back(std::move(m));
            continue;
        }
        // values live at the far end of the member, so anything that needs them
        // (metadata and axis candidates only - never an image) reads it in full
        auto fullBuf = [&]() -> bool {
            if (buf.size() >= H.dataOff + payload) return true;
            std::string e2;
            return npzExtract(zip, e, buf, e2);
        };
        if (H.esize == 0) {                       // strings, object arrays, ...
            m.role = NpzMember::RMeta;
            m.becomes = "metadata";
            m.why = "dtype " + H.descr + " is not pixels";
            if (fullBuf()) {
                m.text = npzTextValue(buf, H);
                // an ARRAY of strings is not its first element: say which one
                // this is rather than passing element 0 off as the whole thing
                if (!m.text.empty() && n > 1)
                    m.text = "\"" + m.text + "\" (first of " + std::to_string(n) + ")";
                else if (!m.text.empty())
                    m.text = "\"" + m.text + "\"";
            }
            out.push_back(std::move(m));
            continue;
        }
        if (H.shape.empty()) {                    // a 0-D scalar is a number, not a picture
            m.role = NpzMember::RMeta;
            m.becomes = "metadata";
            m.why = "a scalar is not pixels";
            if (fullBuf() && H.dataOff + (size_t)H.esize <= buf.size())
                m.text = fmtExact(npyElem(buf, H, 0));
            out.push_back(std::move(m));
            continue;
        }
        if (H.shape.size() == 1) {                // THE defect: (N,) used to be H=1,W=N
            m.role = NpzMember::RAxis;
            m.becomes = "metadata";               // upgraded to "x axis" when a
            m.why = "a 1-D array is not pixels";  // stack of the same length opens
            // an axis has one value per frame; something with millions of them
            // is not one, and is not worth inflating to find out
            if (n > (1u << 24)) {
                m.role = NpzMember::RMeta;
                m.why = "a 1-D array of " + std::to_string(n) + " values is not a frame axis";
            } else if (fullBuf() && H.dataOff + payload <= buf.size()) {
                m.vals.reserve(n);
                for (size_t k = 0; k < n; k++) m.vals.push_back(npyElem(buf, H, k));
            }
            out.push_back(std::move(m));
            continue;
        }
        std::vector<int64_t> shape = H.shape, strides(shape.size());
        if (H.fortran) { int64_t s = 1; for (size_t k = 0; k < shape.size(); k++) { strides[k] = s; s *= shape[k]; } }
        else           { int64_t s = 1; for (int k = (int)shape.size() - 1; k >= 0; k--) { strides[k] = s; s *= shape[k]; } }
        std::string note, lerr;
        int64_t F, sf, Hh, Ww, Cc, sh, sw, sc;
        // NR_NATIVE: the picker predicts what an Open will do, and an Open of a
        // member has no re-read declaration behind it yet (§3.3 restates a doc
        // that is already open, from the Inspector).
        if (!npyLayout(shape, strides, NR_NATIVE, F, sf, Hh, Ww, Cc, sh, sw, sc, note, lerr)) {
            // Named in one line, not merely counted: "not opened" tells a reader
            // nothing, and this member is the one that used to become a picture
            // by having its leading axes quietly thrown away.
            m.role = H.shape.size() > 4 ? NpzMember::RAmbig : NpzMember::RBad;
            m.becomes = H.shape.size() > 4
                            ? "not a native form: native reads " + std::string(NPY_NATIVE_FORMS)
                            : "not an image: " + lerr;
            m.why = lerr;
            out.push_back(std::move(m));
            continue;
        }
        m.role = NpzMember::RImage;
        m.frames = (int)F;
        m.w = (int)Ww; m.h = (int)Hh; m.ch = (int)Cc;
        m.selected = true;                       // pixels are what an Open is for
        m.becomes = F > 1 ? "stack of " + std::to_string((int)F) + " frames" : "frame";
        out.push_back(std::move(m));
    }
    return out;
}

// Open ONE image-shaped member: the stack (or the frame) it makes, tagged with
// the array name so a session can come back to exactly this member.
// zipStat is the CALLER's stat of the zip, taken BEFORE its bytes were read:
// the multi-second member decode below is exactly the window an overwrite
// would exploit, and identity-before-bytes binds the OLD tuple to the OLD
// pixels - the safe direction (§6.2). Required, not defaulted: a zero stat
// would silently keep the member out of the registry.
static std::string npzOpenMember(const std::string& path, const std::vector<uint8_t>& zip,
                                 const NpzEntry& e, const std::string& arrayName,
                                 int npyRead /*NR_NATIVE = 0*/,
                                 const FrameSource& zipStat) {
    std::vector<uint8_t> member;
    std::string mErr;
    if (!npzExtract(zip, e, member, mErr)) return mErr;
    size_t before = app.images.size();
    std::string lErr = loadNpyBuffer(member, path, baseName(path) + ":" + arrayName, npyRead);
    if (!lErr.empty()) return lErr;
    for (size_t k = before; k < app.images.size(); k++) {
        app.images[k]->src->npzMember = arrayName;   // identity for session restore
        // Watch baseline (§2.1): the npz FILE is the local file these pixels
        // came from - src->path already names it; npzMember keeps members
        // distinct in the registry tuple (§6.2). Identity is complete only
        // HERE (loadNpyBuffer knows no member name), so this is where the
        // member either joins a resident source or registers its own.
        // (Measured before registering: addImage inside loadNpyBuffer ran
        // computeMinMax while the source was still private.)
        app.images[k]->src->mtime = zipStat.mtime;   // the PRE-read stat:
        app.images[k]->src->fsize = zipStat.fsize;   // identity binds those bytes
        shareOrRegisterSource(*app.images[k]);
    }
    return {};
}

// Members that are not pixels, kept against the FILE so the Inspector can show
// them beside the frame that did open (docs/npz-design.md §4 item 3 - metadata
// は Inspector の Image 節に、開いた frame の隣。専用パネルは作らない).
static void npzRememberMeta(const std::string& path, const std::vector<NpzMember>& mem) {
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto& m : mem) {
        if (m.role == NpzMember::RImage) continue;
        std::string v = m.text;
        if (v.empty() && m.role == NpzMember::RAxis)
            v = std::to_string(m.vals.size()) + " value(s)";
        items.emplace_back(m.name, m.shapeText + "  " + m.dtype + (v.empty() ? "" : "  = " + v));
    }
    for (auto& e : app.npzMeta)
        if (e.path == path) { e.items = std::move(items); return; }
    if (!items.empty()) app.npzMeta.push_back({ path, std::move(items) });
}

// What opened and what did not, BY NAME, in one line. Never silent in either
// direction: "3 member(s) skipped" without their names is how the one-pixel-tall
// exposure vector went unnoticed for as long as it did.
static std::string npzSummary(const std::string& path, const std::vector<NpzMember>& mem,
                              const std::vector<std::string>& openedNames,
                              int stacks, int stackFrames, int loneFrames,
                              const std::string& axisNote) {
    std::string s = baseName(path) + ": ";
    if (stacks || loneFrames) {
        s += "opened ";
        if (stacks) s += std::to_string(stacks) + " stack" + (stacks > 1 ? "s" : "") +
                         " (" + std::to_string(stackFrames) + " frames)";
        if (stacks && loneFrames) s += " + ";
        if (loneFrames) s += std::to_string(loneFrames) + " frame" + (loneFrames > 1 ? "s" : "");
    } else s += "opened nothing";
    if (!axisNote.empty()) s += ", " + axisNote;
    std::string notImg, notSel;
    int nNotImg = 0, nNotSel = 0;
    for (const auto& m : mem) {
        bool wasOpened = false;
        for (const auto& o : openedNames) if (o == m.name) { wasOpened = true; break; }
        if (wasOpened) continue;
        if (m.role == NpzMember::RImage) {
            nNotSel++;
            if (!notSel.empty()) notSel += ", ";
            notSel += m.name + " " + m.shapeText;
        } else {
            nNotImg++;
            if (!notImg.empty()) notImg += ", ";
            notImg += m.name + " " + m.shapeText + " -> " + m.becomes;
        }
    }
    if (nNotImg) s += ", " + std::to_string(nNotImg) + " member(s) not images: " + notImg;
    if (nNotSel) s += ", " + std::to_string(nNotSel) + " image member(s) not opened: " + notSel;
    return s;
}

// ---- the viewer container: an npz that declares its own layers ---------------
// docs/input-adapters.md §4.11. This is the ONE npz that does not have to be
// guessed at: the reserved members name the layer of every node, so the file
// answers the question §1 says a file normally cannot answer about itself.
//
// The discriminator is `__viewer` and nothing else (§4.11.1). Without it the
// file is an ordinary npz and the shape classifier owns it exactly as before -
// the two readings never overlap, so npz-design.md §2.5's human-written form is
// untouched by anything here.
static const int VIEWER_NPZ_VERSION = 1;
static std::string readerFileOf(const std::string& spec);   // fwd (§4.12, below)
static std::string readerFuncOf(const std::string& spec);

struct VnzNode {
    std::string layer;                 // "frame" "stack" "series" "batch"
    int parent = -1;
    std::string name, note, cfa;
    bool hasPixels = false;
    size_t pixelEntry = 0;
    std::vector<double> condVals, tsVals, range;
    std::vector<int64_t> rangeShape;
    std::string condName, condUnit, tsName, tsUnit;
    bool hasCond = false, hasTs = false;
    // filled while building
    int seqId = 0, seriesId = 0, docFirst = -1, docCount = 0, memberSeen = 0;
};

// One reserved member, by name. Everything the container says about itself is
// small (a word, a number, a vector) except the pixels, which are handed to the
// same decoder a plain member uses rather than being re-implemented here.
static bool vnzRead(const std::vector<uint8_t>& zip, const std::vector<NpzEntry>& entries,
                    const std::map<std::string, size_t>& byName, const std::string& name,
                    std::vector<uint8_t>& buf, NpyHead& H, std::string& err) {
    auto it = byName.find(name);
    if (it == byName.end()) { err = "missing member " + name; return false; }
    if (!npzExtract(zip, entries[it->second], buf, err)) return false;
    if (!npyPeekHeader(buf, H, err)) { err = name + ": " + err; return false; }
    return true;
}

static std::string vnzStr(const std::vector<uint8_t>& zip, const std::vector<NpzEntry>& entries,
                          const std::map<std::string, size_t>& byName, const std::string& name) {
    std::vector<uint8_t> buf; NpyHead H; std::string e;
    if (!vnzRead(zip, entries, byName, name, buf, H, e)) return {};
    return npzTextValue(buf, H);
}

static bool vnzNum(const std::vector<uint8_t>& zip, const std::vector<NpzEntry>& entries,
                   const std::map<std::string, size_t>& byName, const std::string& name,
                   double& out, std::string& err) {
    std::vector<uint8_t> buf; NpyHead H;
    if (!vnzRead(zip, entries, byName, name, buf, H, err)) return false;
    if (H.esize == 0) { err = name + " is not a number"; return false; }
    out = npyElem(buf, H, 0);
    return true;
}

static bool vnzVec(const std::vector<uint8_t>& zip, const std::vector<NpzEntry>& entries,
                   const std::map<std::string, size_t>& byName, const std::string& name,
                   std::vector<double>& out, std::vector<int64_t>& shape, std::string& err) {
    std::vector<uint8_t> buf; NpyHead H;
    if (!vnzRead(zip, entries, byName, name, buf, H, err)) return false;
    if (H.esize == 0) { err = name + " is not numeric"; return false; }
    size_t n = 1;
    for (int64_t d : H.shape) {
        if (d < 0 || (d && n > (size_t)-1 / (size_t)d)) { err = name + " has an impossible shape"; return false; }
        n *= (size_t)d;
    }
    if (H.dataOff + n * (size_t)H.esize > buf.size()) { err = name + " is truncated"; return false; }
    out.clear();
    out.reserve(n);
    // Values at FULL precision: conditions and timestamps are DATA and are
    // plotted and exported, so they never pass through float on the way in.
    for (size_t i = 0; i < n; i++) out.push_back(npyElem(buf, H, i));
    shape = H.shape;
    return true;
}

// Is this npz a viewer container? Only `__viewer` answers, and a version from
// the future is refused by name rather than read with today's meanings.
static bool vnzIsContainer(const std::vector<uint8_t>& zip, const std::vector<NpzEntry>& entries,
                           int& version) {
    for (const auto& e : entries)
        if (e.name == "__viewer.npy") {
            std::vector<uint8_t> buf; NpyHead H; std::string err;
            if (!npzExtract(zip, e, buf, err) || !npyPeekHeader(buf, H, err)) return false;
            version = H.esize ? (int)npyElem(buf, H, 0) : 0;
            return true;
        }
    return false;
}

static int vnzCfaPattern(const std::string& cfa, int& kind) {
    std::string p = cfa;
    kind = 1;
    if (p.compare(0, 5, "quad:") == 0) { kind = 2; p = p.substr(5); }
    for (int i = 0; i < 4; i++) if (p == CFA_PATTERNS[i]) return i;
    kind = 0;
    return -1;
}

// Build the layers a container declares. Refusals name the member at fault:
// stage 6's rule is that a malformed container says WHAT is wrong, and "this
// npz is invalid" would send the reader back to a hex editor.
// `origin` is the file the USER opened, which is not always the file these bytes
// came out of: when a reader produced this container, the bytes live in the
// cache under a hash. Everything the user sees or that identifies these pixels
// later - the batch name, the frame labels, FrameSource::path, the key the
// reader memory and Watch are looked up under - names `origin`. Getting this
// wrong labelled every frame with a hash and left the Inspector's own reader row
// unable to find the reader that had just run.
// How the build phase gets one node's pixels. The tree, the names, the axes and
// the layer rules are the same whether the container arrived as an npz or down a
// pipe; only the fetch differs, and it differs in exactly one line. Passing it in
// is what keeps there from being two copies of the layer rules to drift apart.
using VnzFetch = std::function<bool(int i, const VnzNode& v,
                                    std::vector<uint8_t>& out, std::string& err)>;
static std::string vnzBuild(std::vector<VnzNode>& nodes, int n, const std::string& origin,
                            const std::string& readerSpec, const VnzFetch& fetch,
                            const std::vector<std::pair<std::string, std::string>>& metaIn,
                            const FrameSource& originStat);

// ---- the framed stream ------------------------------------------------------
// What the harness writes with --stream: a line-based header, then the pixel
// blobs raw and in the order their `pixels` lines named them. An npz cannot be
// streamed - its directory is at the END, so reading one means holding all of
// it, which is the buffer this exists to remove.
//
// Header rules are the session file's: one key per line, the value is the rest
// of the line, and a key this build does not know is skipped rather than
// refused, so an older viewer still reads what it does understand.
static const int VIEWER_STREAM_VERSION = 1;

// A .npy header for a blob the stream described, written into space reserved at
// the front of the SAME buffer the bytes are read into. That is what lets the
// existing decoder be used unchanged and without a second copy of the pixels:
// the read lands directly where the decoder will look for it.
static size_t writeNpyHeaderFor(std::vector<uint8_t>& buf, const std::string& dtype,
                                const std::vector<int64_t>& shape) {
    std::string d = "{'descr': '<" + dtype + "', 'fortran_order': False, 'shape': (";
    for (size_t i = 0; i < shape.size(); i++) {
        d += std::to_string(shape[i]);
        if (i + 1 < shape.size() || shape.size() == 1) d += ",";
        if (i + 1 < shape.size()) d += " ";
    }
    d += "), }";
    size_t pre = 10;                       // magic(6) + version(2) + len(2)
    while ((pre + d.size() + 1) % 64) d += ' ';
    d += '\n';
    buf.resize(pre + d.size());
    const char magic[] = "\x93NUMPY";
    memcpy(buf.data(), magic, 6);
    buf[6] = 1; buf[7] = 0;
    uint16_t hlen = (uint16_t)d.size();
    buf[8] = (uint8_t)(hlen & 0xff);
    buf[9] = (uint8_t)(hlen >> 8);
    memcpy(buf.data() + pre, d.data(), d.size());
    return buf.size();
}

struct VnzBlob { int node = 0; std::string dtype; std::vector<int64_t> shape; uint64_t nbytes = 0; };

static std::string loadViewerStream(const std::string& path, const std::string& readerSpec,
                                    const std::string& origin,
                                    const FrameSource& originStat) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) return "cannot open what the reader wrote: " + path;
    std::string line;
    if (!std::getline(f, line)) return "the reader produced nothing";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    int ver = 0;
    if (sscanf(line.c_str(), "VIEWERSTREAM %d", &ver) != 1)
        return "not a viewer stream: first line is \"" + line.substr(0, 60) + "\"";
    if (ver > VIEWER_STREAM_VERSION)
        return "this stream is version " + std::to_string(ver) + " and this viewer reads " +
               std::to_string(VIEWER_STREAM_VERSION);

    std::vector<VnzNode> nodes;
    std::vector<VnzBlob> blobs;
    std::vector<std::pair<std::string, std::string>> meta;
    int n = -1;
    auto need = [&](int i) -> VnzNode* {
        if (i < 0 || i >= n) return nullptr;
        return &nodes[(size_t)i];
    };
    bool sawEnd = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "end") { sawEnd = true; break; }
        size_t sp = line.find(' ');
        std::string key = sp == std::string::npos ? line : line.substr(0, sp);
        std::string rest = sp == std::string::npos ? std::string() : line.substr(sp + 1);
        if (key == "n") {
            n = atoi(rest.c_str());
            if (n <= 0 || n > 100000) return "n is " + rest + ": not a node count";
            nodes.assign((size_t)n, VnzNode{});
            continue;
        }
        if (n < 0) return "the stream declares its nodes before it describes them (n is missing)";
        size_t sp2 = rest.find(' ');
        int idx = atoi(rest.c_str());
        std::string val = sp2 == std::string::npos ? std::string() : rest.substr(sp2 + 1);
        VnzNode* v = need(idx);
        if (!v) continue;                       // a node this build does not have
        auto nums = [&](const std::string& s, std::vector<double>& out) {
            std::istringstream is(s);
            size_t cnt = 0;
            is >> cnt;
            out.clear();
            double d;
            while (out.size() < cnt && is >> d) out.push_back(d);
        };
        if      (key == "layer")  v->layer = val;
        else if (key == "parent") v->parent = atoi(val.c_str());
        else if (key == "name")   v->name = val;
        else if (key == "note")   v->note = val;
        else if (key == "cfa")    v->cfa = val;
        else if (key == "range")  nums(val, v->range);
        else if (key == "timestamps") { nums(val, v->tsVals);   v->hasTs = true; }
        else if (key == "conditions") { nums(val, v->condVals); v->hasCond = true; }
        else if (key == "timestamps_name") v->tsName = val;
        else if (key == "timestamps_unit") v->tsUnit = val;
        else if (key == "conditions_name") v->condName = val;
        else if (key == "conditions_unit") v->condUnit = val;
        else if (key == "meta") {
            size_t s3 = val.find(' ');
            if (s3 != std::string::npos)
                meta.emplace_back(val.substr(0, s3), val.substr(s3 + 1));
        }
        else if (key == "pixels") {
            VnzBlob b;
            b.node = idx;
            std::istringstream is(val);
            int ndim = 0;
            is >> b.dtype >> ndim;
            for (int k = 0; k < ndim; k++) { int64_t d = 0; is >> d; b.shape.push_back(d); }
            is >> b.nbytes;
            v->hasPixels = true;
            v->pixelEntry = blobs.size();
            blobs.push_back(b);
        }
        // any other key: skipped on purpose, see the header comment
    }
    if (!sawEnd) return "the stream stopped before its header ended - the reader was cut short";
    if (n < 0) return "the stream never said how many nodes it has";

    // The blobs follow, in order. Read one, hand it to the same decoder a loose
    // .npy goes through, move on: only one blob is in memory at a time.
    size_t next = 0;
    return vnzBuild(nodes, n, origin, readerSpec,
                    [&](int, const VnzNode& v, std::vector<uint8_t>& out,
                        std::string& e) -> bool {
                        if (v.pixelEntry != next) {
                            e = "the stream's blobs are out of order";
                            return false;
                        }
                        const VnzBlob& b = blobs[v.pixelEntry];
                        size_t pre = writeNpyHeaderFor(out, b.dtype, b.shape);
                        out.resize(pre + (size_t)b.nbytes);
                        f.read((char*)out.data() + pre, (std::streamsize)b.nbytes);
                        if ((uint64_t)f.gcount() != b.nbytes) {
                            e = "the stream ended " +
                                std::to_string(b.nbytes - (uint64_t)f.gcount()) +
                                " byte(s) short of what it declared";
                            return false;
                        }
                        next++;
                        return true;
                    },
                    meta, originStat);
}

static std::string loadViewerNpz(const std::vector<uint8_t>& zip,
                                 const std::vector<NpzEntry>& entries, int version,
                                 const std::string& readerSpec, const std::string& origin,
                                 const FrameSource& originStat) {
    if (version > VIEWER_NPZ_VERSION) {
        char b[192];
        snprintf(b, sizeof b, "__viewer %d: this file was written by a newer viewer "
                              "(this one reads %d) - the layer meanings may have changed",
                 version, VIEWER_NPZ_VERSION);
        return b;
    }
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < entries.size(); i++) {
        const std::string& n = entries[i].name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".npy") == 0)
            byName[n.substr(0, n.size() - 4)] = i;
    }
    std::string err;
    double nd = 0;
    if (!vnzNum(zip, entries, byName, "__n", nd, err))
        return "not a usable container: " + err + " (a container declares __n, the node count)";
    int n = (int)nd;
    if (n <= 0 || n > 100000) return "__n is " + std::to_string(n) + ": not a node count";

    std::vector<VnzNode> nodes((size_t)n);
    for (int i = 0; i < n; i++) {
        VnzNode& nd2 = nodes[(size_t)i];
        std::string si = std::to_string(i);
        if (byName.find("__layer_" + si) == byName.end())
            return "__layer_" + si + " is missing: every node declares its layer "
                   "(frame / stack / series / batch)";
        nd2.layer = vnzStr(zip, entries, byName, "__layer_" + si);
        if (nd2.layer != "frame" && nd2.layer != "stack" && nd2.layer != "series" &&
            nd2.layer != "batch")
            return "__layer_" + si + " is \"" + nd2.layer + "\": not a layer "
                   "(frame / stack / series / batch)";
        double pv = -1;
        if (byName.count("__parent_" + si)) {
            if (!vnzNum(zip, entries, byName, "__parent_" + si, pv, err)) return err;
        } else if (i != 0) {
            return "__parent_" + si + " is missing: a node that is not the root names its parent";
        }
        nd2.parent = (int)pv;
        if (i == 0 && nd2.parent != -1)
            return "__parent_0 is " + std::to_string(nd2.parent) + ": node 0 is the root (-1)";
        if (i != 0 && (nd2.parent < 0 || nd2.parent >= i))
            return "__parent_" + si + " is " + std::to_string(nd2.parent) +
                   ": a parent must be an earlier node (depth first, root 0)";
        nd2.name = vnzStr(zip, entries, byName, "__name_" + si);
        nd2.note = vnzStr(zip, entries, byName, "__note_" + si);
        nd2.cfa  = vnzStr(zip, entries, byName, "__cfa_" + si);
        auto px = byName.find("__pixels_" + si);
        if (px != byName.end()) { nd2.hasPixels = true; nd2.pixelEntry = px->second; }
        if ((nd2.layer == "frame" || nd2.layer == "stack") && !nd2.hasPixels)
            return "__pixels_" + si + " is missing: a " + nd2.layer + " is pixels";
        if (byName.count("__range_" + si) &&
            !vnzVec(zip, entries, byName, "__range_" + si, nd2.range, nd2.rangeShape, err))
            return err;
        for (int f = 0; f < 2; f++) {
            const char* field = f == 0 ? "timestamps" : "conditions";
            std::string base = std::string("__") + field + "_values_" + si;
            if (!byName.count(base)) continue;
            std::vector<double> vals; std::vector<int64_t> shp;
            if (!vnzVec(zip, entries, byName, base, vals, shp, err)) return err;
            if (shp.size() != 1)
                return base + " is not a 1-D vector: one value per " +
                       (f == 0 ? "frame" : "stack");
            std::string nm = vnzStr(zip, entries, byName,
                                    std::string("__") + field + "_name_" + si);
            std::string un = vnzStr(zip, entries, byName,
                                    std::string("__") + field + "_unit_" + si);
            if (f == 0) { nd2.tsVals = std::move(vals); nd2.tsName = nm; nd2.tsUnit = un; nd2.hasTs = true; }
            else        { nd2.condVals = std::move(vals); nd2.condName = nm; nd2.condUnit = un; nd2.hasCond = true; }
        }
    }
    // Structure, before anything is opened: a tree that cannot hold is refused
    // whole rather than half-built into the session.
    std::vector<int> kids((size_t)n, 0);
    for (int i = 1; i < n; i++) {
        const VnzNode& c = nodes[(size_t)i];
        const VnzNode& p = nodes[(size_t)c.parent];
        kids[(size_t)c.parent]++;
        if (p.layer == "frame" || p.layer == "stack")
            return "__parent_" + std::to_string(i) + " is a " + p.layer +
                   ": only a series or a batch holds other layers";
        if (p.layer == "series" && c.layer != "stack" && c.layer != "frame")
            return "node " + std::to_string(i) + " is a " + c.layer +
                   " inside a series: a series holds stacks or frames (§4.4)";
        if (c.layer == "batch")
            return "__layer_" + std::to_string(i) + " is a batch: a batch does not nest";
    }
    for (int i = 0; i < n; i++) {
        const VnzNode& v = nodes[(size_t)i];
        if (v.layer == "series") {
            if (!kids[(size_t)i])
                return "node " + std::to_string(i) + " is a series with no stacks";
            if (v.hasCond && (int)v.condVals.size() != kids[(size_t)i]) {
                char b[192];
                snprintf(b, sizeof b, "__conditions_values_%d has %d value(s) but the series "
                                      "holds %d stack(s) - one condition per stack, and a "
                                      "partial mapping would lie about the rest",
                         i, (int)v.condVals.size(), kids[(size_t)i]);
                return b;
            }
        }
    }

    // __meta_* lives in the zip, so it is read here where the zip is.
    std::vector<std::pair<std::string, std::string>> meta;
    for (const auto& kv : byName) {
        if (kv.first.compare(0, 7, "__meta_") != 0) continue;
        std::vector<uint8_t> b; NpyHead H; std::string e;
        if (!vnzRead(zip, entries, byName, kv.first, b, H, e)) continue;
        meta.emplace_back(kv.first.substr(7), npzTextValue(b, H));
    }
    return vnzBuild(nodes, n, origin, readerSpec,
                    [&](int, const VnzNode& v, std::vector<uint8_t>& out,
                        std::string& e) { return npzExtract(zip, entries[v.pixelEntry], out, e); },
                    meta, originStat);
}

// ---- build ---------------------------------------------------------------
// Shared by both carriers (see VnzFetch). Everything here reads `nodes`; the one
// thing that knew where the bytes live is now the callback.
static std::string vnzBuild(std::vector<VnzNode>& nodes, int n, const std::string& origin,
                            const std::string& readerSpec, const VnzFetch& fetch,
                            const std::vector<std::pair<std::string, std::string>>& metaIn,
                            const FrameSource& originStat) {
    int prevBatch = app.loadBatchId;
    std::string batchName = nodes[0].layer == "batch" && !nodes[0].name.empty()
                                ? nodes[0].name : baseName(origin);
    app.loadBatchId = newBatch(uniqueBatchName(batchName));
    int batchId = app.loadBatchId;
    int frames = 0, stacks = 0, seriesN = 0;
    std::string firstErr;

    for (int i = 0; i < n; i++) {
        VnzNode& v = nodes[(size_t)i];
        if (v.layer == "batch") continue;
        if (v.layer == "series") {
            v.seriesId = newSeries(batchId, v.name);
            seriesN++;
            for (auto& s : app.series)
                if (s.id == v.seriesId) {
                    // §4.5: the condition belongs to the SERIES. Putting it on the
                    // stacks is exactly what lets sigma_t be computed across a
                    // sweep, which is the failure the layer split exists to stop.
                    s.paramName = v.condName;
                    // An empty unit is a REAL state here, not a missing one:
                    // Series::unit is documented empty-means-unset and the panel
                    // lets the user type it in. §4.3.2 - the values stay either way.
                    snprintf(s.unit, sizeof s.unit, "%s", v.condUnit.c_str());
                }
            continue;
        }
        // frame / stack: the pixels go through the SAME decoder a plain member
        // uses, so a container and a loose .npy of the same array cannot drift.
        std::vector<uint8_t> member;
        std::string mErr;
        if (!fetch(i, v, member, mErr)) {
            firstErr = "__pixels_" + std::to_string(i) + ": " + mErr;
            break;
        }
        size_t before = app.images.size();
        // The fallback name counts POSITION AMONG SIBLINGS, from zero - so the
        // first stack of a series is ":0" and lines up with stacks[0] in the
        // Python that produced it. It used to be the container's flat node
        // index, which counts the batch and the series nodes too: the first
        // stack of a plain sweep came out ":2" and matched nothing the author
        // could point at.
        int ord = 0;
        for (int k = 0; k < i; k++)
            if (nodes[(size_t)k].parent == v.parent &&
                nodes[(size_t)k].layer != "batch" && nodes[(size_t)k].layer != "series")
                ord++;
        std::string label = v.name.empty() ? baseName(origin) + ":" + std::to_string(ord) : v.name;
        std::string lErr = loadNpyBuffer(member, origin, label);
        if (!lErr.empty()) {
            firstErr = "__pixels_" + std::to_string(i) + ": " + lErr;
            break;
        }
        v.docFirst = (int)before;
        v.docCount = (int)(app.images.size() - before);
        if (v.docCount <= 0) { firstErr = "__pixels_" + std::to_string(i) + ": nothing decoded"; break; }
        v.seqId = app.images[before]->seqId;
        frames += v.docCount;
        if (v.layer == "stack") stacks++;

        int cfaKind = 0;
        int cfaPat = v.cfa.empty() ? -1 : vnzCfaPattern(v.cfa, cfaKind);
        for (int k = 0; k < v.docCount; k++) {
            ImageDoc* d = app.images[(size_t)v.docFirst + (size_t)k].get();
            d->src->npzMember = "__pixels_" + std::to_string(i);
            // the CALLER's stat, taken before the origin's bytes were read
            // (identity before bytes, §6.2): a stat taken here, after the
            // multi-second container decode, could bind NEW disk state to OLD
            // pixels and the Watch baseline would never notice the change
            d->src->mtime = originStat.mtime;
            d->src->fsize = originStat.fsize;
            if (!v.note.empty())
                d->note = d->note.empty() ? v.note : d->note + ", " + v.note;
            if (cfaPat >= 0) { d->cfa = cfaKind; d->cfaPattern = cfaPat; d->texDirty = true; }
            else if (!v.cfa.empty())
                d->note += (d->note.empty() ? "" : ", ") + std::string("cfa \"") + v.cfa +
                           "\" is not a known pattern";
        }
        // range: (2,) for the whole node, (F,2) one per frame (§4.3.3)
        if (v.rangeShape.size() == 1 && v.range.size() == 2) {
            for (int k = 0; k < v.docCount; k++) {
                app.images[(size_t)v.docFirst + (size_t)k]->black = (float)v.range[0];
                app.images[(size_t)v.docFirst + (size_t)k]->white = (float)v.range[1];
                app.images[(size_t)v.docFirst + (size_t)k]->texDirty = true;
            }
        } else if (v.rangeShape.size() == 2 && v.rangeShape[1] == 2 &&
                   (int)v.rangeShape[0] == v.docCount) {
            for (int k = 0; k < v.docCount; k++) {
                ImageDoc* d = app.images[(size_t)v.docFirst + (size_t)k].get();
                d->black = (float)v.range[(size_t)k * 2];
                d->white = (float)v.range[(size_t)k * 2 + 1];
                d->texDirty = true;
            }
            // The same warning montage's per-frame range carries: two frames that
            // look alike may have been stretched differently. Measurements are
            // untouched; only the display is.
            for (int k = 0; k < v.docCount; k++) {
                ImageDoc* d = app.images[(size_t)v.docFirst + (size_t)k].get();
                d->note += (d->note.empty() ? "" : ", ") + std::string("display range varies per frame");
            }
        } else if (!v.range.empty()) {
            for (int k = 0; k < v.docCount; k++)
                app.images[(size_t)v.docFirst + (size_t)k]->note +=
                    (app.images[(size_t)v.docFirst + (size_t)k]->note.empty() ? "" : ", ") +
                    std::string("__range_") + std::to_string(i) + " does not fit this layer - not applied";
        }
        // timestamps: the stack's own frame axis, landing in the very field the
        // paste path and the .npz picker write, so the panels need no new case.
        if (v.hasTs && v.seqId) {
            App::SeqInfo* si = seqInfo(v.seqId);
            if (si) {
                if ((int)v.tsVals.size() != v.docCount) {
                    for (int k = 0; k < v.docCount; k++)
                        app.images[(size_t)v.docFirst + (size_t)k]->note +=
                            ", timestamps do not cover every frame - not applied";
                } else {
                    // Set directly rather than through applyFrameAxisVals: that
                    // one requires a unit, and §4.3.2 is explicit that a missing
                    // unit must not cost the user the numbers. Unit empty = not
                    // applied as a label; the values are here and the x-axis
                    // popup is already seeded from them, so typing the unit
                    // finishes the job instead of retyping the measurements.
                    si->axisName = v.tsName.empty() ? "time" : v.tsName;
                    si->axisUnit = v.tsUnit;
                    si->axisVals = v.tsVals;
                }
            }
        }
        // join the parent series, in declaration order
        if (v.parent >= 0 && nodes[(size_t)v.parent].layer == "series") {
            VnzNode& par = nodes[(size_t)v.parent];
            int slot = par.memberSeen++;
            for (auto& s : app.series)
                if (s.id == par.seriesId && v.seqId) {
                    App::Series::Member m;
                    m.seqId = v.seqId;
                    // No unit does NOT mean no value (§4.3.2): Member::value is
                    // NaN when unset, and these are set.
                    if (par.hasCond && slot < (int)par.condVals.size())
                        m.value = par.condVals[(size_t)slot];
                    s.members.push_back(m);
                }
        }
    }
    app.loadBatchId = prevBatch;

    // metadata: __meta_<k> at the root, __meta_<i>_<k> below it. The viewer
    // CARRIES meta and never rewrites it (§4.3.1), so it lands beside the frame
    // as provenance rather than becoming a panel of its own. Collected by
    // whoever parsed the container, because that is the half that knows how the
    // members are stored.
    std::vector<std::pair<std::string, std::string>> meta = metaIn;
    if (!readerSpec.empty()) meta.emplace_back("reader", readerSpec);
    if (!meta.empty()) {
        bool replaced = false;
        for (auto& e : app.npzMeta)
            if (e.path == origin) { e.items = meta; replaced = true; break; }
        if (!replaced) app.npzMeta.push_back({ origin, meta });
    }

    if (!firstErr.empty()) return firstErr;
    if (!frames) return "the container declares no pixels";
    app.imagesRev++;
    char say[320];
    snprintf(say, sizeof say, "%s: %d frame(s), %d stack(s), %d series%s",
             baseName(origin).c_str(), frames, stacks, seriesN,
             readerSpec.empty() ? ""
                 : (" via " + baseName(readerFileOf(readerSpec)) + ":" +
                    readerFuncOf(readerSpec)).c_str());
    fprintf(stderr, "viewer npz: %s\n", say);
    toast(say);
    return {};
}

// An .npz is a CONTAINER, not a picture (docs/npz-design.md §2): the FILE is a
// batch and each member is a stack or a frame. Members are classified by shape
// first, and nothing that is not an image is opened as one - the (N,) exposure
// vector that used to arrive as a one-pixel-tall picture is offered as the
// stack's x axis instead, and scalars / strings stay metadata.
//
// onlyMember != "" restores a single array (sessions record which one): no
// picker and no batch of its own, because the session owns both.
static std::string loadNpz(const std::string& path, const std::string& onlyMember = "",
                           int npyRead = 0 /*NR_NATIVE*/,
                           const std::string& readerSpec = std::string(),
                           const std::string& origin = std::string()) {
    // identity BEFORE the bytes: see decodeNpyFrame - the multi-second member
    // decode below is exactly the window an overwrite would exploit. Every
    // member opened from THIS zip binds to this stat (npzOpenMember).
    FrameSource zipStat;
    zipStat.path = path;
    statSourceFile(zipStat);
    std::vector<uint8_t> zip;
    if (!readFileBytes(path, zip)) return "cannot read file";
    std::vector<NpzEntry> entries;
    std::string err;
    if (!npzList(zip, entries, err)) return err;
    // §4.11.1: `__viewer` and nothing else decides which of the two readings
    // this file gets. Checked before the members are classified, because a
    // container's reserved members would otherwise be offered as pictures.
    int vver = 0;
    if (vnzIsContainer(zip, entries, vver)) {
        // The file the user opened, which is this one unless a cache artifact
        // is standing in for it.
        const std::string& src = origin.empty() ? path : origin;
        if (!onlyMember.empty()) {
            // A session records one line per doc, but a container is ONE unit and
            // rebuilds every doc at once: the second line onward would duplicate
            // the whole tree. Already-here is the signal, so no state is carried
            // between the calls.
            for (const auto& d : app.images)
                if (d->src->path == src &&
                    d->src->npzMember.compare(0, 9, "__pixels_") == 0)
                    return {};
        }
        return loadViewerNpz(zip, entries, vver, readerSpec, src, zipStat);
    }
    std::vector<NpzMember> mem = npzScan(zip, entries);
    if (mem.empty()) return "no arrays in npz";
    if (!onlyMember.empty()) {
        for (const auto& m : mem) {
            if (m.name != onlyMember) continue;
            if (m.role != NpzMember::RImage)
                return "member " + onlyMember + " " + m.shapeText + " is not an image (" +
                       m.why + ")";
            npzRememberMeta(path, mem);
            // npyRead rides along: a session restoring a member, and a §3.3
            // re-read of one, both come through here and both must land on the
            // reading the user declared rather than on the native one.
            return npzOpenMember(path, zip, entries[m.entry], m.name, npyRead, zipStat);
        }
        return "no member named " + onlyMember + " in this npz";
    }
    int images = 0, axes = 0;
    for (const auto& m : mem) if (m.role == NpzMember::RImage) images++;
    // An axis CANDIDATE is a 1-D numeric member exactly as long as some image
    // member's frame count. Anything else 1-D stays metadata - and either way
    // it is never pixels.
    for (const auto& m : mem) {
        if (m.role != NpzMember::RAxis || m.vals.empty()) continue;
        for (const auto& im : mem)
            if (im.role == NpzMember::RImage && im.frames > 1 &&
                (size_t)im.frames == m.vals.size()) { axes++; break; }
    }
    // Nothing in here is pixels. Say what IS in here - a dialog whose only live
    // button is Cancel answers a question nobody can act on.
    if (images == 0) {
        std::string what;
        for (const auto& m : mem)
            what += (what.empty() ? "" : ", ") + m.name + " " + m.shapeText + " " + m.dtype;
        npzRememberMeta(path, mem);
        return "no image-shaped arrays in this npz (" + what + ")";
    }
    // ONE image and nothing to decide: open it, no dialog. That is what an .npz
    // holding a single array has always done and it must keep doing it.
    if (images == 1 && axes == 0) {
        npzRememberMeta(path, mem);
        for (const auto& m : mem) {
            if (m.role != NpzMember::RImage) continue;
            // the file is the batch here too, so the two ways in agree
            int prevBatch = app.loadBatchId;
            app.loadBatchId = newBatch(uniqueBatchName(baseName(path)));
            std::string oErr = npzOpenMember(path, zip, entries[m.entry], m.name,
                                             0 /*NR_NATIVE*/, zipStat);
            app.loadBatchId = prevBatch;
            if (!oErr.empty()) return baseName(path) + ":" + m.name + ": " + oErr;
            std::string say = npzSummary(path, mem, { m.name }, m.frames > 1 ? 1 : 0,
                                         m.frames > 1 ? m.frames : 0, m.frames > 1 ? 0 : 1,
                                         "");
            fprintf(stderr, "npz: %s\n", say.c_str());
            toast(say);
            return {};
        }
    }
    // More than one picture, or a 1-D key that could be this stack's axis:
    // ask, in the folder picker's words. One dialog at a time - a drop of three
    // .npz files used to leave only the last one's members on screen, with the
    // other two dropped in silence.
    if (app.npzPickOpen) {
        app.npzPickQueue.push_back(path);
        toast(baseName(path) + ": waiting its turn - choose arrays for " +
              baseName(app.npzPickPath) + " first (" +
              std::to_string(app.npzPickQueue.size()) + " file(s) waiting)");
        return {};
    }
    app.npzPick = std::move(mem);
    app.npzPickPath = path;
    app.npzAxisName[0] = '\0';        // the key name is offered when one is
    app.npzAxisUnit[0] = '\0';        // ticked; the unit is NEVER pre-filled
    app.npzPickOpen = true;
    return {};
}

// A .npy FILE. The frame loop lives in loadNpyBuffer, which the .npz members
// have used all along; this reads the bytes and hands them over.
//
// It used to have its own copy of that loop calling decodeNpyFrame per frame -
// and decodeNpyFrame reads the WHOLE FILE every time it is called. A 120-frame
// 1024x1024 u16 stack (252 MB) was therefore read 120 times, 30 GB through the
// page cache to open one file: 12.2 s measured, against 0.35 s reading it once.
// The duplicated loop is also how the two doors drifted apart - 452c62a taught
// the .npz copy to say "n of N" on a partial load and this one kept claiming
// the full count, which docs/terminology.md 実装上の不変条件 forbids.
static std::string loadNpy(const std::string& path, int npyRead = 0 /*NR_NATIVE*/) {
    // identity BEFORE the bytes (§6.2): an overwrite completing during the
    // read binds the OLD tuple to the OLD pixels - the safe direction
    FrameSource preStat;
    preStat.path = path;
    statSourceFile(preStat);
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read file";
    size_t before = app.images.size();
    std::string err = loadNpyBuffer(buf, path, "", npyRead);
    if (!err.empty()) return err;
    // The Watch baseline, stamped from the PRE-read stat. decodeNpyBuffer
    // cannot: for an .npz member the file on disk is the CONTAINER, not the
    // array, so the buffer path deliberately leaves the source alone. Here the
    // file on disk IS the array. Identity is complete only now, so this is
    // also where each frame either joins a resident source or registers its
    // own (§6.2 - addImage inside loadNpyBuffer already ran computeMinMax, so
    // no source enters the registry showing the default 0/1 range).
    for (size_t i = before; i < app.images.size(); i++)
        if (app.images[i]->src) {
            app.images[i]->src->mtime = preStat.mtime;
            app.images[i]->src->fsize = preStat.fsize;
            shareOrRegisterSource(*app.images[i]);
        }
    return {};
}

// A PICTURE FILE: PNG, JPEG - and TIFF, which refuses by name in this build.
// The decoding is behind core/imagefile.h so that the library can be replaced
// without this file learning its name; what comes back is the same Image the
// .npy door produces, so everything downstream is unchanged.
//
// It sits HERE, next to loadNpy, deliberately. The alternative - a second
// "open a picture" path with its own document creation, its own note and its
// own error wording - is how two doors to the same room drift apart, which
// this file has already paid for once (loadNpy and the .npz member loop said
// different things about a partial stack until they were made one function).
static std::string loadImageFile(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!readFileBytes(path, buf)) return "cannot read file";
    imagefile::Image img;
    std::string err;
    if (!imagefile::decode(path, buf, img, err)) return err;

    auto im = std::make_unique<ImageDoc>();
    im->name = baseName(path);
    FrameSource& S = *im->src;
    S.path = path;
    S.w = img.w; S.h = img.h; S.ch = img.ch;
    S.dtype = img.dtype;
    S.data = std::move(img.data);
    // npyShape stays empty on purpose: these formats declare their own axes, so
    // there is no second reading of them to offer (§3.3's menu is computed from
    // that shape, and an empty one means "no other reading exists").
    im->note = img.note;
    // Only what the decoder READ. No format here can be relied on to declare a
    // CFA pattern, so none of them claims one - and --cfa still applies in
    // addImage, because that is the user declaring it rather than us guessing.
    im->cfa = img.cfa;
    im->cfaPattern = img.cfaPattern & 3;
    im->syncMirrors();
    statSourceFile(S);              // the file on disk IS these pixels: Watch baseline
    addImage(std::move(im));
    return {};
}

// ---- §3.3: say it again, and mean it ----------------------------------------
// The successor to --npy-axis. Not one guess corrected by another: the reading
// is a DECLARATION, so it is remembered on the source, saved with the session,
// and applied to every frame of the stack it rebuilds.

// What re-reading COSTS, named before it happens (§3.3 requires this said in
// advance). Re-reading builds a new document with a new identity, so anything
// that pointed at the old one by identity - a compare slot pinned to it, a
// frame number that other state refers to - points at nothing afterwards.
// Empty = nothing is riding on this document and the re-read costs nothing.
static std::string npyReReadLoss(const ImageDoc* d) {
    if (!d) return {};
    std::vector<std::string> lost;
    std::string slots;
    for (const auto& q : app.images) {
        if (q->seqId != 0 ? q->seqId != d->seqId : q.get() != d) continue;
        if (app.compareBUid == q->uid) slots += slots.empty() ? "B" : ", B";
        for (size_t i = 0; i < app.cmpExtra.size(); i++)
            if (app.cmpExtra[i] == q->uid) {
                if (!slots.empty()) slots += ", ";
                slots += slotName(i);
            }
    }
    if (!slots.empty()) lost.push_back("compare slot " + slots + " unpins");
    // A crop is a rectangle in a grid the new reading may not even have, so it
    // is dropped rather than carried onto pixels it no longer describes.
    if (d->src->srcW > 0 && (d->w != d->src->srcW || d->h != d->src->srcH))
        lost.push_back("the crop is dropped (the new reading is a different grid)");
    int frames = d->seqId != 0 ? (int)framesOfSeq(d->seqId).size() : 1;
    if (frames > 1)
        lost.push_back("the stack's " + std::to_string(frames) +
                       " frames are rebuilt, so anything keyed to a frame NUMBER "
                       "(follow-frame pairing, a pinned frame) is re-established");
    if (lost.empty()) return {};
    std::string s = "re-reading rebuilds this document: ";
    for (size_t i = 0; i < lost.size(); i++) {
        if (i > 0) s += "; ";
        s += lost[i];
    }
    return s;
}

// Read the SAME file again under reading r. The old document is dropped only
// after the new one exists, so a re-read that cannot happen leaves what was on
// screen exactly where it was.
static std::string npyReReadAs(ImageDoc* d, int r) {
    if (!d || !d->src) return "no image";
    const std::string path = d->src->path, member = d->src->npzMember;
    if (path.empty()) return "these pixels have no file to read again";
    if (d->src->npyShape.empty()) return "not a .npy: there is no other reading of it";
    if (npyReadLabel(d->src->npyShape, r).empty())
        return npyShapeText(d->src->npyShape) + " cannot be read that way";
    // What belongs to the FILE and not to the reading travels across; the range
    // does not, because a different reading is a different set of pixels.
    const int cfa = d->cfa, pat = d->cfaPattern, lut = d->displayLut, batch = d->batchId;
    const bool col = d->cfaColorize;
    const int oldSeq = d->seqId;
    const uint64_t oldUid = d->uid;

    const size_t before = app.images.size();
    const int prevBatch = app.loadBatchId;
    app.loadBatchId = batch;              // a re-read stays in the batch it was in
    const bool quietWas = g_quietLoad;
    g_quietLoad = true;                   // selection is set below, once
    std::string err = member.empty() ? loadNpy(path, r) : loadNpz(path, member, r);
    g_quietLoad = quietWas;
    app.loadBatchId = prevBatch;
    if (err.empty() && app.images.size() == before) err = "nothing opened";
    if (!err.empty()) return err;         // nothing was closed: the old doc stands

    std::vector<ImageDoc*> made;
    for (size_t k = before; k < app.images.size(); k++) made.push_back(app.images[k].get());
    // now, and only now, the reading it replaces goes
    if (oldSeq != 0) closeStack(oldSeq);
    else {
        for (size_t k = 0; k < app.images.size(); k++)
            if (app.images[k]->uid == oldUid) { closeImages({ (int)k }); break; }
    }
    for (ImageDoc* m : made) {
        m->cfa = cfa; m->cfaPattern = pat; m->cfaColorize = col;
        m->displayLut = lut;
        m->texDirty = true;
    }
    for (size_t k = 0; k < app.images.size(); k++)
        if (app.images[k].get() == made[0]) { app.current = (int)k; break; }
    app.imagesRev++;
    return {};
}

// The Inspector ASKS; the work happens here, between frames. Re-reading
// destroys the very document whose panel requested it, so running it inside the
// draw would pull the ground out from under the rest of that panel.
static uint64_t g_reReadAsk  = 0;            // uid whose confirm popup is up
static int      g_reReadWant = NR_NATIVE;    // the reading it is asking about
static uint64_t g_reReadGo   = 0;            // uid confirmed; runs next frame
static void pumpReRead() {
    if (!g_reReadGo) return;
    const uint64_t uid = g_reReadGo;
    const int want = g_reReadWant;
    g_reReadGo = 0; g_reReadAsk = 0; g_reReadWant = NR_NATIVE;
    ImageDoc* d = nullptr;
    for (const auto& q : app.images) if (q->uid == uid) { d = q.get(); break; }
    if (!d) return;                          // closed while the popup was up
    const std::string label = npyReadLabel(d->src->npyShape, want);
    const std::string name = baseName(d->src->path);
    std::string e = npyReReadAs(d, want);
    if (!e.empty()) toast(name + ": " + e, true);
    else            toast(name + ": read as " + label);
}

static void runProcessor(int idx) {
    ImageDoc* im = cur();
    if (!im || idx < 0 || idx >= (int)plugin_host::processors().size()) return;
    const ProcessorPluginInfo& p = plugin_host::processors()[idx];
    psFrame in = makeFrame(*im), out = {};
    char err[256] = { 0 };
    if (p.v.process(&in, &out, plugin_host::hostApi(), err, sizeof err) != 0) {
        toast(p.name + ": " + (err[0] ? err : "failed"), true);
        return;
    }
    // contract validation — a misbehaving plugin must never crash the host
    if (!out.data || out.dtype != PS_DTYPE_F32 || out.loc != PS_MEM_CPU ||
        out.ch < 1 || out.ch > 4 || out.w < 1 || out.h < 1 || out.w > 32768 || out.h > 32768 ||
        out.pitch_bytes < (size_t)out.w * out.ch * sizeof(float)) {
        if (out.data) plugin_host::frameFree(out.data);
        toast(p.name + ": plugin returned an invalid frame", true);
        return;
    }
    auto doc = std::make_unique<ImageDoc>();
    doc->name = im->name + " [" + p.name + "]";
    FrameSource& S = *doc->src;            // the plugin's result: new pixels, no file
    S.w = (int)out.w; S.h = (int)out.h; S.ch = (int)out.ch;
    S.dtype = "f32";
    doc->note = "processed by " + p.name;
    doc->cfa = out.cfa_type; doc->cfaPattern = out.cfa_pattern & 3;
    S.data.resize((size_t)out.w * out.h * out.ch);
    size_t rowFloats = (size_t)out.w * out.ch;
    for (uint32_t y = 0; y < out.h; y++)   // pitch-aware copy into the ImageDoc
        memcpy(S.data.data() + (size_t)y * rowFloats,
               (const char*)out.data + (size_t)y * out.pitch_bytes, rowFloats * sizeof(float));
    doc->syncMirrors();
    plugin_host::frameFree(out.data);      // host frees, always
    float bk = out.black, wt = out.white;
    // processed results have no file path: sessions skip them (v2: re-run recipe)
    addImage(std::move(doc));
    if (wt > bk) { cur()->black = bk; cur()->white = wt; cur()->texDirty = true; }
    toast("processed: " + cur()->name);
}
