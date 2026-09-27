// Scratch read benchmark: OpenImageIO. NOT part of the viewer build.
// Deliberately the same work as bench_exr.cpp: decode the whole 4K float RGB
// image into float memory, no tone mapping, no format conversion beyond
// "give me float". read_image with TypeDesc::FLOAT is the honest equivalent
// of the OpenEXR FrameBuffer path (OIIO hands back interleaved RGB; OpenEXR
// hands back planar -- noted in the write-up, it is not a free difference).
#include <OpenImageIO/imageio.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: oiiobench <file.exr> [runs]\n"); return 2; }
    const char* path = argv[1];
    int runs = argc > 2 ? std::atoi(argv[2]) : 9;

    double checksum = 0.0;
    for (int r = 0; r < runs; ++r) {
        auto t0 = std::chrono::steady_clock::now();

        auto in = OIIO::ImageInput::open(path);
        if (!in) { std::fprintf(stderr, "open failed: %s\n", OIIO::geterror().c_str()); return 1; }
        const OIIO::ImageSpec& spec = in->spec();
        int W = spec.width, H = spec.height, C = spec.nchannels;
        std::vector<float> px((size_t)W * H * C);
        in->read_image(0, 0, 0, C, OIIO::TypeDesc::FLOAT, px.data());
        in->close();

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        checksum += px[0] + px[px.size() / 2] + px[px.size() - 1];
        std::printf("oiio %s run=%d ms=%.2f %dx%dx%d\n", path, r, ms, W, H, C);
        std::fflush(stdout);
    }
    std::fprintf(stderr, "checksum %.6f\n", checksum);
    return 0;
}
