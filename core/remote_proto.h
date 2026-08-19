// Wire format shared by the client and `viewer --serve`.
//
// The transport is deliberately the ssh stdio pipe: the client runs
//     ssh user@host viewer --serve
// and talks to its stdin/stdout, exactly the way rsync and git do it. No port to
// open, no daemon to keep alive, no listening socket to secure - ssh already did
// the authentication, and a process that owns no socket cannot be reached by
// anyone who is not already through ssh.
//
// What crosses the link is only what the screen needs: a decimated view of the
// region being looked at, in the source dtype (so pixel values stay exact), and
// measurement RESULTS rather than the pixels they were measured from.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>                   // snprintf, for exactNum
#include <stdlib.h>                  // strtod, for exactNum's round-trip check
#include <string.h>                  // memcpy, for the half <-> float bit moves.
                                     // MSVC and libc++ pull it in transitively;
                                     // libstdc++ does not, so leaving it out
                                     // built everywhere except the Linux runner.
#include <string>
#include <vector>

namespace rp {

static const uint32_t MAGIC = 0x56525031;   // "VRP1"
// 1 = LIST/META/TILE; 2 adds MEASURE; 3 extends the LIST reply (mtime, npy
// header peek, synthetic stack-group entries) and adds GLOB/SCAN. The server
// answers LIST in the v2 shape when the client's HELLO said 2, so either end
// may lag the other by one protocol without breaking the session.
//
// 4 changes no wire format at all: the sequence-GROUPING rules changed ('?'
// only on the varying digit runs; the frame axis is the LAST varying run).
// The number is what makes an already-installed peer update itself on connect
// - versioning the MEANING, not just the framing. The user hit the gap this
// closes: a peer updated mid-way through those changes grouped a linearity
// folder differently than the same folder opened locally, with nothing
// anywhere saying why. If the answers can differ, the versions must.
//
// 5: same story again - the DISPLAYED pattern of a sequence group now carries
// its extent ("frame_000..023.npy", not "frame_???.npy"). Wire format
// unchanged, but a stale peer sends the old text while the local scanner
// produces the new one, so the same folder reads differently depending on
// which side listed it. Versioning the meaning is what makes the peer update
// itself on connect.
//
// 6: the MEANING again, and this time of a number people paste into reports.
// MOP_TEMPORAL_STATS' `sigma_fpn [DN]` is now the CORRECTED quantity - the
// temporal residual mean(s_t,i^2/n_i) subtracted, ddof=1 spatial variance,
// clamped at 0 and saying so (#57 judgment 2, docs/features/analysis/flat-field-stats.md (b)) -
// and the reply carries `fpn_corr [DN]` / `fpn_clamped` beside it. A v5 peer
// answers the same key with the UNCORRECTED upper bound, which the panel would
// then print under a label declaring a correction it never had: the same folder
// measured here and there would differ by sigma_t^2/N with nothing saying why.
// Wire format is unchanged (MEASURE carries named items), so this number exists
// solely to make an installed peer update itself on connect.
//
// 7: MOP_PLUGIN_ANALYZE - plugin analysis run on the peer under name+version
// PARITY, with the peer's own ledger returned as provenance (docs/reference/abi-v3.md
// §10, #104 judgment 8). This is FRAMING, like 2 (adds MEASURE) and 3 (adds
// GLOB/SCAN) and unlike 4/5/6: no answer a v6 peer already gives changes
// meaning, and MOP_ANALYZER keeps computing exactly what it computed.
//
// It is numbered anyway, and for the reason those framing bumps were. A v6
// peer answers the new op with "unknown measure op" - a refusal, correct, and
// indistinguishable from the parity refusal this op exists to produce. §10's
// whole content is that a refusal must say WHICH mismatch it is and never turn
// into a silent local run, so "the peer is too old" has to be a sentence the
// client can write BEFORE it sends, from a number, rather than a sentence it
// guesses afterwards from an error string. That number is this one - and it is
// also what makes an installed viewer-serve update itself on connect, which is
// the reason a lab that copied the peer once ever sees the op at all.
//
// 8: MOP_SET_FOLD - the fold half of a SET analysis run on the peer, under a
// parity check on the fold's own declared form (docs/analysis-layers.md §3.5 /
// §6, #57 judgments 4/5). FRAMING again, and for §10's reason rather than a new
// one: this op adds a REQUEST GRAMMAR the older head cannot express. A set is
// {role: stack} and MeasureReqHead carries one flat path list with nowhere to
// say where one stack stops and the next begins, so the grouping arrives in a
// role block appended after the rois - which a v7 peer never reads, because it
// refuses on the op number first.
//
// That refusal is correct and unreadable: "unknown measure op" is the same
// sentence a v7 peer gives for a typo, and it is indistinguishable from the
// parity refusal this op exists to produce. With the number the client refuses
// BEFORE it sends, names which mismatch it is, and never turns a too-old peer
// into a quiet local fold over pixels it would have had to fetch.
//
// 9: the §3.3 escape hatch reaches a document opened through a peer (issue
// #124). META now carries the shape the FILE DECLARED, and META and TILE both
// take a DECLARED READING of it, so the peer can serve "(48,40,1) read as
// (F,H,W)" and not only the reading its own rule picked.
//
// The two bumps before this one were both about a refusal being ILLEGIBLE - a
// v7 peer answering "unknown measure op" cannot be told apart from a parity
// refusal or a typo. This one is the opposite failure and, for a user, the
// worse one: there would be NO refusal. The reading is appended after TileReq
// exactly the way MOP_PLUGIN_ANALYZE's fields are appended after
// MeasureReqHead - so that not one byte moves for a request an older peer
// already understands - and the consequence of that layout is that a v8 peer
// reads the head it knows, never reads the four bytes behind it, and answers
// SUCCESSFULLY with the native reading. The user declares "read this mono
// (H,W,1) capture as a stack", the same single 40x48 frame comes back, and the
// Inspector then labels those pixels with the reading that was asked for. A
// silent wrong answer wearing the right label is not a degraded escape hatch;
// it is worse than the blank line #124 was filed about, because the blank line
// at least says nothing.
//
// So the number is what lets the client refuse BEFORE it sends and name which
// mismatch it is - the same discipline 7 and 8 established, reached from the
// other direction. It is also what makes the REPLY readable: without it, "no
// shape trailer on this META" cannot be told from "this file has no shape to
// declare", and the Inspector would have to guess which silence it was looking
// at. And it is, as ever, what makes an installed viewer-serve update itself on
// connect, which is the only reason a lab that copied the peer once ever sees
// the feature at all.
// 10: the peer serves the PICTURE FORMATS - PNG, JPEG, TIFF, OpenEXR, y4m -
// and not only .npy (issue #148, judgment B). No wire format moves by one byte:
// MSG_TILE was always "-> (path, frame, rect, step) <- pixels", in the source
// dtype, and there has never been a send-a-file op. What changes is the set of
// files an answer exists for, and that is exactly the kind of change 4, 5 and 6
// were - the MEANING, not the framing.
//
// It has to be a number for the two reasons those three had. First, a v9 peer
// answers META on a .png with "not a .npy file", which is indistinguishable
// from a corrupt file and blames the file for a limit that belongs to the
// peer's build; from the number the client refuses BEFORE it sends and names
// which mismatch it is (rp::pictureTooOldText). Second - and this is what makes
// a lab that copied viewer-serve once ever see the feature - a version
// difference is what makes an installed peer update itself on connect.
//
// WHY A NUMBER AND NOT A LIST OF EXTENSIONS IN THE HELLO REPLY. It was the
// obvious alternative and it is the weaker one: a served-suffix list on the
// wire is a SECOND place that knows which formats cross the link, beside
// core/imagefile.cpp's table, and two places that know one thing is the defect
// #148 is about. The number says the same thing in the form the rest of this
// header already uses, and it says it about a build rather than about a guess.
// The day two peers of the SAME protocol legitimately differ - one built
// without OpenEXR, say - a number stops being enough and a list becomes
// necessary; there is no such build today (#53 made OpenEXR unconditional), so
// there is nothing for a list to say that the number does not.
// 11: the peer serves HEADERLESS RAW - .bin .raw .yuv .dat .rggb - when the
// request carries the geometry. This is the first op family whose answer
// depends on something the FILE does not contain: every format before it
// declared its own shape, and a headerless one declares none, so the shape
// travels with the request as a RawWire trailer (below).
//
// It has to be a number, for this header's usual two reasons and a third that
// is sharper here. A v10 peer handed a .raw answers META with "not a .npy
// file" - a sentence about the file, for a limit that belongs to the peer -
// and the trailer it never reads is exactly the declaration that would have
// made the request answerable. Worse than #124's case: there the peer returned
// the wrong reading of real pixels; here it would return nothing while blaming
// the file. The third reason is that the client must ALSO be able to tell "this
// peer will not carry a recipe" from "this request forgot to carry one", and
// only a version separates those.
//
// The trailer is APPENDED, so a v10 peer's parse of a v10 request does not move
// by one byte - the discipline every append in this header has kept.
//
// 12: READERS RUN ON THE PEER (issue #180 judgment B, docs/
// docs/features/remote/remote-reader-design.md). docs/features/adapters/input-adapters.md §4.13.1 settled on
// 2026-08-03 that "an adapter runs where the file lives"; until now there was
// no door for it. MSG_READER_RUN carries the reader's THREE TEXTS to the peer,
// the peer runs the harness there under a gate its own launcher opened
// (--serve-readers, closed by default), and META / TILE reach what it produced
// through a trailer naming the peer-issued cache key and the tree node.
//
// FRAMING, like 7 (MOP_PLUGIN_ANALYZE) and 8 (MOP_SET_FOLD): no answer a v11
// peer already gives changes meaning, and the new op is one a v11 peer refuses
// on the op number before it reads a byte of the payload.
//
// It has to be a number for this header's usual reason and for a sharper one
// that is #124's exactly. The reader trailer on META / TILE is APPENDED, and an
// older peer does not refuse a trailer - it never reads it. So a v11 peer
// handed "META, key <k>, node 0" reads the path, finds a real .npy sitting
// there (the origin the reader was pointed at IS often a file the peer serves),
// and answers SUCCESSFULLY with the native reading of it. The client would then
// label those pixels with the reader's name. A silent wrong answer wearing the
// right label is worse than a blank, which is the whole of protocol 9's
// conclusion - so the client refuses BEFORE it sends, from this number
// (rp::readerTooOldText), and never learns it by asking.
//
// The three states a user can be in are three different sentences, and none of
// them is guessed from an error string: "your peer is too old" is written here
// from the number, "this peer was started without --serve-readers" is written
// by the peer that has the flag and not the consent, and a v11 peer's "unknown
// request" is neither of those and says so.
//
// 13: A CONTAINER CROSSES THE LINK (issue #217, docs/features/remote/remote-reader-design.md
// §10). An .npz is a zip of .npy arrays and the wire has only ever addressed
// FILES, so a .npz row was refused with "the peer serves one array per file,
// not a container" - a true sentence about a door nobody had built.
// MSG_NPZ_SCAN lists the members, and the trailer protocol 12 introduced does
// the rest: [str key][u32 node] already means "one array inside something the
// peer materialised", and a reader's output was only the first thing that could
// be (§10.5). No new addressing, no npz-specific pixel path - the member is
// inflated once into the peer's cache and read by openNpy from there.
//
// It is FRAMING, like 7, 8 and 12: no answer a v12 peer gives changes meaning.
// It is numbered for this header's usual two reasons and for the one that
// applies to every gate here - a v12 peer answers MSG_NPZ_SCAN with "unknown
// request", which is what it also says for a typo, so the client refuses BEFORE
// it sends (rp::npzTooOldText) and names which mismatch it is.
//
// Why 13 and not "inside 12": §10.7 fixed the rule and the rule decided it.
// Twelve had not shipped when §10 was written, so the verb could have been
// free; it shipped (#180 stage 1-2, PR #218) before this was built, and from
// that moment a peer announcing 12 is a peer that has no MSG_NPZ_SCAN. Sharing
// the number would make "refuse before you send" unwritable, which is the one
// discipline this header does not bend.
//
// 14: MEASURE REACHES ONE ARRAY INSIDE A MATERIALISATION (issue #180 stage 5,
// docs/features/remote/remote-reader-design.md §8). Twelve and 13 taught META and TILE to
// address a reader's node and a .npz member; MEASURE could still only name
// FILES, so the one thing a document opened that way could not do was the thing
// the link exists for - have its statistics computed where its pixels are. The
// aggregate ops take the same [str key][u32 node] the other two do, declared by
// MeasureReqHead::flags bit1 (MRF_KEYED) and written after the rois.
//
// FRAMING, like 7, 8, 12 and 13: no answer a v13 peer already gives changes
// meaning, and every request an older client sends is byte for byte what it was
// (`flags` bit1 was reserved-0, and the block only exists when the bit is set).
//
// It has to be a number, and here BOTH failure modes this header collects are
// in play at once. A v13 peer does not refuse the keyed block - it never reads
// it - and what it does with the rest of the request depends on what the
// request says the subject is:
//   nPaths 0  it answers "bad MEASURE header", a sentence about a malformed
//             request for a limit that belongs to its build (#148's shape);
//   nPaths 1  it would measure THE ORIGIN PATH - the .npz whole, or the file a
//             reader was pointed at - and answer with a sigma_t computed over
//             bytes the user never opened, under the label of the document they
//             are looking at. That is #124 exactly, and it is why the client
//             sends no path at all for a keyed measurement and refuses before
//             it sends (rp::measureKeyedTooOldText).
//
// Why not "inside 13": §10.7's rule again, and it decides this the same way it
// decided the line above. Thirteen shipped (#217 / #180 stage 3, PR #221)
// before this was built, so a peer announcing 13 is a peer whose MEASURE knows
// nothing of a key - and sharing its number would make the refusal above
// unwritable. The design assigned this bit the value 2 when it expected to
// arrive inside 12 (§4.2's MRF_READER); the VALUE is kept, because no byte of
// it has ever been on a wire, and only the number that gates it moves.
static const uint32_t VERSION = 14;

enum MsgType : uint32_t {
    MSG_HELLO      = 1,   // -> (version)                  <- (version, server id)
    MSG_LIST       = 2,   // -> (path)                     <- entries
    MSG_META       = 3,   // -> (path)                     <- shape/dtype/frames
    MSG_TILE       = 4,   // -> (path, frame, rect, step)  <- pixels
    MSG_MEASURE    = 5,   // -> (op, frames, rois, name)   <- emitted results only
    MSG_GLOB       = 6,   // -> (root, pattern, depth, cap) <- matching rel paths
    MSG_SCAN       = 7,   // -> (root, depth, cap)         <- stack groups per subdir
    // -> (origin path, func, 3 named texts)  <- (outcome, err, stderr,
    //                                            python provenance, key, header)
    // The reader itself crosses the link; the pixels it makes do not. See
    // docs/features/remote/remote-reader-design.md §3 for why the file is CARRIED rather than
    // resolved by name on the peer, and §4.2 for the field-by-field wire.
    MSG_READER_RUN = 8,
    // -> (path)  <- (key, kind, one FACT per member of the .npz)
    //
    // What is in this container, and nothing about what it MEANS: a member's
    // name, what its directory says it holds, the bytes of its .npy header,
    // and - for the small ones - its values. The vocabulary that turns those
    // into "stack of 24 frames" / "could be the x axis" / "metadata" stays in
    // the client's one classifier, so a picker row cannot read differently
    // depending on which end listed the file (§10.2).
    //
    // The peer builds no tree, opens no document and inflates no pixels here.
    // The key it issues names the file it listed; a member is reached later by
    // the ordinary [key][node] trailer on META / TILE, and inflated then.
    //
    //   -> [str peerPath]
    //   <- [str key]                   opaque, hex, issued here (§10.6)
    //      [u32 kind]                  NpzKind
    //      [u32 containerVersion]      kind 1 only; 0 otherwise
    //      [u32 nMembers] per member:
    //          [str name]              the array name, without ".npy"
    //          [u64 usize]             what the zip directory says it holds
    //          [u32 node]              the address to quote back on META / TILE
    //          [str err]               "" when the header was read
    //          [u32 flags]             bit0: `bytes` is the WHOLE member
    //          [u32 n][bytes]          the member's .npy HEADER, and its values
    //                                  too when bit0 (nz::Fact)
    //
    // The header bytes travel rather than the fields parsed out of them, so
    // that fortran order, byte order and item size cannot be dropped by a wire
    // format that forgot one: the client runs nz::peekHeader on exactly the
    // bytes the peer ran it on.
    MSG_NPZ_SCAN   = 9,
    MSG_OK         = 128,
    MSG_ERR        = 129,
};

// MSG_NPZ_SCAN reply: which of the two readings docs/features/adapters/input-adapters.md §4.11.1
// splits an .npz into. The peer decides it from the NAMES alone (`__viewer` is
// present or it is not) - the same discriminator the local door uses, asked
// before anything is inflated.
enum NpzKind : uint32_t {
    NK_ORDINARY  = 0,   // members classified by shape; the picker chooses
    NK_CONTAINER = 1,   // a viewer container: it declares its own layers, and
                        // the reply carries every reserved member verbatim
};

// WHAT A MSG_NPZ_SCAN REPLY COSTS BESIDES ITS VARIABLE-LENGTH PIECES (#180
// codex review). The aggregate ceiling (docs/features/remote/remote-reader-design.md §10.2) has
// to be spent on the WHOLE message, and the whole message is not the member
// values: it is those values plus every name, every error string, and the fixed
// fields that frame them. Counting only the values let a file whose values
// summed to 238 MiB produce 524 MiB of wire and arrive at the person as
// "oversized reply from the peer" - a sentence about the transport, said about
// a file, that nobody can act on.
//
// The two numbers live HERE, next to the message they describe, so the peer that
// budgets and the selftest that checks the budget read one definition. They are
// the payload's own bytes; the 12-byte rp::Header is outside the u32 length that
// core/remote.cpp bounds, and outside these.
//
//   per member: [u32 nameLen][name][u32 usizeLo][u32 usizeHi][u32 entry]
//               [u32 errLen][err][u32 whole][u32 nBytes][bytes]
//               -> 4 + 8 + 4 + 4 + 4 + 4 = 28 fixed, plus name + err + bytes
static const uint64_t NPZ_SCAN_FACT_FIXED = 28;
//   ahead of them: [u32 keyLen][key][u32 kind][u32 version][u32 nMembers]
//               -> 4 + 16 + 4 + 4 + 4 = 32, the key being the 16 hex characters
//                  readerHash is always formatted to
static const uint64_t NPZ_SCAN_REPLY_FIXED = 32;
// ...and the ceiling those two are spent against. VIEWER_SERVE_NPZ_SCAN_MAX may
// lower it for tests, never raise it. Half of the 512 MiB core/remote.cpp will
// accept, so a reply that fits this cannot become "oversized reply from the
// peer" no matter how the remaining halves are divided between values, names
// and errors.
static const uint64_t NPZ_SCAN_INLINE_MAX = 256ull << 20;

// Parse the test override strictly and clamp it in the only safe direction.
// Accepting a prefix ("4096x"), a sign ("-1" becomes UINT64_MAX in strtoull),
// or an overflowing digit string can silently raise the ceiling beyond the
// transport limit this constant is meant to stay below.
inline uint64_t npzScanCeilingFor(const char* env) {
    if (!env || !*env) return NPZ_SCAN_INLINE_MAX;
    uint64_t v = 0;
    for (const char* p = env; *p; p++) {
        if (*p < '0' || *p > '9') return NPZ_SCAN_INLINE_MAX;
        const uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return NPZ_SCAN_INLINE_MAX;
        v = v * 10 + d;
    }
    if (!v || v >= NPZ_SCAN_INLINE_MAX) return NPZ_SCAN_INLINE_MAX;
    return v;
}

// What a MSG_READER_RUN reply says happened, and it is `adapter::Run`'s own
// four facts plus the two the caller cannot see from here. Split rather than
// folded into one string because the client writes a DIFFERENT sentence for
// each and one of them (2) names a fix that is on the other machine
// (docs/features/remote/remote-reader-design.md §6).
enum ReaderOutcome : uint32_t {
    RO_OK            = 0,   // it ran, and `key` names what it produced
    RO_GATE_CLOSED   = 1,   // this peer was started without --serve-readers
    RO_NO_PYTHON     = 2,   // adapter::findPython found none WITH numpy
    RO_NOT_STARTED   = 3,   // the interpreter did not start at all
    RO_TIMED_OUT     = 4,   // 300 s, the same limit the local run has
    RO_EXITED        = 5,   // it ran and raised: stderr is the traceback, whole
    RO_UNREADABLE    = 6,   // it returned something this viewer cannot read
};

// The three names MSG_READER_RUN will carry, and no others. A client that could
// name the file it writes on the peer would be a client that can write anywhere
// on the peer's disk; the set is closed here so that path never exists.
static const char* const READER_RUN_FILES[3] = {
    "reader.py", "viewer_import.py", "run_adapter.py"
};
// Three texts. A reader is text (§3.3); 4 MB is far above any of them and far
// below the 64 MB one message may be.
static const uint32_t READER_RUN_MAX_BYTES = 4u << 20;

// META / TILE, protocol 12: which optional blocks follow the reading.
//
// Before 12 the recipe was read by "if bytes remain, they were meant"
// (getRecipe). A SECOND optional trailer cannot be read that way - two blocks
// and one absence rule is how a key gets parsed as a geometry - so a v12 client
// talking to a v12 peer writes this word ALWAYS, even when it is 0, and the
// blocks follow it in bit order. Both sides gate on the SERVED version
// (rp::VERSION at the client is what the peer announced in HELLO), so the seam
// VIEWER_SERVE_PROTOCOL opens keeps working: a peer pretending to be 11 is sent
// exactly the v11 bytes.
enum ReqTrailer : uint32_t {
    RQ_RAW_RECIPE = 1,   // a RawWire follows
    // [str key][u32 node] follows: ONE ARRAY INSIDE SOMETHING THE PEER
    // MATERIALISED. The key names the materialisation, the node names the array
    // in it - and the wire does not know, or need to know, what produced
    // either. Protocol 12 issued keys from MSG_READER_RUN only, which is why
    // this was spelled RQ_READER; 13 issues them from MSG_NPZ_SCAN too, and a
    // native .npz has no reader anywhere near it (docs/features/remote/remote-reader-design.md
    // §10.5). THE VALUE IS UNCHANGED - a v12 peer and a v13 client mean the
    // same 2 by it, and the rename costs no byte on any wire.
    RQ_KEYED      = 2,
};

// LIST reply, v3. v2 was: [u32 n] then per entry [str name][u32 dir][u32 szLo]
// [u32 szHi]. v3 is:      [u32 n] then per entry
//   [str name][u32 flags][u32 szLo][u32 szHi][u32 mtimeLo][u32 mtimeHi]
//   flags & LE_META : [u32 dtype][u32 ndim][u32 dims[4]] declaration order,
//                     0-padded  [u32 fortran]           (.npy header peek)
//   flags & LE_GROUP: [u32 frameCount][frameCount * str memberName]
// mtime is unix seconds (64-bit as lo/hi like the size: 2038 is within the
// service life of a lab tool). A group entry's size is the sum over members,
// its mtime the newest member, its META fields those of the first frame.
enum ListEntryFlags : uint32_t {
    LE_DIR   = 1,   // directory
    LE_META  = 2,   // npy header fields follow
    LE_GROUP = 4,   // synthetic entry for a numbered .npy sequence
};

// MEASURE: run analysis where the data lives. A 300-frame statistic crosses the
// wire as a few hundred bytes of results instead of gigabytes of pixels - and
// it runs immediately, while any pixel transfer proceeds in parallel.
enum MeasureOp : uint32_t {
    MOP_ANALYZER        = 0,   // run a named plugin analyzer on ONE frame
    MOP_TEMPORAL_STATS  = 1,   // per-pixel mean/var over N frames (noise vs FPN)
    MOP_FRAME_ROI_STATS = 2,   // per-frame per-ROI mean/var (PTC / linearity)
    // docs/reference/abi-v3.md §10. MOP_ANALYZER matches an analyzer by NAME, which is
    // all there was to match on before #46 gave a descriptor a version: two
    // machines with the same folder of dlls and different builds inside them
    // answered the same question differently and nothing anywhere said so.
    // This op carries the version too, refuses the pair when they differ
    // (quoting both, never re-routing to a local run), reaches STACK analyzers
    // as well as frame ones, and returns the peer's ledger row so the result
    // can name the dll that actually computed it.
    MOP_PLUGIN_ANALYZE  = 3,
    // docs/analysis-layers.md §3.5's last line ("set の集計も同じ線"). A set
    // analyzer's inputs are N stacks with NAMED ROLES, and its outputs are
    // scalars - so what moves to the peer is the FOLD (per-pixel temporal mean
    // and its residual, reduced to per-plane sums) and what stays here is every
    // named estimator built on top of it. See core/setfold.h for the split and
    // why the per-pixel difference two roles need is reduced on the peer rather
    // than recomposed from moments here.
    MOP_SET_FOLD        = 4,
};

// MOP_SET_FOLD only: what the peer must do ACROSS roles once each one is
// folded. JOIN_NONE answers every role on its own, which is all the direct DSNU
// row and every level of the separation fit need. JOIN_DIFF additionally forms
// D = M[role 0] - M[role 1] per pixel and reduces it there - the one thing a
// client cannot compose from per-role scalars without changing the digits.
// A field rather than an inference from the role count: two roles are a
// perfectly ordinary independent pair (a sweep of two levels is refused for a
// different reason entirely), and the request has to SAY that a difference is
// wanted rather than have the peer guess it.
enum SetJoin : uint32_t {
    SJ_NONE = 0,
    SJ_DIFF = 1,
};

// Which mouth of the plugin the request is aimed at (MOP_PLUGIN_ANALYZE only).
// Not a flag on the name: a frame analyzer and a stack analyzer are separate
// registrations with separate signatures on both sides (plugin_host.h), and
// the list to look in is decided by what the caller ASKED for rather than by
// finding a name in one list and hoping it meant that one.
enum MeasureTarget : uint32_t {
    MT_FRAME = 0,   // psAnalyzerV3::analyze on one frame
    MT_STACK = 1,   // psStackAnalyzerV3::analyze_stack over the frame range
};

// Fixed head of the request; the variable part follows as
//   [str path * nPaths][str analyzer][str paramsJson][{u32 x,y,w,h} * nRois]
// nPaths > 1 means one file per frame, in sequence order. analyzer/params are
// empty for the aggregate ops. nRois == 0 means whole frame.
//
// MOP_PLUGIN_ANALYZE appends, AFTER the rois:
//   [str clientVersion]  what the CLIENT's descriptor declares; "" = a V1/V2
//                        descriptor, which declares nothing (never a guess)
//   [u32 target]         MeasureTarget
// After, not before, so that not one byte moves for the three ops that came
// first: an op a v6 peer knows is parsed by a v6 peer identically, and the op
// it does not know it refuses on the op number before it reads this far.
//
// MOP_SET_FOLD appends, in the same place and for the same reason:
//   [str foldForm]   what the CLIENT's fold declares (setfold::foldForm).
//                    "" = declares nothing, which is refused rather than read
//                    as a match - two absences are not an agreement.
//   [u32 join]       SetJoin
//   [u32 nRoles]     1..64
//     per role: [str role][u32 nPaths][u32 frame0][u32 frameCount]
//
// The ROLE BLOCK is the grammar #134 said this op would need and could not
// inherit. The flat `nPaths` list is KEPT and keeps its meaning - one file per
// frame, in sequence order - and the block says how many of it each role owns,
// in declaration order, so the paths of role 0 come first and so on. A
// COMPANION COUNT ARRAY rather than a nested list because it is the change that
// costs the existing parser nothing: `nPaths` is still the number of strings to
// read, still read by the same loop, and a peer that mis-sums the counts finds
// out immediately (sum != nPaths is a refusal, not a silent re-slice).
//
// frame0/frameCount are PER ROLE and not the head's, because two roles are two
// different stacks: a dark of 480 frames and a flat of 8 have no common range,
// and the head's pair describes one file. The head's own frame0/frameCount are
// ignored for this op.
//
// rois must be empty. A set analyzer reads the frame (core/app/setanalysis.inc:
// "EVERY pixel, no decimation"), so a request carrying one is refused rather
// than quietly measuring a different population than the local path would.
//
// MRF_KEYED (protocol 14) appends, right behind the recipe and in bit order:
//   [str key][u32 node]   ONE ARRAY INSIDE SOMETHING THE PEER MATERIALISED -
//                         the same pair, with the same meaning, that META and
//                         TILE carry (rp::RQ_KEYED, §10.5). The subject of the
//                         measurement is that array, and `nPaths` is then 0:
//                         the request names NO path, for the reason the trailer
//                         on META does. A key resolves inside the peer's own
//                         cache and is never a second way to name a file on its
//                         disk, and a measurement that carried an origin path
//                         beside a key would be a request with two subjects.
struct MeasureReqHead {
    uint32_t op;                 // MeasureOp
    uint32_t frame0, frameCount; // range in a frame-axis file; frameCount 0 = all
    uint32_t cfaType, cfaPattern;// psCfaType / psCfaPattern values
    float    black, white;       // display-range hint handed to the analyzer
    uint32_t nPaths, nRois;      // nPaths 0 only with MRF_KEYED
    uint32_t flags;              // MRF_RAW_RECIPE / MRF_KEYED: what follows the rois
};

// Reply (MSG_OK): a serialization of exactly what the plugin sink emitted, so
// the host renders it through the same grid/plot code as a local run.
//   [u32 serverLoc]  0 = CPU, 1 = CUDA (provenance)
//   [u32 framesUsed]
//   [u32 nCols] per col: [u32 nItems]
//       per item: [u32 kind] [str key] (kind 0 ? [f64 value] : [str value])
//   [u32 nSeries] per series: [str name][str xLabel][str yLabel]
//       [u32 col][u32 hasX][u32 n] (hasX ? [f32*n xs]) [f32*n ys]
//
// MOP_PLUGIN_ANALYZE appends a provenance trailer, and it is the point of the
// op as much as the numbers are (docs/reference/abi-v3.md §10/§11): a remote measurement
// used to arrive with no answer at all to "which plugin, which build, which
// file" - the one thing #46 stage 1 gave every LOCAL result.
//   [str name][str version][str file][str path]   the PEER's ledger row
//   [u32 expected]                                N; framesUsed is n
// The path is the peer's absolute path and is quoted as such - the client
// writes NOTHING about its own dll of the same name here, because it did not
// compute this and a citation names the computer.
//
// MOP_SET_FOLD reuses that trailer EXACTLY - same five fields, same parser -
// with a built-in's answers to the same questions: name = "set fold
// (built-in)", version = the PEER's viewer version, file = "" because a
// built-in has no dll and that absence is the statement, path = the peer's own
// executable. #46's 計算者欄 asks for "viewer 版、またはプラグイン name +
// version + ファイル"; this is the first half of that sentence.
//
// Its columns are ONE PER ROLE, in the request's declaration order, and then -
// when join != SJ_NONE - one more for the join. The keys are neutral sums, not
// named quantities: `p<k>.s1` / `p<k>.s2` / `p<k>.cs` / `p<k>.n` per plane
// (`p<k>.s1a` / `s1b` / `dq` / `cs` / `n` on the join column), plus the shading
// probe's `p<k>.shade_pp` / `shade_pct` / `shade_pct_ok`, plus the role's own
// facts: `frames` / `expected` / `w` / `h` / `ch` / `planes` / `cfa` /
// `cfa_pattern` / `nonfinite` / `dropped`, and `role` as text.
//
// A role the peer could not fold (fewer than 2 frames) still gets its column,
// carrying `frames` / `expected` / the shape and no sums. That is deliberate:
// the sentence a person reads for that case is settled in
// core/app/setanalysis.inc, in words about roles and stacks, and a peer that
// turned it into MSG_ERR would leave the client with nothing to write it from.
// The peer answers with FACTS; the refusals stay where their words are settled.

// shared sample-to-float conversion (defined in serve.cpp, linked everywhere)
void toFloatSamples(const uint8_t* src, uint32_t dtype, size_t n, float* out);

// Every message: [magic][type][payload bytes][payload]. Payloads are packed
// little-endian scalars followed by any blob; both ends are ours, and the format
// is versioned by HELLO, so there is no need for anything more elaborate.
struct Header {
    uint32_t magic;
    uint32_t type;
    uint32_t len;
};

// TILE request. The client asks for the region it is about to draw, decimated to
// roughly the pixels it can actually show: `step` is the sample stride, chosen so
// that one returned sample is about one screen pixel (step ~ 1/zoom). A 4000x3000
// frame fitted into a 1000px-tall pane travels as 1334x1000, not as 12 Mpx.
//
// Since protocol 9 the request is [str path][TileReq][u32 read] (NpyRead): the
// frame index, the rect and the step are all coordinates INSIDE a reading, so
// the reading has to travel with them or they mean a different region than the
// caller meant. Appended after the struct, for the reason every append in this
// header is - a request an older peer understands must not move by one byte -
// and gated on the version at the client for the reason the VERSION note gives:
// an older peer does not refuse the trailer, it silently never reads it.
struct TileReq {
    uint32_t frame;             // frame index within the file (0 for a plain image)
    uint32_t x, y, w, h;        // region in source pixels
    uint32_t step;              // 1 = full resolution, n = every nth sample
    uint32_t flags;             // bit0: compress payload (deflate)
};

// The declared geometry of a headerless file (protocol 11). ONE definition,
// included by both binaries, for the reason NpyRead is one definition: "a
// second copy in serve.cpp is a copy that drifts" (#71).
//
// dtype / interp are the INDICES of the client's RAW_DTYPE_NAMES and
// RAW_INTERP_CLI tables, frozen here. APPEND, NEVER RENUMBER: session files
// already carry these numbers (`raw3` lines) and so does this wire now, so a
// renumbering would make one build's u16 the next one's i16 - the same hazard
// the DType note below spells out.
//
// What is NOT here, each with its reason (docs/features/remote/remote-headerless-design.md §3.2):
//   cfaPattern - RGGB vs BGGR changes no sample, only the planes' NAMES. It
//                rides on MeasureReqHead where it already does, and it is not
//                part of §6.2 identity locally either.
//   crop       - a crop re-scopes a frame rather than being one; the local
//                registry already refuses to share a cropped raw source.
//   frames     - a headerless file has no in-file frame axis (§6.1): there is
//                no door to one locally, so inventing one over the link would
//                give the same file two shapes depending on where it was
//                opened.
struct RawWire {
    uint32_t dtype;             // index into RAW_DTYPE_NAMES: 0 u8, 1 u16, 2 f32, 3 f64
    uint32_t interp;            // index into RAW_INTERP_CLI: 0 gray .. 6 quad-bayer
    uint32_t w, h;              // 1..32768, checked by the peer, never clamped
    uint32_t offset;            // bytes to skip before the frame
    uint32_t flags;             // bit0: little-endian. Rest 0 - room to append.
};
static const uint32_t RW_LITTLE_ENDIAN = 1u;
// What those two indices MEAN in bytes and channels. Here rather than only in
// core/app/loader_npy_raw.inc because the peer has to size the same read and
// does not compile that file - the RawWire note's "one definition" applies to
// the meaning of the numbers as much as to their layout. The client keeps its
// own tables (they also carry the NAMES, which are UI), and
// --rawrecipe-selftest asserts the two agree over every index: a wrapper would
// have hidden a disagreement, an assertion catches one.
static inline uint32_t rawDtypeSize(uint32_t dtype) {   // u8, u16, f32, f64
    static const uint32_t SZ[] = { 1, 2, 4, 8 };
    return dtype < 4 ? SZ[dtype] : 0;                   // 0 = not a dtype
}
static inline uint32_t rawInterpCh(uint32_t interp) {   // gray rgb bgr rgba bgra bayer quad
    static const uint32_t CH[] = { 1, 3, 3, 4, 4, 1, 1 };
    return interp < 7 ? CH[interp] : 0;                 // 0 = not an interpretation
}
// The two that put channel 2 where channel 0 was: the local decoder stores the
// swapped order in the document, so a peer that did not swap would give one
// file two colours depending on which end opened it (#148, one step down).
static inline bool rawInterpSwapsRB(uint32_t interp) { return interp == 2 || interp == 4; }
static const uint32_t RAW_MAX_DIM = 32768;
// MeasureReqHead::flags bit0: a RawWire follows the ROI array. A bit rather
// than "read it if bytes remain", because MEASURE already has op-dependent
// blocks after the ROIs - fixing the POSITION and declaring the PRESENCE in the
// head is the cheapest thing for the parsers that already exist. The field was
// declared "reserved, 0", so a v10 or older client writes 0 there.
static const uint32_t MRF_RAW_RECIPE = 1u;
// ...and bit1 (protocol 14): [str key][u32 node] follows, and the subject of
// the measurement is that ONE ARRAY INSIDE A MATERIALISATION rather than any
// file - so nPaths is 0. Same value, same position discipline, same reason as
// bit0: the blocks behind the rois are read in bit order and each one's
// presence is declared in the head, never inferred from bytes remaining.
//
// The spelling is KEYED and not READER. docs/features/remote/remote-reader-design.md §4.2
// called it MRF_READER when a reader's output was the only thing a key could
// name; §10.5 fixed the meaning of the pair to "the peer materialised
// something, and this is one array in it", and a native .npz stack measured
// here has no reader anywhere near it. The VALUE the design assigned is kept -
// no build ever wrote the other name - and RQ_KEYED on META / TILE was renamed
// for this reason first (protocol 13).
static const uint32_t MRF_KEYED = 2u;

// TILE reply header; the pixel blob follows, possibly deflate-compressed.
struct TileRep {
    uint32_t w, h, ch;          // dimensions AFTER decimation
    uint32_t dtype;             // rp::DType
    uint32_t rawBytes;          // uncompressed size of the blob
    uint32_t flags;             // bit0: blob is deflate-compressed
};

// DT_F16 is APPENDED, not inserted: every number before it is on the wire in
// sessions that already exist, and a renumbering would make one peer's u16 the
// next one's i16. It arrived with protocol 10 (the picture formats) for a
// reason that is the whole point of carrying the source dtype at all - an .exr
// stores halves, the local door calls that "f16", and a peer that widened them
// to f32 would print a different word for the same file depending on which end
// opened it. The samples would have been identical and the label would not,
// which is the same "two answers" defect issue #148 is about.
enum DType : uint32_t {
    DT_U8 = 0, DT_I8, DT_U16, DT_I16, DT_U32, DT_I32, DT_F32, DT_F64, DT_F16, DT_COUNT
};
static inline size_t dtypeSize(uint32_t t) {
    switch (t) {
        case DT_U8: case DT_I8:   return 1;
        case DT_U16: case DT_I16: case DT_F16: return 2;
        case DT_U32: case DT_I32: case DT_F32: return 4;
        case DT_F64: return 8;
        default: return 0;
    }
}

// IEEE-754 binary16 <-> binary32, in the header BOTH ends include, because the
// peer packs and the client unpacks and two copies that disagreed would produce
// pixels that still draw a picture.
//
// Every half is exactly a float, so widening is total. The narrowing direction
// TRUNCATES the significand rather than rounding to nearest, and that is exact
// for the only thing that reaches it: a float that came out of a half has 13
// zero bits at the bottom. Nothing else is ever narrowed here - a float32 EXR
// channel is DT_F32 and takes the other path (core/exrread.cpp dtypeOf) - and
// the bit-identical local-vs-peer assertion in --fmtgate-selftest is what holds
// that claim to account.
inline float halfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) << 31;
    const uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) {
            bits = sign;                                   // +-0
        } else {                                           // subnormal: normalise
            uint32_t shift = 0;
            while (!(m & 0x400u)) { m <<= 1; shift++; }
            m &= 0x3ffu;
            bits = sign | ((113u - shift) << 23) | (m << 13);
        }
    } else if (e == 31) {
        bits = sign | 0x7f800000u | (m << 13);             // inf / NaN, payload kept
    } else {
        bits = sign | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
inline uint16_t floatToHalf(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint16_t sign = (uint16_t)((x >> 16) & 0x8000u);
    const uint32_t be = (x >> 23) & 0xffu;                 // biased exponent
    uint32_t m = x & 0x7fffffu;
    if (be == 0xff)                                        // inf / NaN
        return (uint16_t)(sign | 0x7c00u | (m ? ((m >> 13) | 0x200u) : 0u));
    const int32_t e = (int32_t)be - 127;
    if (e > 15) return (uint16_t)(sign | 0x7c00u);         // overflow -> inf
    if (e >= -14) return (uint16_t)(sign | ((uint32_t)(e + 15) << 10) | (m >> 13));
    if (e < -25) return sign;                              // underflow -> +-0
    m |= 0x800000u;                                        // subnormal half
    return (uint16_t)(sign | (m >> (uint32_t)(-e - 1)));
}
const char* dtypeName(uint32_t t);
uint32_t dtypeFromName(const char* s);

// ---- WHAT FLOAT32 COST, measured rather than assumed ------------------------
//
// The viewer holds pixels as float32 (FrameSource::data). float32 carries a
// 24-bit significand, so three of the dtypes the loaders accept do not fit in
// it: u4 and i4 above 2^24 (16777216) land on a DIFFERENT integer, and f8
// loses digits unconditionally - and can lose the whole number, since 1e300
// becomes inf and 1e-300 becomes 0.
//
// docs/features/adapters/input-adapters.md §4.8 already fixes the rule for this situation:
// something that does not fit is converted AND THE CONVERSION IS RECORDED IN
// THE NOTE. It says it about bfloat16. It was never said about u32/i32/f64,
// which is the defect - not the narrowing itself (that is a storage decision)
// but the SILENCE about it. The same paragraph also claims "an integer rides
// onto f32 with its value intact", which is false above 2^24; this is what
// makes it true or, where it cannot be, audible.
//
// So the claim is MEASURED, per array, at the one place the exact value is
// still in hand - the conversion itself:
//
//   * an f8 file whose values all happen to fit (small integers, or numbers
//     that were float32 before someone widened them) gets NO warning, because
//     nothing was lost. A hedge that fires on the dtype would cry wolf on
//     exactly the files that are fine.
//   * a u4 file with one sample at 2^24+1 says "1 of N", and names it.
//
// It lives HERE because four places narrow and they must all say the same
// thing: the local .npy/.npz decoder, the raw decoder, the remote client
// (core/remote.cpp toFloat - the wire carries the source dtype exactly, so the
// loss there is entirely this last step) and the peer's own pre-measure
// conversion (core/serve.cpp toFloatSamples). A second copy of "did this
// survive" is a second answer to it.
struct F32Loss {
    uint64_t inexact = 0;      // samples float32 cannot hold
    uint64_t total   = 0;      // samples examined
    double   from    = 0;      // the sample that moved furthest, as the FILE holds it
    double   to      = 0;      //   ...and as this program holds it
    double   worstRel = -1;    // its relative deviation; -1 = nothing seen yet

    bool any() const { return inexact != 0; }
    // One sample, exact as the file holds it. Call ONLY for dtypes that can
    // lose (u4 / i4 / f8): u1/i1/u2/i2/f4/b1 are exact in float32 by
    // construction and must not pay a branch per pixel for a question whose
    // answer is known.
    void observe(double exact) {
        total++;
        const float held = (float)exact;
        if (exact != exact) return;                  // NaN stays NaN: nothing lost
        if ((double)held == exact) return;
        inexact++;
        const double d = (double)held - exact;
        const double rel = exact != 0 ? (d < 0 ? -d : d) / (exact < 0 ? -exact : exact)
                                      : (d < 0 ? -d : d);
        if (rel > worstRel) { worstRel = rel; from = exact; to = (double)held; }
    }
    // Deliberately no merge(): every census belongs to ONE array, and the two
    // places that were tempting to fold - a preview replaced by its full frame,
    // and a stack folded into a frame average - are both wrong to fold. The
    // preview's samples are a subset of the full frame's, so a sum would count
    // pixels twice; the average's pixels are a computation and not any file's
    // values at all. The preview case replaces wholesale (remote_client.inc);
    // the average is a named gap.
};

// 2^24: the largest integer float32 holds exactly, and therefore the point from
// which an integer in hand can no longer be traced back to the file's. It is
// the boundary for the callers that have only the float left - fmtVal's, which
// is handed one and asked to print it - and the test there is >=, not >: 2^24
// is representable, but 2^24+1 rounds onto it (ties to even), so a float
// holding exactly 2^24 is already ambiguous. 2^24-1 is not.
static const double F32_EXACT_INT_MAX = 16777216.0;   // 2^24

// A double printed so that reading it back gives the same double, and with no
// more digits than that needs. 15/16/17 in order is the portable spelling of
// std::to_chars' shortest round trip (which libc++ did not carry until LLVM 14,
// and this builds on three toolchains). It matters because these strings are
// the EVIDENCE: "the file holds 1.0000000001" is the whole sentence, and
// %.17g would print 1.0000000000999999 and make the viewer look like the liar.
inline std::string exactNum(double v) {
    char b[40];
    for (int p = 15; p <= 17; p++) {
        snprintf(b, sizeof b, "%.*g", p, v);
        if (strtod(b, nullptr) == v) return b;
    }
    return b;
}

// The sentence the Inspector prints. Empty when nothing was lost - a line that
// says "0 samples were harmed" is noise, and this file's whole point is that a
// claim must be earned.
//
// It quotes the RELATIVE deviation and one sample at that deviation, because
// "how far off" is the question and absolute DN cannot answer it across
// magnitudes: 16777217 -> 16777216 is one DN and 6e-8 of the value, while
// 4294967295 -> 4294967296 is also one DN and 2e-10 of it. Relative also ranks
// the one case that is not about digits at all - a finite f64 that becomes inf
// is an infinite relative deviation and therefore always the quoted one.
inline std::string f32LossNote(const F32Loss& L, const std::string& dtype) {
    if (!L.any()) return {};
    char rel[32];
    snprintf(rel, sizeof rel, "%.2g", L.worstRel);
    return dtype + " does not fit in float32: " +
           std::to_string((unsigned long long)L.inexact) + " of " +
           std::to_string((unsigned long long)L.total) +
           " sample(s) are NOT the value the file holds (worst " + rel +
           " of the value: " + exactNum(L.from) + " is held as " + exactNum(L.to) +
           ") - every number shown for those samples is this program's, not the file's";
}

// ---- how a .npy shape is SAID, spelled once for both doors -----------------
//
// This viewer has two ways in - the local decoder (core/app/loader_npz.inc) and
// the peer (core/serve.cpp) - and issue #71 was what happens when they answer
// the same file differently. The RULE is one rule now (the last axis is
// channels when it is 4 or fewer, on both sides). These are the SENTENCES that
// describe it, and they belong in the one header both sides already include for
// the same reason the rule does: a refusal the peer sends is read by a person
// looking at this viewer's window, so it has to be the sentence the local door
// would have used. A second copy in serve.cpp is a copy that drifts, which is
// precisely how the "3|4" spelling outlived the rule it was describing.
//
// ASCII "C<=4" rather than U+2264: this string is printed to consoles whose
// codepage may be cp932 (which cannot encode that character at all) and is
// quoted back verbatim through a pipe by tools/import/run_adapter.py.
// docs/features/adapters/input-adapters.md §4.13.0 draws the same forms with the real character.
static const char* const NPY_NATIVE_FORMS =
    "(H,W) / (H,W,C<=4) / (F,H,W) / (F,H,W,C<=4)";

// "(24, 480, 640)" / "scalar" - the shape a human recognises from the script
// that wrote it, printed from the header and never from what we made of it.
inline std::string npyShapeText(const std::vector<int64_t>& shape) {
    if (shape.empty()) return "scalar";
    std::string s = "(";
    for (size_t i = 0; i < shape.size(); i++) {
        s += std::to_string(shape[i]);
        if (i + 1 < shape.size()) s += ", ";
        else if (shape.size() == 1) s += ",";
    }
    return s + ")";
}

// docs/features/adapters/input-adapters.md §3.2: name the shape that ARRIVED, and name what
// native DOES read. A refusal that says only "cannot open" sends the reader
// nowhere - which is how a 1-D exposure vector stayed a one-pixel-tall image
// for as long as it did.
//
// Two lines, not three. The local door adds a third ("choose a reader to read
// it another way") because openReaderPicker is there to receive it; the peer
// does not, because the picker is a modal window and the peer has no window.
// The escape hatch itself is no longer local-only - §3.3's Inspector line and
// its re-reading reach a remote document since protocol 9 (issue #124) - but a
// refusal is a document that never opened, and there is nothing on the remote
// side for that third line to point AT. Promising a way out that the remote
// half does not have would be worse than the dead end it replaces, so the peer
// stops at the two lines it can keep.
inline std::string npyNotNativeText(const std::vector<int64_t>& shape) {
    return (shape.empty() ? std::string("a scalar")
                          : "shape " + npyShapeText(shape)) +
           " is not a native form\n  native reads " + std::string(NPY_NATIVE_FORMS);
}

// ---- HOW an array is read, spelled once for both doors ---------------------
//
// docs/features/adapters/input-adapters.md §3.3, the successor to --npy-axis: the shape rule
// (§3.1) decides, and where it guessed wrong the user DECLARES a different
// reading per file. The declaration used to exist only in this process, because
// only the local decoder ever knew a file's shape; since protocol 9 it crosses
// the wire (issue #124), which is what puts these four numbers and the axis
// rule underneath them in the header BOTH binaries include.
//
// That placement is #71's lesson rather than tidiness. The "3|4" spelling
// outlived the rule it described precisely because core/serve.cpp kept a COPY
// of it, and a declared reading is the same hazard doubled: the client computes
// the menu from the shape, the peer serves what the menu offered, and if the
// two disagree about which axis a reading names, the viewer shows pixels under
// a label that is a lie. One definition, both sides.
//
// VALUES ARE WRITTEN TO SESSION FILES and now to the wire: append, never
// renumber.
enum NpyRead : uint32_t {
    NR_NATIVE = 0,   // no choice recorded: §3.1 decides from rank + last axis
    NR_STACK  = 1,   // leading axis is frames: (F,H,W) / (F,H,W,C)
    NR_HWC    = 2,   // 3-D as ONE frame, channels last:  (H,W,C), C = 1..4
    NR_CHW    = 3,   // 3-D as ONE frame, planes first:   (C,H,W), C = 1..4
};

// Which axis is F/H/W/C under reading r, as indices into a shape of this rank.
// -1 = "this reading has no such axis". False = r does not apply to this rank,
// which is how impossible readings are kept off the menu (§3.3) and how a
// declaration that cannot mean anything is refused rather than approximated.
inline bool npyReadAxes(size_t rank, int r, int& iF, int& iH, int& iW, int& iC) {
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
// The last axis is CHANNELS when it is 4 OR FEWER; the argument for the ceiling
// and against "3 or 4" is issue #71 and is written out over the local caller in
// core/app/loader_npz.inc. Settled; this is only where it now LIVES, so that
// the peer reads it out of the same line the client does instead of out of a
// copy that once drifted for months.
inline int npyNativeRead(const std::vector<int64_t>& shape) {
    if (shape.size() == 3) return shape[2] <= 4 ? NR_HWC : NR_STACK;
    if (shape.size() == 4) return NR_STACK;
    return NR_NATIVE;
}

// A DECLARED reading this shape cannot carry. Different from §3.2's refusal:
// that one is about a file nothing can open, this one is about an instruction
// that does not fit a file which opens perfectly well. Reached from a session
// restored against a rewritten file, and from a peer answering a client whose
// menu was computed on a different shape than the one on its disk.
inline std::string npyNotThatWayText(const std::vector<int64_t>& shape) {
    return npyShapeText(shape) + " cannot be read that way";
}

// (F,H,W,C) IS a native form; C > 4 is this viewer's own ceiling, so it is said
// as a ceiling and not as "not a native form" - a different refusal deserves a
// different sentence. Both doors say it, and since protocol 9 the peer says it
// about a DECLARED reading too ("read (8,8,5) as (C,H,W)" is 8 channels), so
// the one spelling lives here rather than being typed out on each side.
inline std::string npyChannelCeilingText(const std::vector<int64_t>& shape, int64_t ch) {
    return npyShapeText(shape) + " would be " + std::to_string(ch) +
           " channels: this viewer shows up to 4";
}

// The peer predates the declared reading (issue #124). Written BEFORE anything
// is sent, from the number the peer announced in HELLO, for the reason the
// VERSION note above gives at length: an older peer does not refuse this
// request, it answers it with the wrong pixels. So the client has to be the one
// that refuses, and it has to say which mismatch it is - "your peer is old",
// not "that shape cannot be read that way", which is what the user would
// otherwise conclude from an escape hatch that appears to do nothing.
inline std::string npyReReadTooOldText(int peerVersion) {
    return "the remote peer is too old to read a .npy another way: it speaks "
           "protocol " + std::to_string(peerVersion) + ", a declared reading needs 9"
           " (update viewer-serve). Nothing was re-read - "
           "the peer would have returned the reading it chose itself, under the "
           "name of the one you asked for.";
}

// The peer predates the picture formats (issue #148). The same discipline as
// the sentence above and for the sharper version of its reason: a v9 peer does
// not merely refuse a .png, it refuses it with "not a .npy file" - a sentence
// about the FILE, for a limit that belongs to the peer's build. So the client
// refuses first, from the number the peer announced in HELLO, and says which of
// the two it is and what fixes it.
inline std::string pictureTooOldText(int peerVersion, const std::string& name) {
    return name + ": the remote peer serves .npy only - it speaks protocol " +
           std::to_string(peerVersion) + ", and the picture formats need 10 "
           "(update viewer-serve). Nothing was opened: this file IS readable "
           "here, so browsing it locally or copying it here both work today.";
}

// The peer predates the carried recipe (verify-matrix G1). Same discipline as
// the two sentences above: written BEFORE anything is sent, from the number the
// peer announced in HELLO, because a v10 peer does not refuse this request
// usefully - it answers "not a .npy file", which blames the file for a limit
// that belongs to the peer's build, and silently ignores the very trailer that
// would have made the request answerable.
//
// The way out is the LINK's limit (the rule G9 / PR #176 settled) plus the door
// that does work - and here that door can be NAMED, because a headerless file
// is one this build definitely opens locally.
inline std::string rawTooOldText(int peerVersion, const std::string& name) {
    return name + ": this file states no shape of its own, so the read has to "
           "carry one - and the remote peer speaks protocol " +
           std::to_string(peerVersion) + ", which has no field for it (a recipe "
           "needs 11; update viewer-serve). Nothing was opened: copy the file "
           "here and open it with File > Open, which does carry the recipe.";
}

// The peer predates readers running on it (issue #180, docs/
// docs/features/remote/remote-reader-design.md §4.1). Written BEFORE anything is sent, from the
// number the peer announced in HELLO, and for the reason the three sentences
// above are - but here the failure an unrefused send produces is the WORST of
// the four. A v11 peer does not refuse MSG_READER_RUN usefully ("unknown
// request", indistinguishable from a typo), and worse, it does not refuse the
// META / TILE trailer at all: it never reads it, opens the origin natively, and
// answers with real pixels that are not the reader's. #124's "a silent wrong
// answer wearing the right label" is what this sentence exists to prevent.
//
// The way out is named and it is the one that works today: local:// already
// falls through to the local door (openRemote), so what is actually lost is
// ssh - and for ssh the honest answer is to bring the file here, because there
// is no send-a-file op in either direction and inventing one is a change of
// principle rather than a feature (§7).
inline std::string readerTooOldText(int peerVersion, const std::string& name) {
    return name + ": a reader cannot run on this peer - it speaks protocol " +
           std::to_string(peerVersion) + ", and readers over the link need 12 "
           "(update viewer-serve). Nothing ran: the file stays where it is; "
           "copy it here to use a reader today.";
}

// The peer predates a container crossing the link (issue #217, docs/
// docs/features/remote/remote-reader-design.md §10.4). Same discipline as the four sentences above,
// and the failure it prevents is the one MSG_NPZ_SCAN's own absence would
// produce: a v12 peer answers the verb with "unknown request" - the sentence it
// also gives for a typo - and answers a META that addresses the whole .npz as
// one array with "not a .npy file", which blames the file for a limit that
// belongs to the peer's build.
//
// The way out is named and it is the one that works today. The file IS readable
// here, and for local:// the local door already opens it (openRemote falls
// through), so what is actually lost is ssh - where the honest answer is to
// bring the file over, because there is no send-a-file op in either direction.
inline std::string npzTooOldText(int peerVersion, const std::string& name) {
    return name + ": the remote peer cannot list what is inside a container - it "
           "speaks protocol " + std::to_string(peerVersion) + ", and a .npz needs "
           "13 (update viewer-serve). Nothing was opened: this file IS readable "
           "here, so browsing it locally or copying it here both work today.";
}

// The peer predates MEASURE reaching one array inside a materialisation (issue
// #180 stage 5). The sixth sentence of this family, and the one whose unrefused
// send would be worst of all: a v13 peer reads the head it knows, never reads
// the key behind the rois, and - if the request carried an origin path - would
// answer with a real statistic over the WHOLE .npz or over the file a reader
// was pointed at. A number under the right label, computed from bytes nobody
// opened. So the client sends no path with a key at all (an older peer then
// refuses the head instead of measuring the wrong thing), and refuses HERE
// first, from the number the peer announced in HELLO.
//
// One sentence for both families of key, unlike the two META / TILE gates,
// because here there is only one fix: the reader's 12 and the container's 13
// send a person to different places, and "your peer cannot measure inside a
// materialisation" sends them to the same place whichever issued the key.
//
// The way out is the one that is true: the pixels are already reachable - this
// document opens and draws - so the measurement can be made HERE once the
// frames are local, which is exactly what the panel falls back to.
//
// The only sentence of the six that names NOTHING. The other five are about a
// file and print its name; the subject here is one array inside a
// materialisation, whose only wire name is a hex token issued by the peer -
// and a person reads this under the heading of the stack it is about, which
// already says which one it is. A key in a sentence would be noise wearing the
// shape of an identifier.
inline std::string measureKeyedTooOldText(int peerVersion) {
    return "the remote peer cannot measure one array inside what it materialised"
           " - it speaks protocol " + std::to_string(peerVersion) + ", and a keyed "
           "measurement needs 14 (update viewer-serve). Nothing was measured on the "
           "peer: the frames arrive here in the background, and the panel computes "
           "the same quantity locally once they have.";
}

// A key that names nothing on the peer any more - or never did. ONE sentence,
// and deliberately issuer-neutral: a key is opaque by construction (§4.2), so
// when the cache holds nothing under it there is no way to know whether a
// reader made it or a container listing did. The peer used to answer "run the
// reader again", which is a correct instruction exactly half the time and sends
// the other half to re-run something that was never involved.
//
// It names the two ways back because both are true of the thing that IS gone:
// the materialisation. Which of them applies is knowable at the CLIENT (it is
// looking at the document) and not here, so the sentence offers both rather
// than guessing at one.
inline std::string keyGoneText() {
    return "this peer has nothing under that key any more: whatever it named has "
           "left its cache. Open the file again - a container is listed again, a "
           "reader is run again.";
}

// The gate the peer's own launcher opened, or did not (§2.1). A DIFFERENT
// sentence from the one above, because the two are fixed in different places by
// different people: the version is fixed by updating the peer, and this one by
// whoever starts viewer-serve deciding that code sent from a client may run on
// their machine. Three states - too old / closed / open - and each reads as
// itself.
inline std::string readerGateClosedText() {
    return "this peer was started without --serve-readers: whoever starts "
           "viewer-serve decides whether readers sent by a client may run here";
}

// rp::DType -> the numpy descr letter pair, which is what a .npy header says
// and therefore what the client writes when it rebuilds one around samples that
// arrived through TILE. dtypeName() spells the same types for a PERSON ("u16"),
// and the two must not be confused: one is a label, this is a format.
inline const char* dtypeNpy(uint32_t t) {
    switch (t) {
        case DT_U8:  return "u1";
        case DT_I8:  return "i1";
        case DT_U16: return "u2";
        case DT_I16: return "i2";
        case DT_U32: return "u4";
        case DT_I32: return "i4";
        case DT_F32: return "f4";
        case DT_F64: return "f8";
        case DT_F16: return "f2";
        default:     return "";
    }
}

// `viewer --serve`: answer requests on stdin/stdout until the peer closes.
int runServeMode();

// --serve-readers, off unless the launcher said so (§2.1). Set from the argv of
// whichever binary is being the peer (core/serve_main.cpp, core/main.cpp), so
// the consent belongs to the process that was started and not to a file, an
// environment variable or a client's request.
void setServeReaders(bool on);
bool serveReadersOpen();

// Natural order over the WHOLE name: every digit run compares as a number
// ("img2_gain10" < "img10_gain2"), case-insensitive elsewhere. Lives here
// because BOTH ends must agree on frame order - the client sorts what it
// opens, the server sorts what a scan folds into an unnumbered group.
inline bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            size_t i0 = i, j0 = j;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') i++;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') j++;
            size_t ia = i0, jb = j0;                 // strip leading zeros
            while (ia < i - 1 && a[ia] == '0') ia++;
            while (jb < j - 1 && b[jb] == '0') jb++;
            size_t la = i - ia, lb = j - jb;
            if (la != lb) return la < lb;
            int c = a.compare(ia, la, b, jb, lb);
            if (c != 0) return c < 0;
        } else {
            int la = ca >= 'A' && ca <= 'Z' ? ca + 32 : ca;
            int lb2 = cb >= 'A' && cb <= 'Z' ? cb + 32 : cb;
            if (la != lb2) return la < lb2;
            i++; j++;
        }
    }
    return a.size() - i < b.size() - j;
}

// The DISPLAYED form of a sequence pattern: the frame-axis '?' run replaced by
// the range it actually covers.
//
//   "????.npy"      over 0000.npy .. 0003.npy    -> "0000..0003.npy"
//   "frame_???.npy" over frame_000 .. frame_023  -> "frame_000..023.npy"
//   "f_?.npy"       over f_9, f_10, f_11         -> "f_9..11.npy"
//
// (with U+2025 TWO DOT LEADER, not two periods, where this comment writes "..")
//
// A folder of 0000.npy .. 0003.npy groups as "????.npy": correct by the rule -
// every digit varies - and it says nothing at all. The extent says what the
// stack IS, and it is exactly as cheap: the member names are already in hand
// wherever a pattern is built.
//
// Lives HERE, inline, because both ends produce patterns and both ends must
// agree character for character: the peer names the LIST / SCAN group row, the
// client names the stack it opens from a local folder, and a capture opened
// both ways has to read the same in the Files panel and in the session file.
// Only the frame-axis run is touched; every other digit stays literal, so
// "gain10_???.npy" becomes "gain10_000..007.npy" and never loses the gain.
inline std::string patternWithExtent(const std::string& pattern,
                                     const std::vector<std::string>& members) {
    size_t qs = pattern.find('?');
    if (qs == std::string::npos || members.size() < 2) return pattern;
    size_t qe = qs;
    while (qe < pattern.size() && pattern[qe] == '?') qe++;
    // more than one '?' run means a degenerate grouping the caller could not
    // analyse; rewriting one of the two runs would claim a frame axis it never
    // decided on, so the pattern is left exactly as it came.
    if (pattern.find('?', qe) != std::string::npos) return pattern;
    auto isDig = [](char c) { return c >= '0' && c <= '9'; };
    // which digit run of a member name the '?' run stands for
    size_t runIdx = 0;
    for (size_t i = 0; i < qs; i++)
        if (isDig(pattern[i]) && (i == 0 || !isDig(pattern[i - 1]))) runIdx++;
    std::vector<std::string> vals;
    vals.reserve(members.size());
    for (const std::string& m : members) {
        size_t run = 0, i = 0;
        bool found = false;
        while (i < m.size()) {
            if (!isDig(m[i])) { i++; continue; }
            size_t j = i;
            while (j < m.size() && isDig(m[j])) j++;
            if (run++ == runIdx) { vals.push_back(m.substr(i, j - i)); found = true; break; }
            i = j;
        }
        if (!found) return pattern;          // member structure differs: give up
    }
    // by VALUE, not by string: frame_9 and frame_10 are not lexicographic, and
    // the zero padding of the winning member is kept as it is on disk
    size_t loI = 0, hiI = 0;
    unsigned long long loV = ~0ull, hiV = 0;
    for (size_t k = 0; k < vals.size(); k++) {
        unsigned long long n = 0;
        for (char c : vals[k]) {
            if (n > 1000000000000000ull) break;      // absurd run: stop accumulating
            n = n * 10 + (unsigned long long)(c - '0');
        }
        if (n < loV) { loV = n; loI = k; }
        if (n > hiV) { hiV = n; hiI = k; }
    }
    if (vals[loI] == vals[hiI]) return pattern;
    return pattern.substr(0, qs) + vals[loI] + "\xE2\x80\xA5" + vals[hiI] +
           pattern.substr(qe);
}

// META request: [str path], and since protocol 9 [u32 read] (NpyRead).
//
// The reading is on the REQUEST and not merely on TILE because META's whole job
// is to say how big the picture is before a pixel moves, and a reading changes
// exactly that: (48,40,1) is one 40x48 frame read natively and 48 frames of
// 1x40 read as a stack. Asking META natively and re-deriving the geometry here
// would put the layout rule on both sides of the link again with nothing
// checking that they agree - which is the fault #71 spent a release on. The
// peer is asked what it will SERVE, and then serves it.
//
// META reply: what the client needs to lay out the image before any pixel moves.
// Since protocol 9 the fixed struct is followed by
//   [u32 ndim][u32 dims[4]]   declaration order, 0-padded
// when flags & MR_SHAPE - the shape the FILE declared, before any
// interpretation, the same fields and the same spelling LIST already uses.
// Appended AFTER the struct rather than added to it so that a v8 client reading
// a v9 peer's reply parses byte for byte what it always parsed and simply stops
// early.
//
// Without it the client knows w/h/ch/frames and nothing about the array they
// came from, so it can neither print §3.3's "read as" line nor compute which
// OTHER readings the shape permits - the whole of issue #124. ndim is 2, 3 or 4
// (the peer accepts no other rank), so four dims hold it exactly; a shape that
// does not fit is sent as no shape at all rather than as a clamped one, the
// rule PR #121 settled for the listing.
enum MetaFlags : uint32_t {
    MR_SHAPE = 1,   // the declared-shape trailer follows
    // The DECLARED reading did not fit this rank, so the peer used native. The
    // local decoder's answer to the same case is a fallback plus the note "re-
    // read choice does not fit (...); read natively" (core/app/loader_npz.inc),
    // and the two doors have to answer one file one way - so the peer does the
    // same thing and says which it did, rather than refusing where the other
    // door opens. The client turns this bit into that note, verbatim, and keeps
    // the declaration on the source exactly as the local decoder does.
    MR_READ_FELL_BACK = 2,
};
struct MetaRep {
    uint32_t w, h, ch;
    uint32_t dtype;
    uint32_t frames;            // 1 for a plain image, N for a frame axis
    uint32_t flags;             // MetaFlags
};

}  // namespace rp
