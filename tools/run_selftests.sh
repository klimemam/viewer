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
#      selftest not labelled `nogl` needs - the ones that drive real ImGui
#      frames, six of the 36 today - because "there is no GL here" and "an
#      assert failed" are different events that used to look identical. The
#      figures in this header are the only ones written down: what a given run
#      actually did is the "ran N, skipped M" line it prints at the end, which
#      is computed from ctest and cannot go stale the way these can (they had,
#      by six, until the audit that added the invariant check below);
#   4. runs ctest, one line per selftest, printing the full output of any that
#      fail - these tests say "NAME: assert text PASS/FAIL", and that text is
#      what you need, not an exit code;
#   5. names everything that did NOT run - skipped or quarantined - so a test
#      that never ran is never silent and a skip never reads as a pass;
#   6. archives the output of any test that FAILED, out of the build tree and
#      one file per test, because three intermittent failures this week were
#      each diagnosed by re-running and each destroyed by the re-run.
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
# Five selftests create a real GLFW window and an OpenGL context; the other 30
# take the --no-window startup path and want no display at all. Saying which
# case this machine is in, once and up front, beats five identical "failed to
# create window" failures further down.
if [ "$(uname -s)" = "Linux" ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        # NOT fatal any more: the 30 windowless selftests still run and still
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

# ---- the evidence of a failing run ------------------------------------------
# ctest writes every test's output to Testing/Temporary/LastTest.log, and the
# NEXT ctest overwrites it - including the one-test glprobe twenty lines below,
# so the record dies before the re-run that was meant to reproduce the failure
# has started a single test.
#
# Copying that file aside on failure was the first attempt at this (4a7fabc) and
# it was not enough. Three intermittent failures escaped afterwards anyway;
# browse-keys took four sightings to catch and selftest.browse's one failure is
# simply gone. What the copy actually did wrong, checked rather than assumed:
#
#   * it kept the WHOLE RUN. build-mingw/Testing/failed-20260804-152746.log is
#     193 kB of 26 tests, of which the failing one is 360 lines somewhere in the
#     middle, and the name says only what minute it was taken. Nothing about
#     that file announces which test failed or what it said, so reading it is a
#     deliberate act of archaeology - and the thing people actually did instead
#     was read the terminal, which is exactly what does not survive.
#   * it lived in the BUILD TREE, which is where evidence goes to die. It is
#     gitignored, it is what `rm -rf` is aimed at the moment a build misbehaves,
#     and on CI it is deleted with the runner about ninety seconds after the
#     assert that is the only reason anyone cares. Every CI failure this project
#     has had was unrecoverable by construction.
#   * it only fired when the failure came through THIS SCRIPT. The natural way
#     to ask "does it reproduce" is `ctest -R selftest.browse-keys`, and that
#     overwrites LastTest.log and archives nothing: looking destroyed the thing
#     being looked for.
#
# So the archive is per-test, named after the test, carries the failing asserts
# at the top, and lives OUTSIDE the build tree - see docs/diagnostics/auto/ and
# the note above it. And it runs twice: once here, BEFORE the glprobe's ctest
# overwrites LastTest.log, which salvages whatever a bare `ctest` left behind,
# and once after the suite. Nobody has to have planned ahead for either.
archive_dir="docs/diagnostics/auto"
archived=""                             # paths written by the last archive call

archive_failures() {                    # $1: how we came to be looking
    local why="$1"
    local last="$build_dir/Testing/Temporary/LastTest.log"
    archived=""
    [ -f "$last" ] || return 0
    # One failure, one archive. Without this the salvage pass at the top of a run
    # and the keep at the end of one would file every failure twice, and a run
    # that fails twice running would bury the first copy under two identical
    # ones. The first line of the
    # file is "Start testing: <date>", so line-one plus size identifies the run
    # without needing a portable stat(1) - `date -r FILE` is not the same thing
    # on macOS as it is on Linux, and this script runs on both.
    local stamp seen
    stamp="$(head -1 "$last" 2>/dev/null)|$(wc -c <"$last" 2>/dev/null | tr -d ' ')"
    seen="$(cat "$archive_dir/.last-archived" 2>/dev/null || true)"
    [ "$stamp" = "$seen" ] && return 0

    mkdir -p "$archive_dir" || return 0
    local ts head_sha dirty
    ts="$(date -u +%Y%m%dT%H%M%SZ)"
    head_sha="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    dirty=""
    git diff --quiet HEAD 2>/dev/null || dirty=" +dirty"

    # Split LastTest.log into its per-test sections and write out the ones that
    # did not pass. The section markers are ctest's own, so this needs no list
    # of test names - the same rule the rest of this script follows.
    #
    # "Did not pass" is asked TWO ways, because the obvious one is not enough.
    # A test ctest could not LAUNCH - the command is not there, the exe was
    # never built - prints "(Not Run)" on the console and is then written into
    # LastTest.log under "Test Passed.". Measured, not assumed: a test whose
    # command is a path that does not exist produces exactly that. Its section
    # gives itself away by an empty `start time:`, because nothing ever started,
    # and that is the second question asked below. Reading the verdicts alone
    # would have archived NOTHING for the one failure mode this project has
    # already been bitten by twice - a test that did not run.
    #
    # The QUARANTINED test looks identical from inside the log - it never
    # started either, and ctest blanks `Command:` for both - and it must NOT be
    # filed as a failure: it is reported further down on its own terms, and
    # filing it every single run would bury the real captures under a hundred
    # copies of a known one. So the disabled test is excluded BY NAME, from
    # ctest's own LastTestsDisabled.log, which the same run wrote beside the
    # same log. Not from a list of names kept here - that would be the second
    # copy of the test list this whole harness exists to not have.
    local disabled_list="$build_dir/Testing/Temporary/LastTestsDisabled.log"
    [ -f "$disabled_list" ] || disabled_list=""

    # `when` comes from date(1) rather than awk's strftime(), which is a gawk and
    # mawk extension and is not in the awk macOS ships - and macOS is a third of
    # the matrix.
    archived="$(awk -v dir="$archive_dir" -v ts="$ts" -v why="$why" \
                    -v sha="$head_sha$dirty" -v host="$(uname -s) $(uname -m)" \
                    -v when="$(date -u '+%Y-%m-%d %H:%M:%S UTC')" \
                    -v disabledlist="$disabled_list" '
      BEGIN {
          if (disabledlist != "")
              while ((getline ln < disabledlist) > 0) {
                  sub(/^[0-9]+:/, "", ln)
                  if (ln != "") disabled[ln] = 1
              }
      }
      function emit(   f, i, k, parts, nl, safe) {
          if (nostart && !(name in disabled)) failed = 1
          if (name == "" || !failed) return
          safe = name                       # a test name is not a filename
          gsub(/[^A-Za-z0-9._-]/, "_", safe)
          f = dir "/" ts "-" safe ".log"
          print "# " name " did NOT pass, " when > f
          print "# " why > f
          print "# repo " sha ", " host > f
          print "#" > f
          print "# Written automatically by tools/run_selftests.sh. Untracked and" > f
          print "# unreviewed: if this failure is worth keeping, trim it to its" > f
          print "# signature and commit that to docs/diagnostics/ by hand." > f
          print "#" > f
          print "# ---- what failed ----------------------------------------" > f
          nl = split(block, parts, "\n")
          k = 0
          for (i = 1; i <= nl; i++)
              if (parts[i] ~ /FAIL/) { print parts[i] > f; k++ }
          if (k == 0)
              print "# (nothing here said FAIL. A test that printed no assert at" \
                    " all\n# either died before it could or was never launched -" \
                    " check the\n# Command: line below and whether it exists.)" > f
          print "" > f
          print "# ---- the whole of what it printed -----------------------" > f
          printf "%s", block > f
          close(f)
          print f
      }
      /^[0-9]+\/[0-9]+ Testing: / {
          emit()
          name = $0; sub(/^[0-9]+\/[0-9]+ Testing: /, "", name)
          block = ""; failed = 0; nostart = 0
      }
      {
          block = block $0 "\n"
          # ctest verdicts: anything that is not "Test Passed." is evidence...
          if ($0 ~ /^Test (Failed|Timeout|Not Run)/ || $0 ~ /^\*\*\*/) failed = 1
          # ...and so is a blank start time on a test ctest was not told to
          # skip, which is the shape of one that was never launched and is then
          # filed, wrongly, under "Test Passed."
          if ($0 ~ /^".*" start time:[ \t]*$/) nostart = 1
      }
      END { emit() }
    ' "$last")"

    [ -n "$archived" ] || return 0
    # The full run beside the extracts: which tests ran before this one, and in
    # what order, is the question an intermittent failure always raises next.
    cp "$last" "$archive_dir/$ts-fullrun.log" 2>/dev/null \
        && archived="$archived
$archive_dir/$ts-fullrun.log"
    printf '%s' "$stamp" > "$archive_dir/.last-archived"

    # Bounded, but generously: the requirement is that a failure survives the
    # next two suite runs, and 200 files is a hundred times that. Pruning is
    # itself a way to lose evidence, so it errs enormously on the side of keeping.
    local over
    over="$(ls -t "$archive_dir"/*.log 2>/dev/null | tail -n +201)"
    [ -n "$over" ] && printf '%s\n' "$over" | while IFS= read -r f; do rm -f "$f"; done

    echo "  kept, per test, outside the build tree:"
    printf '%s\n' "$archived" | sed 's/^/    /'
}

# Salvage first: if a bare `ctest` failed here since the last suite run, its
# output is still in LastTest.log and the glprobe below is about to destroy it.
archive_failures "found in $build_dir before this run - left by a ctest invocation outside this script"

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
probe_out="$(ctest --test-dir "$build_dir" -C "$config" -L "^glprobe$" -V 2>&1)"
probe_rc=$?
gl_reason="$(printf '%s\n' "$probe_out" | sed -n 's/.*no OpenGL context on this machine: //p' | head -1)"
gl_ok="$(printf '%s\n' "$probe_out" | sed -n 's/.*OpenGL context OK: //p' | head -1)"

have_gl=1
# VIEWER_FORCE_NO_GL has been documented at the top of this file since the probe
# was written and was never implemented, so the switch a developer would use to
# check what Windows and macOS actually run did nothing at all - which is a
# small version of the very thing it exists to catch. Three lines, here, so that
# "29 ran, 5 skipped on the runners with no context" is something a developer
# can MEASURE on a box that has one, instead of waiting for CI to say it.
if [ -n "${VIEWER_FORCE_NO_GL:-}" ] && [ "${VIEWER_FORCE_NO_GL}" != "0" ]; then
    have_gl=0
    gl_reason="VIEWER_FORCE_NO_GL is set (the machine may well have one)"
    echo "  $gl_reason"
elif [ -n "$gl_reason" ]; then
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
    # ANCHORED. `ctest -L` takes a REGEX, not a name, so an unanchored `-L
    # selftest` also selects `typo-selftest`, `selftest-slow` and anything else
    # merely CONTAINING the word - measured, not assumed. Membership of the gate
    # is meant to be a yes/no fact about a label, so it is asked as one; without
    # the anchors a mislabelled test can join the run set by accident, which is
    # the same silence as leaving it out, only pointing the other way.
    ctest --test-dir "$build_dir" -C "$config" -N -L "^$1\$" 2>/dev/null \
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

# ---- the labelling invariant, ASKED rather than trusted ---------------------
# Both lists below are computed from `selftest`. `nogl` only chooses which
# BRANCH a test runs in, so a test carrying `nogl` and not `selftest` is in
# neither list in either branch: with a context it does not run and is not
# named; without one it runs and is not named as having run. It is the single
# hole in this script's promise that a test which did not run is never silent,
# and it is not hypothetical - test_prnu sat in it from the day it was written
# until 1d951d3, through every green run on three runners, and was found by
# accident rather than by anything asking.
#
# ctest cannot ask this: labels are free-form strings and only this file knows
# what they are supposed to mean here. So the question is asked here, once,
# beside the two sets it decides between. CMakeLists.txt states the rule where
# the tests are registered; this is the check that it held.
stray_nogl="$(minus "$nogl_tests" "$all_tests")"
if [ -n "$stray_nogl" ]; then
    echo >&2
    echo "run_selftests: MISLABELLED - carries 'nogl' but not 'selftest':" >&2
    printf '%s\n' "$stray_nogl" | sed 's/^/run_selftests:     /' >&2
    echo "run_selftests: such a test runs ONLY where there is no GL context, and" >&2
    echo "run_selftests: is reported in neither the ran list nor the skipped one." >&2
    echo "run_selftests: give it LABELS \"selftest;nogl\" in CMakeLists.txt - see" >&2
    echo "run_selftests: THE LABELLING RULE above the hand-registered tests there." >&2
    exit 1
fi

# ...and the other shape of the same silence: a registered test that is in no
# run set at all - a typo'd label, a missing one, or a dependency that started
# registering tests of its own into our ctest (the EXR subprojects are told
# BUILD_TESTING=OFF for exactly that reason, which is a thing that can regress).
# NAMED rather than fatal: unlike the case above it is not always a mistake,
# and naming is all the promise requires. `glprobe` is the one legitimate
# member - it answers the question this script asks before the suite, and
# deliberately does not count as one of the tests.
registered="$(ctest --test-dir "$build_dir" -C "$config" -N 2>/dev/null \
              | sed -n 's/^ *Test *#[0-9]*: *//p')"
in_a_run_set="$(printf '%s\n%s\n%s\n' "$all_tests" "$quarantined" "$(tests_labelled glprobe)" \
                | grep -v '^$')"
orphans="$(minus "$registered" "$in_a_run_set")"

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
ctest --test-dir "$build_dir" -C "$config" -L "^$run_label$" \
      --output-on-failure --no-tests=error
rc=$?

# Keep the evidence, before anything can re-run. See archive_failures() above
# for why the copy this replaced was not enough.
kept_paths=""
if [ "$rc" -ne 0 ]; then
    echo
    archive_failures "failed during tools/run_selftests.sh $build_dir"
    kept_paths="$archived"
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
  why: read the block above the test's viewer_selftest() line in CMakeLists.txt.
       The reason a test is out of the gate belongs beside the test, where the
       person re-enabling it is already looking - a second copy here went stale
       the moment the first one was investigated. A quarantine is never a
       loosened assert: the assert stays exactly as written and only stops
       running, and the block must say what would have to be true to lift it.
EOF
fi

echo
echo "== in no run set (NOT a gate - these did NOT run) =="
# The counterpart of the quarantine block: a test ctest knows about that no
# label puts in either branch. It cannot be silent even though it is not fatal.
if [ -z "$orphans" ]; then
    echo "  (none - every registered test is in a run set)"
else
    printf '%s\n' "$orphans" | sed 's/^/  /'
    echo "  why: no 'selftest' label, so neither branch of the GL probe selects"
    echo "       them. If one is ours, label it - CMakeLists.txt, THE LABELLING"
    echo "       RULE above the hand-registered tests."
fi

echo
# The one line to read. A skip is never allowed to look like a pass: it is
# counted here, named above, and carries the reason it could not run.
if [ "$n_skip" -gt 0 ]; then
    echo "run_selftests: ran $n_ran, skipped $n_skip (no GL context): $skip_line"
else
    echo "run_selftests: ran $n_ran, skipped 0"
fi
# ctest counts the quarantined test in its "N/M" progress denominator but not in
# its "out of M-1" total, so the two numbers a reader sees above genuinely
# differ. Reconcile them here, computed rather than asserted, so nobody has to
# work out which of the three counts is the one that answers their question.
echo "run_selftests: (ctest lists $(count "$all_tests") tests labelled 'selftest': \
$n_ran ran + $n_skip skipped + $(count "$quarantined") quarantined)"
# exit 0 for a skip - but only for the skip. Anything that DID run and failed
# still reddens the build, which is the whole point of running the nogl set.
if [ $rc -ne 0 ]; then
    echo "run_selftests: FAIL (ctest exit $rc)"
    # Repeated HERE, as the last thing printed, and not only where it was
    # written 200 lines up. The pointer to the evidence has to outlive the
    # scrollback of the run that produced it, or it is worth as much as the
    # scrollback was.
    if [ -n "$kept_paths" ]; then
        printf '%s\n' "$kept_paths" | sed 's|^|run_selftests: kept: |'
    fi
elif [ "$n_skip" -gt 0 ]; then
    echo "run_selftests: PASS (partial - $n_skip selftests could not run here)"
else
    echo "run_selftests: PASS"
fi
exit $rc
