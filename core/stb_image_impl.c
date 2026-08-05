// stb_image's implementation, in a translation unit of its own.
//
// Same treatment miniz.c gets in CMakeLists.txt: third-party code compiled with
// its warnings off, so that core/imagefile.cpp - the code this repository owns -
// keeps being built with -Wall -Wextra and stays clean under them.
//
// ONLY the two formats that are actually offered. stb_image can also read BMP,
// GIF, PSD, TGA, HDR, PIC and PNM, and every one of those would then open with
// no row in imagefile.cpp's table, no note describing what its samples mean and
// no line in THIRD-PARTY-NOTICES. What the viewer reads is a decision, not a
// side effect of which library was linked.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO          // the viewer reads the bytes itself (readFileBytes)
#include "stb_image.h"
