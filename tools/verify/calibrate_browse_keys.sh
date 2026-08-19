#!/bin/bash
# Calibration: prove the Browse UI harness can actually FAIL.
#
#   bash tools/verify/calibrate_browse_keys.sh [<checkout-dir>]
#
# A UI test that cannot fail proves nothing. This runs the SAME scripted
# gesture twice, changing only the expected row NAME on the command line -
# no product code is edited, nothing is rebuilt. The wrong expectation must
# exit 1 and print FAIL; the right one must exit 0 and print ok.
#
# The recipe below is also the template for every new --browse-keys script
# (docs/verification/ui.md section C uses it verbatim). Two non-obvious rules:
#
#   PROLOG  waitdir:<leaf>,viewreset,w400,home
#           viewreset is the ABSOLUTE grouped+list+folded pin (the flat/tree/
#           more actions are relative), and w400 floats the panel at a fixed
#           width so injected clicks do not land wherever the operator's saved
#           layout happens to put the rows.
#   EPILOG  >=6 nav keys while focused, then blur,down,up,end,home, then
#           rawopen,popupcheck,seqask,popupcheck
#           because the run's exit code is
#             keysOk = routeOk && popOk && keysCheckBad == 0   (main.cpp:29132)
#           and routeOk/popOk read counters that only the blur and popup
#           actions ever set. Omit the epilog and a script whose checks all
#           pass still exits 1 with "the action list did not finish".
set -u
cd "${1:-.}" || exit 1
V=./build-mingw/viewer.exe
ISO="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/vcal.$$")/cfg"
mkdir -p "$ISO/viewer"; export APPDATA="$ISO" HOME="$ISO"

PRO="waitdir:rb,viewreset,w400,home,down"
EPI="down,down,down,down,down,blur,down,up,end,home,rawopen,popupcheck,seqask,popupcheck"

run() {   # run <expected-row-name>
  "$V" --browse-keys-selftest tools/testdata/rb \
       --browse-keys "$PRO,chkatrow:$1,$EPI" 2>&1
  echo "rc=$?"
}

echo "### 1. DELIBERATELY WRONG expectation (chkatrow:expset) - must FAIL"
run expset | grep -E 'chkatrow|panel check|rc='
echo
echo "### 2. CORRECT expectation (chkatrow:digitset) - must PASS"
run digitset | grep -E 'chkatrow|panel check|rc='
