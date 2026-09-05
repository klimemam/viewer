// core/main.cpp から #include される断片。単独ではコンパイルしない。担当: ハーネス基盤
// ---- --uiscale-selftest: how big to draw the UI (#257) ----------------------
// Issue #257「Linux(Wayland)で UI が二重に拡大される」. The report was a laptop
// where the text was enormous and the window did not fit on the screen, and the
// cause was one question asked wrongly: the code asked WHICH OS when what
// decides this is WHAT A WINDOW UNIT MEANS.
//
// This test exists because THE FAILING PLATFORM CANNOT BE RUN HERE. Windows
// reports GLFW_PLATFORM_WIN32 and CI's Ubuntu runs under xvfb, i.e. X11 - both
// PHYSICAL, both the branch that was already right. Wayland is reachable only
// as an input, so core/app/uiscale.inc is pure and this is its table. Nobody
// should mistake a pass here for "it was tried on a Wayland laptop": what is
// asserted is the arithmetic the laptop will run, and that is the most this
// machine can honestly claim.
//
// The rows are the issue's own decision table, plus the restore half of the
// same bug: a saved 4000x3000 window must not come back larger than the screen.
//
// NOGL and fixture-free on the framesize/histhl pattern, and one step further -
// uiScaleDecision() and fitSavedWindow() are PURE, so this test touches no App
// field at all and has nothing to give back to the shared selftest home.
static int uiScaleSelftest() {
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        fprintf(stderr, "uiscaleselftest: %-62s %s\n", what, cond ? "PASS" : "FAIL");
        if (!cond) ok = false;
    };
    const bool LOGICAL = true, PHYSICAL = false;

    {   // ---- U1..U5: the decision table --------------------------------------
        // Every expected pair is derived by hand from one sentence: a LOGICAL
        // platform has already multiplied, so the UI scale it still owes is 1
        // and only the glyphs are baked large; a PHYSICAL one has multiplied
        // nothing, so the app owes the whole factor.
        struct Row {
            bool logical;
            float content, over;
            float wantUi, wantFont;
            const char* wantHow;
            const char* what;
        };
        const Row rows[] = {
            // X11 / Win32 at 200%: unchanged from before this issue, and that is
            // the half that must not move.
            { PHYSICAL, 2.0f, 0.0f, 2.0f, 2.0f, "auto",
              "U1 physical 200% -> ui 2 font 2 (the X11 behaviour, unchanged)" },
            // Wayland / Cocoa at 200%: THE BUG. ui 2 here is 4x on screen.
            { LOGICAL,  2.0f, 0.0f, 1.0f, 2.0f, "auto",
              "U2 logical 200% -> ui 1 font 2 (the platform already doubled)" },
            // An override means the same thing on both, or the escape hatch
            // would be reinterpreted by the detection it exists to overrule.
            { PHYSICAL, 2.0f, 1.5f, 1.5f, 1.5f, "--ui-scale",
              "U3 physical + override 1.5 -> ui 1.5 font 1.5" },
            { LOGICAL,  2.0f, 1.5f, 1.5f, 1.5f, "--ui-scale",
              "U4 logical + override 1.5 -> the same 1.5 / 1.5" },
            // No display to ask (a --no-window run), or a driver that answered
            // nonsense: 1.0 is what a 100% screen gives and is the only answer
            // that cannot make the window unusable.
            { PHYSICAL, 0.0f, 0.0f, 1.0f, 1.0f, "auto",
              "U5 content scale 0 -> 1 / 1 on either coordinate system" },
            { LOGICAL,  0.0f, 0.0f, 1.0f, 1.0f, "auto", "U5 ...logical too" },
            { PHYSICAL, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f, "auto",
              "U5 ...and a NaN content scale is not a scale" },
            { LOGICAL,  std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f, "auto",
              "U5 ...on either one" },
        };
        for (const Row& r : rows) {
            const UiScaleDecision d =
                uiScaleDecision(r.logical, r.content, r.over, "--ui-scale");
            fprintf(stderr, "uiscaleselftest: %-8s content=%.2f over=%.2f -> ui %.2f "
                            "font %.2f (%s)  want %.2f / %.2f (%s)\n",
                    r.logical ? "logical" : "physical", r.content, r.over,
                    d.ui, d.font, d.how, r.wantUi, r.wantFont, r.wantHow);
            check(d.ui == r.wantUi && d.font == r.wantFont &&
                  !strcmp(d.how, r.wantHow), r.what);
        }
    }

    {   // ---- U6: an override outside the window is not an override -----------
        // 判断8's shape: refused, not clamped, and the run then carries on with
        // what the display said. 12 is what somebody types meaning "much
        // bigger" and 0.1 is a typo for 1; both would produce a UI nobody can
        // read or a window nobody can fit.
        const float bad[] = { 12.0f, 0.1f, -2.0f,
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN() };
        bool refused = true;
        for (float b : bad) {
            if (uiScaleOverrideValid(b)) refused = false;
            const UiScaleDecision d = uiScaleDecision(PHYSICAL, 2.0f, b);
            if (d.ui != 2.0f || d.font != 2.0f || strcmp(d.how, "auto")) refused = false;
        }
        check(refused, "U6 an out-of-range override is ignored, not clamped");
        // ...and the ends of the window ARE accepted, or the assertion above
        // would pass with the whole feature switched off.
        check(uiScaleOverrideValid(UI_SCALE_MIN) && uiScaleOverrideValid(UI_SCALE_MAX) &&
              uiScaleDecision(PHYSICAL, 2.0f, UI_SCALE_MAX).ui == UI_SCALE_MAX,
              "U6 ...and 0.5 and 4 themselves are accepted");
    }

    {   // ---- U7: the restored window fits the screen -------------------------
        // The other half of #257, and the half the user actually saw: a window
        // saved on a big screen, or doubled by the very bug above, must not come
        // back larger than the work area it is restored onto. 4000x3000 into a
        // 1920x1080 work area is 1920x1080 - the clamp is a min(), so both
        // numbers are arithmetic rather than a threshold.
        const std::vector<WinRect> work = { { 0, 0, 1920, 1080 } };
        WinRect r = { 0, 0, 4000, 3000 };
        bool keptPos = fitSavedWindow(r, work);
        fprintf(stderr, "uiscaleselftest: U7 4000x3000 into 1920x1080 -> %dx%d at %d,%d "
                        "(position kept: %d)\n", r.w, r.h, r.x, r.y, keptPos ? 1 : 0);
        check(r.w == 1920 && r.h == 1080, "U7 a saved window is clamped to the work area");
        // The saved rectangle covers the whole work area, so a quarter of the
        // WINDOW is not on it: the position is dropped and the window manager
        // places it, which is the documented behaviour and not new here.
        check(!keptPos, "U7 ...and a rectangle mostly off-screen loses its position");

        // Wayland: positions do not exist, so the overlap arithmetic has
        // nothing to say - but the SIZE still has to fit, against the first
        // monitor. Same numbers, and no position is ever returned.
        WinRect w = { 0, 0, 4000, 3000 };
        bool keptW = fitSavedWindow(w, work, /*positionsKnown=*/false);
        fprintf(stderr, "uiscaleselftest: U7 no positions: -> %dx%d (position kept: %d)\n",
                w.w, w.h, keptW ? 1 : 0);
        check(w.w == 1920 && w.h == 1080 && !keptW,
              "U7 with no window positions the size is still clamped");

        // ...and a window that DOES fit is not touched - the clamp must not be
        // a resize every start. 1280x800 on the same work area, placed at
        // 100,50: it overlaps entirely, its grab strip is inside, so both the
        // size and the position come back exactly as they went in.
        WinRect f = { 100, 50, 1280, 800 };
        bool keptF = fitSavedWindow(f, work);
        check(keptF && f.x == 100 && f.y == 50 && f.w == 1280 && f.h == 800,
              "U7 ...while a window that fits comes back untouched");
        // The no-positions path must not resize that one either.
        WinRect g = { 0, 0, 1280, 800 };
        fitSavedWindow(g, work, /*positionsKnown=*/false);
        check(g.w == 1280 && g.h == 800,
              "U7 ...on the no-positions path as well");
    }

    {   // ---- U8: the platform names the startup line prints -------------------
        // The line exists so the NEXT report of this shape is answered by asking
        // for it. A platform that printed as "none" when it is Wayland would
        // make that line worthless, so the mapping is asserted rather than read.
        check(!strcmp(uiScalePlatformName(GLFW_PLATFORM_X11), "x11") &&
              !strcmp(uiScalePlatformName(GLFW_PLATFORM_WAYLAND), "wayland") &&
              !strcmp(uiScalePlatformName(GLFW_PLATFORM_WIN32), "win32") &&
              !strcmp(uiScalePlatformName(GLFW_PLATFORM_COCOA), "cocoa") &&
              !strcmp(uiScalePlatformName(GLFW_PLATFORM_NULL), "none"),
              "U8 every GLFW platform has a name in the startup line");
    }

    fprintf(stderr, "uiscaleselftest: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
