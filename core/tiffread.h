#pragma once
// The TIFF backend behind core/imagefile.h - the one row of that table, and the
// one function it points at.
//
// This header exists so that core/imagefile.cpp stays what its own comment says
// it is: a table of formats and the wrapper that dispatches on bytes, not a
// place where a decoder's internals live. Nothing but core/imagefile.cpp
// includes this, exactly as nothing but core/imagefile.cpp includes stb_image.h.
//
// WHY THE READER IS OURS AND NOT A LIBRARY. Every other decoder in this project
// is somebody else's (stb_image, miniz) and that is the right default. TIFF is
// where it stops paying:
//
//   * libtiff cannot be vendored (it is a configured, multi-file library), and
//     fetching it would break offline clean-clone builds - the exact property
//     third_party/stb/ exists to protect (CMakeLists.txt, the stb block). An
//     existing checkout has the current dependencies cached; a NEW fetched one
//     is the only kind that makes a fresh clone need the network.
//   * a TIFF reader for THIS program is small, because most of TIFF is not
//     something this program should read anyway. What is left after refusing
//     tiles, CCITT, JPEG-in-TIFF, palettes and CFA is: walk the IFD chain,
//     un-RLE or inflate some strips, undo a difference, widen to float.
//   * the refusals ARE the deliverable here (docs/input-adapters.md §3.6). A
//     library hands back an error code; this file hands back the sentence a
//     user reads, naming the feature and the tag it came from.
//
// Consequently there is no THIRD-PARTY-NOTICES entry for it and no licence to
// keep compatible: it is this project's own code under the repository's licence.
// The only outside code it uses is miniz, which is already linked for .npz.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imagefile.h"

namespace imagefile {

// What is linked for TIFF in this build, for the `library` column of the table
// and for the refusals that name it. Version 1: the reader described above.
extern const char* const TIFF_LIBRARY;

// Decode every page of a TIFF. Matches Backend::decode: one call, one or many
// pictures, and a reason when there are none.
//
// One picture per PAGE (IFD), in file order, minus any page that DECLARES
// itself a reduced-resolution copy (NewSubfileType bit 0) - a thumbnail is not
// a frame of a measurement, and the note of the page that is kept says one was
// dropped. What the caller does with several is the caller's business: a
// multi-page TIFF is a stack, and "stack" is App::SeqInfo's word.
bool tiffDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err);

}  // namespace imagefile
