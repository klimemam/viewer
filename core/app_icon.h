// The application mark, drawn rather than shipped.
//
// One description serves every size the desktop asks for (16 px taskbar button
// up to a 256 px Explorer tile), so there is no set of hand-made bitmaps to keep
// in sync and no binary blob in the source tree. It is also why the mark can
// change while the app runs: a window showing a server's pixels gets a
// different frame color, which is the only thing telling two viewer buttons
// apart in the taskbar.
#pragma once
#include <vector>

namespace app_icon {

enum Variant {
    Local  = 0,   // accent frame: the theme's blue
    Remote = 1,   // accent frame: the green the status bar uses for a live peer
};

// size x size RGBA8, straight alpha, top-down rows - the layout GLFWimage wants
// and the one the ICO/PNG writers in tools/mkicon.cpp expect.
std::vector<unsigned char> render(int size, Variant v);

}  // namespace app_icon
