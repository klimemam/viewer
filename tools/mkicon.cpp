// Build-time tool: turns core/app_icon.cpp's drawing into the file formats the
// three desktops want.
//
//     mkicon <outdir>
//       <outdir>/viewer.ico          Windows: linked into the exe as a resource,
//                                    which is what Explorer, the shortcut and a
//                                    pinned taskbar button read
//       <outdir>/viewer.png          Linux .desktop / macOS .app bundle (256 px)
//       <outdir>/viewer-remote.ico   the green variant, for the shortcut that
//       <outdir>/viewer-remote.png   connects to a server: it says "this one
//                                    starts on the far machine" on the desktop,
//                                    before anything is running to recolor its
//                                    own taskbar button
//
// Generating them keeps binaries out of the source tree - the repo carries the
// description of the mark, not seven bitmaps of it.
#include "app_icon.h"
#include "miniz.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xff);
}
void putBE32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; i--) v.push_back((x >> (8 * i)) & 0xff);
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "mkicon: cannot write %s\n", path.c_str()); return false; }
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) fprintf(stderr, "mkicon: short write on %s\n", path.c_str());
    return ok;
}

// ---- PNG -------------------------------------------------------------------
void chunk(std::vector<uint8_t>& v, const char* tag, const std::vector<uint8_t>& data) {
    putBE32(v, (uint32_t)data.size());
    std::vector<uint8_t> body(tag, tag + 4);
    body.insert(body.end(), data.begin(), data.end());
    v.insert(v.end(), body.begin(), body.end());
    putBE32(v, (uint32_t)mz_crc32(MZ_CRC32_INIT, body.data(), body.size()));
}

// Every row gets filter 0 and the whole thing one deflate pass: an icon is a
// few flat colors, so the fancy filters would buy nothing measurable.
std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgba, int size) {
    std::vector<uint8_t> raw;                       // filter byte 0 + row, per row
    raw.reserve((size_t)size * (size * 4 + 1));
    for (int y = 0; y < size; y++) {
        raw.push_back(0);
        const uint8_t* row = &rgba[(size_t)y * size * 4];
        raw.insert(raw.end(), row, row + (size_t)size * 4);
    }
    mz_ulong zlen = mz_compressBound((mz_ulong)raw.size());
    std::vector<uint8_t> z(zlen);
    if (mz_compress(z.data(), &zlen, raw.data(), (mz_ulong)raw.size()) != MZ_OK) {
        fprintf(stderr, "mkicon: deflate failed\n");
        return {};
    }
    z.resize(zlen);

    std::vector<uint8_t> v = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    putBE32(ihdr, (uint32_t)size); putBE32(ihdr, (uint32_t)size);
    ihdr.push_back(8);              // bit depth
    ihdr.push_back(6);              // color type: RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(v, "IHDR", ihdr);
    chunk(v, "IDAT", z);
    chunk(v, "IEND", {});
    return v;
}

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, int size) {
    const std::vector<uint8_t> png = encodePng(rgba, size);
    return !png.empty() && writeFile(path, png);
}

// ---- ICO -------------------------------------------------------------------
// Small entries are 32bpp BGRA DIBs, the format every shell surface has read
// since forever. The 128 and 256 px ones are stored as PNG instead (Vista and
// later, i.e. everything that runs this build): as raw DIBs those two alone
// would be 320 KB of the executable, against a few KB compressed.
std::vector<uint8_t> icoImage(const std::vector<uint8_t>& rgba, int size) {
    std::vector<uint8_t> v;
    const uint32_t xorBytes = (uint32_t)size * size * 4;
    const uint32_t maskStride = (uint32_t)((size + 31) / 32) * 4;   // 1bpp, 4-byte rows
    const uint32_t andBytes = maskStride * size;
    put32(v, 40);                       // biSize
    put32(v, (uint32_t)size);           // biWidth
    put32(v, (uint32_t)size * 2);       // biHeight: XOR image + AND mask
    put16(v, 1);                        // biPlanes
    put16(v, 32);                       // biBitCount
    put32(v, 0);                        // BI_RGB
    put32(v, xorBytes + andBytes);      // biSizeImage
    put32(v, 0); put32(v, 0);           // pixels-per-meter
    put32(v, 0); put32(v, 0);           // palette
    for (int y = size - 1; y >= 0; y--)               // DIB rows run bottom-up
        for (int x = 0; x < size; x++) {
            const uint8_t* p = &rgba[((size_t)y * size + x) * 4];
            v.push_back(p[2]); v.push_back(p[1]); v.push_back(p[0]); v.push_back(p[3]);
        }
    // The alpha channel carries the shape; the 1bpp mask stays all-opaque. Left
    // set, XP-era surfaces would punch the whole icon out.
    v.insert(v.end(), andBytes, 0);
    return v;
}

bool writeIco(const std::string& path, const std::vector<int>& sizes, app_icon::Variant variant) {
    std::vector<std::vector<uint8_t>> imgs;
    for (int s : sizes) {
        const std::vector<uint8_t> rgba = app_icon::render(s, variant);
        imgs.push_back(s >= 128 ? encodePng(rgba, s) : icoImage(rgba, s));
        if (imgs.back().empty()) return false;
    }

    std::vector<uint8_t> v;
    put16(v, 0); put16(v, 1); put16(v, (uint16_t)sizes.size());       // ICONDIR
    uint32_t off = 6 + 16 * (uint32_t)sizes.size();
    for (size_t i = 0; i < sizes.size(); i++) {
        v.push_back(sizes[i] >= 256 ? 0 : (uint8_t)sizes[i]);         // 0 means 256
        v.push_back(sizes[i] >= 256 ? 0 : (uint8_t)sizes[i]);
        v.push_back(0); v.push_back(0);                               // palette, reserved
        put16(v, 1); put16(v, 32);                                    // planes, bpp
        put32(v, (uint32_t)imgs[i].size());
        put32(v, off);
        off += (uint32_t)imgs[i].size();
    }
    for (const auto& im : imgs) v.insert(v.end(), im.begin(), im.end());
    return writeFile(path, v);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: mkicon <outdir>\n"); return 2; }
    const std::string dir = argv[1];
    const std::string sep = dir.empty() || dir.back() == '/' || dir.back() == '\\' ? "" : "/";
    const std::string base = dir + sep;

    // 256 is what Explorer's large-tile views ask for; 16/32 are what the
    // taskbar and title bar actually get seen at.
    const std::vector<int> sizes = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    if (!writeIco(base + "viewer.ico", sizes, app_icon::Local)) return 1;
    if (!writeIco(base + "viewer-remote.ico", sizes, app_icon::Remote)) return 1;
    if (!writePng(base + "viewer.png", app_icon::render(256, app_icon::Local), 256)) return 1;
    if (!writePng(base + "viewer-remote.png", app_icon::render(256, app_icon::Remote), 256)) return 1;
    printf("mkicon: wrote viewer{,-remote}.{ico,png} to %s\n", dir.c_str());
    return 0;
}
