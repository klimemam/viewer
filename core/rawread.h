#pragma once
// Vendor RAW, for core/imagefile.h - the one row of that table, and the one
// function it points at.
//
// This header exists so that core/imagefile.cpp stays what its own comment says
// it is: a table of formats and the wrapper that dispatches on bytes, not a
// place where a decoder's internals live. Nothing but core/imagefile.cpp
// includes this, and nothing anywhere else in this program includes a LibRaw
// header - which is what keeps "which library reads a .NEF" a build-time detail.
//
// WHAT IS READ, AND WHY IT IS THE MOSAIC AND NOT A PICTURE
//
// `LibRaw::unpack()` hands over `rawdata.raw_image`: the CFA mosaic exactly as
// the sensor counted it, with the pattern, the black level, the white level and
// the bit depth the FILE declares. That - and only that - is what this reader
// takes. `dcraw_process()` and `dcraw_make_mem_image()` are never called and
// are not even LINKED (see CMakeLists.txt): they demosaic, white-balance,
// apply a colour matrix and tone map, and every one of those turns a
// measurement into a picture of a measurement.
//
// So a RAW opens here as a one-channel mosaic in [DN], which is the same thing
// `--cfa bayer` has always made of a mono .npy, with one difference that is the
// whole point: the pattern was READ (core/imagefile.h rule 3) instead of
// declared by the user, so the per-plane statistics cannot be wrong about which
// plane is which.
//
// THE BLACK LEVEL IS SHOWN, NEVER SUBTRACTED. #52's survey - docs/media-formats.md
// on the media-format-strategy branch, which is where that document lives - §4.7
// measured a Nikon NEF that DECLARES black level 0 while its darkest samples sit
// at 80: subtracting a declared black would have been wrong there and invisible
// everywhere. The declared numbers travel in `note`; the pixels are the file's
// own counts. A number the file did not state is not invented.
//
// WHY A LIBRARY HERE AND OUR OWN READER FOR TIFF. Same rule as OpenEXR, from
// the other end: read the format correctly or refuse it. TIFF shrinks when you
// refuse what this program should not read anyway (core/tiffread.h). Vendor RAW
// does not shrink at all - it is thirty vendors' compression schemes, and Canon
// CRX alone is an inverse wavelet transform. There is no small honest reader
// here, so the choice is the library or nothing.
//
// The cost of that is real and is not hidden: docs/input-adapters.md §3.6.5
// carries the measured build and binary cost, and THIRD-PARTY-NOTICES.md the
// licence - LibRaw is dual LGPL-2.1 / CDDL-1.0, and this project takes the
// CDDL, because a single statically-linked exe published automatically by CI
// cannot meet LGPL's relinking obligation.
//
// There is deliberately NO build option to leave this out. `.exr` has one for a
// historical reason and it is being retired; adding a second format that can
// silently be missing from a binary would multiply the number of viewers that
// have to be reasoned about, and the `absent` state of the table exists for
// formats nobody has written a reader for, not as a switch.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imagefile.h"

namespace imagefile {

// What is linked for vendor RAW in this build, for the `library` column of the
// table and for the refusals that name it.
extern const char* const RAW_LIBRARY;

// Do these bytes look like a vendor RAW, rather than like one of the other
// formats in the table? It lives HERE and not beside the other sniffers in
// core/imagefile.cpp because it is the only one that is not four magic bytes,
// and the reason it is not is a fact about this format:
//
//   MOST VENDOR RAW CONTAINERS ARE TIFF FILES. A .NEF, a .ARW, a .PEF and a
//   .DNG all begin with the same "II*\0" a scanner's TIFF begins with, so a
//   sniff that stopped at the magic would send every one of them to
//   core/tiffread.cpp - which would refuse them, correctly but under the wrong
//   format's name, and the user would be told about TIFF when they opened a
//   photograph off their camera.
//
// So this reads a little further into the file, and what it reads is still the
// FILE's own word about itself - never the extension (core/imagefile.cpp's
// dispatch rule). See rawread.cpp for exactly which words those are.
bool rawSniff(const uint8_t* p, size_t n);

// Decode. Matches Backend::decode: one call, one picture, and a reason when
// there is none.
//
// ONE picture, always. A RAW file may hold several shots (Olympus ORI, Sony
// ARQ, a dual-pixel CR3); LibRaw selects between them by reopening the file
// with a shot index, and picking one silently would answer a question the file
// asked. So the first is read and the note says how many there were.
bool rawDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err);

}  // namespace imagefile
