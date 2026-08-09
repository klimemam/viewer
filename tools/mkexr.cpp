// The .exr fixtures for --media-selftest, written by a TOOL and not by the
// viewer.
//
// WHY THIS IS ITS OWN BINARY. Every other media fixture in this repository is
// written by tools/gen_testdata.py from its own samples, so that what a file
// DECLARES and what it HOLDS are both known exactly on this side of the round
// trip. OpenEXR cannot be written that way at a sane price: NONE and ZIP are a
// header plus zlib, but PIZ is a wavelet and a Huffman table and DWAB is a DCT,
// and those two are the ONLY reason docs/media-support.md §1 rejected a
// hand-rolled reader - real .exr files off a renderer are PIZ or DWA, and a
// build that reads "NONE and ZIP" would produce a stream of files that will not
// open. Coverage that stops exactly where the argument starts is worth little.
//
// So the writer half of the library is used, and the cost of that is paid HERE
// rather than in viewer.exe. The original branch (issue #53, judgment item 2)
// built these fixtures inside the product binary, which put OpenEXR's writer,
// its tiled writer and its deep writer into the shipped executable so that a
// test could run. A separate tool is the same fixtures at zero shipped bytes,
// and it is the precedent this build already uses for generated assets
// (`mkicon`, which draws the application icon at build time).
//
// WHAT IT DOES NOT PROVE, said out loud: these files come from the same project
// as the reader, so this is a test of the viewer's INTERPRETATION - channels,
// layers, dtype, ranges, refusals - and not of byte-level parsing of a third
// party's file. The values are chosen so that interpretation is what fails.
//
//     mkexr <dir>        writes the fixtures into <dir> (created if needed)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <ImfChannelList.h>
#include <ImfDeepFrameBuffer.h>
#include <ImfDeepScanLineOutputFile.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfTiledOutputFile.h>
#include <ImathBox.h>
#include <half.h>

namespace fs = std::filesystem;

static const int XW = 8, XH = 8, XN = XW * XH;

// The five values every fixture carries in its first five pixels, and what each
// one is FOR. They are the whole point of the format being here: this is a
// measurement tool, so the only acceptable transform on load is none.
//
//   12.5   scene-linear and > 1: survives only if nothing tone maps
//   0.5    an ordinary value, exact in half
//   256.0  >> 1, still exact in half
//   -3.25  negative: survives only if nothing clamps
//   0.1    NOT exact in half - it must come back as half(0.1), which is a
//          DIFFERENT number from 0.1f, and exactly that one
static std::vector<float> chan(float lead) {
    std::vector<float> v((size_t)XN);
    for (int i = 0; i < XN; i++) v[(size_t)i] = lead + (float)i * 0.5f;
    v[0] = 12.5f; v[1] = 0.5f; v[2] = 256.0f; v[3] = -3.25f; v[4] = 0.1f;
    return v;
}

typedef std::vector<std::pair<std::string, std::vector<float>>> Chans;

// One flat scanline .exr.
//
// NB: OutputFile does NOT convert on write - the slice type must equal the
// channel type. (InputFile is the one that converts, which is why the reader
// can ask for FLOAT and get half widened for free.) So a HALF file is fed half
// memory, built here.
static void writeFlat(const fs::path& p, const Chans& chans, Imf::PixelType pt,
                      Imf::Compression comp, const Imath::Box2i* dataWindow = nullptr) {
    Imf::Header hdr(XW, XH);
    if (dataWindow) {
        hdr.displayWindow() = Imath::Box2i(Imath::V2i(0, 0), Imath::V2i(XW * 2 - 1, XH * 2 - 1));
        hdr.dataWindow() = *dataWindow;
    }
    for (const auto& c : chans) hdr.channels().insert(c.first, Imf::Channel(pt));
    hdr.compression() = comp;
    const Imath::Box2i dw = hdr.dataWindow();
    const int w = dw.max.x - dw.min.x + 1;

    Imf::OutputFile out(p.string().c_str(), hdr);
    Imf::FrameBuffer fb;
    std::vector<std::vector<Imath::half>> hbuf;
    hbuf.reserve(chans.size());              // stable addresses for the slices
    for (const auto& c : chans) {
        // OpenEXR addresses pixels in IMAGE coordinates, so a slice base is
        // where pixel (0,0) WOULD sit. For a data window that does not start at
        // the origin that address is outside the buffer, which is the idiom the
        // format's own headers document - and it is the arithmetic the READER
        // has to get right too, which is why window.exr exists.
        if (pt == Imf::HALF) {
            hbuf.emplace_back(c.second.begin(), c.second.end());
            const size_t xs = sizeof(Imath::half), ys = xs * (size_t)w;
            char* base = (char*)hbuf.back().data() - ((size_t)dw.min.x * xs + (size_t)dw.min.y * ys);
            fb.insert(c.first, Imf::Slice(Imf::HALF, base, xs, ys));
        } else {
            const size_t xs = sizeof(float), ys = xs * (size_t)w;
            char* base = const_cast<char*>((const char*)c.second.data())
                       - ((size_t)dw.min.x * xs + (size_t)dw.min.y * ys);
            fb.insert(c.first, Imf::Slice(Imf::FLOAT, base, xs, ys));
        }
    }
    out.setFrameBuffer(fb);
    out.writePixels(dw.max.y - dw.min.y + 1);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mkexr <dir>\n");
        return 2;
    }
    const fs::path dir = fs::u8path(argv[1]);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path seq = dir / "exrseq";
    fs::create_directories(seq, ec);

    const std::vector<float> cR = chan(0.0f), cG = chan(1.0f), cB = chan(2.0f), cA = chan(3.0f);
    int n = 0;
    auto wrote = [&](const fs::path& p) { n++; printf("mkexr: %s\n", p.string().c_str()); };

    try {
        // ---- what must be READ ------------------------------------------------
        // R,G,B half: the baseline. One 3-channel document, dtype f16.
        writeFlat(dir / "rgb_half.exr", { { "R", cR }, { "G", cG }, { "B", cB } },
                  Imf::HALF, Imf::NO_COMPRESSION);
        wrote(dir / "rgb_half.exr");

        // R,G,B,A float, with one value half could not have held.
        std::vector<float> fR = cR;
        fR[5] = 12345.6789f;
        writeFlat(dir / "rgba_f32.exr",
                  { { "R", fR }, { "G", cG }, { "B", cB }, { "A", cA } },
                  Imf::FLOAT, Imf::NO_COMPRESSION);
        wrote(dir / "rgba_f32.exr");

        // A lone Y: one channel, not a colour image with two channels missing.
        writeFlat(dir / "lum.exr", { { "Y", cR } }, Imf::HALF, Imf::NO_COMPRESSION);
        wrote(dir / "lum.exr");

        // Two named layers, the shape a multi-member .npz has.
        writeFlat(dir / "layers.exr",
                  { { "diffuse.R", cR }, { "diffuse.G", cG }, { "diffuse.B", cB },
                    { "specular.R", cG }, { "specular.G", cB }, { "specular.B", cR } },
                  Imf::HALF, Imf::NO_COMPRESSION);
        wrote(dir / "layers.exr");

        // A named layer BESIDE loose channels: "diffuse" is RGB, and Z and mask
        // are one plane each because no order over them means anything.
        writeFlat(dir / "mixed.exr",
                  { { "diffuse.R", cR }, { "diffuse.G", cG }, { "diffuse.B", cB },
                    { "Z", cA }, { "mask", cB } },
                  Imf::HALF, Imf::NO_COMPRESSION);
        wrote(dir / "mixed.exr");

        // A data window that does not start at the origin, and a display window
        // twice its size - what a renderer writes when it crops. The document is
        // the DATA window; getting the slice base wrong here shifts every pixel.
        {
            const Imath::Box2i dw(Imath::V2i(5, 3), Imath::V2i(5 + XW - 1, 3 + XH - 1));
            writeFlat(dir / "window.exr", { { "R", cR } }, Imf::HALF, Imf::NO_COMPRESSION, &dw);
            wrote(dir / "window.exr");
        }

        // ---- the compressed paths, which are the whole argument ---------------
        // ZIP/ZIPS/RLE/PIZ are lossless: byte for byte the same values as the
        // uncompressed file. DWAB is lossy and is asserted close, never equal.
        const std::pair<Imf::Compression, const char*> comps[] = {
            { Imf::ZIP_COMPRESSION,  "zip" },
            { Imf::ZIPS_COMPRESSION, "zips" },
            { Imf::RLE_COMPRESSION,  "rle" },
            { Imf::PIZ_COMPRESSION,  "piz" },
            { Imf::DWAB_COMPRESSION, "dwab" },
        };
        for (const auto& c : comps) {
            const fs::path p = dir / (std::string("comp_") + c.second + ".exr");
            writeFlat(p, { { "R", cR }, { "G", cG }, { "B", cB } }, Imf::HALF, c.first);
            wrote(p);
        }

        // ---- a folder of them is a stack --------------------------------------
        // One .exr is one frame: the frame axis comes from the files on disk,
        // through the sequence machinery that already exists.
        for (int i = 0; i < 3; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "f_%03d.exr", i);
            // 100 apart, not 1000: every one of these has to be exact in HALF
            // or the selftest that reads them back is asserting on rounding
            // rather than on which file became which frame (2012.5 is not a
            // half; 212.5 is).
            std::vector<float> v = cR;
            for (float& s : v) s += (float)i * 100.0f;
            writeFlat(seq / nm, { { "Y", v } }, Imf::HALF, Imf::NO_COMPRESSION);
            wrote(seq / nm);
        }

        // ---- what must be REFUSED, and each one is a VALID .exr ----------------
        // A refusal that passed because the file was malformed would prove
        // nothing. Every file below is one OpenEXR itself writes and reads.

        // Chroma subsampling: Y at 1x1, RY/BY at 2x2. No chroma reconstruction
        // happens here, and handing back three planes named Y, RY and BY as if
        // they were pictures would be three lies.
        {
            const fs::path p = dir / "chroma.exr";
            Imf::Header hdr(XW, XH);
            hdr.channels().insert("Y",  Imf::Channel(Imf::HALF, 1, 1));
            hdr.channels().insert("RY", Imf::Channel(Imf::HALF, 2, 2));
            hdr.channels().insert("BY", Imf::Channel(Imf::HALF, 2, 2));
            hdr.compression() = Imf::NO_COMPRESSION;
            Imf::OutputFile out(p.string().c_str(), hdr);
            std::vector<Imath::half> full((size_t)XN, Imath::half(1.0f));
            std::vector<Imath::half> quarter((size_t)(XW / 2) * (XH / 2), Imath::half(0.25f));
            const size_t hs = sizeof(Imath::half);
            Imf::FrameBuffer fb;
            fb.insert("Y",  Imf::Slice(Imf::HALF, (char*)full.data(), hs, hs * XW));
            fb.insert("RY", Imf::Slice(Imf::HALF, (char*)quarter.data(), hs, hs * (XW / 2), 2, 2));
            fb.insert("BY", Imf::Slice(Imf::HALF, (char*)quarter.data(), hs, hs * (XW / 2), 2, 2));
            out.setFrameBuffer(fb);
            out.writePixels(XH);
            wrote(p);
        }

        // A mip pyramid: the SAME picture at several resolutions. Which level is
        // "the image" is a question the file asks and a loader may not answer,
        // so this one is refused - unlike tiled.exr below, which is one level
        // and is read.
        {
            const fs::path p = dir / "mipmap.exr";
            Imf::Header hdr(XW, XH);
            hdr.channels().insert("R", Imf::Channel(Imf::HALF));
            hdr.compression() = Imf::NO_COMPRESSION;
            hdr.setTileDescription(Imf::TileDescription(4, 4, Imf::MIPMAP_LEVELS));
            Imf::TiledOutputFile out(p.string().c_str(), hdr);
            for (int lv = 0; lv < out.numLevels(); lv++) {
                const int lw = out.levelWidth(lv), lh = out.levelHeight(lv);
                std::vector<Imath::half> v((size_t)lw * (size_t)lh, Imath::half(2.0f + (float)lv));
                Imf::FrameBuffer fb;
                fb.insert("R", Imf::Slice(Imf::HALF, (char*)v.data(),
                                          sizeof(Imath::half), sizeof(Imath::half) * (size_t)lw));
                out.setFrameBuffer(fb);
                out.writeTiles(0, out.numXTiles(lv) - 1, 0, out.numYTiles(lv) - 1, lv);
            }
            wrote(p);
        }

        // One-level tiled. This one is READ: tiling is a storage layout and the
        // linked library undoes it losslessly, so refusing it would cost a user
        // a file this build can open exactly right. Its values are the same as
        // rgb_half.exr's R channel, so "read as tiles" is asserted against
        // "read as scanlines" rather than against a hope.
        {
            const fs::path p = dir / "tiled.exr";
            Imf::Header hdr(XW, XH);
            hdr.channels().insert("R", Imf::Channel(Imf::HALF));
            hdr.compression() = Imf::NO_COMPRESSION;
            hdr.setTileDescription(Imf::TileDescription(4, 4, Imf::ONE_LEVEL));
            Imf::TiledOutputFile out(p.string().c_str(), hdr);
            std::vector<Imath::half> v(cR.begin(), cR.end());
            Imf::FrameBuffer fb;
            fb.insert("R", Imf::Slice(Imf::HALF, (char*)v.data(),
                                      sizeof(Imath::half), sizeof(Imath::half) * XW));
            out.setFrameBuffer(fb);
            out.writeTiles(0, out.numXTiles() - 1, 0, out.numYTiles() - 1);
            wrote(p);
        }

        // Deep: a pixel is a LIST of samples, not a value. There is no
        // defensible flattening, so there is no flattening.
        {
            const fs::path p = dir / "deep.exr";
            Imf::Header hdr(XW, XH);
            hdr.channels().insert("R", Imf::Channel(Imf::HALF));
            hdr.setType(Imf::DEEPSCANLINE);
            hdr.compression() = Imf::NO_COMPRESSION;
            Imf::DeepScanLineOutputFile out(p.string().c_str(), hdr);
            std::vector<unsigned int> counts((size_t)XN, 1u);
            std::vector<Imath::half> store((size_t)XN, Imath::half(1.0f));
            std::vector<Imath::half*> ptrs((size_t)XN);
            for (int i = 0; i < XN; i++) ptrs[(size_t)i] = &store[(size_t)i];
            Imf::DeepFrameBuffer dfb;
            dfb.insertSampleCountSlice(Imf::Slice(Imf::UINT, (char*)counts.data(),
                                                 sizeof(unsigned int), sizeof(unsigned int) * XW));
            dfb.insert("R", Imf::DeepSlice(Imf::HALF, (char*)ptrs.data(),
                                           sizeof(Imath::half*), sizeof(Imath::half*) * XW,
                                           sizeof(Imath::half)));
            out.setFrameBuffer(dfb);
            out.writePixels(XH);
            wrote(p);
        }

        // Multi-part: several images in one file, each with its own header.
        // Which one the user meant is not something a loader may decide.
        {
            const fs::path p = dir / "multipart.exr";
            std::vector<Imf::Header> hs(2);
            for (int i = 0; i < 2; i++) {
                hs[(size_t)i] = Imf::Header(XW, XH);
                hs[(size_t)i].channels().insert("R", Imf::Channel(Imf::HALF));
                hs[(size_t)i].compression() = Imf::NO_COMPRESSION;
                hs[(size_t)i].setType(Imf::SCANLINEIMAGE);
                hs[(size_t)i].setName(i == 0 ? "left" : "right");
            }
            Imf::MultiPartOutputFile out(p.string().c_str(), hs.data(), 2);
            std::vector<Imath::half> v((size_t)XN, Imath::half(3.0f));
            for (int i = 0; i < 2; i++) {
                Imf::OutputPart part(out, i);
                Imf::FrameBuffer fb;
                fb.insert("R", Imf::Slice(Imf::HALF, (char*)v.data(),
                                          sizeof(Imath::half), sizeof(Imath::half) * XW));
                part.setFrameBuffer(fb);
                part.writePixels(XH);
            }
            wrote(p);
        }

        // A UINT channel: an id or an object index, not a measurement, and
        // widening it to float would silently lose values above 2^24.
        {
            const fs::path p = dir / "uintchan.exr";
            Imf::Header hdr(XW, XH);
            hdr.channels().insert("id", Imf::Channel(Imf::UINT));
            hdr.compression() = Imf::NO_COMPRESSION;
            Imf::OutputFile out(p.string().c_str(), hdr);
            std::vector<unsigned int> v((size_t)XN);
            for (int i = 0; i < XN; i++) v[(size_t)i] = (unsigned int)i * 7u;
            Imf::FrameBuffer fb;
            fb.insert("id", Imf::Slice(Imf::UINT, (char*)v.data(),
                                       sizeof(unsigned int), sizeof(unsigned int) * XW));
            out.setFrameBuffer(fb);
            out.writePixels(XH);
            wrote(p);
        }

        // Not an .exr at all, under an .exr name: the bytes decide.
        {
            const fs::path p = dir / "broken.exr";
            FILE* f = fopen(p.string().c_str(), "wb");
            if (!f) throw std::runtime_error("cannot write broken.exr");
            // A correct magic and version, then nothing - a truncated .exr is a
            // different event from a file that was never one.
            const unsigned char head[8] = { 0x76, 0x2f, 0x31, 0x01, 0x02, 0, 0, 0 };
            fwrite(head, 1, sizeof head, f);
            fclose(f);
            wrote(p);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "mkexr: FAILED: %s\n", e.what());
        return 1;
    }
    printf("mkexr: %d file(s) into %s\n", n, dir.string().c_str());
    return 0;
}
