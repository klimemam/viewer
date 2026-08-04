#!/usr/bin/env bash
#
# Every selftest the viewer carries, exactly as CI runs them.
#
#     tools/run_selftests.sh [build-dir]
#
# This is the single entry point: the CI job calls this script, so a developer
# who runs it locally gets precisely what the matrix ran. The test LIST lives in
# CMakeLists.txt (the `viewer_selftest(...)` block) - there is no second copy of
# the argument lists here, and adding a test there adds it to CI.
#
# The script, in order:
#   1. builds - a stale binary passing tests for code that no longer compiles is
#      the single most expensive failure mode there is, so a failed build stops
#      the run instead of quietly testing yesterday's exe;
#   2. regenerates tools/testdata (deterministic, gitignored, never committed);
#   3. asks the machine ONCE whether it can make the OpenGL context that every
#      selftest not labelled `nogl` needs (today that is five of the 22 - the
#      ones that drive real ImGui frames), because "there is no GL here" and
#      "an assert failed" are different events that used to look identical;
#   4. runs ctest, one line per selftest, printing the full output of any that
#      fail - these tests say "NAME: assert text PASS/FAIL", and that text is
#      what you need, not an exit code;
#   5. names everything that did NOT run - skipped or quarantined - so a test
#      that never ran is never silent and a skip never reads as a pass.
#
# Env:
#   VIEWER_TEST_CONFIG   Release (default) - multi-config generators (MSVC)
#   VIEWER_TEST_JOBS     build parallelism (default: 8)
#   VIEWER_SELFTEST_REQUIRE_GL
#                        set it where a GL context is KNOWN to exist - CI's
#                        Linux job installs xvfb and libgl1-mesa-dri precisely
#                        so that it does. Its absence is then a regression in
#                        that environment, not a fact about it, and this script
#                        fails instead of skipping. It is what keeps the Linux
#                        gate exactly as strong as it was.
#   VIEWER_FORCE_NO_GL   make the probe report no context whatever the machine
#                        really has: the only way to exercise the skip path on
#                        a developer box that works.
#
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root" || exit 1

config="${VIEWER_TEST_CONFIG:-Release}"
jobs="${VIEWER_TEST_JOBS:-8}"

build_dir="${1:-}"
if [ -z "$build_dir" ]; then                 # the local MinGW tree, else CI's
    for d in build-mingw build; do
        if [ -f "$d/CMakeCache.txt" ]; then build_dir="$d"; break; fi
    done
fi
build_dir="${build_dir:-build}"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "run_selftests: no configured build in '$build_dir'" >&2
    echo "run_selftests: configure one first, e.g." >&2
    echo "    cmake -S . -B $build_dir -DCMAKE_BUILD_TYPE=Release" >&2
    exit 2
fi

# ---- preflight: a display ---------------------------------------------------
# Five selftests create a real GLFW window and an OpenGL context; the other 17
# take the --no-window startup path and want no display at all. Saying which
# case this machine is in, once and up front, beats five identical "failed to
# create window" failures further down.
if [ "$(uname -s)" = "Linux" ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        # NOT fatal any more: the 17 windowless selftests still run and still
        # gate here, and the probe below will name the five that cannot.
        echo "run_selftests: headless Linux with no xvfb-run - the selftests that" >&2
        echo "run_selftests: need a window cannot run (they are named at the end)." >&2
        echo "run_selftests: to run everything: apt-get install -y xvfb, or set DISPLAY." >&2
    else
        echo "run_selftests: headless Linux - each selftest gets its own xvfb server"
    fi
fi

# Probe by RUNNING it, not by looking it up: on Windows a "python3" App
# Execution Alias is on PATH by default and is a stub that prints an advert for
# the Microsoft Store and exits non-zero. numpy is part of the probe because
# that is the only thing gen_testdata.py actually needs.
python_bin=""
for p in python3 python py; do
    if command -v "$p" >/dev/null 2>&1 && "$p" -c "import numpy" >/dev/null 2>&1; then
        python_bin="$p"; break
    fi
done
if [ -z "$python_bin" ]; then
    echo "run_selftests: no working python with numpy on PATH." >&2
    echo "run_selftests: tools/gen_testdata.py needs it to write the fixtures." >&2
    echo "run_selftests: tried python3, python, py - install numpy (pip install numpy)." >&2
    exit 2
fi

echo "== build ($build_dir, $config) =="
if ! cmake --build "$build_dir" --config "$config" -j "$jobs"; then
    echo >&2
    echo "run_selftests: BUILD FAILED - refusing to run the selftests." >&2
    echo "run_selftests: the previous binary is still on disk and would 'pass'." >&2
    exit 1
fi

echo
echo "== fixtures ($python_bin tools/gen_testdata.py) =="
if ! "$python_bin" tools/gen_testdata.py; then
    echo "run_selftests: fixture generation FAILED" >&2
    exit 1
fi

echo
echo "== OpenGL probe =="
# WHY this exists: every selftest not labelled `nogl` opens a real 1600x1000
# window and an OpenGL context, so on a machine that has none they die inside
# glfwCreateWindow - and nothing in the output said so. They read as that many
# broken asserts, which is how the matrix stayed red with no way to see why
# (back when startup made a window for all 22 of them, not just these five).
# The question is asked once, up front, and answered in GLFW's own words.
#
# It is a question about the MACHINE, not about its OS name. Where a context
# exists, everything runs; where none does, the `nogl` tests still run and the
# rest are skipped by name. Nothing here knows which tests those are: the labels
# come from ctest, so a test that stops needing a window starts running here the
# moment CMakeLists.txt says NOGL, with no edit to this script.
probe_out="$(ctest --test-dir "$build_dir" -C "$config" -L glprobe -V 2>&1)"
probe_rc=$?
gl_reason="$(printf '%s\n' "$probe_out" | sed -n 's/.*no OpenGL context on this machine: //p' | head -1)"
gl_ok="$(printf '%s\n' "$probe_out" | sed -n 's/.*OpenGL context OK: //p' | head -1)"

have_gl=1
if [ -n "$gl_reason" ]; then
    have_gl=0
    echo "  no OpenGL context on this machine: $gl_reason"
elif [ -n "$gl_ok" ]; then
    echo "  OpenGL context OK: $gl_ok"
else
    # The probe said neither thing: a build tree older than the glprobe test, a
    # ctest too old for -L, something unforeseen. Fail TOWARDS running. A probe
    # that cannot answer must never be the reason a gate did not run.
    echo "  probe inconclusive (ctest exit $probe_rc) - running everything anyway" >&2
fi

# Where a context is KNOWN to exist - CI's Linux job installs xvfb and
# libgl1-mesa-dri precisely so that it does - its absence is a regression in
# that environment, not a fact about it. There, refuse to skip: this is what
# keeps the Linux gate exactly as strong as it was before any of this.
if [ "$have_gl" -eq 0 ] && [ -n "${VIEWER_SELFTEST_REQUIRE_GL:-}" ] \
                        && [ "${VIEWER_SELFTEST_REQUIRE_GL}" != "0" ]; then
    echo >&2
    echo "run_selftests: VIEWER_SELFTEST_REQUIRE_GL is set and there is NO GL context." >&2
    echo "run_selftests: this environment is supposed to have one, so that is a" >&2
    echo "run_selftests: failure of the environment - not a licence to skip the gate." >&2
    echo "run_selftests: reason: $gl_reason" >&2
    exit 1
fi

# ---- what will run, and what will not --------------------------------------
# Both lists come from ctest's own labels. There is no list of test names in
# this file on purpose: the day a test stops needing a window it gains NOGL in
# CMakeLists.txt and moves between these sets by itself.
tests_labelled() {
    ctest --test-dir "$build_dir" -C "$config" -N -L "$1" 2>/dev/null \
        | sed -n 's/^ *Test *#[0-9]*: *//p'
}
minus() {                        # lines of $1 that are not whole lines of $2
    [ -z "$1" ] && return 0
    if [ -z "$2" ]; then printf '%s\n' "$1"; else printf '%s\n' "$1" | grep -vxF "$2"; fi
}
count() { if [ -z "$1" ]; then echo 0; else printf '%s\n' "$1" | grep -c .; fi; }

all_tests="$(tests_labelled selftest)"
nogl_tests="$(tests_labelled nogl)"
quarantined="$(tests_labelled quarantine)"
# Quarantined tests are DISABLED: reported below on their own terms, and neither
# "ran" nor "skipped for want of GL".
gated="$(minus "$all_tests" "$quarantined")"

if [ "$have_gl" -eq 1 ]; then
    run_label=selftest
    ran="$gated"
    skipped=""
else
    run_label=nogl
    ran="$(minus "$nogl_tests" "$quarantined")"
    skipped="$(minus "$gated" "$nogl_tests")"
fi
n_ran="$(count "$ran")"
n_skip="$(count "$skipped")"
skip_line="$(printf '%s\n' "$skipped" | tr '\n' ' ' | sed 's/  *$//')"

# "We skipped everything and called it a pass" is the one outcome this script
# must never produce. It is not hypothetical: ctest exits 0 for an empty
# selection, so a labelling mistake that emptied the set would turn the whole
# gate green and silent. The count is what decides, not the exit code.
if [ "$n_ran" -eq 0 ]; then
    echo "run_selftests: NOTHING WOULD RUN - 0 selftests selected." >&2
    echo "run_selftests: labels come from ctest, so this means the build tree is" >&2
    echo "run_selftests: stale or the viewer_selftest() labels are wrong." >&2
    exit 1
fi

echo
echo "== selftests =="
# Deliberately NOT parallel. Every selftest polls the loader against a
# wall-clock budget (120-600 s); docs/verify-functional.md "1. 方法" records, in
# the 決定性の担保 table, that running them at the same time trips those budgets
# and produces false FAILs.
ctest --test-dir "$build_dir" -C "$config" -L "$run_label" \
      --output-on-failure --no-tests=error
rc=$?

# Keep the evidence of a failing run. ctest writes every test's output to
# Testing/Temporary/LastTest.log and the NEXT run overwrites it, so an
# intermittent failure is routinely diagnosed by re-running -- which destroys
# the only record of what it printed. Three separate investigations of
# selftest.browse-keys have now lost the failure this way. Copy it aside
# before anyone can re-run.
if [ "$rc" -ne 0 ]; then
    last="$build_dir/Testing/Temporary/LastTest.log"
    if [ -f "$last" ]; then
        kept="$build_dir/Testing/failed-$(date +%Y%m%d-%H%M%S).log"
        cp "$last" "$kept" && echo "  kept the failing run's output: $kept"
    fi
fi

echo
echo "== skipped: no GL context (NOT a gate - these did NOT run) =="
if [ "$n_skip" -eq 0 ]; then
    echo "  (none - this machine has a GL context, so every selftest ran)"
else
    echo "  reason: $gl_reason"
    printf '%s\n' "$skipped" | sed 's/^/  /'
fi

echo
echo "== quarantined (NOT a gate - these did NOT run) =="
# Listed from ctest itself, so a quarantined test can never go unnoticed and a
# new one cannot be added without showing up here.
if [ -z "$quarantined" ]; then
    echo "  (none)"
else
    echo "$quarantined" | sed 's/^/  /'
    cat <<'EOF'
  why: docs/tasks.csv "abstats A2既知失敗" - the A2 assert (the B slot's cached
       sigma_t against an independent recompute, 1e-6 relative) was recorded as
       failing under --cfa bayer, cause never investigated. It passes 10/10 on
       MinGW against merged main, so it is either already fixed or intermittent.
       Re-enable it in CMakeLists.txt once somebody establishes which. It is
       quarantined, NOT loosened: the assert is untouched.
EOF
fi

echo
# The one line to read. A skip is never allowed to look like a pass: it is
# counted here, named above, and carries the reason it could not run.
if [ "$n_skip" -gt 0 ]; then
    echo "run_selftests: ran $n_ran, skipped $n_skip (no GL context): $skip_line"
else
    echo "run_selftests: ran $n_ran, skipped 0"
fi
# ctest counts the quarantined test in its "N/23" progress denominator but not
# in its "out of 22" total, so the two numbers a reader sees above genuinely
# differ. Reconcile them here, computed rather than asserted, so nobody has to
# work out which of the three counts is the one that answers their question.
echo "run_selftests: (ctest lists $(count "$all_tests") tests labelled 'selftest': \
$n_ran ran + $n_skip skipped + $(count "$quarantined") quarantined)"
# exit 0 for a skip - but only for the skip. Anything that DID run and failed
# still reddens the build, which is the whole point of running the nogl set.
if [ $rc -ne 0 ]; then
    echo "run_selftests: FAIL (ctest exit $rc)"
elif [ "$n_skip" -gt 0 ]; then
    echo "run_selftests: PASS (partial - $n_skip selftests could not run here)"
else
    echo "run_selftests: PASS"
fi
exit $rc
