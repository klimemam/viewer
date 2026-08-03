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
#   3. runs ctest, one line per selftest, printing the full output of any that
#      fail - these tests say "NAME: assert text PASS/FAIL", and that text is
#      what you need, not an exit code;
#   4. names anything quarantined, so a skipped test is never silent.
#
# Env:
#   VIEWER_TEST_CONFIG   Release (default) - multi-config generators (MSVC)
#   VIEWER_TEST_JOBS     build parallelism (default: 8)
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
# Only --remote-selftest runs before the window exists; the other 21 create a
# real GLFW window and an OpenGL context. Saying so once here beats 21
# identical "failed to create window" failures.
if [ "$(uname -s)" = "Linux" ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        echo "run_selftests: SKIPPING EVERYTHING - headless Linux with no xvfb-run." >&2
        echo "run_selftests: 21 of the 22 selftests create a real GLFW window." >&2
        echo "run_selftests: install it (apt-get install -y xvfb) or set DISPLAY." >&2
        exit 2
    fi
    echo "run_selftests: headless Linux - each selftest gets its own xvfb server"
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
echo "== selftests =="
# Deliberately NOT parallel. Every selftest polls the loader against a
# wall-clock budget (120-600 s); docs/verify-functional.md:55 records that
# running them at the same time trips those budgets and produces false FAILs.
ctest --test-dir "$build_dir" -C "$config" \
      --output-on-failure --no-tests=error
rc=$?

echo
echo "== quarantined (NOT a gate - these did NOT run) =="
# Listed from ctest itself, so a quarantined test can never go unnoticed and a
# new one cannot be added without showing up here.
quarantined="$(ctest --test-dir "$build_dir" -C "$config" -N -L quarantine 2>/dev/null \
               | sed -n 's/^ *Test *#[0-9]*: *//p')"
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
if [ $rc -eq 0 ]; then
    echo "run_selftests: PASS"
else
    echo "run_selftests: FAIL (ctest exit $rc)"
fi
exit $rc
