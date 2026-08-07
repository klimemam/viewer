// viewer v0.1 — native image viewer for engineering data
// Features: .npy / .bin/.raw / .png/.jpg loading, hover pixel inspection, rulers,
//           zoom/pan, black/white point normalization.
#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION
  #include <OpenGL/gl3.h>            // 3.2+ core declarations
#elif defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>               // GL/gl.h needs APIENTRY/WINGDIAPI
  #include <GL/gl.h>
#else
  #include <GL/gl.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_internal.h"          // DockBuilder for the default layout
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "portable-file-dialogs.h"
#include "plugin_host.h"
#include "miniz.h"                   // deflate for compressed .npz members
#include "ui_theme.h"
#include "version.h"                 // the commit this binary was built from
#include "remote.h"
#include "adapter.h"                 // running an input adapter (docs/input-adapters.md §4)
#include "imagefile.h"               // PNG / JPEG / TIFF, behind one seam
#include "remote_proto.h"
#include "app_icon.h"
#include "window_frame.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <csignal>
#include <fstream>
// low-level IO for the crash handler: it must not go through iostreams
#include <fcntl.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/wait.h>              // the detached double-fork in spawnDetached
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>           // sysctlbyname for the physical-memory budget
#endif
#include <memory>
#include <unordered_map>
#include <unordered_set>              // residentImageBytes: dedupe by srcId
#include <mutex>
#include <condition_variable>
#include <limits>
#include <iomanip>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>                      // wall-clock stamp on measurement results
#include <thread>
#include <vector>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// state.h first (P6): self-contained, and util.inc below uses its pathFromUtf8.
#include "app/state.h"
// THE App instance. state.h declares it extern; the definition stays in the
// spine (docs/split-plan.md §6) — external linkage since P6, so the real TUs
// state.h exists for (core/analysis/, #84) link against this one object.
App app;

// P7 (docs/split-plan.md §3): Browse compiles as its own two TUs
// (core/browse/nav.cpp, core/browse/panel.cpp). What the spine, the fragments
// below and the selftests inside main() may call of it is declared in
// browse.h; the seam the browse side calls back through is g_browseHost
// (host.h), filled at the end of the include block where all fourteen targets
// are visible. host.h also declares the viewer-side helpers the browse TUs
// call by name (and owns struct PathSeg), so it precedes util.inc.
#include "browse/host.h"
#include "browse/browse.h"

#include "app/util.inc"

#include "app/remote_client.inc"

#include "app/series_model.inc"

#include "app/annotations.inc"

#include "app/plugin_glue.inc"

#include "app/loader_npz.inc"

#include "app/loader_npy_raw.inc"

#include "app/session.inc"

#include "app/sequence.inc"

#include "app/temporal_model.inc"

#include "app/open_dispatch.inc"

// ---------------------------------------------------------------- view helpers
static bool tileEngaged();   // fwd: defined with the tile geometry below
static void fitToCanvas(ImVec2 canvasSize) {
    ImageDoc* im = cur();
    if (!im || canvasSize.x < 10 || canvasSize.y < 10) return;
    // Fit spans the LARGER dimensions of what is on screen (user decision,
    // 2026-07-30): every side shares one view anchored at image (0,0), so
    // fitting A alone crops a bigger B. The extras join the union exactly when
    // the tiles have them on screen. Compare off: A alone, as always.
    float w = (float)im->w, h = (float)im->h;
    if (ImageDoc* b = cmpB()) {
        w = std::max(w, (float)b->w);
        h = std::max(h, (float)b->h);
    }
    if (tileEngaged())
        for (const ResolvedSlot& rs : resolveSlots()) {
            w = std::max(w, (float)rs.doc->w);
            h = std::max(h, (float)rs.doc->h);
        }
    app.view.zoom = std::min(canvasSize.x / w, canvasSize.y / h) * 0.97f;
    app.view.center = ImVec2(w * 0.5f, h * 0.5f);
}

static std::string fmtVal(float v, const std::string& dtype) {
    char b[64];
    if (dtype == "u8" || dtype == "u16" || dtype == "i8" || dtype == "i16" ||
        dtype == "u32" || dtype == "i32" || dtype == "bool")
        snprintf(b, 64, "%.0f", v);
    else
        snprintf(b, 64, "%.5g", v);
    return b;
}

// ---------------------------------------------------------------- UI
#include "ui/canvas.inc"
#include "ui/inspector.inc"
#include "ui/panel_histogram.inc"
#include "ui/panel_projection.inc"
#include "ui/modal_series.inc"
#include "ui/panel_series_analysis.inc"
#include "ui/panel_temporal.inc"
#include "ui/panel_rois.inc"
#include "ui/panel_analysis.inc"

#include "ui/modal_derive.inc"
#include "ui/file_list.inc"
#include "ui/menus.inc"
#include "app/cli.inc"

// ---------------------------------------------------------------- BrowseHost
// The browse -> viewer seam, filled (docs/split-plan.md §3 P7). Every target
// is a static function of THIS TU: the addresses cross the TU boundary, the
// internal linkage stays - de-statics were only paid where browse calls a
// helper by name (host.h lists those). Plain function pointers, so the table
// is constant-initialized and no browse call can ever beat it. This sits after
// cli.inc because the spine has only now seen all fourteen definitions.
static void browseWakeUi() { glfwPostEmptyEvent(); }   // rbWorker's UI wake
const BrowseHost g_browseHost = {
    &toast,
    &savePrefs,
    &browseWakeUi,
    &openRemote,
    &openRemoteStack,
    &openStackForAverage,
    &requestBrowseTemporal,
    &openPickerWith,
    &openReaderPicker,
    &browseFolderDialog,
    &selectImage,
    &promotePreview,
    &dropPreview,
    &stepPreviewFrame,
};

// ---------------------------------------------------------------- main
// Idle throttling: an immediate-mode UI normally redraws 60x per second, which
// over ssh X11 forwarding means pushing the whole window across the network
// continuously. Input wakes the loop; when nothing happens we block in
// glfwWaitEventsTimeout and draw nothing at all.
//
// The wake budget is a DEADLINE, not a frame count. ImGui's own timers run on
// io.DeltaTime - a submenu needs 300 ms of hovering, a modal backdrop 167 ms to
// fade - and three frames after an input is ~1 ms of wall clock on a fast
// machine, so those timers used to advance by almost nothing and fire only on
// the next idle timeout. Low-bandwidth mode restores the old frame-count
// behaviour for remote sessions.
static double g_wakeUntil = 0;
static uint64_t g_inputSeq = 0;      // bumped by every real input: idle can tell
                                     // a genuine event from a spurious wake-up
// Set once in main(); lets the window callbacks draw a frame themselves while the
// OS holds the thread in a modal loop (resizing / moving the window).
static std::function<void()> g_drawFrame;
static bool g_inFrame = false;
static void redrawNow() {
    if (!g_drawFrame || g_inFrame) return;   // never reenter a frame
    g_inFrame = true;
    g_drawFrame();
    g_inFrame = false;
}
static double g_lastInputAt = 0;      // for the input-latency readout
static void wakeUi(int frames = 3) {
    app.wakeFrames = std::max(app.wakeFrames, frames);
    g_inputSeq++;
    if (g_lastInputAt == 0) g_lastInputAt = nowSec();   // first of a burst
    if (!app.lowBandwidth) g_wakeUntil = nowSec() + 0.25;
}

// ---- window / taskbar icon ---------------------------------------------------
// Drawn, not loaded (core/app_icon.cpp), for one reason beyond having no asset
// file to ship: it can be redrawn while the app runs. A window looking at a
// server's pixels gets the green frame the status bar uses for a live peer, so
// two viewer buttons in the taskbar say which is which before you hover them.
// macOS is excluded: there glfwSetWindowIcon is documented to fail (the dock
// icon comes from the .app bundle, which tools/install_shortcut.sh writes).
static void applyWindowIcon(GLFWwindow* w, bool remote) {
#if !defined(__APPLE__)
    static const int SIZES[] = {16, 24, 32, 48, 64, 128};
    constexpr int N = (int)(sizeof(SIZES) / sizeof(SIZES[0]));
    // rendered once per variant and kept: the pixels must outlive the call, and
    // a reconnect must not re-rasterize six bitmaps
    static std::vector<unsigned char> pix[2][N];
    const int v = remote ? 1 : 0;
    GLFWimage imgs[N];
    for (int i = 0; i < N; i++) {
        if (pix[v][i].empty())
            pix[v][i] = app_icon::render(SIZES[i], remote ? app_icon::Remote : app_icon::Local);
        imgs[i].width = imgs[i].height = SIZES[i];
        imgs[i].pixels = pix[v][i].data();
    }
    glfwSetWindowIcon(w, N, imgs);
#else
    (void)w; (void)remote;
#endif
}

static void dropCallback(GLFWwindow*, int count, const char** paths) {
    wakeUi(4);
    for (int i = 0; i < count; i++) openPath(paths[i]);
}
static void installWakeCallbacks(GLFWwindow* win) {
    // installed BEFORE ImGui's backend, which chains to these
    glfwSetCursorPosCallback(win, [](GLFWwindow*, double, double) { wakeUi(); });
    glfwSetMouseButtonCallback(win, [](GLFWwindow*, int, int, int) { wakeUi(4); });
    glfwSetScrollCallback(win, [](GLFWwindow*, double, double) { wakeUi(4); });
    glfwSetKeyCallback(win, [](GLFWwindow*, int, int, int, int) { wakeUi(4); });
    glfwSetCharCallback(win, [](GLFWwindow*, unsigned int) { wakeUi(4); });
    glfwSetCursorEnterCallback(win, [](GLFWwindow*, int) { wakeUi(); });
    glfwSetWindowFocusCallback(win, [](GLFWwindow*, int) { wakeUi(4); });
    // these two fire from inside the OS resize/move modal loop: draw right here,
    // or the window stays frozen for as long as the user holds the edge
    glfwSetWindowSizeCallback(win, [](GLFWwindow*, int, int) { wakeUi(4); redrawNow(); });
    // ... and so does a move: with an integrated frame the OS runs the drag in
    // its own modal loop, and without this the window content freezes mid-drag
    glfwSetWindowPosCallback(win, [](GLFWwindow*, int, int) { wakeUi(2); redrawNow(); });
    glfwSetWindowRefreshCallback(win, [](GLFWwindow*) { wakeUi(2); redrawNow(); });
}

#if defined(_WIN32)
// The black window a double-click leaves behind, and why this is not solved by
// building for the WINDOWS subsystem.
//
// viewer.exe stays a CONSOLE-subsystem binary on purpose: cmd does not wait for
// a GUI-subsystem process, so `viewer --help`, `--bench N`, `--lin-selftest`
// and `--remote-selftest` would all return the prompt before printing anything,
// and their exit codes would stop reaching the shell (the VSCode tasks read
// both). The console is the cost of keeping the command line honest.
//
// It is only unwanted in one case, and that case is detectable: when nothing
// else is attached to the console, it was created FOR this process - Explorer,
// a desktop shortcut, a file association - rather than inherited from a shell,
// and closing it immediately is exactly right. Started from a terminal, that
// terminal is in the list too and this does nothing.
// (install_shortcut.ps1 also marks its shortcuts "start minimized", so the
// console does not even flash in the moment before we get here.)
static void dropOwnConsole() {
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) == 1) FreeConsole();
}
#endif

// ---- the GL context this binary asks for, and whether the machine has one ---
//
// The hints live in one function because the probe below and the real window
// must ask for exactly the SAME context. A machine that will hand out a context
// but not a 3.0 (3.2 core on macOS) one is precisely the case the probe exists
// to catch, and it would miss it the moment the two lists drifted apart.
// glslVersion travels with them: it is one decision, not two.
static const char* applyGlContextHints() {
#if defined(__APPLE__)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    return "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    return "#version 130";
#endif
}

// GLFW's own account of the last thing that went wrong. It is only STORED here,
// never printed on arrival: the failure sites decide what is worth saying, and
// say it once. Until this existed GLFW's reason was simply discarded, so every
// failure to make a window said "window creation failed" and nothing more -
// which is exactly what the Windows and macOS runners printed, once per test,
// with no way to tell a dead runner from a broken viewer.
static std::string g_glfwLastError;
static void glfwErrorSink(int code, const char* desc) {
    g_glfwLastError = "GLFW error " + std::to_string(code) + ": " +
                      (desc && *desc ? desc : "(no description)");
}
static std::string glfwReasonOr(const char* fallback) {
    return g_glfwLastError.empty() ? std::string(fallback) : g_glfwLastError;
}

// --gl-probe: "this machine cannot make a GL context" and "an assert failed"
// are different events, and until this existed nothing could tell them apart.
// Startup opens a real 1600x1000 window unconditionally, so every selftest but
// one dies inside glfwCreateWindow on a runner that has no context, and each of
// them reads as a broken test - which is how CI stayed red for hours with
// nothing in the output that said why. Answering it once, out loud, is the fix.
//
// The probe does what those tests do in their first moments - glfwInit, the
// same hints, one window - and when it cannot, it says WHY in GLFW's own words,
// taken from the error callback rather than guessed at from the failure site.
// Exit 0: there is a context, run everything. Exit 3: there is not, and
// tools/run_selftests.sh turns that into a named skip instead of a red matrix.
// It answers for the harness today; the app can ask it the same question later.
static int glProbe() {
    // The no-GL branch of run_selftests.sh has to be provable on a machine that
    // HAS GL, or nobody watches it work until CI does - and CI is unreadable.
    if (const char* f = getenv("VIEWER_FORCE_NO_GL"); f && *f && strcmp(f, "0")) {
        printf("no OpenGL context on this machine: forced by VIEWER_FORCE_NO_GL=%s\n", f);
        return 3;
    }
    glfwSetErrorCallback(glfwErrorSink);
    if (!glfwInit()) {
        printf("no OpenGL context on this machine: %s\n",
               glfwReasonOr("glfwInit failed, no GLFW error reported").c_str());
        return 3;
    }
    applyGlContextHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // a probe must never flash a window
    // This is the call the runners die in - 0.08-0.20 s per test, 21 times a run.
    GLFWwindow* w = glfwCreateWindow(64, 64, "viewer gl probe", nullptr, nullptr);
    if (!w) {
        printf("no OpenGL context on this machine: %s\n",
               glfwReasonOr("window creation failed, no GLFW error reported").c_str());
        glfwTerminate();
        return 3;
    }
    glfwMakeContextCurrent(w);
    // Named, because "which GL did that machine actually get" is the next
    // question every time a render result differs between a runner and a desk.
    const char* rend = (const char*)glGetString(GL_RENDERER);
    const char* ver  = (const char*)glGetString(GL_VERSION);
    printf("OpenGL context OK: %s / %s\n", rend ? rend : "(no renderer)",
                                           ver  ? ver  : "(no version)");
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}

// ---- restoring and remembering the window -----------------------------------
// The three things fitSavedWindow cannot know on its own, all of them questions
// about the machine at THIS moment rather than about the saved numbers. They sit
// here because they need glfwInit() to have happened and nothing else.

// The monitors as they are right now, work areas only. GLFW promises the primary
// monitor is first, which is what makes work[0] the sane fallback.
static std::vector<WinRect> monitorWorkAreas() {
    std::vector<WinRect> out;
    int n = 0;
    GLFWmonitor** m = glfwGetMonitors(&n);
    for (int i = 0; i < n && m; i++) {
        WinRect r;
        glfwGetMonitorWorkarea(m[i], &r.x, &r.y, &r.w, &r.h);
        if (r.w > 0 && r.h > 0) out.push_back(r);
    }
    return out;
}

// The scale to turn the SAVED logical size back into screen coordinates: the
// content scale of the monitor the saved position falls on, expressed the way
// startup expresses uiScale so the two can never drift apart. Asked before the
// window exists, which is the whole reason it goes by position rather than by
// glfwGetWindowContentScale.
static float scaleAtPos(int x, int y) {
#if defined(__APPLE__)
    (void)x; (void)y;
    return 1.0f;                     // Cocoa coordinates are points; the backend does the rest
#else
    int n = 0;
    GLFWmonitor** m = glfwGetMonitors(&n);
    GLFWmonitor* hit = (n > 0 && m) ? m[0] : nullptr;
    for (int i = 0; i < n && m; i++) {
        WinRect r;
        glfwGetMonitorWorkarea(m[i], &r.x, &r.y, &r.w, &r.h);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { hit = m[i]; break; }
    }
    if (!hit) return 1.0f;
    float sx = 1, sy = 1;
    glfwGetMonitorContentScale(hit, &sx, &sy);
    (void)sy;
    return sx > 0.1f ? std::max(sx, 1.0f) : 1.0f;
#endif
}

// What to remember, read off the live window.
//
// Maximized is a STATE, not a size. A maximized window reports the maximized
// rectangle, and writing that back would mean quitting maximized and coming back
// to a window that fills the screen but no longer knows how to restore DOWN. So
// the flag is always sampled and the rectangle only while the window is not
// maximized: together they say "come up maximized, and this is what the restore
// button gives back". Fullscreen is NOT in scope - this viewer has no fullscreen
// mode to be in; window_frame's two modes are System and Integrated, and both
// are ordinary windows.
//
// The divisor is app.uiScale, fixed at startup from the monitor the window was
// born on. It does not follow the window to a differently scaled screen, and it
// should not: it is the scale the app is actually DRAWING at, so it is the scale
// the size on screen actually means.
// A geometry change that has been seen but not yet written, waiting out the
// rate limit below. It is at file scope because the IDLE path has to know the
// loop still owes prefs.txt a write: the frame body is skipped entirely while
// nothing is happening, and "nothing is happening" is the precise state a
// window sits in for the two seconds after it was resized. Without this, a
// resize followed by a kill loses the resize.
static bool g_geomWriteDue = false;

static void sampleWindowGeometry(GLFWwindow* w) {
    app.winMax = glfwGetWindowAttrib(w, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (app.winMax) return;
    int x = 0, y = 0, ww = 0, hh = 0;
    glfwGetWindowPos(w, &x, &y);
    glfwGetWindowSize(w, &ww, &hh);
    if (ww <= 0 || hh <= 0) return;
    float s = app.uiScale > 0.1f ? app.uiScale : 1.0f;
    app.winX = x;
    app.winY = y;
    app.winW = (int)lround(ww / s);
    app.winH = (int)lround(hh / s);
}

#include "selftest/util.inc"

#include "selftest/anaprov.inc"

int main(int argc, char** argv) {
#if defined(_WIN32)
    {
        // --serve is the exception: its stdin/stdout are the protocol, and it is
        // started by ssh, never by a person clicking something.
        bool serve = false;
        for (int i = 1; i < argc; i++) serve |= !strcmp(argv[i], "--serve");
        if (!serve) dropOwnConsole();
    }
#endif
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printUsage(); return 0; }
    // Serve mode is what ssh starts on the machine holding the data: no window,
    // no GL, no socket - it answers pixel requests on stdin/stdout. It must be
    // handled before anything touches GLFW.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--serve")) return rp::runServeMode();
    {   // --remote-exe must reach the selftest too, or "test the standalone peer"
        // silently tests this binary against itself
        const char* rexe = nullptr;
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--remote-exe")) rexe = argv[i + 1];
        for (int i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--remote-selftest"))
                return remoteSelfTest(rexe ? rexe : argv[0], argv[i + 1]);
    }
    // Asked before prefs are read and before an autosave slot is claimed: a
    // question ABOUT the machine must not alter the machine it is asking about.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--gl-probe")) return glProbe();
    // --no-window: the startup path that makes no window and touches no GL.
    //
    // Startup created a window unconditionally, so every selftest but
    // --remote-selftest died inside glfwCreateWindow on a runner with no GL
    // context - even though most of them never draw anything. They load data,
    // call the panels' own helpers and assert on the state that comes back;
    // the window they were given was pure ceremony. This flag skips the whole
    // ceremony: no glfwInit, no window, no ImGui backends, no OpenGL symbol
    // called at any point. Everything else about startup is unchanged - prefs,
    // plugins, the ImGui context itself and the CLI are all still there,
    // because those are what the tests are made of.
    //
    // Which selftests can take it is declared ONCE, in CMakeLists.txt: the word
    // NOGL on a viewer_selftest() line both labels the test for
    // tools/run_selftests.sh and passes this flag. A test that DOES draw and is
    // marked NOGL by mistake stops at needWindow() below with its name and the
    // line to fix, rather than dying inside a backend with no device.
    bool noWindow = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--no-window")) noWindow = true;
    // Is this run SCRIPTED - a selftest, a bench, or the windowless path? Then
    // its window is a fixed 1600x1000 and nothing about it is written back.
    //
    // ba40ba8 stopped scripted runs reading the user's layout.ini for exactly
    // this reason, and tonight's benchmark that was silently measuring one panel
    // of three is what it costs when a measurement inherits geometry instead of
    // naming it. A remembered WINDOW size is the same hazard with the same
    // answer: a suite whose window is whatever the developer last dragged is a
    // suite that measures the developer. Read once, here, from argv, because
    // that is the only thing that exists this early - the layout.ini guard
    // below asks parseCli's g_browseKeys instead and is dead for it, which is a
    // mistake this must not copy.
    //
    // --window-offset and g_rbForceW stay exactly as they are: those are
    // overrides a run asked for BY NAME, which is the opposite of inheriting one.
    bool scriptedRun = noWindow;
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n >= 9 && !strcmp(argv[i] + n - 9, "-selftest")) scriptedRun = true;
        if (!strncmp(argv[i], "--bench", 7)) scriptedRun = true;
    }
    // --bench N: render N frames in a hidden window and report frame times, so
    // performance can be measured (and regressions caught) instead of guessed.
    int benchFrames = 0, crashAfter = 0, cliFrame = -1;
    int winOffX = 0, winOffY = 0;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--bench")) benchFrames = std::max(1, atoi(argv[i + 1]));
        // developer flag: verify the crash safety net actually writes a session
        if (!strcmp(argv[i], "--crash-test")) crashAfter = std::max(1, atoi(argv[i + 1]));
        // the window frame has to be decided before the window exists
        if (!strcmp(argv[i], "--frame")) cliFrame = !strcmp(argv[i + 1], "system") ? 0 : 1;
        // "Open in new window" cascade: applied right after the window exists
        if (!strcmp(argv[i], "--window-offset"))
            sscanf(argv[i + 1], "%d,%d", &winOffX, &winOffY);
    }
    // --secondary: spawned by "Open in new window". Known before loadPrefs so
    // nothing between here and there can write prefs back (see savePrefs).
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secondary")) g_secondary = true;
    // --bench-step: advance A by one frame before every benched frame. --bench
    // alone holds one image still, so every cache key stays hit and the loop
    // measures drawing only. The A/B question is the opposite one - what a
    // FRAME STEP costs when compareFollowFrame moves B too and both sides'
    // histogram / projection caches miss - and it cannot be measured without
    // actually stepping.
    bool benchStep = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-step")) benchStep = true;
    // --bench-tiles: arm two EXTRA compare slots for the bench, so the frames it
    // renders go through the tiled side-by-side path, and report the pane
    // rectangles drawCanvas actually produced. --tile-selftest proves the
    // geometry is right; this proves the canvas is the thing using it.
    bool benchTiles = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-tiles")) benchTiles = true;
    // --bench-panels: dock Projection, Histogram and Temporal side by side for
    // the bench instead of tabbing them, so all three DRAW. The default layout
    // puts them in one node, and ImGui::Begin returns false for a tab that is
    // not selected - so a bench on the default layout pays for exactly one of
    // the three, whichever happens to lead. docs/ab-stats-plan.md's A/B
    // measurement is specified with Histogram AND Projection showing, and until
    // ba40ba8 it got that by accident, from whatever layout.ini the machine
    // running the bench happened to have. That is why f424dae's numbers could
    // not be re-taken: a scripted run is now correctly isolated from the user's
    // layout, which also isolated it from the arrangement being measured. This
    // flag names the arrangement instead of inheriting it.
    bool benchPanels = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--bench-panels")) benchPanels = true;
    app.exePath = argv[0];
    // the out-of-the-box A/B range mode, read before anything can override it:
    // --range-selftest checks it, and prefs on the machine running the test
    // would otherwise decide what "the default" is
    g_abRangeDefault = app.compareRangeMode;
    {   // Which autosave is OURS: claim the lowest free slot before anything
        // reads or writes one - two windows sharing autosave.vsession is how
        // the second one eats the first one's session (todo-open item 28/A7).
        std::string cfgDir = viewerConfigDir();
        if (!cfgDir.empty()) g_instanceSlot = claimAutosaveSlot(cfgDir, g_instanceLockPath);
        // Selftests return from main() early and a normal quit has one exit
        // path already; atexit covers them all. A CRASH skips it on purpose:
        // the stale lock left behind is what marks that autosave reclaimable.
        atexit(releaseAutosaveLock);
    }
    loadPrefs();       // before the theme is applied and before the CLI is parsed
    if (cliFrame >= 0) app.frameMode = cliFrame;   // for this run only: not saved
    // Installed before anything can fail, so the two messages below can quote
    // GLFW instead of shrugging. Storing only - it prints nothing by itself.
    GLFWwindow* win = nullptr;
    const char* glslVersion = nullptr;
    if (!noWindow) {
        glfwSetErrorCallback(glfwErrorSink);
        if (!glfwInit()) {
            fprintf(stderr, "glfwInit failed: %s\n",
                    glfwReasonOr("no GLFW error reported").c_str());
            return 1;
        }
        glslVersion = applyGlContextHints();   // the same context --gl-probe asks for
        if (benchFrames || !g_browseKeys.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        // How a Linux desktop matches a window to its launcher: without these the
        // WM_CLASS is GLFW's own "GLFW-Application", the .desktop file written by
        // tools/install_shortcut.sh cannot claim the window, and GNOME/KDE show a
        // generic icon in a second, ungrouped dock entry. Ignored elsewhere.
        glfwWindowHintString(GLFW_X11_CLASS_NAME, "viewer");
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "viewer");
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, "viewer");
        // How big, and where. Restored from prefs unless this run has a reason
        // not to, and there are three:
        //   * a SCRIPTED run has to be reproducible on any machine (above);
        //   * a SECONDARY window is placed by the --window-offset cascade it was
        //     spawned with, and restoring the main window's geometry on top of
        //     that offset would undo the one thing the offset exists to do -
        //     it would also come up maximized over the parent that spawned it,
        //     which is the parent it was meant to sit beside;
        //   * nothing has been saved yet, which is every first run.
        // The size is created directly rather than set afterwards, so the window
        // never flashes at 1600x1000 on its way to the size it is supposed to be.
        int startW = WIN_DEF_W, startH = WIN_DEF_H;
        WinRect want;
        bool wantPos = false, wantMax = false;
        if (!scriptedRun && !g_secondary && !winOffX && !winOffY && app.winW > 0) {
            float s = scaleAtPos(app.winX, app.winY);
            want.x = app.winX;
            want.y = app.winY;
            want.w = (int)lround(app.winW * s);
            want.h = (int)lround(app.winH * s);
            wantPos = fitSavedWindow(want, monitorWorkAreas());
            startW = want.w;
            startH = want.h;
            wantMax = app.winMax;
        }
        win = glfwCreateWindow(startW, startH,
                               (std::string("viewer  ") + viewerVersion()).c_str(),
                               nullptr, nullptr);
        if (!win) {
            // THE line the Windows and macOS CI runners printed once per selftest,
            // with no reason attached - which is what made a machine with no GL
            // indistinguishable from 21 broken tests. GLFW knew why the whole time.
            fprintf(stderr, "window creation failed: %s\n",
                    glfwReasonOr("no GLFW error reported").c_str());
            fprintf(stderr, "  is it this machine or is it this test? "
                            "`viewer --gl-probe` answers that alone.\n");
            return 1;
        }
        applyWindowIcon(win, false);
        // The frame comes up before the first frame is drawn, so the window never
        // flashes a system title bar it is about to lose. --frame on the command
        // line wins over the preference for this run and is not written back: it is
        // the way out if a window manager makes a mess of the integrated one.
        window_frame::init(win);
        window_frame::setMode(app.frameMode ? window_frame::Integrated : window_frame::System);
        // After setMode, never before: switching the frame off changes the
        // decoration, and a window manager is entitled to move the window when
        // it does. Maximizing last, so the rectangle above stays the one the
        // restore button gives back.
        if (wantPos) glfwSetWindowPos(win, want.x, want.y);
        if (wantMax) glfwMaximizeWindow(win);
        if (winOffX || winOffY) {
            // "Open in new window" passes --window-offset 40,40: the child must not
            // land exactly on top of its parent, or nobody can tell it opened
            int wx = 0, wy = 0;
            glfwGetWindowPos(win, &wx, &wy);
            glfwSetWindowPos(win, wx + winOffX, wy + winOffY);
        }
        glfwMakeContextCurrent(win);
        glfwSwapInterval(benchFrames ? 0 : 1);   // benchmark must not be vsync-limited
        installWakeCallbacks(win);        // before ImGui's backend: it chains to these
        glfwSetDropCallback(win, dropCallback);
        g_haveWindow = true;
    }   // if (!noWindow)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // a blinking caret would force a redraw several times per second, which over
    // ssh X11 means retransmitting the window just to animate a cursor
    io.ConfigInputTextCursorBlink = false;
    // remember the panel layout between runs (user preference, not session data)
    static std::string iniPath;
    {
        std::error_code ec;
        std::filesystem::path cfg;
#if defined(_WIN32)
        if (const char* ad = getenv("APPDATA")) cfg = std::filesystem::u8path(ad);
#else
        if (const char* hm = getenv("HOME")) cfg = std::filesystem::u8path(hm) / ".config";
#endif
        // scripted runs (bench, browse-keys, --no-window) must neither read nor
        // write the user's layout - same rule the exit path applies to
        // autosave/prefs. A run with no window has no layout to remember either.
        //
        // NOT switched to scriptedRun, though the window geometry above is, and
        // it is worth writing down why rather than leaving it to look like an
        // oversight. `!g_browseKeys.empty()` is set by parseCli - several
        // hundred lines BELOW this point - so for the keys selftests it is
        // always false here, and only the APPDATA that CMakeLists.txt pins
        // keeps ctest off the developer's real layout.ini (docs/verify-ui.md
        // measured the md5 changing on a run by hand). Closing that hole makes
        // selftest.browse-dbl fail every time: its scripted double-click is
        // aimed at a panel whose place currently comes from the layout.ini an
        // EARLIER test in the same pinned home left behind. That is a real
        // defect and it is not this change's - a test that only lands its
        // clicks because of another test's leftovers needs its own geometry
        // pinned first, the way g_rbForceW already pins the width.
        if (benchFrames || !g_browseKeys.empty() || noWindow) cfg.clear();
        if (!cfg.empty()) {
            cfg /= "viewer";
            std::filesystem::create_directories(cfg, ec);
            if (!ec) {
                iniPath = (cfg / "layout.ini").u8string();
                io.IniFilename = iniPath.c_str();
                app.resetLayout = !std::filesystem::exists(cfg / "layout.ini", ec);
            }
        }
        if (iniPath.empty()) { io.IniFilename = nullptr; app.resetLayout = true; }
    }
    // One-time migration: the browser window was renamed "Remote" ->
    // "Browse###Remote". ImHashStr treats the ### part as the whole identity,
    // so a saved [Window][Remote] entry no longer matches and the panel would
    // silently lose its docked place. Rewrite the ini before ImGui reads it.
    if (!iniPath.empty()) {
        std::string txt;
        if (readWholeFile(iniPath, txt) &&
            txt.find("[Window][Remote]") != std::string::npos) {
            size_t p;
            while ((p = txt.find("[Window][Remote]")) != std::string::npos)
                txt.replace(p, strlen("[Window][Remote]"), "[Window][Browse###Remote]");
            std::ofstream mf(pathFromUtf8(iniPath), std::ios::binary);
            if (mf) mf << txt;
        }
    }

    float xs = 1, ys = 1;
    // 1.0 with no window: the scale is a property of the monitor the window
    // landed on, and there is neither. Everything downstream (uiScale, the font
    // size, ui_theme) then reads exactly what a 100% display would give.
    if (win) glfwGetWindowContentScale(win, &xs, &ys);
#if defined(__APPLE__)
    float uiScale = 1.0f;                    // Cocoa coords are points; backend handles px
    float fontScale = std::max(xs, 1.0f);    // rasterize glyphs at retina resolution
#else
    float uiScale = std::max(xs, 1.0f);
    float fontScale = uiScale;
#endif
    app.uiScale = uiScale;
    ui_theme::apply(app.themeVariant, app.themeAccent, uiScale, app.compactUi);
    std::string fontPath = jpFontPath();
    // The Japanese ranges plus U+2025 TWO DOT LEADER: stack names carry it
    // (frame_000‥023.npy - see rp::patternWithExtent), and a glyph the atlas
    // does not hold renders as a fallback '?', which is precisely the
    // uninformative character the extent exists to remove.
    static ImVector<ImWchar> fontRanges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        b.AddChar((ImWchar)0x2025);
        // ...and U+29C9 TWO JOINED SQUARES, the Files panel's share mark (§4).
        // Not every CJK font carries it: shareGlyph() checks the built atlas
        // and falls back to the word "shared" rather than showing '?'.
        b.AddChar((ImWchar)0x29C9);
        // GetGlyphRangesJapanese covers Latin-1 and the kana/kanji but NOT
        // Greek, so a table of noise figures could only write "sigma_f" where
        // it means one symbol. (Unicode has no subscript f or v, so the axis
        // letter rides alongside the symbol rather than under it.)
        b.AddChar((ImWchar)0x03C3);        // sigma
        b.AddChar((ImWchar)0x03BC);        // mu
        b.BuildRanges(&fontRanges);
    }
    ImFont* jp = fontPath.empty() ? nullptr
        : io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f * fontScale, nullptr,
                                       fontRanges.Data);
    if (!jp) {
        ImFontConfig cfg; cfg.SizePixels = 13.0f * fontScale;
        io.Fonts->AddFontDefault(&cfg);
        toast("CJK font not found - Japanese filenames may not display correctly", true);
    }
#if defined(__APPLE__)
    io.FontGlobalScale = 1.0f / fontScale;
#endif

    // The two BACKENDS - platform and renderer - are the only part of ImGui that
    // needs the window and the GL context. The context itself, the style, the
    // font atlas and every panel helper that computes rather than draws are
    // pure CPU and are set up above regardless, which is what lets a selftest
    // run with --no-window. Drawing a frame still needs these: see needWindow().
    if (win) {
        ImGui_ImplGlfw_InitForOpenGL(win, true);
        ImGui_ImplOpenGL3_Init(glslVersion);
    }

    // write the session on the way out of any crash, then let it crash normally.
    // The file is opened now, while opening files still works.
    openCrashFile();
    for (int sig : { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM }) signal(sig, crashHandler);
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);   // a dead ssh peer must not kill the viewer mid-write
#endif

    plugin_host::loadAll(
        { plugin_host::exeDir() + "/plugins", plugin_host::exeDir() + "/../plugins" },
        [](const std::string& m, bool err) {
            if (err) toast(m, true);
            fprintf(stderr, "%s\n", m.c_str());
        });

    parseCli(argc, argv);

    #include "selftest/picker.inc"

    #include "selftest/localbrowse.inc"

    #include "selftest/browse.inc"

    #include "selftest/rtemporal.inc"

    #include "selftest/batch.inc"

    #include "selftest/export.inc"

    #include "selftest/series.inc"

    #include "selftest/sweepfile.inc"

    #include "selftest/close.inc"

    #include "selftest/newwin.inc"

    #include "selftest/srcmap.inc"

    #include "selftest/media.inc"

    // The ROI table's numbers. Windowless, so it runs on every runner in the
    // matrix rather than only on the one that has a GL context - see
    // roiStatsSelftest(), and V19 below for the half of it that does draw.
    if (g_roiStatsSelftest) return roiStatsSelftest();

    // A and B on one document. Windowless for the same reason - what it asserts
    // is what the Statistics panel PRINTS, and that is CPU on both sides of the
    // window branch.
    if (g_abEqSelftest) return abEqSelftest();

    // Which dll computed the Analysis grid (#46 stage 1): the host's ledger,
    // through the real panel. Windowless because every assertion is a string.
    if (g_anaProvSelftest) return anaProvSelftest();

    #include "selftest/verify.inc"

    #include "selftest/derive.inc"

    #include "selftest/reload.inc"

    #include "selftest/stackavg.inc"

    #include "selftest/abstats.inc"

    #include "selftest/tile.inc"

    #include "selftest/scan.inc"

    #include "selftest/range.inc"

    #include "selftest/export-tsv.inc"

    #include "selftest/framestats.inc"

    #include "selftest/lin.inc"

    #include "selftest/seriespanel.inc"

    // Everything past this point draws. Every selftest that can run windowless
    // has returned above, so reaching here with no window means either
    // --no-window on a test that needs one (say so, by name) or --no-window on
    // a normal start, which is a window the user asked for and cannot have.
    if (!g_haveWindow) {
        if (!g_browseKeys.empty()) needWindow("--browse-keys-selftest");
        else if (benchFrames)      needWindow("--bench");
        else fprintf(stderr, "--no-window: there is nothing to run without a "
                             "window - it exists for the selftests that never "
                             "draw, and none was asked for.\n");
        return 1;
    }

    #include "selftest/browse-keys.inc"

    std::vector<double> benchMs;
    int benchLeft = benchFrames;
    if (benchFrames) {                 // exercise every panel, not just the defaults
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = true;
        app.showHistogram = app.showTemporal = app.showProjection = true;
        app.showSeriesAnalysis = true;      // ...which now includes the series selector
        benchMs.reserve(benchFrames);
    }
    if (benchTiles) {
        // ...except here. Every panel open squeezes the canvas to its 50 px
        // floor, where four panes are correctly REFUSED - which is the one
        // thing this run is not trying to measure.
        app.showFiles = app.showInspector = app.showRois = app.showAnalysis = false;
        app.showHistogram = app.showTemporal = app.showProjection = false;
        app.showSeriesAnalysis = false;
    }
    double lastFrameEnd = nowSec();
    // The frame body lives in a callable so it can also run from the window
    // refresh/size callbacks. Win32 runs a MODAL message loop while the user
    // drags a window edge: DispatchMessage never returns until the drag ends, so
    // a main loop that only draws between events shows a frozen window for the
    // whole resize. GLFW 3.4 sets no timer there, so the repaint has to come from
    // inside the callback.
    g_drawFrame = [&]() {
        double frameBodyT0 = nowSec();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        if (g_injMouse.x >= 0) {            // see g_injMouse: after the backend
            io.AddMousePosEvent(g_injMouse.x, g_injMouse.y);
            static int injHeld = -1;
            if (injHeld != g_injMouseBtn) {
                if (injHeld >= 0) io.AddMouseButtonEvent(injHeld, false);
                if (g_injMouseBtn >= 0) io.AddMouseButtonEvent(g_injMouseBtn, true);
                injHeld = g_injMouseBtn;
            }
        }
        ImGui::NewFrame();
        // --browse-keys "blur": give focus to nothing, so the next keys prove
        // the main view still owns them when the Browse panel does not
        if (g_browseKeysBlur) { g_browseKeysBlur = false; ImGui::SetWindowFocus(nullptr); }
        // ImGui hands input out over several frames (ConfigInputTrickleEventQueue):
        // NewFrame stops mid-queue on a wheel-after-move or a second action on one
        // button and KEEPS the rest for later. An event-driven loop that sleeps
        // there replays stale input at the idle timeout instead - one wheel notch
        // every 250 ms, for seconds, after the user has stopped. Measured: a
        // 30-event burst took ~15 s to drain; with this line, ~7 ms.
        if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) wakeUi(1);

        // shortcuts
        pollFileDialog();
        pollReader();          // a running reader, and what it has printed so far
        // modals (RAW dialog etc.) own the keyboard: no global shortcuts underneath —
        // Ctrl+W during reinterpret would shift the replaceIdx target image
        bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // The Browse panel binds the arrows, Home/End and Ctrl+F for its own
        // row navigation, gated on ITS focus. This block runs before any panel
        // draws and knows nothing about focus, so every key both of them bind
        // fires TWICE - once here on the main view, once there on the listing.
        // One rule, computed once, applied to every key the panel claims.
        ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow;
        bool remoteFocused = nav && nav->RootWindow &&
                             rbIsBrowseWindowName(nav->RootWindow->Name);
        if (!io.WantTextInput && !popupOpen) {
            // Cmd/Ctrl+O means "open what is selected" (2026-08-03, user), the
            // way it does on macOS - not "show me a file dialog". Inside Browse
            // it rides with Enter, where the selection is. Outside it, there is
            // no selection to open, so it goes to the place where there would
            // be one rather than doing nothing at all.
            if (!remoteFocused && ImGui::IsKeyChordPressed(MODK | ImGuiKey_O))
                rbShowInstance(rbActive());
            // Open Folder keeps its chord: nothing else claims it.
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Shift | ImGuiKey_O)) openFolderDialog();
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_W)) closeCurrent();
            // the layer variants (docs/terminology.md): frame-only escape hatch
            // and whole-batch close
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Alt | ImGuiKey_W)) closeCurrent(true);
            if (ImGui::IsKeyChordPressed(MODK | ImGuiMod_Shift | ImGuiKey_W) && cur())
                closeBatch(cur()->batchId);
            if (ImGui::IsKeyChordPressed(MODK | ImGuiKey_S)) saveSessionDialog();
            // Emacs-style navigation: time axis = C-f/C-b, stack axis = C-n/C-p,
            // sequence start/end = C-a/C-e (always Ctrl, also on macOS).
            // Ctrl+F is "find" while the Remote browser owns focus (it jumps to
            // the filter box there), so frame-stepping must yield to it.
            if (!remoteFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) gotoFrame(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) gotoFrame(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) gotoStack(1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) gotoStack(-1);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) gotoFrame(0, true, true);
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) gotoFrame(0, true, false);
            // Shift+C: the comparison PAST B. It has to be a chord - the
            // plain-key block below is gated on KeyMods == None, so a Shift
            // test inside it can never be true. (It was written there first,
            // and the item was simply dead.) Shift+Backslash is NOT an alias:
            // that is the swap, further down, and 5419de1 briefly binding both
            // keys to both actions made one press do the two at once.
            if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_C))
                showCompareSlots();
        }
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_None) {   // plain keys
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) app.fitRequested = true;
            if (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad1, false))
                app.view.zoom = 1.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_G, false)) app.showGrid = !app.showGrid;
            // C is an alias: ImGuiKey_Backslash follows the US scancode, which is
            // not where "\" sits on a JIS keyboard
            if (ImGui::IsKeyPressed(ImGuiKey_Backslash, false) ||
                ImGui::IsKeyPressed(ImGuiKey_C, false)) cycleCompare();
            // B is hold-to-see-B (handled in drawCanvas); Shift+B pins A as B.
            // In blink mode it is a toggle instead - "ぱちぱち" needs a tap, not
            // a held key. Space doubles as the toggle there (it only pans while
            // a drag is in progress, which a blink comparison never is).
            if (app.compareMode == App::CmpFlip && cmpB()) {
                if (ImGui::IsKeyPressed(ImGuiKey_B, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                    app.flipShowB = !app.flipShowB;
                    app.flipNext = ImGui::GetTime() + std::max(app.flipPeriod, 0.05f);
                }
            }
            // nudge the divider: small differences show up when you step it, not
            // when you sweep it with the mouse
            if (app.compareMode != App::CmpOff) {
                float& fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) fr = std::clamp(fr - 0.01f, 0.03f, 0.97f);
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) fr = std::clamp(fr + 0.01f, 0.03f, 0.97f);
            }
            // bare O is not bound either - same reasoning as the chord above
            if (ImGui::IsKeyPressed(ImGuiKey_H, false)) app.showHelp = !app.showHelp;
            // M = measure again: rerun the selected analyzer on the current
            // image and ROI set, and bring the Analysis panel forward. This is
            // the whole multi-image comparison loop: arrow key, M, read.
            if (ImGui::IsKeyPressed(ImGuiKey_M, false) && cur() &&
                !plugin_host::analyzers().empty())
                requestMeasure(app.anaSel);
            // P drops a pin at the pixel under the cursor - no click, no modifier
            if (ImGui::IsKeyPressed(ImGuiKey_P, false) && app.hoverX >= 0 &&
                !annBlockedOnPreview())
                addAnn(1, app.hoverX, app.hoverY, 0, 0);
            // X/Y: TOGGLE the selected ROI between its rect and full width / height.
            // Press once = row/column band, press again = restore the remembered rect.
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) toggleBand(true);
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) toggleBand(false);
            // arrows: horizontal = time (frames), vertical = stacks. These six
            // are the Browse panel's row navigation too, so they yield to it
            // exactly as Ctrl+F does - otherwise one Down both steps the browse
            // cursor AND moves the main view to the next STACK, and with
            // rangeScope = per frame that second effect REWRITES the stored
            // black/white of an image the user never navigated to.
            bool navKey = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                          ImGui::IsKeyPressed(ImGuiKey_Home, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_End, false);
            if (remoteFocused) {
                if (navKey) g_navKeyYielded++;
            } else {
                if (navKey) g_navKeyGlobal++;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) gotoFrame(1);
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) gotoFrame(-1);
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) gotoStack(1);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) gotoStack(-1);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) gotoFrame(0, true, true);
                if (ImGui::IsKeyPressed(ImGuiKey_End, false)) gotoFrame(0, true, false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && app.selectedAnn >= 0)
                deleteAnn(app.selectedAnn);
            // ESC steps outward: deselect the ROI, or - nothing selected -
            // leave the comparison, seats kept. ONE dispatch (escapePressed),
            // never two handlers racing on the same press.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) escapePressed();
            if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
                app.view.zoom = std::clamp(app.view.zoom * 2.0f, 1.0f / 512, 256.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
                app.view.zoom = std::clamp(app.view.zoom * 0.5f, 1.0f / 512, 256.0f);
        }
        // Shift+\ : flip which one is on top. C is NOT an alias here the way it
        // is for the plain key: Shift+C belongs to the slots (showCompareSlots,
        // above), and both bindings firing on one press swapped A and B at the
        // very moment you asked for the tiles. JIS keyboards keep the swap on
        // the status-bar button and the Files row menu.
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            ImGui::IsKeyPressed(ImGuiKey_Backslash, false))
            swapCompare();
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            ImGui::IsKeyPressed(ImGuiKey_B, false))
            pinCurrentAsB();
        if (!io.WantTextInput && !popupOpen && io.KeyMods == ImGuiMod_Shift &&
            app.compareMode != App::CmpOff) {   // Shift+[ ] : coarse divider steps
            float& fr = app.compareMode == App::CmpSplit ? app.splitFrac : app.wipeFrac;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) fr = std::clamp(fr - 0.10f, 0.03f, 0.97f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) fr = std::clamp(fr + 0.10f, 0.03f, 0.97f);
        }
        // the caption / exclusion rectangles are rebuilt every frame: menus grow
        // and shrink with the plugins that are loaded
        window_frame::beginFrame(uiScale);
        drawMenuBar(win);
        // before any panel draws: the B cache slots exist only while compare does
        abStatsFrame();
        // ...and before any panel draws, because it closes documents (§3.3)
        pumpReRead();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(2);

        const float STATUS_H = 26 * uiScale;
        // dock space fills everything above the status bar; panels dock into it
        ImGuiID dockId = ImGui::GetID("MainDock");
        ImGui::DockSpace(dockId, ImVec2(0, ImGui::GetContentRegionAvail().y - STATUS_H),
                         ImGuiDockNodeFlags_PassthruCentralNode);
        if (app.resetLayout) {        // first run, or View > Reset layout
            app.resetLayout = false;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, ImVec2(vp->WorkSize.x, vp->WorkSize.y - STATUS_H));
            ImGuiID center = dockId, left, right, bottom;
            left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
            right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
            bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
            ImGui::DockBuilderDockWindow("Files", left);
            ImGui::DockBuilderDockWindow("Browse###Remote", left);   // tabbed with Files
            ImGui::DockBuilderDockWindow("Image View", center);
            ImGui::DockBuilderDockWindow("Inspector", right);
            // tab order = dock order; Projection leads, and the explicit
            // focus below makes it the SELECTED tab, not merely the leftmost
            if (benchPanels) {          // --bench-panels: all three at once
                ImGuiID bp1 = bottom, bp2, bp3;
                bp2 = ImGui::DockBuilderSplitNode(bp1, ImGuiDir_Right, 0.66f, nullptr, &bp1);
                bp3 = ImGui::DockBuilderSplitNode(bp2, ImGuiDir_Right, 0.50f, nullptr, &bp2);
                ImGui::DockBuilderDockWindow("Projection", bp1);
                ImGui::DockBuilderDockWindow("Histogram", bp2);
                ImGui::DockBuilderDockWindow("Temporal", bp3);
            } else {
                ImGui::DockBuilderDockWindow("Projection", bottom);
                ImGui::DockBuilderDockWindow("Histogram", bottom);
                ImGui::DockBuilderDockWindow("Temporal", bottom);
            }
            ImGui::DockBuilderFinish(dockId);
            ImGui::SetWindowFocus("Projection");
            // ROIs and Analysis stay floating (they follow the work, not the frame)
            ImGui::SetWindowPos("ROIs", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                               vp->WorkPos.y + vp->WorkSize.y * 0.08f));
            ImGui::SetWindowSize("ROIs", ImVec2(620 * uiScale, 360 * uiScale));
            ImGui::SetWindowPos("Analysis", ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.34f,
                                                   vp->WorkPos.y + vp->WorkSize.y * 0.42f));
            ImGui::SetWindowSize("Analysis", ImVec2(560 * uiScale, 420 * uiScale));
        }

        if (app.showFiles) { if (ImGui::Begin("Files", &app.showFiles)) drawFileList(); ImGui::End(); }
        // Browse: N windows, one per instance. Instance 1 is "Browse###Remote"
        // (the singleton's id, so layouts and the dock builder keep working)
        // and only ever HIDES - View > Panels > Browse and the session's
        // "panels" line speak app.showRemote for it, exactly as before. The
        // others are "Browse N###BrowseN" and closing one destroys it (worker
        // joined first). Destruction is collected and done AFTER the loop:
        // the loop is iterating the vector it would mutate.
        {
            rbMain();                       // the primordial instance exists
            int destroyNum = 0;
            for (size_t bi = 0; bi < app.browsePanels.size(); bi++) {
                App::BrowseInstance& I = *app.browsePanels[bi];
                bool primordial = I.num == 1;
                bool& show = primordial ? app.showRemote : I.open;
                // an extra whose flag is already down (the window's X last
                // frame, or a scripted close) is destroyed here too - any
                // path that clears `open` closes the instance
                if (!show) { if (!primordial) destroyNum = I.num; continue; }
                // app.focusRemote = "bring the ACTIVE browse forward" (the
                // Temporal panel hands focus back through it); focusReq is an
                // instance asking for itself
                if (I.focusReq || (app.focusRemote && I.num == g_rbActiveNum)) {
                    ImGui::SetNextWindowFocus();
                    I.focusReq = false;
                    if (I.num == g_rbActiveNum) app.focusRemote = false;
                }
                // --browse-keys-selftest's "w<px>" action: float the panel at
                // an exact width so the toolbar's flow layout can be asserted
                // at the widths a human would drag it to. Off (0) otherwise.
                if (g_rbForceW > 0 && primordial) {
                    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
                    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8),
                                            ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(g_rbForceW,
                                                    g_rbForceH > 0 ? g_rbForceH
                                                                   : vp->WorkSize.y * 0.8f),
                                             ImGuiCond_Always);
                }
                if (!primordial)
                    ImGui::SetNextWindowSize(ImVec2(420 * uiScale, 520 * uiScale),
                                             ImGuiCond_FirstUseEver);
                // The title names the machine (10.3). Recomputed here, every
                // frame, because connecting and disconnecting change it - and
                // only the part BEFORE ### changes, so ImGui's identity (and
                // with it the docking, the layout file and the session) is
                // exactly what it was when the title was a fixed string.
                I.wtitle = rbPanelTitle(I.num, I.b.connected ? I.b.host : std::string());
                // "+" asked for this one to land beside the panel it came from.
                // Once only: after the first frame the panel is wherever the
                // user dragged it, and the layout file owns that.
                if (I.dockInto) {
                    ImGui::SetNextWindowDockID((ImGuiID)I.dockInto, ImGuiCond_Always);
                    I.dockInto = 0;
                }
                if (ImGui::Begin(I.wtitle.c_str(), &show)) drawPanelRemote(I);
                ImGui::End();
                if (!show && !primordial) destroyNum = I.num;
            }
            if (destroyNum) rbDestroyInstance(destroyNum);
        }
        if (app.showMessages) {
            ImGui::SetNextWindowSize(ImVec2(720 * uiScale, 300 * uiScale), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Messages", &app.showMessages)) drawMessagesPanel();
            ImGui::End();
        }
        if (ImGui::Begin("Image View", nullptr, ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse))
            drawCanvas(ImGui::GetContentRegionAvail());
        ImGui::End();
        if (app.showInspector) {
            if (ImGui::Begin("Inspector", &app.showInspector)) drawInspector();
            ImGui::End();
        }
        if (app.showHistogram) {
            if (ImGui::Begin("Histogram", &app.showHistogram)) drawPanelHistogram();
            ImGui::End();
        }
        if (app.showTemporal) {
            if (app.focusTemporal) {
                // Surface the tab so the browser-fired result is visible - but
                // give the keyboard straight back to the Browse panel (next
                // frame: its Begin ran earlier this one). Leaving focus here is
                // what killed the , / . preview scrub and the arrow keys right
                // after asking for server stats: the panel the user was
                // browsing in silently stopped being the panel that hears them.
                ImGui::SetNextWindowFocus();
                app.focusTemporal = false;
                app.focusRemote = true;
            }
            if (ImGui::Begin("Temporal", &app.showTemporal)) drawPanelTemporal();
            ImGui::End();
        }
        if (app.showProjection) {
            if (ImGui::Begin("Projection", &app.showProjection)) drawPanelProjection();
            ImGui::End();
        }
        if (app.showSeriesAnalysis) {
            ImGui::SetNextWindowSize(ImVec2(760 * uiScale, 520 * uiScale), ImGuiCond_FirstUseEver);
            // "###Linearity" keeps the window's ImGui IDENTITY while the title
            // says the layer (docs/analysis-layers.md §3.3 renamed the panel):
            // the Browse###Remote precedent, so saved docking survives via the
            // session loader's ini migration.
            if (ImGui::Begin("Series Analysis###Linearity", &app.showSeriesAnalysis))
                drawPanelSeriesAnalysis();
            ImGui::End();
        }
        if (app.showRois) {   // min size: the table stays readable even if dragged small
            ImGui::SetNextWindowSizeConstraints(ImVec2(460 * uiScale, 300 * uiScale),
                                                ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin("ROIs", &app.showRois)) drawPanelRois();
            ImGui::End();
        }
        if (app.showAnalysis) {
            ImGui::SetNextWindowSizeConstraints(ImVec2(420 * uiScale, 260 * uiScale),
                                                ImVec2(FLT_MAX, FLT_MAX));
            if (ImGui::Begin("Analysis", &app.showAnalysis)) drawPanelAnalysis();
            ImGui::End();
        }

        // status bar
        {
            ImageDoc* im = cur();
            char st[512];
            snprintf(st, 512, "no image");
            if (im) {
                std::string hover;
                // the readout follows the pane: in a split compare, values under
                // the cursor in pane B are B's, and must say so
                ImageDoc* hv = app.hoverInB && cmpB() ? cmpB() : im;
                if (app.hoverX >= 0 && app.hoverX < hv->w && app.hoverY < hv->h) {
                    hover = "  |  ";
                    if (cmpB()) hover += app.hoverInB ? "B " : "A ";
                    hover += "(" + std::to_string(app.hoverX) + ", " + std::to_string(app.hoverY) + ")";
                    if (hv->cfa)
                        hover += std::string(" [") + CFA_CH_NAMES[cfaChannelAt(*hv, app.hoverX, app.hoverY)] + "]";
                    hover += " =";
                    for (int c = 0; c < hv->ch; c++)
                        hover += " " + fmtVal(hv->sample(app.hoverX, app.hoverY, c), hv->dtype);
                }
                char seqInfoStr[64] = "";
                if (im->seqId != 0) {
                    std::vector<int> fr = framesOfSeq(im->seqId);
                    int pos = 0;
                    for (int i = 0; i < (int)fr.size(); i++) if (fr[i] == app.current) pos = i;
                    snprintf(seqInfoStr, sizeof seqInfoStr, "  frame %d/%d", pos + 1, (int)fr.size());
                }
                // zoom moved into the Image View footer (C3): magnification is
                // a property of the pixels being looked at, so it sits beside
                // them, not at the far edge of the screen
                snprintf(st, 512, "%s%s   %dx%d %dch %s%s",
                         im->name.c_str(), seqInfoStr, im->w, im->h, im->ch,
                         im->dtype.c_str(), hover.c_str());
            }
            // Remote link indicator, VSCode-style: the leftmost thing on the
            // status bar, always present while a server is involved. A toast
            // expires; "am I connected?" must be answerable at any moment.
            // One segment per Browse INSTANCE that has anything to say (a
            // phase, a connection or a failure) - with one panel open this is
            // byte-for-byte the old bar.
            for (size_t sbi = 0; sbi < app.browsePanels.size(); sbi++) {
                App::BrowseInstance& SI = *app.browsePanels[sbi];
                ImGui::PushID((int)(9100 + SI.num));
                std::string phase;
                { std::lock_guard<std::mutex> lk(SI.mtx); phase = SI.phase; }
                const App::RemoteBrowse& B = SI.b;
                if (!phase.empty()) {
                    // The spinner and the phase text both live in cells of a
                    // FIXED width. The font is proportional, so "|" and "/" are
                    // not the same width: spinning re-laid out the row eight
                    // times a second and walked the cancel button out from
                    // under the cursor - a button that dodges the pointer while
                    // you aim at it. Same for the phase, which changes length
                    // as it goes ("connecting to..." -> "listing...").
                    const char* spin[4] = { "|", "/", "-", "\\" };
                    const int fi = (int)(ImGui::GetTime() * 8) & 3;
                    float sw = 0;
                    for (const char* g : spin) sw = std::max(sw, ImGui::CalcTextSize(g).x);
                    const float pad = ImGui::GetStyle().ItemSpacing.x;
                    const float phaseW = ImGui::CalcTextSize("installing the viewer peer").x;
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    ImDrawList* sdl = ImGui::GetWindowDrawList();
                    const ImU32 amber = ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.4f, 1));
                    // centred in its cell, so it turns on the spot
                    float gw = ImGui::CalcTextSize(spin[fi]).x;
                    sdl->AddText(ImVec2(at.x + (sw - gw) * 0.5f, at.y), amber, spin[fi]);
                    sdl->PushClipRect(ImVec2(at.x + sw + pad, at.y),
                                      ImVec2(at.x + sw + pad + phaseW,
                                             at.y + ImGui::GetTextLineHeight()), true);
                    sdl->AddText(ImVec2(at.x + sw + pad, at.y), amber, phase.c_str());
                    sdl->PopClipRect();
                    ImGui::Dummy(ImVec2(sw + pad + phaseW, ImGui::GetTextLineHeight()));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", phase.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("cancel##rb")) {
                        // the in-flight ssh still finishes - but its result is
                        // now stale on arrival instead of destructive
                        SI.b.scanGen++;
                        SI.search.gen++;
                        SI.search.running = false;
                        std::lock_guard<std::mutex> lk(SI.mtx);
                        SI.queue.clear();
                        // The click must land NOW. The worker is still inside
                        // its blocking call and cannot notice the bump until
                        // the server answers, so the spinner kept saying
                        // "scanning..." long after the cancel - which read as
                        // the button doing nothing. "cancelling" is the honest
                        // phase: the remote side IS still working, only its
                        // answer is already condemned.
                        if (!SI.phase.empty())
                            SI.phase = "cancelling (the server is still finishing)...";
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                } else if (B.connected) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1), "%s  %s",
                                       peerTag(B.host), peerLabel(B.host).c_str());
                    if (ImGui::IsItemHovered()) {
                        // published counters plus this thread's own session: no
                        // draw-path code touches the worker's Session
                        uint64_t rx = B.rxBytes + app.uiSession.bytesReceived();
                        ImGui::SetTooltip(B.host.empty()
                            ? "browsing this machine  -  %s\nthe peer runs here, over a pipe: "
                              "nothing is connected and nothing is on the network\n"
                              "peer protocol v%d, %.1f MB read\nclick to close the browse"
                            : "connected  -  %s\npeer protocol v%d, %.1f MB received\n"
                              "click to disconnect",
                                          B.dir.c_str(), B.peerVersion, rx / 1048576.0);
                    }
                    if (ImGui::IsItemClicked()) {
                        app.uiSession.stop();     // ours to stop
                        App::RbJob j;             // the worker's is the worker's
                        j.kind = App::RbDisconnect;
                        rbEnqueue(SI, std::move(j));
                        SI.b = App::RemoteBrowse{};
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                } else if (!B.err.empty()) {
                    // The reason is the useful part: a tooltip hides it behind a
                    // hover nobody thinks to try. Show the first line inline and
                    // keep the full text one click away.
                    std::string first = B.err.substr(0, B.err.find('\n'));
                    if (first.size() > 90) first = first.substr(0, 87) + "...";
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s  %s",
                                       peerTag(B.host), first.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n\n(click for details)", B.err.c_str());
                    if (ImGui::IsItemClicked()) app.showRemoteError = true;
                    ImGui::SameLine();
                    ImGui::TextDisabled("|");
                    ImGui::SameLine();
                }
                ImGui::PopID();
            }
            ImGui::TextUnformatted(st);
            // compare is a persistent state: a toast that expires is not enough
            if (app.compareMode != App::CmpOff) {
                ImageDoc* b = cmpB();
                ImGui::SameLine();
                // amber for the states that need attention (no B, or paused on
                // it), the compare blue for a live pair - one text source, so
                // the selftest reads the exact sentence the bar shows
                ImGui::TextColored(b ? ImVec4(0.55f, 0.78f, 1.0f, 1)
                                     : ImVec4(0.95f, 0.72f, 0.35f, 1),
                                   "   |  %s", abStatusChipText().c_str());
                // swapping A and B is a thing you reach for constantly, so it needs
                // a visible control and not only a keyboard shortcut
                if (b) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("swap A/B")) swapCompare();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Put %s on the A side and %s on the B side"
                                          "   (Shift+\\)",
                                          b->name.c_str(), cur() ? cur()->name.c_str() : "");
                    // how the STATISTICS panels show the same pair, next to the
                    // divider setting they belong with (also in View > Compare A/B)
                    static const char* ABL[3] = { "stats: auto", "stats: overlay",
                                                  "stats: side by side" };
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::CalcTextSize(ABL[2]).x +
                                            ImGui::GetFrameHeight() * 1.4f);
                    int alv = std::clamp(app.abStatsLayout, 0, 2);
                    if (ImGui::BeginCombo("##abstatslayout", ABL[alv])) {
                        for (int i = 0; i < 3; i++)
                            if (ImGui::Selectable(ABL[i], alv == i)) app.abStatsLayout = i;
                        ImGui::EndCombo();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Histogram / Projection / Temporal layout for this\n"
                                          "comparison. Auto follows the image: split -> side by\n"
                                          "side, wipe / blink / difference -> overlay.");
                }
            }
            if (app.showFps) {
                // Frame time alone cannot tell you why the UI feels slow: this app
                // is event-driven, so the interesting number is how long an input
                // waited for its frame. Both are shown, plus the redraw mode,
                // because "low bandwidth" is the one setting that makes typing
                // feel like a teletype.
                ImGui::SameLine();
                ImGui::TextDisabled("   %.1f ms/frame (draw %.1f + present %.1f)   input %.1f ms (max %.0f)%s",
                                    1000.0f / ImGui::GetIO().Framerate, app.cpuMs, app.swapMs,
                                    app.inputLagMs, app.inputLagMaxMs,
                                    app.lowBandwidth ? "   [low bandwidth]" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("draw = building and rendering the frame here.\n"
                                      "present = handing it to the display (SwapBuffers):\n"
                                      "  large = the display path, not this app - a remote\n"
                                      "  X11 link ships the whole window every frame.\n"
                                      "input = event -> the frame answering it.\n"
                                      "Click to reset the maximum.");
                if (ImGui::IsItemClicked()) app.inputLagMaxMs = 0;
            }
            // A toast fades; the place to go read it afterwards has to be visible,
            // or "what did that error say?" has no answer.
            if (!app.msgLog.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("   |");
                ImGui::SameLine();
                if (app.msgUnreadErr) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.4f, 1));
                if (ImGui::SmallButton(app.msgUnreadErr ? "messages (!)" : "messages")) {
                    app.showMessages = true;
                    app.msgUnreadErr = false;
                }
                if (app.msgUnreadErr) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n\n(everything the app has said, copyable)",
                                      app.msgLog.back().text.c_str());
            }
            // Title bar and taskbar icon both carry "this window is looking at
            // another machine": the title for the window list and the tooltip,
            // the icon (green frame instead of blue) for the button itself.
            // With an integrated frame there is no system title bar to read the
            // title off - it is drawn in the menu bar instead - but it is still
            // what the taskbar button and the window list show.
            // ...and "another machine" is the same test the title uses: a local
            // browse is not a connection, so it gets no green frame either.
            const bool remoteNow = !viewingHost().empty();
            static std::string lastTitle;
            std::string title = windowTitleText();
            if (title != lastTitle) { glfwSetWindowTitle(win, title.c_str()); lastTitle = title; }
            static int lastIconVariant = -1;
            if ((int)remoteNow != lastIconVariant) {
                applyWindowIcon(win, remoteNow);
                lastIconVariant = (int)remoteNow;
            }
        }

        drawRawModal();
        drawSequenceModal();
        drawSeriesModal();
        drawDeriveModal();
        drawFolderPickModal();
        drawNpzPickModal();
        drawReaderPanel();
        drawReaderList();
        drawRemoteOpenModal();
        drawRemoteErrorWindow();
        drawHelpAbout();

        // toast
        if (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImVec2 ts = ImGui::CalcTextSize(app.toast.c_str());
            ImVec2 p(vp->WorkPos.x + std::max((vp->WorkSize.x - ts.x) * 0.5f, 12.0f * uiScale),
                     vp->WorkPos.y + vp->WorkSize.y - 60 * uiScale);
            fg->AddRectFilled(ImVec2(p.x - 12 * uiScale, p.y - 6 * uiScale),
                              ImVec2(p.x + ts.x + 12 * uiScale, p.y + ts.y + 6 * uiScale),
                              app.toastErr ? IM_COL32(90, 30, 30, 235) : IM_COL32(35, 42, 48, 235), 6);
            fg->AddText(p, IM_COL32(230, 235, 240, 255), app.toast.c_str());
        }

        ImGui::End();
        // Escape closes a menu or a context popup. ImGui does that itself only
        // with keyboard NAV enabled, and this app cannot enable it - the Browse
        // panel owns the arrow keys, and nav would take them - so a menu opened
        // by accident stayed open until it was clicked away. That is not just
        // untidy: an open menu OWNS the keyboard, and every accelerator typed
        // underneath it went nowhere (a Ctrl+W with the File menu up closed
        // nothing at all). Runs here, after every popup this frame has been
        // submitted, so the stack it reads is complete.
        //   - modals are left alone: theirs is a decision, and their own
        //     Cancel / Escape already owns the key.
        //   - an ACTIVE item gets the key first, so Escape in the "New batch"
        //     field inside Move to batch reverts the text instead of throwing
        //     the whole menu away.
        {
            bool escNow = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool anyPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                        ImGuiPopupFlags_AnyPopupLevel);
            if (escNow && !ImGui::IsAnyItemActive() && anyPopup) {
                ImGui::ClosePopupsExceptModals();
                escProbeNote("popup");                  // step 1
            } else if (escNow && io.WantTextInput) {
                // Step 2, and it is ImGui that performs it: an active text item
                // reverts its own edit. Both steps below are gated on this same
                // flag, so nothing else has run - which is exactly the claim
                // that had nowhere to be written down (E7).
                escProbeNote("textedit");
            } else if (escNow && g_escProbeFrame != ImGui::GetFrameCount()) {
                // The press reached no layer at all (a modifier held, say). It
                // still gets an entry: a chain that silently drops presses
                // cannot be read as a chain.
                escProbeNote("unclaimed");
            }
        }
        // after every panel has had its say about the mouse: drag, resize and
        // the cursor shape along the window edges
        window_frame::endFrame();
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        ImVec4 cc = ui_theme::clearColor(app.themeVariant);
        glClearColor(cc.x, cc.y, cc.z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Split the frame: our drawing versus getting it onto the screen. Over a
        // remote display the second number is the whole story (a full window per
        // frame across the link), and no amount of work on our side moves it.
        double swapT0 = nowSec();
        glfwSwapBuffers(win);
        app.swapMs = (float)((nowSec() - swapT0) * 1000.0);
        app.cpuMs = (float)((swapT0 - frameBodyT0) * 1000.0);
        if (g_lastInputAt != 0) {      // this frame answered an input: how late was it?
            float ms = (float)((nowSec() - g_lastInputAt) * 1000.0);
            app.inputLagMs = ms;
            app.inputLagMaxMs = std::max(app.inputLagMaxMs, ms);
            g_lastInputAt = 0;
        }
    };

    // Browse is open when the app comes up, standing where it stood last time
    // (2026-08-03, user). Until now the ONLY way to get a Browse panel was
    // File > Browse Folder (Local)..., which opens the OS folder dialog first -
    // so reaching the tool that replaces the OS dialog required going through
    // it. The place comes from rbRecents, which prefs.txt already carries: the
    // session's rbplace cannot serve here, because the autosave is offered by
    // a menu item and never restored on its own at startup.
    //
    // Not for scripted runs: --bench and the keys selftests drive their own
    // panels, and starting a local peer under them would change what they
    // measure. A CLI path was an explicit request to look at THAT, so the
    // panel stays where it is rather than jumping somewhere else.
    if (!benchFrames && g_browseKeys.empty() && !g_secondary && app.showRemote) {
        App::BrowseInstance& I = rbMain();
        if (!I.b.connected && I.b.host.empty() && I.b.dir == "~") {
            std::string place = app.rbRecents.empty() ? std::string() : app.rbRecents.front();
            if (place.empty()) {
#if defined(_WIN32)
                const char* h = getenv("USERPROFILE");
#else
                const char* h = getenv("HOME");
#endif
                if (h && *h) {
                    std::string hp = h;
                    std::replace(hp.begin(), hp.end(), '\\', '/');
                    place = "local://" + hp;
                }
            }
            if (!place.empty()) goToPlace(I, place);
        }
    }

    while (!glfwWindowShouldClose(win)) {
        double frameT0 = nowSec();
        // work that must keep animating even without input
        // rbBusy / mPending: a connect, a peer install or a server measurement is
        // in flight. Without these the idle path draws NOTHING while they run -
        // the window stops repainting and Windows paints it white and calls it
        // "not responding", which is exactly what a spinner is supposed to deny.
        bool busy = app.seqRunning || !app.seqQueue.empty() || app.rfPending > 0 ||
                    rbAnyBusy() || app.mPending > 0 ||
                    app.rdJob != nullptr ||          // a reader is running
                    app.anyFileDialog() ||
                    (!app.toast.empty() && ImGui::GetTime() < app.toastUntil) ||
                    // the A/B step throttle is a DEADLINE, not an event: without
                    // a frame after it expires, B's statistics stay stale until
                    // the user happens to move the mouse (the input wake tail is
                    // 0.25 s, the throttle 0.30, and low-bandwidth has no tail)
                    frameT0 < app.abStepBusyUntil ||
                    // auto blink alternates on a timer, so it needs frames with
                    // no input at all - same case as a background load
                    (app.compareMode == App::CmpFlip && app.flipAuto);
        // --bench on a FOLDER: accept the picker the way the Load button does,
        // and do not start counting until the loaders are idle. Without the
        // first, a folder given to --bench never loads at all; without the
        // second, the numbers are the decode, not the frame.
        static bool benchWarm = false;
        if (!g_browseKeys.empty()) { glfwPollEvents(); app.wakeFrames = 1; busy = true; }
        if (benchFrames) {
            glfwPollEvents(); app.wakeFrames = 1; busy = true;
            if (app.folderPickOpen && !app.folderPickRemote) pickerAccept();
            benchWarm = (app.seqRunning || !app.seqQueue.empty() || seqReadyPending() ||
                         app.folderPickOpen || app.rfPending > 0 ||
                         app.pendingCompare >= 0) &&
                        nowSec() < 600.0;
            // ...and only once the folder HAS loaded can the slots be armed: at
            // setup time app.images is still empty for a folder argument
            if (benchTiles && !benchWarm && app.cmpExtra.empty() && app.images.size() >= 4) {
                selectImage(0);
                setCompareB(app.images[1].get());
                addCompareSlot(app.images[2].get());
                addCompareSlot(app.images[3].get());
                app.compareMode = App::CmpSplit;
                fprintf(stderr, "benchtiles: armed %d extra slot(s), %d image(s), engaged=%d\n",
                        (int)app.cmpExtra.size(), (int)app.images.size(),
                        tileEngaged() ? 1 : 0);
            }
        }
        bool active = app.wakeFrames > 0 || frameT0 < g_wakeUntil;
        if (active || busy) {
            // Frame pacing lives here, not in the swap: the driver may ignore
            // vsync entirely (measured ~4000 fps with SwapInterval(1)), and this
            // wait returns the instant an event arrives, so it costs no latency.
            // (~100 fps in practice: the OS wait rounds up to its timer tick.)
            double budget = app.lowBandwidth ? 1.0 / 30.0 : 1.0 / 120.0;
            if (!active) budget = 0.08;        // background progress only
            double left = budget - (frameT0 - lastFrameEnd);
            // Never sleep on a backlog: ImGui hands out one trickled event per
            // frame, so pacing would show a 100-event fling one notch at a time.
            if (!ImGui::GetCurrentContext()->InputEventsQueue.empty()) left = 0;
            if (benchFrames || left <= 0.0005) {
                glfwPollEvents();
            } else if (app.lowBandwidth && !ImGui::GetIO().WantTextInput) {
                // Remote: the budget is a real cap, so keep waiting out the rest
                // of it even when events arrive - bandwidth beats latency here,
                // and it costs latency: ~32 ms on a streamed drag against 0.4 ms
                // in the default mode. The OS wait rounds up to its timer quantum
                // (15.6 ms on Windows), so the rate lands near 20-25 fps.
                //
                // TYPING IS EXEMPT. The bandwidth argument is about redraw floods
                // from mouse drags, not about 20 characters a second - and with
                // the cap applied, typing measured 32 ms per character (49 ms
                // p90) against 1.7 ms without it. That is the difference between
                // a text field and a teletype.
                do { glfwWaitEventsTimeout(left);
                     left = budget - (nowSec() - lastFrameEnd);
                } while (left > 0.0005);
            } else {
                glfwWaitEventsTimeout(left);   // returns at once on input
            }
            if (app.wakeFrames > 0) app.wakeFrames--;
        } else {
            // Idle: block until something happens. The caret does not blink
            // (ConfigInputTextCursorBlink=false), so text fields need no heartbeat.
            // The wait also returns on messages GLFW handles without calling any
            // callback; drawing a frame for those (let alone arming the 250 ms
            // wake tail) is what turned a 1 fps idle into a 40 fps one.
            uint64_t before = g_inputSeq;
            // While a text field owns the keyboard, a wake-up with no callback is
            // not proof that nothing happened: GLFW 3.4 has no IME support, so an
            // entire Japanese composition arrives as WM_IME_* and VK_PROCESSKEY
            // messages that it swallows without calling anything (win32_window.c
            // returns early on VK_PROCESSKEY). Skipping the frame there means the
            // window does not repaint for as long as the user is composing.
            bool typing = ImGui::GetIO().WantTextInput;
            // A worker that starts between the busy check above and this wait
            // must not leave the window unpainted: re-check before skipping.
            glfwWaitEventsTimeout(rbAnyBusy() || app.mPending > 0 ? 0.05 : 1.0);
            bool working = rbAnyBusy() || app.mPending > 0 || app.rfPending > 0 || app.seqRunning;
            // Results a worker has FINISHED but no frame has integrated yet, and
            // modals waiting for a frame to open them. Without these the remote
            // scan's picker never appeared: the worker went idle (working=false),
            // no input arrived, every frame was skipped - and the pumps live
            // inside the frame, so the result that would have opened the dialog
            // sat in rbDone until the user happened to move the mouse. Twice.
            if (!working) {
                working |= rbAnyDonePending();
                { std::lock_guard<std::mutex> lk(app.rfMtx); working |= !app.rfDone.empty(); }
                { std::lock_guard<std::mutex> lk(app.mMtx);  working |= !app.mDone.empty(); }
                working |= seqReadyPending();
                working |= app.folderPickOpen || app.seqAskImage >= 0 || app.remoteDlgOpen;
                working |= app.anyFileDialog();   // pollFileDialog lives in the frame
                working |= app.rdJob != nullptr;  // and so does pollReader
                working |= app.seriesEdit.open;   // the create/edit modal wants a frame
                working |= !app.rbOpenQueue.empty() || !app.seqQueue.empty();
                working |= !app.seqRestore.empty();
                // a session's series wait for their stacks, then need one frame
                working |= !app.seriesRestore.empty() || !app.seqLevelLegacy.empty();
                working |= !app.seriesPending.empty();   // ...and so do the picker's
                working |= nowSec() < app.abStepBusyUntil;   // B refresh pending
                // a tree node whose LIST is still out: its "(listing...)" row
                // has to become children without waiting for the mouse to move
                working |= rbAnyTreePending();
                // A window that was just resized owes prefs.txt a write, and it
                // sits perfectly still until the user does something else. The
                // write lives in the frame body, so the frame has to happen.
                working |= g_geomWriteDue;
            }
            // (--crash-test counts frames, so it must not be skipped)
            if (g_inputSeq == before && !typing && !working && !crashAfter) continue;
            app.wakeFrames = std::max(app.wakeFrames, 1);   // not wakeUi: no tail
        }
        lastFrameEnd = nowSec();
        if (crashAfter && --crashAfter == 0) raise(SIGSEGV);   // --crash-test
        if (!app.pendingLayout.empty()) {   // between frames: safe point to re-dock
            ImGui::LoadIniSettingsFromMemory(app.pendingLayout.c_str(), app.pendingLayout.size());
            app.pendingLayout.clear();
        }
        if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {   // minimised: draw nothing
            glfwWaitEvents();
            continue;
        }
        pumpSequenceAndQueue();       // integrate decoded frames, chain queued stacks
        pumpRemoteFetch();            // swap in full-resolution remote frames
        pumpMeasure();                // integrate server-side measurement results
        pumpRemoteBrowse();           // connect/list results from the browse worker
        pumpRemoteOpenQueue();        // folder-scan stacks, opened one at a time
        // --compare, deferred until the files (and their background-loaded frames)
        // are actually here. B is the first doc from a DIFFERENT source file, so a
        // stack on the command line does not end up compared against itself.
        if (app.pendingCompare >= 0 && !app.seqRunning) {
            if (app.pendingCompare == App::CmpOff || app.images.size() < 2) {
                if (app.images.size() < 2 && app.pendingCompare != App::CmpOff)
                    fprintf(stderr, "--compare needs two images\n");
            } else {
                selectImage(0);
                const ImageDoc* a0 = app.images[0].get();
                for (const auto& d : app.images) {
                    // A different STACK when A is in one. "A different source
                    // file" was the old rule and it picks frame_001.npy of A's
                    // OWN stack for a folder-of-frames capture - which is the
                    // self-comparison this loop exists to avoid, and it also
                    // pins compareFollowFrame (it only steps B across stacks).
                    bool other = a0->seqId != 0
                               ? d->seqId != a0->seqId
                               : (d->src->path != a0->src->path ||
                                  d->src->npzMember != a0->src->npzMember);
                    if (other) { setCompareB(d.get()); break; }
                }
                if (!resolveB()) setCompareB(app.images[1].get());   // same file twice
                app.compareMode = app.pendingCompare;
            }
            app.pendingCompare = -1;
        }
        {   // Preferences are tiny: write them the moment they change, so a crash
            // or a kill never costs the user their setup.
            static uint64_t lastPrefs = 0;
            uint64_t h = (uint64_t)app.themeVariant * 31 + (uint64_t)app.themeAccent * 131 +
                         (app.compactUi ? 1u : 0u) * 7 + (app.dragPans ? 1u : 0u) * 13 +
                         (app.wheelZoomPlain ? 1u : 0u) * 17 + (app.fitOnSwitch ? 1u : 0u) * 19 +
                         (uint64_t)app.seqLoadMode * 23 + (app.showFps ? 1u : 0u) * 29 +
                         (app.lowBandwidth ? 1u : 0u) * 43 +
                         (uint64_t)(app.dispGamma * 10) * 37 + (app.showGrid ? 1u : 0u) * 41 + 1;
            if (lastPrefs && h != lastPrefs) { app.prefsDirty = true; savePrefs(); }
            lastPrefs = h;
        }
        {   // The window's own geometry, on the same cadence but not in the hash
            // above: a checkbox changes once and is worth a write, whereas a
            // window changes on every frame of a drag and rewriting prefs.txt
            // sixty times a second to record a resize in progress is not saving
            // a preference, it is following a mouse. So the sample is taken
            // every frame and the WRITE is rate-limited to one every two
            // seconds, which bounds what a kill can cost to the last two
            // seconds of dragging. The exit path takes whatever the last change
            // left dirty, so a clean quit always records the final rectangle.
            //
            // Scripted runs and secondary windows never get here at all - the
            // first must not inherit or leave a geometry, the second must not
            // write prefs.txt at all (savePrefs says why).
            static uint64_t lastGeom = 0;
            static double lastGeomWrite = -1e9;
            static bool geomPrimed = false;
            if (win && !g_secondary && !scriptedRun &&
                !glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
                sampleWindowGeometry(win);
                uint64_t g = (uint64_t)(uint32_t)app.winX * 2654435761ull +
                             (uint64_t)(uint32_t)app.winY * 40503ull +
                             (uint64_t)(uint32_t)app.winW * 2246822519ull +
                             (uint64_t)(uint32_t)app.winH * 3266489917ull +
                             (app.winMax ? 1u : 0u);
                double nowG = nowSec();
                // The first sample is what the restore produced, not a change
                // the user made, so priming it keeps a launch-and-quit from
                // rewriting prefs.txt for nothing. It does NOT make a refused
                // position sticky: once the window settles anywhere the
                // fallback becomes the new saved position, and the geometry
                // from the screen that went away is gone. That is the choice
                // between two imperfect things - the alternative is freezing
                // the position for the whole run, which would also throw away
                // a move the user made deliberately on that same run. This one
                // always converges on where they last put it.
                if (!geomPrimed) { geomPrimed = true; lastGeom = g; lastGeomWrite = nowG; }
                else if (g != lastGeom) { lastGeom = g; g_geomWriteDue = true; app.prefsDirty = true; }
                if (g_geomWriteDue && nowG - lastGeomWrite > 2.0) {
                    g_geomWriteDue = false;
                    lastGeomWrite = nowG;
                    savePrefs();
                }
            }
        }
        {   // Autosave on change, debounced. A hard kill cannot run any handler,
            // so the safety net has to be written while things still work.
            static uint64_t lastState = 0;
            static double dirtySince = -1, lastAutosave = 0;
            uint64_t state = app.imagesRev * 1000003ull + app.annRev * 31ull +
                             (uint64_t)(app.current + 1) + (app.linkRange ? 7ull : 0) +
                             (uint64_t)(app.view.zoom * 1000) + (uint64_t)app.view.center.x +
                             (uint64_t)app.view.center.y + (uint64_t)(app.dispGamma * 10) +
                             (uint64_t)(app.abStatsLayout * 8192 + (app.histPlane + 1) * 65536) +
                             (uint64_t)(app.showGrid + app.histLog * 2 + app.anaSel * 4 +
                                        app.projMode * 64 + app.projYMode * 256 +
                                        app.roiChannel * 1024 + app.selectedAnn * 4096) +
                             (uint64_t)(app.showFiles + app.showInspector * 2 + app.showRois * 4 +
                                        app.showAnalysis * 8 + app.showHistogram * 16 +
                                        app.showTemporal * 32 + app.showProjection * 64) * 131ull;
            double nowA = nowSec();
            static double lastSnap = -1;
            if (state != lastState) { lastState = state; dirtySince = nowA; }
            // Re-render the crash copy far more often than the autosave debounce
            // (a crash 100 ms after a change should lose nothing) but not every
            // frame: dragging a ROI bumps annRev continuously.
            if (dirtySince >= 0 && nowA - lastSnap > 0.4) {
                lastSnap = nowA;
                refreshCrashSnapshot();
            }
            bool due = dirtySince >= 0 && nowA - dirtySince > 3.0 && nowA - lastAutosave > 5.0;
            if (!benchFrames && g_browseKeys.empty() && due && !app.images.empty()) {
                dirtySince = -1;
                lastAutosave = nowA;
                autosaveSession();
            }
        }
        // through the guard, never g_drawFrame() directly: the frame body pumps
        // Win32 messages while a file dialog is open (pfd's ready() calls
        // PeekMessage/DispatchMessage), which delivers WM_PAINT/WM_SIZE to our
        // own callbacks and would start a second ImGui frame inside this one.
        // IM_ASSERT is compiled out in release, so that corrupts silently.
        // --bench-step: one frame step per benched frame, bouncing off the ends
        // so every step is a real cache miss and never a no-op clamp at the last
        // frame. Stepped BEFORE the draw, so the frame we time is the one that
        // has to recompute (and, with compare on, recompute B as well).
        if (benchStep && !benchWarm && cur() && cur()->seqId != 0) {
            static int benchDir = 1;
            std::vector<int> bf = framesOfSeq(cur()->seqId);
            int bpos = 0;
            for (int i = 0; i < (int)bf.size(); i++) if (bf[i] == app.current) bpos = i;
            if (bpos >= (int)bf.size() - 1) benchDir = -1;
            else if (bpos <= 0) benchDir = 1;
            gotoFrame(benchDir);
        }
        // --browse-keys-selftest: one scripted UI action per 8 frames, replayed
        // into the REAL input queue so the panel cannot tell it from a human.
        if (!g_browseKeys.empty()) {
            static double reproT0 = nowSec();
            static int reproIdle = 0;
            static bool reproReady = false;
            static double reproActT0 = 0;      // the ACTION phase's own clock
            // A wall clock on the action phase, the same shape as the listing
            // phase's 60 s above. It had none: waitdir/waitimg bound each WAIT
            // at 60 s, and nothing bounds the run. Measured on this build, a
            // list with three impossible waits sits for 3m03s (60 s apiece);
            // the stock list carries fifteen waits, so an abnormal run - A-16's
            // shape, where a stale layout puts every scripted click somewhere
            // else and every wait therefore expires - waits about a quarter of
            // an hour and is then killed by ctest's own 900 s TIMEOUT, which
            // says only "Timeout" and names no action. This is eight times a
            // healthy run (~35 s) and comfortably inside that TIMEOUT, so the
            // test dies by its own hand, names the action it died on, and
            // FAILS. A suite that waits is a suite nobody runs.
            const double reproActBudget = 300.0;
            ImGuiIO& rio = ImGui::GetIO();
            auto reproKey = [](const std::string& a) -> ImGuiKey {
                if (a == "down")  return ImGuiKey_DownArrow;
                if (a == "up")    return ImGuiKey_UpArrow;
                if (a == "left")  return ImGuiKey_LeftArrow;
                if (a == "right") return ImGuiKey_RightArrow;
                if (a == "enter") return ImGuiKey_Enter;
                if (a == "home")  return ImGuiKey_Home;
                if (a == "end")   return ImGuiKey_End;
                if (a == "back")  return ImGuiKey_Backspace;
                if (a == "esc")   return ImGuiKey_Escape;
                if (a == "comma")  return ImGuiKey_Comma;    // preview scrub
                if (a == "period") return ImGuiKey_Period;
                return ImGuiKey_None;
            };
            if (!reproReady) {
                if (rbKeysT().b.connected && !rbKeysT().b.entries.empty()) {
                    reproReady = true;
                    reproActT0 = glfwGetTime();  // the actions start HERE, not
                                                 // at startup: the listing has
                                                 // its own 60 s below
                    rbMain().focusReq = true;    // = clicking the panel
                    fprintf(stderr, "browsekeys: listing ready, %d entr(ies)\n",
                            (int)rbKeysT().b.entries.size());
                } else if (nowSec() - reproT0 > 60.0) {
                    fprintf(stderr, "browsekeys: no listing for %s (%s)\n",
                            g_browseKeys.c_str(), rbKeysT().b.err.c_str());
                    break;
                }
            } else if (keyAct < keyActs.size() &&
                       glfwGetTime() - reproActT0 > reproActBudget) {
                fprintf(stderr, "browsekeys: action phase gave up after %.0f s at "
                                "action %d/%d '%s' (phase %d), dir=%s imgs=%d: FAILED\n",
                        glfwGetTime() - reproActT0, (int)keyAct, (int)keyActs.size(),
                        keyActs[keyAct].c_str(), keyPhase, rbKeysT().b.dir.c_str(),
                        (int)app.images.size());
                fflush(stderr);
                break;                          // keysOk stays false: rc = 1
            } else if (keyAct < keyActs.size()) {
                // the armed never-expanded watch (see g_expNeverPath): probed
                // HERE, once per frame, whatever the current action or phase -
                // this block runs before the frame's draw, so each probe reads
                // exactly the state the previous frame rendered
                if (!g_expNeverPath.empty()) {
                    g_expNeverFrames++;
                    if (rbHas(rbKeysT().expanded, g_expNeverPath)) g_expNeverHits++;
                }
                const std::string& a = keyActs[keyAct];
                // "op:arg" actions (checks, waits); op == a when there is no ':'
                size_t kColon = a.find(':');
                std::string op = a.substr(0, kColon == std::string::npos ? a.size() : kColon);
                int arg = kColon == std::string::npos ? -1 : atoi(a.c_str() + kColon + 1);
                std::string sarg = kColon == std::string::npos ? std::string()
                                                               : a.substr(kColon + 1);
                // what the preview / double-click checks assert against: the
                // live preview doc (if any), the scrub length, and the NAMES of
                // everything open - "2 images appeared" cannot say which one
                // was the stray, the names can
                auto pvDoc = []() -> ImageDoc* {
                    for (const auto& d : app.images)
                        if (d->uid == app.previewUid && d->preview) return d.get();
                    return nullptr;
                };
                auto scrubLen = []() {   // mirrors the scrub bar: files, or frame axis
                    return app.previewFiles.size() >= 2 ? (int)app.previewFiles.size()
                                                        : app.previewFrames;
                };
                auto imgNames = []() {
                    std::string s;
                    for (const auto& d : app.images) {
                        if (!s.empty()) s += ",";
                        s += d->name;
                        if (d->preview) s += "[pv]";
                    }
                    return s;
                };
                // a chk* line prints the state it judged, ok or FAIL - and a
                // FAIL fails the run (keysCheckBad feeds the summary gate)
                auto chk = [&](bool cond, const std::string& extra = std::string()) {
                    fprintf(stderr, "browsekeys: %-18s -> imgs=%d seqs=%d pv=%d scrub=%d "
                                    "idx=%d cur=%s%s%s: %s\n",
                            a.c_str(), (int)app.images.size(), (int)app.seqs.size(),
                            pvDoc() ? 1 : 0, scrubLen(), app.previewIndex,
                            cur() ? cur()->name.c_str() : "-",
                            extra.empty() ? "" : "  ", extra.c_str(),
                            cond ? "ok" : "FAIL");
                    fflush(stderr);
                    if (!cond) keysCheckBad++;
                };
                bool hold = false;                 // waitimg: stay on this action
                static size_t klogged = (size_t)-1;
                static int escProbeAtPress = 0;
                if (keyPhase == 0 && klogged != keyAct) {
                    klogged = keyAct;
                    // one press, one layer: the count is taken BEFORE the key
                    // goes into the queue, and read back at phase 7
                    if (a == "esc") escProbeAtPress = g_escProbeN;
                    // every action names what the panel is showing when it runs,
                    // so a crash log ends on the action that caused it
                    fprintf(stderr, "browsekeys: %2d %-6s dir=%s rows=%d imgs=%d preview=%s\n",
                            (int)keyAct, a.c_str(), rbKeysT().b.dir.c_str(),
                            (int)rbKeysT().b.entries.size(), (int)app.images.size(),
                            app.previewLabel.empty() ? "-" : app.previewLabel.c_str());
                    fflush(stderr);
                }
                if (op == "dbl" || op == "dbloff" || op == "click" || op == "clickoff" ||
                    op == "ctrlclick" ||
                    op == "chevclick" || op == "starclick" || op == "pinclick" ||
                    op == "mback" || op == "mfwd") {
                    // A real gesture into the real queue, aimed at the row the
                    // keyboard cursor is on - a double-click only exists as real
                    // clicks. "click" lands 40 px right of "dbl" (same row: rows
                    // span the table) so a click that follows a double-click can
                    // never chain into a triple. "ctrlclick" holds Ctrl around
                    // the click (multi-select). "chevclick" lands on the cursor
                    // row's CHEVRON (the tree's expand hit zone) and asserts the
                    // toggle landed within two frames of the press - a toggle
                    // deferred past the double-click window would still be
                    // unexpanded here. mback / mfwd press mouse
                    // buttons 4 / 5 (ImGui 3 / 4) over the listing: the history
                    // handler is gated on the panel being hovered or focused.
                    if (keyPhase == 0) {
                        if (op == "chevclick" && rbKeysT().cursorChev.x < 0)
                            chk(false, "cursor row has no chevron");
                        // "starclick" is aimed at the bookmark star at the right
                        // end of the PATH line - not at a row. It is a real
                        // click because the point of the move is that the star
                        // is reachable there, which only a real click can show.
                        if (op == "starclick" && rbKeysT().toolbar.starCentre.x < 0)
                            chk(false, "no bookmark star on the path line");
                        // "pinclick" is aimed at the OUTERMOST row the pinned
                        // band is holding - the folder that has scrolled off
                        // the top. The whole claim being tested is that it is
                        // still a row: the click goes to real pixels, and the
                        // verb that answers is the one that row has always had.
                        if (op == "pinclick" && rbKeysT().toolbar.pinCentre.x < 0)
                            chk(false, "nothing is pinned above the listing");
                        // "dbloff:N" lands N rows BELOW the cursor row - the one
                        // aim the keyboard cannot pre-warm. Every other click
                        // action is aimed at the cursor, and the keyboard put
                        // the cursor there, which on a file row has ALREADY
                        // previewed it: click one of such a double-click finds
                        // its preview live and costs nothing. A human's first
                        // contact with a row is the mouse, and click one then
                        // pays for the whole fetch - see rbActivateRow.
                        float rowH = rbKeysT().cursorRect[1].y - rbKeysT().cursorRect[0].y;
                        g_injMouse = op == "chevclick" ? rbKeysT().cursorChev
                                   : op == "starclick" ? rbKeysT().toolbar.starCentre
                                   : op == "pinclick"  ? rbKeysT().toolbar.pinCentre
                            : ImVec2((rbKeysT().cursorRect[0].x + rbKeysT().cursorRect[1].x) * 0.5f +
                                     (op == "click" ? 40.0f : 0.0f),
                                     (rbKeysT().cursorRect[0].y + rbKeysT().cursorRect[1].y) * 0.5f +
                                     ((op == "dbloff" || op == "clickoff") ? rowH * arg : 0.0f));
                    }
                    else if (keyPhase == 1 && op == "ctrlclick")
                        rio.AddKeyEvent(ImGuiMod_Ctrl, true);
                    else if (keyPhase == 1 && op == "chevclick")
                        g_chevPreExp = (int)rbKeysT().expanded.size();
                    else if (keyPhase == 2)
                        g_injMouseBtn = op == "mback" ? 3 : op == "mfwd" ? 4 : 0;
                    else if (keyPhase == 3) g_injMouseBtn = -1;
                    else if (keyPhase == 4 && (op == "dbl" || op == "dbloff")) g_injMouseBtn = 0;
                    else if (keyPhase == 4 && op == "chevclick")
                        chk((int)rbKeysT().expanded.size() != g_chevPreExp,
                            "expanded " + std::to_string(g_chevPreExp) + " -> " +
                            std::to_string((int)rbKeysT().expanded.size()) +
                            " by two frames after the press");
                    else if (keyPhase == 5 && (op == "dbl" || op == "dbloff")) g_injMouseBtn = -1;
                    else if (keyPhase == 5 && op == "ctrlclick")
                        rio.AddKeyEvent(ImGuiMod_Ctrl, false);
                } else if (op == "altleft" || op == "altright") {
                    // the keyboard mirror of mouse back/forward
                    ImGuiKey k2 = op == "altleft" ? ImGuiKey_LeftArrow : ImGuiKey_RightArrow;
                    if (keyPhase == 0) { rio.AddKeyEvent(ImGuiMod_Alt, true);
                                         rio.AddKeyEvent(k2, true); }
                    else if (keyPhase == 1) { rio.AddKeyEvent(k2, false);
                                              rio.AddKeyEvent(ImGuiMod_Alt, false); }
                } else if (keyPhase == 0) {
                    // ("more" - fold the drawer open - is gone with the drawer.)
                    if (a == "flat")       rbKeysT().flat = !rbKeysT().flat;
                    else if (a == "tree")  rbKeysT().tree = !rbKeysT().tree;
                    else if (a == "natorder")
                        rbKeysT().nameNatural = !rbKeysT().nameNatural;
                    else if (a == "viewreset") {
                        // an ABSOLUTE state pin: the toggles above are relative,
                        // and a segment that assumes grouped+list must not
                        // depend on the parity of every toggle before it
                        rbKeysT().flat = false;
                        rbKeysT().tree = false;
                        rbKeysT().nameNatural = true;
                        rbTreeForget(rbKeysT());
                    }
                    else if (a == "focus") rbShowInstance(rbKeysT());
                    // ---- Browse INSTANCES (item 17) ------------------------
                    else if (a == "reconnect") {
                        // reconnect the target to the test dir (post-"disc"),
                        // with a clean history - the checks below assert what
                        // the OTHER panel's navigation adds to it (nothing)
                        std::string kd = g_browseKeys;
                        std::replace(kd.begin(), kd.end(), '\\', '/');
                        while (kd.size() > 1 && kd.back() == '/') kd.pop_back();
                        rbKeysT().histBack.clear();
                        rbKeysT().histFwd.clear();
                        startRemote(rbKeysT(), "local://" + kd);
                    }
                    else if (a == "newpanel") {
                        // exactly what "+" / View > New Browse Panel do, then
                        // aim it at a DIFFERENT place than instance 1's
                        std::string kd = g_browseKeys;
                        std::replace(kd.begin(), kd.end(), '\\', '/');
                        while (kd.size() > 1 && kd.back() == '/') kd.pop_back();
                        App::BrowseInstance& NI = rbNewInstance();
                        g_rbKeysTarget = NI.num;
                        startRemote(NI, "local://" + kd + "/scanroot");
                    }
                    else if (op == "target") g_rbKeysTarget = arg;
                    else if (a == "closep") {
                        // the window's X: extras are destroyed by the frame loop
                        if (rbKeysT().num != 1) rbKeysT().open = false;
                        else app.showRemote = false;
                    }
                    else if (a == "hidep") app.showRemote = false;
                    else if (a == "showp") rbShowInstance(rbMain());
                    else if (op == "filt")
                        snprintf(rbKeysT().filter, sizeof rbKeysT().filter,
                                 "%s", sarg.c_str());
                    else if (op == "chkfilt")
                        chk(std::string(rbKeysT().filter) ==
                            (sarg == "-" ? std::string() : sarg),
                            std::string("filter=\"") + rbKeysT().filter + "\"");
                    else if (op == "chkpanels")
                        chk((int)app.browsePanels.size() == arg,
                            "panels=" + std::to_string((int)app.browsePanels.size()));
                    else if (op == "chkshown") {
                        int shown2 = 0;
                        for (auto& bp : app.browsePanels)
                            if (rbInstanceShown(*bp)) shown2++;
                        chk(shown2 == arg, "shown=" + std::to_string(shown2));
                    }
                    else if (op == "chksel") {
                        int nsel = 0;
                        for (char c2 : rbKeysT().sel) if (c2) nsel++;
                        chk(nsel == arg, "sel=" + std::to_string(nsel));
                    }
                    else if (a == "sessrt") {
                        // item 10 round-trip: write the session, tear every
                        // instance down, then replay the rbplace lines through
                        // the SAME helper the session reader uses. The checks
                        // that follow prove both instances reconnect - through
                        // their own workers - to the places they stood at.
                        std::ostringstream ss2;
                        writeSessionTo(ss2);
                        std::vector<std::pair<int, std::string>> places;
                        std::istringstream is2(ss2.str());
                        std::string l2;
                        while (std::getline(is2, l2))
                            if (l2.rfind("rbplace ", 0) == 0) {
                                std::istringstream pl(l2.substr(8));
                                int n2 = 0;
                                pl >> n2;
                                std::string u2;
                                std::getline(pl, u2);
                                while (!u2.empty() && u2.front() == ' ') u2.erase(0, 1);
                                places.emplace_back(n2, u2);
                            }
                        while (app.browsePanels.size() > 1)
                            rbDestroyInstance(app.browsePanels.back()->num);
                        rbMain().b = App::RemoteBrowse{};
                        rbTreeForget(rbMain());
                        fprintf(stderr, "browsekeys: sessrt saved %d rbplace "
                                        "line(s), all instances torn down\n",
                                (int)places.size());
                        for (auto& pr : places)
                            sessionRestoreBrowsePlace(pr.first, pr.second);
                        g_rbKeysTarget = 1;
                    }
                    // --------------------------------------------------------
                    else if (a == "rawopen") {
                        App::PendingGroup pg;
                        pg.files = { "a.raw", "b.raw" };
                        pg.isRaw = true;
                        app.seqQueue.push_back(pg);
                        rawDlg = RawDialog{};
                        rawDlg.path = "a.raw";
                        rawDlg.fileSize = 1024;
                        rawDlg.open = true;
                        rawDlg.forQueue = true;
                        rawDlg.queueCount = 1;
                    }
                    else if (a == "seqask") {          // the competing root modal
                        app.seqAskImage = 0;
                        app.seqAskFiles = { "a.npy", "b.npy" };
                        app.seqAskPattern = "?.npy";
                    }
                    else if (a == "popupcheck") {
                        if (g_popupChecks < 2)
                            g_popupCheck[g_popupChecks++] = g_rawPopupAlive ? 1 : 0;
                    }
                    else if (a == "disc") { rbKeysT().b = App::RemoteBrowse{}; rbTreeForget(rbKeysT()); }
                    else if (a == "fmenu") {
                        // A real click on the menu bar's "File". The move, the
                        // press and the release get a phase each: ImGui trickles
                        // a queued burst over several frames, and a click whose
                        // hover is still settling opens nothing.
                        if (ImGuiWindow* mb = ImGui::FindWindowByName("##MainMenuBar")) {
                            const ImGuiStyle& mst = ImGui::GetStyle();
                            g_injMouse = ImVec2(mb->Pos.x + mst.WindowPadding.x +
                                                mst.ItemSpacing.x +
                                                ImGui::CalcTextSize("File").x * 0.5f,
                                                mb->Pos.y + mb->Size.y * 0.5f);
                        }
                    } else if (a == "rctx" && rbKeysT().toolbar.rowY > 0) {
                        // ...and the other half of the defect: a right-click
                        // context popup on a listing row.
                        g_injMouse = ImVec2(rbKeysT().toolbar.rowX, rbKeysT().toolbar.rowY);
                    }
                    else if (a == "blur")  { g_browseKeysBlur = true;
                                             g_navKeyAtBlur = g_navKeyGlobal;
                                             g_navKeyYieldAtBlur = g_navKeyYielded; }
                    else if (a == "img0")  selectImage(0);
                    else if (a == "svtemp") {
                        // what the group row's "Temporal stats (server)" context
                        // item does - fired directly because a menu item cannot
                        // be scripted, the focus consequences are identical
                        if (!app.previewFiles.empty())
                            requestBrowseTemporal(app.previewHost, app.previewFiles,
                                                  "svtemp", app.previewPort);
                    }
                    else if (op == "chkfocus") {   // the TARGET instance owns the keys
                        ImGuiWindow* nw = ImGui::GetCurrentContext()->NavWindow;
                        bool f2 = nw && rbKeysT().wtitle == nw->Name;
                        chk(f2 == (arg != 0), nw ? nw->Name : "(no nav window)");
                    }
                    else if (op == "waitdir") {    // navigation lands async
                        static double waitD0 = 0;
                        std::string d3 = rbKeysT().b.dir;
                        size_t sl3 = d3.find_last_of('/');
                        bool at = (sl3 == std::string::npos ? d3 : d3.substr(sl3 + 1)) == sarg;
                        if (at) { chk(true, "dir=" + d3); waitD0 = 0; }
                        else if (waitD0 == 0) { waitD0 = nowSec(); hold = true; }
                        else if (nowSec() - waitD0 < 60.0) hold = true;
                        else { chk(false, "dir=" + d3); waitD0 = 0; }
                    }
                    else if (op == "idle") {       // burn N frames: a delay knob
                        static int idleLeft = -1;
                        if (idleLeft < 0) idleLeft = arg;
                        if (idleLeft > 0) { idleLeft--; hold = true; }
                        else idleLeft = -1;
                    }
                    else if (op == "waitimg") {    // fetches land between frames
                        static double waitT0 = 0;
                        if ((int)app.images.size() >= arg) { chk(true); waitT0 = 0; }
                        else if (waitT0 == 0) { waitT0 = nowSec(); hold = true; }
                        else if (nowSec() - waitT0 < 60.0) hold = true;
                        else { chk(false, imgNames()); waitT0 = 0; }   // timed out
                    }
                    else if (op == "chkimg")  chk((int)app.images.size() == arg, imgNames());
                    else if (op == "chkpv")                  // a live preview, LOOKED AT
                        chk(pvDoc() && cur() == pvDoc() && scrubLen() == arg);
                    else if (op == "chkidx") {               // the scrub sits at K...
                        ImageDoc* pv = pvDoc();
                        bool at = pv && app.previewIndex == arg && arg < scrubLen();
                        // ...and the slot really holds frame K, not just the label
                        if (at && app.previewFiles.size() >= 2)
                            at = pv->src->remoteUrl ==
                                 makeRemoteUrl(app.previewHost,
                                               app.previewFiles[arg], app.previewPort);
                        else if (at) at = pv->src->remoteFrame == arg;   // frame axis
                        chk(at, pv ? pv->src->remoteUrl : "-");
                    } else if (op == "chkopen") {            // registered, nothing else
                        bool okc = !pvDoc() && app.previewUid == 0 &&
                                   (int)app.seqs.size() == arg && app.previewFiles.empty();
                        for (const auto& d : app.images)     // no stray poster frame
                            if (d->preview) okc = false;
                        chk(okc, imgNames());
                    }
                    else if (op == "chkcur")  chk(pvDoc() && cur() == pvDoc());
                    else if (op == "chkcurn") chk(cur() && cur()->name == sarg);
                    else if (op == "chknames") {
                        // spec: pat[*N](+pat[*N])...; '#' matches one digit; a
                        // doc that is still a preview wears a [pv] suffix. The
                        // open images must match the multiset EXACTLY - the
                        // point is to NAME the surplus image, not to count it.
                        auto patMatch = [](const std::string& p, const std::string& s) {
                            if (p.size() != s.size()) return false;
                            for (size_t x = 0; x < p.size(); x++)
                                if (p[x] == '#' ? !isdigit((unsigned char)s[x])
                                                : p[x] != s[x]) return false;
                            return true;
                        };
                        std::vector<std::string> have;
                        for (const auto& d : app.images)
                            have.push_back(d->name + (d->preview ? "[pv]" : ""));
                        std::string missing;
                        for (size_t x = 0, y; x <= sarg.size(); x = y + 1) {
                            y = sarg.find('+', x);
                            if (y == std::string::npos) y = sarg.size();
                            if (y == x) { if (y == sarg.size()) break; continue; }
                            std::string item = sarg.substr(x, y - x);
                            int n = 1;
                            size_t st = item.rfind('*');
                            if (st != std::string::npos && st + 1 < item.size() &&
                                isdigit((unsigned char)item[st + 1])) {
                                n = atoi(item.c_str() + st + 1);
                                item.erase(st);
                            }
                            for (int q = 0; q < n; q++) {
                                bool found = false;
                                for (size_t h = 0; h < have.size(); h++)
                                    if (patMatch(item, have[h])) {
                                        have.erase(have.begin() + h);
                                        found = true;
                                        break;
                                    }
                                if (!found)
                                    missing += (missing.empty() ? "" : ",") + item;
                            }
                            if (y == sarg.size()) break;
                        }
                        std::string extra;
                        for (const auto& h : have)
                            extra += (extra.empty() ? "" : ",") + h;
                        chk(missing.empty() && extra.empty(),
                            "missing=[" + missing + "] surplus=[" + extra + "]");
                    }
                    else if (op == "chkdir") {     // the leaf of the browsed dir
                        std::string d3 = rbKeysT().b.dir;
                        size_t sl3 = d3.find_last_of('/');
                        chk((sl3 == std::string::npos ? d3 : d3.substr(sl3 + 1)) == sarg,
                            "dir=" + d3);
                    }
                    else if (op == "chkcursor") chk(rbKeysT().cursor == arg,
                                                    "cursor=" + std::to_string(rbKeysT().cursor));
                    else if (op == "chkatrow") chk(rbKeysT().cursorName == sarg,
                                                   "cursor row=" + rbKeysT().cursorName);
                    else if (op == "chkback") chk((int)rbKeysT().histBack.size() == arg,
                                                  "back=" + std::to_string((int)rbKeysT().histBack.size()) +
                                                  " fwd=" + std::to_string((int)rbKeysT().histFwd.size()));
                    else if (op == "chkfwd")  chk((int)rbKeysT().histFwd.size() == arg,
                                                  "back=" + std::to_string((int)rbKeysT().histBack.size()) +
                                                  " fwd=" + std::to_string((int)rbKeysT().histFwd.size()));
                    else if (op == "chkexp")  chk((int)rbKeysT().expanded.size() == arg);
                    // The band, read as text: every level the listing has
                    // scrolled past, outermost first, ";"-terminated - "-" for
                    // "nothing is pinned". This is the whole feature stated the
                    // way a human would read it off the screen, and it is a
                    // FRAME check rather than an arithmetic one on purpose: the
                    // rows have to have been laid out for the string to fill in.
                    else if (op == "chkpin")
                        chk(rbKeysT().toolbar.pinnedNames == (sarg == "-" ? std::string() : sarg),
                            "pinned=\"" + rbKeysT().toolbar.pinnedNames + "\"");
                    // ...and the one chkexp cannot say: is the row the cursor is
                    // ON expanded? chkexp counts the whole panel, so "this row
                    // toggled" was only expressible when nothing else in the
                    // panel was open - and the keyboard section far above leaves
                    // one open with Right, so it was not expressible at all.
                    // cursorFull is the exact key `expanded` is written in, at
                    // any depth, so this asks the question directly.
                    else if (op == "chkrowexp")
                        chk(rbHas(rbKeysT().expanded, rbKeysT().cursorFull) == (arg != 0),
                            "row \"" + rbKeysT().cursorFull + "\" expanded=" +
                            std::to_string(rbHas(rbKeysT().expanded, rbKeysT().cursorFull) ? 1 : 0) +
                            " of " + std::to_string((int)rbKeysT().expanded.size()) +
                            " expanded in the panel");
                    else if (a == "imgmark") g_rbImgMark = (int)app.images.size();
                    else if (a == "chkimgmark")
                        chk((int)app.images.size() == g_rbImgMark,
                            "opened " + std::to_string((int)app.images.size() - g_rbImgMark) +
                            " document(s) since the mark (" +
                            std::to_string(g_rbImgMark) + ")");
                    else if (a == "exparm") {
                        // arm the per-frame watch on the CURSOR row's path -
                        // the row the next gesture will be aimed at
                        g_expNeverPath = rbKeysT().cursorFull;
                        g_expNeverHits = g_expNeverFrames = 0;
                    }
                    else if (op == "chkexpn") {
                        chk(g_expNeverHits == arg && g_expNeverFrames > 0,
                            g_expNeverPath + " expanded on " +
                            std::to_string(g_expNeverHits) + "/" +
                            std::to_string(g_expNeverFrames) + " watched frame(s)");
                        g_expNeverPath.clear();
                    }
                    // ---- the "more" drawer is gone, and each thing it held is
                    // asserted in its NEW home (docs/browse-topbar-design.md
                    // 10.3). These are the only checks that can tell "moved"
                    // from "deleted".
                    else if (a == "chktitle") {
                        // The panel is named after the machine it stands on -
                        // the fact that used to be invisible until you unfolded
                        // the drawer, so that two panels were pixel-identical.
                        // BOTH directions, because the wrong one is a lie: a
                        // local panel must not claim ssh, and a remote one must
                        // not keep the plain name. And the ### id is untouched
                        // either way, or saved layouts stop docking the panel.
                        const App::BrowseInstance& T = rbKeysT();
                        bool ok = rbPanelTitle(1, "") == "Browse###Remote" &&
                                  rbPanelTitle(1, "trc2") == "Browse [ssh: trc2]###Remote" &&
                                  rbPanelTitle(2, "") == "Browse 2###Browse2" &&
                                  rbPanelTitle(2, "trc2") == "Browse 2 [ssh: trc2]###Browse2" &&
                                  // ...and the LIVE title of a local panel says
                                  // nothing about ssh (local:// is not remote)
                                  T.b.connected && T.b.host.empty() &&
                                  T.wtitle == rbPanelTitle(T.num, "") &&
                                  T.wtitle.find("[ssh:") == std::string::npos;
                        chk(ok, "live=\"" + T.wtitle + "\" ssh form=\"" +
                                rbPanelTitle(1, "trc2") + "\"");
                    }
                    else if (op == "chkstat") {
                        // The bottom status line, which is where every fact the
                        // drawer and the two orange bands used to carry now
                        // lives. Counts BOTH directions: the numbers it prints
                        // are the numbers the panel holds, and the "selected"
                        // clause is present exactly when a selection exists.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        int realSel = 0;
                        for (char c : rbKeysT().sel) if (c) realSel++;
                        char want[64], wantSel[64];
                        if (g.statusShown != g.statusTotal)
                            snprintf(want, sizeof want, "%d of %d items",
                                     g.statusShown, g.statusTotal);
                        else
                            snprintf(want, sizeof want, "%d item%s", g.statusTotal,
                                     g.statusTotal == 1 ? "" : "s");
                        snprintf(wantSel, sizeof wantSel, "%d of %d selected",
                                 g.statusSel, g.statusTotal);
                        bool says = g.statusFull.find(" selected") != std::string::npos;
                        bool ok = g.statusSel == arg && realSel == arg &&
                                  g.statusTotal > 0 &&
                                  g.statusFull.find(want) != std::string::npos &&
                                  says == (arg > 0) &&
                                  (arg == 0 ||
                                   g.statusFull.find(wantSel) != std::string::npos) &&
                                  // The host is NOT on this line: the panel
                                  // title names the machine, and the bottom row
                                  // says only what nothing else on screen says.
                                  // Kept as an assert pointing the other way
                                  // rather than deleted with the text it used
                                  // to check, or nothing would notice the host
                                  // coming back.
                                  g.statusFull.find(peerTag(rbKeysT().b.host)) ==
                                      std::string::npos &&
                                  // it is one line
                                  g.statusText.find('\n') == std::string::npos;
                        chk(ok, "\"" + g.statusFull + "\"");
                    }
                    else if (op == "chkframes") {
                        // What the selected ROWS stand for. One grouped row is
                        // one row and N frames, and the row count alone is the
                        // wrong answer to "am I about to open 24 files or 480".
                        // Both directions: the clause is absent when the two
                        // numbers are equal, or every ordinary selection would
                        // carry a redundant second count.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        char want[48];
                        snprintf(want, sizeof want, "%d frames", arg);
                        bool says = g.statusFull.find(" frames") != std::string::npos;
                        chk(arg == 0 ? !says
                                     : (says && g.statusFull.find(want) != std::string::npos),
                            "\"" + g.statusFull + "\"");
                    }
                    else if (a == "starmark") g_rbStar0 = rbKeysT().toolbar.starLit;
                    else if (op == "chkstar") {
                        // The star is a STATE before it is a verb: lit means
                        // this place is in the bookmark list. Asserted as a flip
                        // from the mark, because a scripted run inherits the
                        // user's real bookmarks and must not assume the test
                        // folder is absent from them.
                        const App::BrowseInstance& T = rbKeysT();
                        std::string u = placeUrl(T.b.host, T.b.port, T.b.dir);
                        bool listed = std::find(app.rbBookmarks.begin(),
                                                app.rbBookmarks.end(), u) !=
                                      app.rbBookmarks.end();
                        int wantLit = arg ? !g_rbStar0 : g_rbStar0;
                        bool ok = g_rbStar0 >= 0 &&
                                  T.toolbar.starLit == wantLit &&
                                  (T.toolbar.starLit != 0) == listed &&
                                  T.toolbar.starCentre.x > 0;   // and it is ON the path line
                        chk(ok, "star lit=" + std::to_string(T.toolbar.starLit) +
                                " bookmarked=" + std::to_string(listed ? 1 : 0) +
                                " " + u);
                    }
                    else if (a == "marklist") g_rbListTopY0 = rbKeysT().toolbar.listTopY;
                    else if (a == "seterr")
                        rbKeysT().b.err = "listing failed: no such file or directory";
                    else if (a == "clrerr") rbKeysT().b.err.clear();
                    else if (op == "chkerr") {
                        // A failure speaks on the status line and NOWHERE else.
                        // The old shape was a wrapped orange band above the
                        // listing: it said the failure in the one place the
                        // failure did not happen, and it pushed every file row
                        // down by one to three lines on its way in and out.
                        // listTopY is the proof - the listing does not move.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        bool inLine = g.statusFull.find("failed: ") != std::string::npos;
                        float moved = g.listTopY - g_rbListTopY0;
                        if (moved < 0) moved = -moved;
                        chk(inLine == (arg != 0) && moved <= 1.0f,
                            "list top " + std::to_string((int)g_rbListTopY0) + " -> " +
                            std::to_string((int)g.listTopY) + "  line=\"" +
                            g.statusFull + "\"");
                    }
                    else if (op == "setpv") {
                        if (g_rbPeerV0 < 0) g_rbPeerV0 = rbKeysT().b.peerVersion;
                        rbKeysT().b.peerVersion = arg;
                    }
                    else if (a == "pvback") {
                        if (g_rbPeerV0 >= 0) rbKeysT().b.peerVersion = g_rbPeerV0;
                        g_rbPeerV0 = -1;
                    }
                    else if (op == "chkproto") {
                        // The protocol-mismatch notice was the OTHER orange row
                        // above the listing. Same two claims as chkerr: it is on
                        // the status line, and it does not take a row from the
                        // files. The sentence names BOTH versions - a warning
                        // that says only the peer's leaves the reader guessing
                        // what it is being compared against.
                        const RbToolbarGeom& g = rbKeysT().toolbar;
                        char peer[32], mine[32];
                        snprintf(peer, sizeof peer, "protocol %d", rbKeysT().b.peerVersion);
                        snprintf(mine, sizeof mine, "speaks %d", (int)rp::VERSION);
                        bool said = g.statusFull.find("protocol") != std::string::npos;
                        bool both = g.statusFull.find(peer) != std::string::npos &&
                                    g.statusFull.find(mine) != std::string::npos;
                        float moved = g.listTopY - g_rbListTopY0;
                        if (moved < 0) moved = -moved;
                        chk(said == (arg != 0) && (arg == 0 || both) && moved <= 1.0f &&
                            // ...and the agreeing case is SILENT, which is the
                            // rule the whole redesign rests on
                            rbProtocolNote((int)rp::VERSION).empty(),
                            "peer v" + std::to_string(rbKeysT().b.peerVersion) +
                            " list top " + std::to_string((int)g.listTopY) +
                            "  line=\"" + g.statusFull + "\"");
                    }
                    else if (a[0] == 'w')  g_rbForceW = (float)atof(a.c_str() + 1);
                    // "h<px>" - a digit is required, or "home" and "hidep" would
                    // both be read as a height. It only bites while w<px> is on,
                    // which is where every check that uses it lives.
                    else if (a[0] == 'h' && a.size() > 1 && a[1] >= '0' && a[1] <= '9')
                        g_rbForceH = (float)atof(a.c_str() + 1);
                    else if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), true);
                } else if (keyPhase == 1) {
                    if (reproKey(a) != ImGuiKey_None) rio.AddKeyEvent(reproKey(a), false);
                } else if (keyPhase == 2) {
                    if (a == "fmenu") g_injMouseBtn = 0;
                    if (a == "rctx")  g_injMouseBtn = 1;
                } else if (keyPhase == 4) {
                    g_injMouseBtn = -1;
                } else if (keyPhase == 7 && (a == "fmenu" || a == "rctx" || a == "esc")) {
                    // Escape must close a menu. ImGui only does that with
                    // keyboard nav on, which this app cannot have, so the File
                    // menu stayed up - and an open menu owns the keyboard, which
                    // is how a Ctrl+W underneath it went nowhere. "fmenu" and
                    // "rctx" assert the click landed (without that, "esc"
                    // proves nothing); "esc" asserts the popup is gone.
                    bool up = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                          ImGuiPopupFlags_AnyPopupLevel);
                    bool want = (a != "esc");
                    if (up != want) keysCheckBad++;
                    fprintf(stderr, "browsekeys: after %-5s a popup is %s "
                                    "(cursor %.0f,%.0f): %s\n",
                            a.c_str(), up ? "open" : "closed",
                            rio.MousePos.x, rio.MousePos.y,
                            up == want ? "ok" : "FAIL");
                    fflush(stderr);
                    if (a == "esc") {
                        // ...and it was the POPUP layer that took it, and only
                        // that layer. "the popup went away" is compatible with
                        // the same press also reaching escapePressed() and
                        // dropping an ROI or leaving a comparison underneath -
                        // the exact failure the one-step-outward rule forbids,
                        // and the one thing this test could not see before
                        // g_escProbe existed (docs/verify-ui.md E7).
                        int took = g_escProbeN - escProbeAtPress;
                        std::string chain = g_escProbe.size() > 60
                            ? g_escProbe.substr(g_escProbe.size() - 60) : g_escProbe;
                        bool escOk = took == 1 &&
                                     g_escProbe.size() >= 6 &&
                                     g_escProbe.compare(g_escProbe.size() - 6, 6, "popup;") == 0;
                        if (!escOk) keysCheckBad++;
                        fprintf(stderr, "browsekeys: esc consumed by %d layer(s), "
                                        "chain ...'%s': %s\n",
                                took, chain.c_str(), escOk ? "ok" : "FAIL");
                        fflush(stderr);
                    }
                } else if (keyPhase == 7 && a == "disc") {
                    // View > Panels > Browse lands here with nothing open, and
                    // it must not be a remote-only dead end: the panel browses
                    // this disk too, so it has to say so without the File menu.
                    bool hasLocal = rbKeysT().toolbar.emptyLocalBtn != 0;
                    if (!hasLocal) keysCheckBad++;
                    fprintf(stderr, "browsekeys: empty state offers a local browse: %s\n",
                            hasLocal ? "ok" : "FAIL");
                    fflush(stderr);
                } else if (keyPhase == 7 && g_rbForceW > 0) {
                    // The toolbar's contract at ANY width: the filter box and
                    // the item that ENDS the row are inside the panel, and the
                    // filter is wide enough to hold a glob. Both used to be
                    // submitted past the right edge at the default docked width.
                    //
                    // SPEC CHANGE (drawer removal): the row's last item was the
                    // "more" fold button (g.moreR); it is the "..." panel menu
                    // now (g.menuR). The button being asserted changed because
                    // the button changed - the contract, "whatever ends the
                    // toolbar row is reachable at every width", did not, and it
                    // is still checked at the same seven widths below.
                    const RbToolbarGeom& g = rbKeysT().toolbar;
                    float slack = 1.0f;             // one pixel of rounding
                    bool inside = g.filterR <= g.x1 + slack && g.filterL >= g.x0 - slack &&
                                  g.menuR <= g.x1 + slack;
                    bool usable = g.filterR - g.filterL >= ImGui::GetFontSize() * 3.5f;
                    // ...and the bottom status line, at the same widths: it
                    // elides middle-out into the room left beside the one verb
                    // it carries, and it NEVER takes a second row (a status line
                    // that wraps is one more thing moving the listing).
                    bool statusOk = g.statusTextW <= g.statusAvailW + slack &&
                                    g.statusAvailW > 0;
                    // ...and the "modified" column: shown means it FITS. It is
                    // allowed to be absent (a panel too narrow to afford it) and
                    // it is allowed to be short - it is not allowed to print
                    // three characters of a sixteen-character stamp.
                    bool dateOk = g.dateTextW <= 0 || g.dateTextW <= g.dateCellW + slack;
                    if (!inside || !usable || !dateOk || !statusOk) keysCheckBad++;
                    fprintf(stderr, "browsekeys: panel w=%.0f content=[%.0f,%.0f] "
                                    "filter=[%.0f,%.0f] menu_r=%.0f date=%.0f/%.0f "
                                    "status=%.0f/%.0f: %s\n",
                            g_rbForceW, g.x0, g.x1, g.filterL, g.filterR, g.menuR,
                            g.dateTextW, g.dateCellW, g.statusTextW, g.statusAvailW,
                            (inside && usable && dateOk && statusOk) ? "ok" : "FAIL");
                    fflush(stderr);
                }
                if (!hold && ++keyPhase >= 8) { keyPhase = 0; keyAct++; }
            } else if (++reproIdle > 20) {
                // KEY ROUTING. The Browse panel binds Down/Up/Left/Right/Home/
                // End for its rows and the main loop's shortcut table binds the
                // same six for stack and frame navigation. Neither claims key
                // ownership, so before the focus gate one Down BOTH stepped the
                // browse cursor and ran gotoStack(1) on the main view - and
                // with rangeScope = per frame that rewrote the stored
                // black/white of an image the user never navigated to.
                // Both of the summary claims below rest on evidence one named
                // action collects ("blur", "popupcheck"). A --browse-keys list
                // that does not contain that action is not making the claim, so
                // it must not be JUDGED on it - without this, every overridden
                // list failed on two assertions about actions it never ran, and
                // the override was useful only for reading logs. Asked for by
                // the list rather than inferred from the counters: dropping
                // "blur" from the canned list then drops the claim visibly,
                // instead of turning it green by leaving no evidence.
                auto listHas = [&](const char* op) {
                    for (const auto& s2 : keyActs) if (s2 == op) return true;
                    return false;
                };
                int afterBlur = g_navKeyAtBlur < 0 ? -1 : g_navKeyGlobal - g_navKeyAtBlur;
                bool routeAsked = listHas("blur");
                bool routeOk = !routeAsked ||
                               (g_navKeyAtBlur == 0 && g_navKeyYieldAtBlur >= 6 &&
                                afterBlur >= 4);
                fprintf(stderr, "browsekeys: key routing: with the panel focused the "
                                "main view ran %d nav key(s) and stood down for %d; "
                                "after blur it ran %d more: %s\n",
                        g_navKeyAtBlur, g_navKeyYieldAtBlur, afterBlur,
                        !routeAsked ? "not asked for" : routeOk ? "ok" : "FAIL");
                // POPUP COLLISION. ImGui's OpenPopupEx closes whatever sits at
                // the current stack LEVEL when a different id opens there. The
                // RAW dialog used to be one-shot (its flag consumed the frame it
                // opened), so any competing root-level OpenPopup destroyed it for
                // good - and with rawDlg.forQueue set, the only code that clears
                // that flag lives inside the now-unreachable modal, so every
                // later Open Folder sat at "queued" until restart.
                bool popAsked = listHas("popupcheck");
                bool popOk = !popAsked ||
                             (g_popupCheck[0] == 1 && g_popupCheck[1] == 1 &&
                              (!rawDlg.forQueue || rawDlg.open));
                fprintf(stderr, "browsekeys: root popup collision: RAW dialog open "
                                "before the competing modal=%d, after=%d; forQueue "
                                "implies a live dialog=%d: %s\n",
                        g_popupCheck[0], g_popupCheck[1],
                        (!rawDlg.forQueue || rawDlg.open) ? 1 : 0,
                        !popAsked ? "not asked for" : popOk ? "ok" : "FAIL");
                keysOk = routeOk && popOk && keysCheckBad == 0;
                fprintf(stderr, "browsekeys: %d action(s) through real frames, "
                                "no crash, %d panel check(s) failed: %s\n",
                        (int)keyActs.size(), keysCheckBad,
                        keysOk ? "ok" : "FAILED");
                fflush(stderr);
                break;
            }
        }
        redrawNow();
        if (benchFrames && !benchWarm) {
            glFinish();               // include GPU work in the measurement
            benchMs.push_back((nowSec() - frameT0) * 1000.0);
            if (--benchLeft <= 0) break;
        }
    }
    if (benchFrames && !benchMs.empty()) {
        std::vector<double> s(benchMs.begin() + std::min<size_t>(5, benchMs.size() - 1), benchMs.end());
        std::sort(s.begin(), s.end());
        double sum = 0;
        for (double v : s) sum += v;
        fprintf(stderr,
                "bench: frames=%zu mean=%.2fms median=%.2fms p95=%.2fms max=%.2fms (%.0f fps median)\n",
                s.size(), sum / s.size(), s[s.size() / 2], s[(size_t)(s.size() * 0.95)],
                s.back(), 1000.0 / std::max(s[s.size() / 2], 1e-6));
    }
    if (benchTiles) {
        fprintf(stderr, "benchtiles: drawCanvas tiled %d pane(s) on the last frame "
                        "(canvas %.0f px; -1 = not tiled, 0 = refused as too narrow)\n",
                g_tilePanesDrawn, g_tileCanvasW);
        for (int i = 0; i < g_tilePanesDrawn && i < 16; i++)
            fprintf(stderr, "benchtiles: pane %d x0=%.2f w=%.2f\n",
                    i, g_tilePaneX0[i], g_tilePaneW[i]);
    }

    // it captures main's locals by reference, and teardown can still fire callbacks
    g_drawFrame = nullptr;
    // a selftest must not leave its scripted clicks in the user's session or
    // their preferences (the frameless ones return before ever getting here)
    if (g_browseKeys.empty()) {
        autosaveSession();            // also covers a normal quit
        // Only what the user changed in this run: a one-off --stack flag or
        // the gamma inside a --session must not quietly become the default.
        if (app.prefsDirty) savePrefs();
    }
    stopSequenceLoader();             // join the workers before tearing anything down
    stopRemoteFetcher();
    stopMeasureWorker();
    stopRbWorker();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    plugin_host::unloadAll();
    if (!g_browseKeys.empty() && !keysOk) {
        fprintf(stderr, "browsekeys: FAILED (the action list did not finish)\n");
        return 1;
    }
    return 0;
}
