// The window's own frame: title bar, buttons, drag, resize.
//
// Two modes. "System" is what a GLFW window normally is - the desktop draws a
// title bar above the app and owns the close button. "Integrated" turns that
// off and moves all of it into the menu bar the app already draws, the way
// VSCode / Chrome / Blender do it, so the window is one surface instead of an
// app sitting inside someone else's box.
//
// Turning decorations off is one line. Everything the decorations were doing
// for us is the rest of this file:
//
//   Windows  the window keeps WS_THICKFRAME/WS_CAPTION and answers
//            WM_NCHITTEST itself, so the OS still runs the drag, the resize,
//            Aero Snap, Win+arrow, Snap Layouts (Win11), the Alt+Space menu,
//            the drop shadow and the rounded corners. Nothing is reimplemented.
//   X11      _NET_WM_MOVERESIZE hands the drag/resize back to the window
//            manager, which is where edge tiling lives.
//   fallback anywhere else, the move and the resize are done by hand.
//   Wayland  cannot be done from GLFW at all (a client may not place itself);
//            the mode stays System and says why.
//   macOS    stays System: the traffic lights belong to the OS there, and
//            faking them is worse than using them.
//
// The UI half lives in main.cpp (it owns the menu bar); this side only needs to
// be told which parts of the bar are draggable.
#pragma once

struct GLFWwindow;

namespace window_frame {

enum Mode { System = 0, Integrated = 1 };

// Call once, right after the window exists.
void init(GLFWwindow* w);

bool available();                 // can this platform/session do Integrated?
const char* unavailableReason();  // one line for the menu tooltip; "" if it can

void setMode(Mode m);             // requesting Integrated where it is not available keeps System
Mode mode();

// ---- per-frame, in window coordinates (pixels, origin top-left) -------------
void beginFrame(float uiScale);
void addCaption(float x0, float y0, float x1, float y1);     // drag here moves the window
void addExclusion(float x0, float y0, float x1, float y1);   // ... except here (menus, buttons)
void setMaximizeButton(float x0, float y0, float x1, float y1);
// True while the OS is hovering the maximize button on our behalf (Win11 opens
// the Snap Layouts flyout there and sends us no mouse events, so the button has
// to be told to paint itself hot).
bool maximizeButtonHot();
// Does the hit testing, the cursor shape, and - where the OS does not do it
// for us - the drag and the resize. Call it after the whole UI has been built,
// so it can see whether a widget is already using the mouse.
void endFrame();

bool maximized();
void minimize();
void toggleMaximize();
void requestClose();

}  // namespace window_frame
