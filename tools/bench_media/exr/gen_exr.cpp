// Scratch benchmark fixture generator. NOT part of the viewer build.
// Writes a 4K float RGB EXR in several compressions, with sensor-like content
// (gradient + fixed-pattern + per-pixel noise) so the codecs have real work to
// do. A flat image compresses to nothing and would make decode timing a lie.
#include <ImfOutputFile.h>
#include <ImfChannelList.h>
#include <ImfHeader.h>
#include <ImfFrameBuffer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: exrgen <out.exr> <zip|piz|none|zips> <W> <H>\n");
        return 2;
    }
    const char* out = argv[1];
    std::string comp = argv[2];
    int W = argc > 3 ? std::atoi(argv[3]) : 4096;
    int H = argc > 4 ? std::atoi(argv[4]) : 2160;

    std::vector<float> R((size_t)W * H), G((size_t)W * H), B((size_t)W * H);
    // deterministic LCG: same fixture on every machine/run
    unsigned s = 12345u;
    auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t i = (size_t)y * W + x;
            float base = (float)x / (float)W;            // scene gradient
            float fpn  = ((x % 7) + (y % 13)) * 0.002f;  // fixed pattern
            float n    = (rnd() - 0.5f) * 0.05f;         // shot-ish noise
            R[i] = base + fpn + n;
            G[i] = base * 0.9f + fpn + n * 1.1f;
            B[i] = base * 0.7f + fpn + n * 0.9f;
        }
    }

    Imf::Compression c = Imf::ZIP_COMPRESSION;
    if (comp == "piz") c = Imf::PIZ_COMPRESSION;
    else if (comp == "none") c = Imf::NO_COMPRESSION;
    else if (comp == "zips") c = Imf::ZIPS_COMPRESSION;

    Imf::Header h(W, H);
    h.compression() = c;
    h.channels().insert("R", Imf::Channel(Imf::FLOAT));
    h.channels().insert("G", Imf::Channel(Imf::FLOAT));
    h.channels().insert("B", Imf::Channel(Imf::FLOAT));

    Imf::OutputFile file(out, h);
    Imf::FrameBuffer fb;
    fb.insert("R", Imf::Slice(Imf::FLOAT, (char*)R.data(), sizeof(float), sizeof(float) * W));
    fb.insert("G", Imf::Slice(Imf::FLOAT, (char*)G.data(), sizeof(float), sizeof(float) * W));
    fb.insert("B", Imf::Slice(Imf::FLOAT, (char*)B.data(), sizeof(float), sizeof(float) * W));
    file.setFrameBuffer(fb);
    file.writePixels(H);
    std::printf("wrote %s  %dx%d  %s\n", out, W, H, comp.c_str());
    return 0;
}
