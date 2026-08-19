#pragma once
// The y4m (YUV4MPEG2) backend behind core/imagefile.h - the one row of that
// table, and the one function it points at.
//
// WHY A VIDEO CONTAINER IS IN THE PICTURE-FORMAT TABLE. Because after the
// question docs/features/media/video-support.md asks, it is one: y4m is a one-line text header
// followed by raw planes, with no compression and no inter-frame prediction, so
// a file is N pictures of one shape in order - which is byte for byte the same
// thing a multi-page TIFF is, and the seam already has a word for it (unnamed
// pictures are the frames of a stack, core/imagefile.h). Everything that makes
// "video" a different kind of problem - a codec, a GOP, a seek that can land on
// the wrong picture, a presentation clock - is absent from this format. So it
// needs no second door, no second vocabulary and no lazy-decode machinery; it
// needs a decoder that returns a vector<Image>.
//
// WHY ONLY THIS ONE, AND NOT libavcodec. docs/features/media/video-support.md measured it
// rather than argued it: an 8-bit lossy round trip turns a known sigma_t of 40
// DN16 into 0.00 - not degraded, GONE - and noise that IS representable at 8
// bits comes back 11% low with a GOP-periodic bias, I-frames retaining 3.9%
// more than P-frames. A per-frame noise plot over such a stack measures the
// encoder. So the formats libavcodec would open are, with few exceptions,
// exactly the formats this tool must not measure; the honest subset arrives
// with no dependency at all. The conditions under which that flips are §6 of
// that document.
//
// WHY THE LUMA PLANE ONLY. A subsampled chroma plane is one sample per 2x2, and
// making three full-resolution channels out of it means interpolating - and an
// interpolated value is not a measurement. Luma is read, chroma is said to be
// present and not read, and the note carries both facts.
//
// This reader is this project's own code: no third party, nothing for
// THIRD-PARTY-NOTICES.md, the same as core/tiffread.cpp.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imagefile.h"

namespace imagefile {

// What is linked for y4m in this build, for the `library` column and for the
// refusals that name it.
extern const char* const Y4M_LIBRARY;

// Decode every frame of a y4m. Matches Backend::decode: one call, one or many
// pictures, and a reason when there are none.
//
// One picture per FRAME, in presentation order, luma only, with Image::member
// left EMPTY - which is what tells the caller these are the frames of a stack
// and not the named parts of a container.
//
// N is arithmetic from the byte count, because the format declares no frame
// count anywhere. A file that ends inside a frame therefore yields the whole
// frames it has, and the caller is told n of N (docs/terminology.md forbids
// reporting a partial stack without the ratio).
bool y4mDecode(const uint8_t* p, size_t n, std::vector<Image>& out, std::string& err);

// (videoRefusal is declared in imagefile.h, not here: it is the one thing in
// this file the REST of the program calls, and the rest of the program is only
// ever allowed to include the seam header.)

}  // namespace imagefile
