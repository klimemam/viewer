// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---- how big to draw the UI (issue #257) ------------------------------------
//
// The report was "Linux のノート PC で文字が非常に大きく、ウィンドウも画面に
// 収まらない". The cause is not Linux and it is not HiDPI: it is that the
// program asked the wrong question. It asked WHICH OS, and the question that
// decides this is WHAT DO WINDOW COORDINATES MEAN.
//
//   PHYSICAL window coordinates (X11, Win32). One window unit is one device
//   pixel. At 200% nothing scales anything for us, so the app has to: the
//   style is doubled and the glyphs are baked at 34 px. That is what the code
//   has always done, and on X11 it is right.
//
//   LOGICAL window coordinates (Cocoa points, and Wayland). One window unit is
//   one *logical* pixel and the platform already multiplies: GLFW's Wayland
//   backend scales the framebuffer itself (GLFW_SCALE_FRAMEBUFFER, on by
//   default) and the ImGui GLFW backend then draws through
//   DisplayFramebufferScale = 2. An app that ALSO doubles its own style lands
//   at 4x on a 200% laptop - text, padding, and a restored window twice as
//   wide as the screen it is restored onto. Here the UI scale must be 1 and
//   only the FONT is baked large, with io.FontGlobalScale = 1/font putting it
//   back to one logical unit per point. That is exactly what the macOS branch
//   has always done; Wayland was falling into the X11 half.
//
// So the decision is made from the COORDINATE SYSTEM, and the OS name is only
// how the caller works out which one it is
// (logical = __APPLE__ || glfwGetPlatform() == GLFW_PLATFORM_WAYLAND).
//
// Pure, and separate from every GLFW call, because the platform that is wrong
// is the one platform this project cannot run: Windows and CI's xvfb Ubuntu
// both report a PHYSICAL platform, so the Wayland branch can only ever be
// exercised as a function of its inputs. --uiscale-selftest is that table.

// The window a manual override is allowed to ask for. Below 0.5 the menu bar
// is unreadable and above 4 a 1600x1000 window does not fit on any panel that
// exists - both ends are "the value cannot have been meant", which is why they
// are REFUSED by the settings/CLI layer rather than clamped (判断8: a file that
// asked for 12 meant something, and silently giving it 4 answers a question it
// did not ask).
static const float UI_SCALE_MIN = 0.5f;
static const float UI_SCALE_MAX = 4.0f;

static bool uiScaleOverrideValid(float s) {
    return std::isfinite(s) && s >= UI_SCALE_MIN && s <= UI_SCALE_MAX;
}

struct UiScaleDecision {
    float ui = 1.0f;      // the multiplier for style metrics and every `* uiScale`
    float font = 1.0f;    // the resolution glyphs are baked at
    const char* how = "auto";   // "auto", or the name of whatever overrode it
};

// contentScale is glfwGetWindowContentScale's x (1.0 where there is no window).
// overrideScale is 0 when nothing overrode it; `how` names the layer that did,
// for the one startup line and for the Preferences badge - the decision itself
// does not depend on it, which is why it is the last argument and defaulted.
static UiScaleDecision uiScaleDecision(bool logicalCoords, float contentScale,
                                       float overrideScale,
                                       const char* how = nullptr) {
    UiScaleDecision d;
    // An override means the same thing on both coordinate systems: "draw the UI
    // this many times life size". It is the escape hatch for the cases
    // detection cannot get right - XWayland with an Xft.dpi the compositor
    // disagrees with, fractional scaling, a remote desktop - so it must not be
    // re-interpreted by the very detection it exists to overrule.
    if (uiScaleOverrideValid(overrideScale)) {
        d.ui = d.font = overrideScale;
        d.how = how ? how : "override";
        return d;
    }
    // No content scale, or a nonsense one (a monitor list that answered 0, a
    // NaN out of a driver): 1.0 is what a 100% display would give, and it is
    // the only answer that cannot make the window unusable.
    const float cs = (std::isfinite(contentScale) && contentScale > 0.0f)
                     ? std::max(contentScale, 1.0f) : 1.0f;
    d.ui = logicalCoords ? 1.0f : cs;
    d.font = cs;                       // glyphs are baked at the display's resolution either way
    d.how = "auto";
    return d;
}

// WHICH layer set app.uiScaleOverride, for the startup line. nullptr while it
// is still at 0 (nothing overrode anything). The origin LEDGER
// (settingsOriginOf(SK_uiScale)) is the same fact for the Preferences badge;
// this is the copy the startup line can reach, because that line is printed
// while the font atlas is being built and takes no SKey.
static const char* g_uiScaleOverrideHow = nullptr;

// The platform, as the one startup line names it. Its own function so the line
// and the decision cannot disagree about which platform this is.
static const char* uiScalePlatformName(int glfwPlatform) {
    switch (glfwPlatform) {
        case GLFW_PLATFORM_X11:     return "x11";
        case GLFW_PLATFORM_WAYLAND: return "wayland";
        case GLFW_PLATFORM_WIN32:   return "win32";
        case GLFW_PLATFORM_COCOA:   return "cocoa";
        default:                    return "none";     // --no-window, or GLFW not up yet
    }
}
