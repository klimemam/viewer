#!/bin/bash
# A/B capture harness for the frame-reference refactor (stage 1 invariance).
#
# Runs an IDENTICAL command list against one build and captures stdout+stderr
# per command, so two builds can be diffed byte-for-byte.
#
# usage: ab_capture.sh <viewer.exe> <viewer-serve.exe> <outdir> <ab-root>
#
# Determinism controls (each is a deliberate choice, see docs/verify-functional.md):
#   * cwd is the SHARED <ab-root>, never a worktree, so every relative fixture
#     path in the command list is the same string for both builds.
#   * APPDATA is pinned to a FRESH empty dir per pass. The selftests read the
#     live user's %APPDATA%/viewer/{layout.ini,prefs.txt}; without this pin a
#     sibling agent writing that state between passes would forge a diff.
#   * TMP/TEMP are pinned per pass, so the .vsession files the selftests write
#     through temp_directory_path() land inside the capture dir and become
#     comparable artifacts instead of cross-contaminating the two passes.
set -u
EXE="$1"; SERVE="$2"; OUT="$3"; ROOT="$4"

rm -rf "$OUT"; mkdir -p "$OUT/caps" "$OUT/appdata" "$OUT/tmp" "$OUT/home"
export APPDATA="$(cygpath -w "$OUT/appdata" 2>/dev/null || echo "$OUT/appdata")"
export TMP="$(cygpath -w "$OUT/tmp" 2>/dev/null || echo "$OUT/tmp")"
export TEMP="$TMP"
# HOME/USERPROFILE too: a scripted selftest leaves io.IniFilename unguarded and
# writes the OPERATOR'S real %APPDATA%/viewer/layout.ini (known, being fixed on
# main). Every env route to the operator's state is pointed at a throwaway dir
# so this harness can neither read nor corrupt it.
export HOME="$(cygpath -w "$OUT/home" 2>/dev/null || echo "$OUT/home")"
export USERPROFILE="$HOME"

cd "$ROOT" || exit 1
# an imgui.ini left in the shared run root by the previous pass would be read by
# the next one: clean the cwd so pass B starts exactly where pass A started
rm -f "$ROOT"/imgui.ini "$ROOT"/layout.ini

run() {
  name="$1"; shift
  "$EXE" "$@" > "$OUT/caps/$name.out" 2>&1
  echo "$?" > "$OUT/caps/$name.rc"
  echo "  captured $name (rc=$(cat "$OUT/caps/$name.rc"))"
}

# --- the 21 the orchestrator suite pins, same args, neutral-root paths --------
run series      --series-selftest linset
run lin         --lin-selftest --stack always --cfa bayer --bayer-pattern RGGB linset
run browse      --browse-selftest tools/testdata/rb
run browsekeys  --browse-keys-selftest tools/testdata/rb
run abstats     --abstats-selftest tools/testdata/multi
run range       --range-selftest tools/testdata/multi
run close       --close-selftest tools/testdata/multi
run batch       --batch-selftest tools/testdata/multi
run picker      --picker-selftest abset
run verify      --verify-selftest tools/testdata/multi
run scan        --scan-selftest linset
run localbrowse --localbrowse-selftest tools/testdata/rb
run rtemporal   --rtemporal-selftest tools/testdata/rb/scanroot/10lx
run framestats  --stack always --framestats-selftest linset/80lx
run export      --export-selftest tools/testdata/multi --stack always
run tile        --tile-selftest tools/testdata/multi
run derive      --derive-selftest tools/testdata
run texport     --export-tsv-selftest tools/testdata/multi --stack always --cfa bayer --bayer-pattern RGGB
run framelin    --frame-lin-selftest tools/testdata
run newwin      --newwin-selftest tools/testdata
run remote      --remote-selftest tools/testdata/bench_stack.npy --remote-exe "$SERVE"

# --- beyond the suite --------------------------------------------------------
# sweepfile is the 22nd selftest in the tree and the ONE the suite never runs.
# levelfiles carries injected ground truth (sens 8.0 DN/lx, offset 64.0 DN).
run sweepfile   --sweepfile-selftest levelfiles
run scanlevel   --scan-selftest levelfiles
# the known A2 mosaic failure - it must fail IDENTICALLY on both builds
run abstatscfa  --abstats-selftest tools/testdata/multi --cfa bayer --bayer-pattern RGGB
run help        --help

# --- session artifacts written through temp_directory_path() -----------------
for f in "$OUT"/tmp/*.vsession; do
  [ -e "$f" ] && cp "$f" "$OUT/caps/session_$(basename "$f").txt"
done
echo "CAPTURE DONE $OUT"
