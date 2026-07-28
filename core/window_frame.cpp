#include "window_frame.h"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_internal.h"      // ActiveIdWindow: see uiBusy() below

#include <algorithm>
#include <vector>

#if defined(_WIN32)
  #define GLFW_EXPOSE_NATIVE_WIN32
  #include <GLFW/glfw3native.h>
  #include <windows.h>
  #include <windowsx.h>
  #include <dwmapi.h>
  #include <shellapi.h>
#elif defined(VIEWER_X11)
  #define GLFW_EXPOSE_NATIVE_X11
  #include <GLFW/glfw3native.h>
  #include <X11/Xlib.h>
  #include <X11/Xatom.h>
#endif

namespace window_frame {
namespace {

struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

GLFWwindow* g_win = nullptr;
Mode        g_mode = System;
bool        g_available = false;
const char* g_why = "";
float       g_scale = 1.0f;
float       g_margin = 6.0f;          // resize band, in pixels

std::vector<Rect> g_caption, g_exclude;
Rect g_maxBtn;
bool g_maxBtnHot = false;

// edges, as a bit set; CAPTION is not an edge but shares the hit test
enum { HZ_NONE = 0, HZ_L = 1, HZ_R = 2, HZ_T = 4, HZ_B = 8, HZ_CAPTION = 16 };

bool inRect(const Rect& r, float x, float y) {
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}
bool inCaption(float x, float y) {
    for (const Rect& r : g_exclude) if (inRect(r, x, y)) return false;
    for (const Rect& r : g_caption) if (inRect(r, x, y)) return true;
    return false;
}

// Window-relative point -> what is under it. Edges lose to nothing: a 6 px band
// at the border beats the caption, or the top-left corner of a maximizable
// window would be undraggable and unresizable at the same time.
int hitZone(float x, float y, int w, int h, bool allowEdges) {
    int z = HZ_NONE;
    if (allowEdges) {
        if (x <= g_margin)          z |= HZ_L;
        if (x >= w - g_margin)      z |= HZ_R;
        if (y <= g_margin)          z |= HZ_T;
        if (y >= h - g_margin)      z |= HZ_B;
    }
    if (z) return z;
    return inCaption(x, y) ? HZ_CAPTION : HZ_NONE;
}

// Is a WIDGET using the mouse right now?
//
// Neither of the obvious tests works. io.WantCaptureMouse is true over the menu
// bar, which is exactly where the caption lives. IsAnyItemActive() is true the
// moment a window is clicked anywhere at all, because ImGui makes the window's
// move id the active id to raise it - including for a window flagged NoMove,
// like the menu bar. What is left is the precise question: there is an active
// id, and it is not that window-move id.
bool uiBusy() {
    const ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || g->ActiveId == 0) return false;
    return !g->ActiveIdWindow || g->ActiveId != g->ActiveIdWindow->MoveId;
}

ImGuiMouseCursor cursorFor(int z) {
    switch (z & (HZ_L | HZ_R | HZ_T | HZ_B)) {
        case HZ_L: case HZ_R:                       return ImGuiMouseCursor_ResizeEW;
        case HZ_T: case HZ_B:                       return ImGuiMouseCursor_ResizeNS;
        case HZ_L | HZ_T: case HZ_R | HZ_B:         return ImGuiMouseCursor_ResizeNWSE;
        case HZ_R | HZ_T: case HZ_L | HZ_B:         return ImGuiMouseCursor_ResizeNESW;
        default:                                    return ImGuiMouseCursor_Arrow;
    }
}

// ---------------------------------------------------------------- Windows ---
#if defined(_WIN32)

WNDPROC g_prevProc = nullptr;
HWND    g_hwnd = nullptr;

int frameThicknessX() {
    return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}
int frameThicknessY() {
    return GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

// A maximized borderless window covers the whole monitor, and an auto-hiding
// taskbar on that monitor then never gets its mouse-at-the-edge event: the bar
// becomes unreachable until the window is restored. Leaving one pixel of that
// edge uncovered is the documented way out.
UINT autoHideEdge(HMONITOR mon) {
    APPBARDATA state = {};
    state.cbSize = sizeof(state);
    if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE)) return (UINT)-1;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return (UINT)-1;
    for (UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
        APPBARDATA q = {};
        q.cbSize = sizeof(q);
        q.uEdge = edge;
        q.rc = mi.rcMonitor;
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q)) return edge;
    }
    return (UINT)-1;
}

LRESULT CALLBACK frameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_mode != Integrated) return CallWindowProc(g_prevProc, h, msg, wp, lp);

    switch (msg) {
        // The whole window becomes client area: the frame is still there (that
        // is what keeps snapping, resizing and the shadow), it just draws
        // nothing and we paint over it.
        case WM_NCCALCSIZE: {
            if (wp != TRUE) break;
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            if (IsZoomed(h)) {
                // maximized: the window rect is the work area GROWN by the
                // frame, so without this the content spills off every edge
                const int fx = frameThicknessX(), fy = frameThicknessY();
                p->rgrc[0].left   += fx;
                p->rgrc[0].right  -= fx;
                p->rgrc[0].bottom -= fy;
                p->rgrc[0].top    += fy;
                const UINT edge = autoHideEdge(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
                if (edge == ABE_TOP)    p->rgrc[0].top    += 1;
                if (edge == ABE_BOTTOM) p->rgrc[0].bottom -= 1;
                if (edge == ABE_LEFT)   p->rgrc[0].left   += 1;
                if (edge == ABE_RIGHT)  p->rgrc[0].right  -= 1;
            }
            return 0;
        }

        // Where the OS asks "what is under the cursor?". Answering HTCAPTION /
        // HTLEFT / ... is what buys the native drag, the native resize, Aero
        // Snap, Aero Shake and the Alt+Space menu without implementing any of
        // them.
        case WM_NCHITTEST: {
            const POINT scr = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT pt = scr;
            ScreenToClient(h, &pt);
            RECT cr;
            GetClientRect(h, &cr);
            const int w = cr.right - cr.left, ht = cr.bottom - cr.top;
            const int z = hitZone((float)pt.x, (float)pt.y, w, ht, !IsZoomed(h));
            if (z & (HZ_L | HZ_R | HZ_T | HZ_B)) {
                if (z == (HZ_L | HZ_T)) return HTTOPLEFT;
                if (z == (HZ_R | HZ_T)) return HTTOPRIGHT;
                if (z == (HZ_L | HZ_B)) return HTBOTTOMLEFT;
                if (z == (HZ_R | HZ_B)) return HTBOTTOMRIGHT;
                if (z & HZ_L) return HTLEFT;
                if (z & HZ_R) return HTRIGHT;
                if (z & HZ_T) return HTTOP;
                return HTBOTTOM;
            }
            // Windows 11 opens the Snap Layouts flyout when the cursor rests on
            // whatever answers HTMAXBUTTON. It also means we stop getting mouse
            // messages there, so the button's hover state comes from here.
            if (inRect(g_maxBtn, (float)pt.x, (float)pt.y)) {
                g_maxBtnHot = true;
                return HTMAXBUTTON;
            }
            g_maxBtnHot = false;
            return (z & HZ_CAPTION) ? HTCAPTION : HTCLIENT;
        }

        // HTMAXBUTTON is non-client, so the click arrives here instead of as a
        // normal mouse event. Swallow the press, act on the release - the same
        // order a real button has.
        case WM_NCLBUTTONDOWN:
            if (wp == HTMAXBUTTON) return 0;
            break;
        case WM_NCLBUTTONUP:
            if (wp == HTMAXBUTTON) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                else             ShowWindow(h, SW_MAXIMIZE);
                return 0;
            }
            break;
        case WM_NCMOUSELEAVE:
            g_maxBtnHot = false;
            break;
    }
    return CallWindowProc(g_prevProc, h, msg, wp, lp);
}

void applyPlatform(bool integrated) {
    if (!g_hwnd) return;
    LONG_PTR style = GetWindowLongPtr(g_hwnd, GWL_STYLE);
    if (integrated) {
        // GLFW made this a WS_POPUP when decorations went off. Put back the
        // parts that are behaviour rather than paint: the sizing frame, the
        // caption bit (Aero Snap and the window animations look for it), and
        // the buttons' system commands.
        style |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
    } else {
        style &= ~(WS_THICKFRAME | WS_CAPTION);
    }
    SetWindowLongPtr(g_hwnd, GWL_STYLE, style);

    if (integrated) {
        // keeps the drop shadow a borderless window would otherwise lose
        const MARGINS m = {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(g_hwnd, &m);
        // Windows 11 rounds its own corners; ask for the same (ignored before)
        const DWORD DWMWA_WINDOW_CORNER_PREFERENCE_ = 33, DWMWCP_ROUND_ = 2;
        DWORD pref = DWMWCP_ROUND_;
        DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE_, &pref, sizeof(pref));
    }
    // force the frame to be recomputed with the new style / NCCALCSIZE answer
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void initPlatform() {
    g_hwnd = glfwGetWin32Window(g_win);
    if (!g_hwnd) { g_why = "no native window handle"; return; }
    g_prevProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)frameProc);
    g_available = g_prevProc != nullptr;
    if (!g_available) g_why = "could not hook the window procedure";
}

// the OS runs both, from WM_NCHITTEST
constexpr bool osHandlesDrag = true;

// ------------------------------------------------------------------- X11 ---
#elif defined(VIEWER_X11)

Display* g_dpy = nullptr;
::Window g_xwin = 0;
bool     g_wmMoveResize = false;

bool wmSupports(const char* name) {
    const Atom netSupported = XInternAtom(g_dpy, "_NET_SUPPORTED", True);
    const Atom want = XInternAtom(g_dpy, name, True);
    if (!netSupported || !want) return false;
    Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(g_dpy, DefaultRootWindow(g_dpy), netSupported, 0, 1024, False,
                           XA_ATOM, &type, &format, &count, &after, &data) != Success || !data)
        return false;
    const Atom* atoms = (const Atom*)data;
    bool found = false;
    for (unsigned long i = 0; i < count && !found; i++) found = atoms[i] == want;
    XFree(data);
    return found;
}

// _NET_WM_MOVERESIZE: "window manager, take over from here". The WM then runs
// its own move/resize loop, which is the only way to get its edge tiling and
// its snapping - a client moving itself with XMoveWindow gets neither.
void wmMoveResize(int direction) {
    static const Atom msg = XInternAtom(g_dpy, "_NET_WM_MOVERESIZE", False);
    int wx = 0, wy = 0;
    double cx = 0, cy = 0;
    glfwGetWindowPos(g_win, &wx, &wy);
    glfwGetCursorPos(g_win, &cx, &cy);

    XUngrabPointer(g_dpy, CurrentTime);          // the WM needs the pointer
    XFlush(g_dpy);

    XEvent e = {};
    e.xclient.type = ClientMessage;
    e.xclient.window = g_xwin;
    e.xclient.message_type = msg;
    e.xclient.format = 32;
    e.xclient.data.l[0] = wx + (long)cx;         // root coordinates
    e.xclient.data.l[1] = wy + (long)cy;
    e.xclient.data.l[2] = direction;
    e.xclient.data.l[3] = Button1;
    e.xclient.data.l[4] = 1;                     // source: a normal application
    XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(g_dpy);
}

// _NET_WM_MOVERESIZE direction codes
int moveResizeDir(int z) {
    switch (z & (HZ_L | HZ_R | HZ_T | HZ_B)) {
        case HZ_L | HZ_T: return 0;
        case HZ_T:        return 1;
        case HZ_R | HZ_T: return 2;
        case HZ_R:        return 3;
        case HZ_R | HZ_B: return 4;
        case HZ_B:        return 5;
        case HZ_L | HZ_B: return 6;
        case HZ_L:        return 7;
        default:          return 8;              // move
    }
}

void applyPlatform(bool) {}

void initPlatform() {
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) {
        g_why = "Wayland: a client cannot place or drag its own window";
        return;
    }
    g_dpy = glfwGetX11Display();
    g_xwin = glfwGetX11Window(g_win);
    g_available = g_dpy != nullptr && g_xwin != 0;
    if (!g_available) { g_why = "no X11 window handle"; return; }
    g_wmMoveResize = wmSupports("_NET_WM_MOVERESIZE");
}

constexpr bool osHandlesDrag = false;

// --------------------------------------------------------------- others ---
#else

void applyPlatform(bool) {}
void initPlatform() {
  #if defined(__APPLE__)
    g_why = "macOS: the window buttons belong to the system";
  #else
    g_available = true;
  #endif
}
constexpr bool osHandlesDrag = false;

#endif

// ---- the by-hand path ------------------------------------------------------
// Used where nothing native is available: a plain drag of the window rectangle.
// No snapping, no tiling - it moves, it resizes, it stays out of the way.
bool  g_dragging = false;
int   g_dragZone = HZ_NONE;
int   g_pressZone = HZ_NONE;      // armed by a press, spent by the first movement
float g_grabScrX = 0, g_grabScrY = 0;
int   g_grabX = 0, g_grabY = 0, g_grabW = 0, g_grabH = 0;

void cursorScreenPos(float& sx, float& sy) {
    int wx = 0, wy = 0;
    double cx = 0, cy = 0;
    glfwGetWindowPos(g_win, &wx, &wy);
    glfwGetCursorPos(g_win, &cx, &cy);
    sx = (float)(wx + cx);
    sy = (float)(wy + cy);
}

void beginManual(int zone) {
    g_dragging = true;
    g_dragZone = zone;
    cursorScreenPos(g_grabScrX, g_grabScrY);
    glfwGetWindowPos(g_win, &g_grabX, &g_grabY);
    glfwGetWindowSize(g_win, &g_grabW, &g_grabH);
}

void stepManual() {
    float sx = 0, sy = 0;
    cursorScreenPos(sx, sy);
    const int dx = (int)(sx - g_grabScrX), dy = (int)(sy - g_grabScrY);
    if (g_dragZone == HZ_CAPTION) {
        glfwSetWindowPos(g_win, g_grabX + dx, g_grabY + dy);
        return;
    }
    const int MINW = 480, MINH = 320;
    int x = g_grabX, y = g_grabY, w = g_grabW, h = g_grabH;
    if (g_dragZone & HZ_L) { const int nw = std::max(MINW, g_grabW - dx); x += g_grabW - nw; w = nw; }
    if (g_dragZone & HZ_R) { w = std::max(MINW, g_grabW + dx); }
    if (g_dragZone & HZ_T) { const int nh = std::max(MINH, g_grabH - dy); y += g_grabH - nh; h = nh; }
    if (g_dragZone & HZ_B) { h = std::max(MINH, g_grabH + dy); }
    if (x != g_grabX || y != g_grabY) glfwSetWindowPos(g_win, x, y);
    glfwSetWindowSize(g_win, w, h);
}

}  // namespace

// ---------------------------------------------------------------------------

void init(GLFWwindow* w) {
    g_win = w;
    g_why = "";
    initPlatform();
    if (!g_available && !*g_why) g_why = "not supported on this platform";
}

bool available() { return g_available; }
const char* unavailableReason() { return g_available ? "" : g_why; }
Mode mode() { return g_mode; }

void setMode(Mode m) {
    if (!g_win) return;
    if (m == Integrated && !g_available) m = System;
    if (m == g_mode) return;
    g_mode = m;
    // GLFW_DECORATED can be changed on a live window; the platform bits go on
    // after it, because turning decorations off resets the window style.
    glfwSetWindowAttrib(g_win, GLFW_DECORATED, m == Integrated ? GLFW_FALSE : GLFW_TRUE);
    applyPlatform(m == Integrated);
    g_dragging = false;
    g_maxBtnHot = false;
}

void beginFrame(float uiScale) {
    g_scale = uiScale;
    g_margin = std::max(5.0f, 6.0f * uiScale);
    g_caption.clear();
    g_exclude.clear();
    g_maxBtn = Rect{};
}

void addCaption(float x0, float y0, float x1, float y1)   { g_caption.push_back({x0, y0, x1, y1}); }
void addExclusion(float x0, float y0, float x1, float y1) { g_exclude.push_back({x0, y0, x1, y1}); }
void setMaximizeButton(float x0, float y0, float x1, float y1) { g_maxBtn = {x0, y0, x1, y1}; }
bool maximizeButtonHot() { return g_maxBtnHot; }

bool maximized() {
    return g_win && glfwGetWindowAttrib(g_win, GLFW_MAXIMIZED) == GLFW_TRUE;
}
void minimize() { if (g_win) glfwIconifyWindow(g_win); }
void toggleMaximize() {
    if (!g_win) return;
    if (maximized()) glfwRestoreWindow(g_win);
    else             glfwMaximizeWindow(g_win);
}
void requestClose() { if (g_win) glfwSetWindowShouldClose(g_win, GLFW_TRUE); }

void endFrame() {
    if (!g_win || g_mode != Integrated) { g_dragging = false; return; }
    // a widget already has the mouse (a slider, a dock splitter next to the
    // border): the frame keeps its hands off
    const bool busy = uiBusy();

    int w = 0, h = 0;
    glfwGetWindowSize(g_win, &w, &h);
    const ImVec2 m = ImGui::GetMousePos();
    const bool inside = m.x >= 0 && m.y >= 0 && m.x < w && m.y < h;
    const int zone = inside ? hitZone(m.x, m.y, w, h, !maximized()) : HZ_NONE;

    // The cursor shape is ours even where the OS runs the resize: the ImGui
    // backend pushes a cursor every frame, so leaving it to WM_SETCURSOR alone
    // makes the arrow flicker along the edge.
    if (!g_dragging && !busy && (zone & (HZ_L | HZ_R | HZ_T | HZ_B)))
        ImGui::SetMouseCursor(cursorFor(zone));

    if (osHandlesDrag) return;      // WM_NCHITTEST already answered; nothing to do

    if (g_dragging) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_dragging = false;
        else stepManual();
        return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_pressZone = HZ_NONE;
    if (busy) return;

    // Double click on the bar is maximize/restore, as on a system title bar.
    if (zone == HZ_CAPTION && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        g_pressZone = HZ_NONE;
        toggleMaximize();
        return;
    }
    // A press only ARMS the drag; it starts once the mouse has actually moved.
    // Handing the pointer to the window manager on the press instead would eat
    // the second half of every double click.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g_pressZone = zone;
    if (g_pressZone == HZ_NONE ||
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f * g_scale))
        return;
    const int startZone = g_pressZone;
    g_pressZone = HZ_NONE;

#if defined(VIEWER_X11)
    if (g_wmMoveResize) { wmMoveResize(moveResizeDir(startZone)); return; }
#endif
    if (startZone == HZ_CAPTION && maximized()) {
        // dragging a maximized window restores it under the cursor, the way a
        // system title bar does
        const float rel = w > 0 ? m.x / (float)w : 0.5f;
        glfwRestoreWindow(g_win);
        int nw = 0, nh = 0;
        glfwGetWindowSize(g_win, &nw, &nh);
        int wx = 0, wy = 0;
        glfwGetWindowPos(g_win, &wx, &wy);
        glfwSetWindowPos(g_win, (int)(wx + m.x - rel * nw), wy);
    }
    beginManual(startZone);
}

}  // namespace window_frame
