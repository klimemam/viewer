#pragma once
// The OpenEXR backend behind core/imagefile.h - the one row of that table, and
// the one function it points at.
//
// This header exists so that core/imagefile.cpp stays what its own comment says
// it is: a table of formats and the wrapper that dispatches on bytes, not a
// place where a decoder's internals live. Nothing but core/imagefile.cpp
// includes this, exactly as nothing but core/imagefile.cpp includes stb_image.h
// or tiffread.h - and nothing anywhere else in this program includes an OpenEXR
// header, which is what keeps "which library reads .exr" a build-time detail.
//
// WHY A LIBRARY HERE AND OUR OWN READER FOR TIFF. The two decisions look
// opposite and come from one rule: read the format correctly or refuse it.
//
//   * TIFF shrinks when you refuse what this program should not read anyway.
//     What is left after tiles, CCITT, JPEG-in-TIFF, palettes and CFA is small
//     enough to own (core/tiffread.h).
//   * EXR does not shrink. Refusing tiled and deep leaves scanline half/float,
//     and scanline half/float is still PIZ (a wavelet plus Huffman) and DWA (a
//     DCT), because that is what renderers actually write. A reader that did
//     NONE and ZIP would refuse most real files - so the honest small reader
//     does not exist here, and the choice is the official library or nothing.
//
// The cost of that is real and is not hidden: OpenEXR + Imath is the heaviest
// dependency in this tree. CMakeLists.txt carries the fetch and the offline
// escape, docs/media-support.md §1 carries the measured build and binary cost,
// and THIRD-PARTY-NOTICES.md carries the licence. -DVIEWER_WITH_EXR=OFF removes
// all of it and leaves .exr LISTED with no decoder, which is a first-class
// state of the table above rather than a hole.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imagefile.h"

namespace imagefile {

// What is linked for .exr in THIS build, for the `library` column and for the
// refusals that name it. "" when there is none.
extern const char* const EXR_LIBRARY;

// Why there is no decoder, or nullptr when there is one. This is the `absent`
// column: with -DVIEWER_WITH_EXR=OFF the format stays listed, stays dispatched
// to, and refuses with a sentence naming the build option - which is a far more
// useful answer than "unknown file, try the raw dialog".
extern const char* const EXR_ABSENT;

// The `decode` column: &exrDecode, or nullptr when EXR_ABSENT is set. A pointer
// rather than a function so that core/imagefile.cpp needs no #ifdef to build
// its table - the table stays a table.
//
// One picture per LAYER, in the file's channel order, each carrying its layer
// name in Image::member. A layer is a NAMED part, so several of them are
// several documents and never the frames of a stack (core/imagefile.h).
extern bool (*const EXR_DECODE)(const uint8_t* p, size_t n,
                                std::vector<Image>& out, std::string& err);

}  // namespace imagefile
