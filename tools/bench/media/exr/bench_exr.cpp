// Scratch read benchmark: official OpenEXR. NOT part of the viewer build.
// Reads the whole image into three float planes -- the same thing loadExr
// would do (channels kept separate, no tone mapping, no RGBA repack).
// Prints one line per run in ms; the caller takes the median and the spread.
#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfThreading.h>
#include <ImathBox.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

// threads: OpenEXR's C++ API defaults to a GLOBAL THREAD COUNT OF 0, i.e. it
// decompresses on the calling thread only. OIIO sets up a thread pool of its
// own. That -- not a better decoder -- is the likeliest source of any "OIIO
// reads EXR faster" impression, since OIIO reads EXR by linking OpenEXR
// itself (measured: OIIO builds OpenEXR 3.3.5 as a dependency).
// So the thread count is a benchmark AXIS here, not a constant.
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: exrbench <file.exr> [runs] [threads]\n"); return 2; }
    const char* path = argv[1];
    int runs = argc > 2 ? std::atoi(argv[2]) : 9;
    int threads = argc > 3 ? std::atoi(argv[3]) : 0;
    Imf::setGlobalThreadCount(threads);

    double checksum = 0.0;
    for (int r = 0; r < runs; ++r) {
        auto t0 = std::chrono::steady_clock::now();

        Imf::InputFile file(path);
        Imath::Box2i dw = file.header().dataWindow();
        int W = dw.max.x - dw.min.x + 1;
        int H = dw.max.y - dw.min.y + 1;

        std::vector<float> R((size_t)W * H), G((size_t)W * H), B((size_t)W * H);
        Imf::FrameBuffer fb;
        auto add = [&](const char* n, std::vector<float>& v) {
            fb.insert(n, Imf::Slice(Imf::FLOAT,
                (char*)(v.data() - (size_t)dw.min.y * W - dw.min.x),
                sizeof(float), sizeof(float) * W));
        };
        add("R", R); add("G", G); add("B", B);
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        checksum += R[0] + G[(size_t)W * H / 2] + B[(size_t)W * H - 1];
        std::printf("openexr-t%d %s run=%d ms=%.2f %dx%d\n", threads, path, r, ms, W, H);
        std::fflush(stdout);
    }
    std::fprintf(stderr, "checksum %.6f\n", checksum);  // keep the reads alive
    return 0;
}
