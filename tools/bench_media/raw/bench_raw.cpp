// Scratch probe: LibRaw. NOT part of the viewer build.
//
// Two questions, not one:
//  (1) how fast does unpack() hand over the sensor values, and
//  (2) does it actually hand over MEASUREMENTS -- i.e. rawdata.raw_image with
//      a stated black level, CFA pattern and bit depth, WITHOUT demosaic,
//      white balance, colour matrix or gamma.
// (2) is the one that decides whether vendor RAW may claim values=dn, so this
// prints the provenance fields rather than trusting the documentation.
//
// Deliberately NOT called: dcraw_process() / dcraw_make_mem_image(). Those
// demosaic and tone-map. The whole point is to stop before them.
#include <libraw/libraw.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: rawbench <file> [runs]\n"); return 2; }
    const char* path = argv[1];
    int runs = argc > 2 ? std::atoi(argv[2]) : 7;

    for (int r = 0; r < runs; ++r) {
        LibRaw p;
        auto t0 = std::chrono::steady_clock::now();
        int rc = p.open_file(path);
        if (rc != LIBRAW_SUCCESS) {
            std::fprintf(stderr, "open_file failed: %s\n", libraw_strerror(rc));
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        rc = p.unpack();                       // sensor values, no processing
        if (rc != LIBRAW_SUCCESS) {
            std::fprintf(stderr, "unpack failed: %s\n", libraw_strerror(rc));
            return 1;
        }
        auto t2 = std::chrono::steady_clock::now();

        double openms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double unpms  = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("libraw %s run=%d ms=%.2f open_ms=%.2f %dx%d\n",
                    path, r, unpms, openms, p.imgdata.sizes.raw_width, p.imgdata.sizes.raw_height);
        std::fflush(stdout);

        if (r == 0) {
            const auto& S = p.imgdata.sizes;
            const auto& C = p.imgdata.color;
            const auto& I = p.imgdata.idata;
            const auto& O = p.imgdata.other;
            std::fprintf(stderr, "\n--- is this a MEASUREMENT? ---\n");
            std::fprintf(stderr, "make/model      : %s %s\n", I.make, I.model);
            std::fprintf(stderr, "raw_image ptr   : %s\n",
                         p.imgdata.rawdata.raw_image ? "yes (1 plane, CFA mosaic)"
                                                     : "NULL (not a bayer raw)");
            std::fprintf(stderr, "raw w x h       : %d x %d   (visible %d x %d)\n",
                         S.raw_width, S.raw_height, S.width, S.height);
            std::fprintf(stderr, "colors / filters: %d / 0x%08x\n", I.colors, I.filters);
            std::fprintf(stderr, "CFA pattern     : %c%c%c%c\n",
                         p.imgdata.idata.cdesc[p.COLOR(0,0)], p.imgdata.idata.cdesc[p.COLOR(0,1)],
                         p.imgdata.idata.cdesc[p.COLOR(1,0)], p.imgdata.idata.cdesc[p.COLOR(1,1)]);
            std::fprintf(stderr, "black level     : %d   (per-channel cblack %d/%d/%d/%d)\n",
                         C.black, C.cblack[0], C.cblack[1], C.cblack[2], C.cblack[3]);
            std::fprintf(stderr, "white/maximum   : %d\n", C.maximum);
            std::fprintf(stderr, "=> implied bit depth: %d bits\n",
                         (int)(C.maximum > 0 ? (32 - __builtin_clz((unsigned)C.maximum)) : 0));
            std::fprintf(stderr, "\n--- EXIF that could fill Series.conditions ---\n");
            std::fprintf(stderr, "shutter         : %g s\n", O.shutter);
            std::fprintf(stderr, "aperture        : f/%g\n", O.aperture);
            std::fprintf(stderr, "iso_speed       : %g\n", O.iso_speed);
            std::fprintf(stderr, "focal_len       : %g mm\n", O.focal_len);
            std::fprintf(stderr, "timestamp       : %lld\n", (long long)O.timestamp);
            std::fprintf(stderr, "cam_mul (WB)    : %g %g %g %g   <- NOT applied by unpack()\n",
                         C.cam_mul[0], C.cam_mul[1], C.cam_mul[2], C.cam_mul[3]);
            // first few raw values, to show they are counts and not 0..1
            if (p.imgdata.rawdata.raw_image) {
                std::fprintf(stderr, "first raw values: ");
                for (int i = 0; i < 8; ++i)
                    std::fprintf(stderr, "%u ", (unsigned)p.imgdata.rawdata.raw_image[i]);
                std::fprintf(stderr, "\n");
            }
        }
        p.recycle();
    }
    return 0;
}
