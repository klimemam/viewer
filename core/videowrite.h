// core/videowrite.h — getting a STACK out as a lossless video (issue #253).
// 担当: ImageView/export
//
// The stance this file keeps is docs/features/media/video-support.md §4's, and it is
// unchanged: this build links no libavcodec. What is new is the DIRECTION.
// Reading video is refused because a decoded 8-bit frame is display-referred
// and not DN (core/y4mread.cpp videoRefusal says so with the numbers); WRITING
// is the opposite case, because what leaves here is display-referred BY
// CONSTRUCTION - it is renderDocRGBA's output, the same mapping the screen and
// the PNG export use. Nothing is lost that was not already spent when the
// range, the gamma and the colormap were chosen.
//
// Two sinks, one interface, chosen by the extension the user typed:
//
//   .avi   AviWriter, below. Uncompressed RGB24 in AVI 1.0 (RIFF/hdrl/movi/
//          idx1). ~200 lines, no dependency, plays in VLC and in Windows'
//          own players. Its limit is AVI 1.0's 32-bit offsets - 4 GB - and
//          that limit is REFUSED BEFORE the first frame is written rather
//          than discovered at frame 900 (aviSizeRefusal).
//   .mkv   ffmpeg on PATH, fed raw RGB24 on its stdin, encoding FFV1 level 3.
//          Lossless compression: the same pixels, a fraction of the bytes,
//          and no 4 GB limit. Absent ffmpeg the .mkv request is refused with
//          the way out attached (ffmpegMissingRefusal) - never silently
//          downgraded to AVI, because the file name is a statement about the
//          format and the user typed it.
//
// Nothing here knows about App, ImageDoc or ImGui: it takes bytes and a path.
// That is what lets the selftest drive it and what keeps the 4 GB arithmetic
// and the process spawning out of the UI file.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace videowrite {

// One frame at a time, top-down RGB24 (w*h*3 bytes, R first). Whatever a
// container wants instead - bottom-up, BGR, row padding - is the sink's job,
// so that every caller hands over the same bytes renderDocRGBA produced.
class VideoSink {
public:
    virtual ~VideoSink() = default;
    // false + `why` on the first failure; a sink that has failed stays failed.
    virtual bool writeFrame(const uint8_t* rgb, std::string& why) = 0;
    // Complete the file (patch headers, close the pipe, reap the child).
    virtual bool finish(std::string& why) = 0;
    // Stop and leave NOTHING behind: close, then delete the partial file. A
    // half-written video that plays for two of its twelve frames is worse than
    // no file, because only the file survives to be looked at later.
    virtual void abort() = 0;
    virtual int framesWritten() const = 0;
};

// ---- the two sinks ---------------------------------------------------------
// Both return null on failure with `why` set. `fps` must be positive finite.
std::unique_ptr<VideoSink> openAvi(const std::string& utf8Path, int w, int h,
                                   double fps, std::string& why);
std::unique_ptr<VideoSink> openFfv1(const std::string& utf8Path, int w, int h,
                                    double fps, std::string& why);

// ---- what can be answered BEFORE anything is written -----------------------
// The ffmpeg executable, or "" when PATH holds none. Not cached: a selftest
// changes PATH to assert the absent branch, and a user may install ffmpeg
// while the viewer is open.
std::string ffmpegPath();

// "" when `n` frames of w*h fit AVI 1.0; the refusal otherwise. Named,
// reasoned, and with the way out attached - the shape every refusal in this
// tree has (docs/features/adapters/input-adapters.md §3.2).
std::string aviSizeRefusal(int w, int h, int n);

// The refusal for a .mkv asked for on a machine with no ffmpeg.
std::string ffmpegMissingRefusal();

// The bytes ONE uncompressed AVI frame occupies, row padding included. Public
// because the size refusal, the progress estimate and the selftest's chunk
// assertion must all be the same number.
uint32_t aviFrameBytes(int w, int h);

}  // namespace videowrite
