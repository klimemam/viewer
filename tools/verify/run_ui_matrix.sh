#!/bin/bash
# docs/verify-ui.md section A/B driver: run every UI item that is automatable
# on a machine that cannot screenshot OpenGL, and print one line per 項番.
#
#   bash tools/verify/run_ui_matrix.sh [<checkout-dir>]
#
# WHY THE APPDATA DANCE (do not remove it):
# the periodic autosave in the frame loop (core/main.cpp, guarded only by
# !benchFrames) means a --browse-keys-selftest run WRITES a real
# autosave.vsession - see docs/verify-ui.md defect D-1. The layout.ini half of
# that defect is closed (#206): io.IniFilename is now nullptr for any scripted
# run. The session half is not, so every run below is still pointed at a
# throwaway config dir and the suite can never touch the operator's own state.
set -u
cd "${1:-.}" || exit 1
V=./build-mingw/viewer.exe
[ -x "$V" ] || { echo "no $V - build first"; exit 2; }

ISO="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/vui.$$")/cfg"
mkdir -p "$ISO/viewer"
export APPDATA="$ISO"          # Windows
export HOME="$ISO"             # POSIX ($HOME/.config/viewer)
OUT="$ISO/out"; mkdir -p "$OUT"

pass=0; fail=0
# item <項番> <name> -- <argv...>: rc==0 is the verdict
item() {
  id="$1"; name="$2"; shift 3
  "$V" "$@" > "$OUT/$id.txt" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then
    printf '%-5s PASS  %s\n' "$id" "$name"; pass=$((pass+1))
  else
    printf '%-5s FAIL  %s (rc=%d)\n' "$id" "$name" "$rc"; fail=$((fail+1))
    sed -n '/FAIL/p' "$OUT/$id.txt" | tail -5
  fi
}

# ---- section A: UI regression on current main -----------------------------
# A1-A12 all ride the one harness that drives real frames with injected mouse
# and key events; the canned action list is the matrix. Run whole, once.
item A1-12 "Browse panel: click/dbl separation, chevron zone, history, multi-select Enter, preview scrub, instances" \
  -- --browse-keys-selftest tools/testdata/rb
# A13-A16 compare seats, ESC one-step-outward, stale B, right-click items
item A13-16 "compare A/B: seat letters+dimming, ESC dispatch, stale hold, no greyed Swap" \
  -- --abstats-selftest tools/testdata/multi
# A17 tiles: per-pane identity, union label, narrow elision
item A17  "tile/montage panes: letter+batch identity, union label, narrow elision" \
  -- --tile-selftest tools/testdata/multi
# A18 montage naming + per-frame-range note + n-of-N honesty
item A18  "ROI montage: name carries (montage H/V[, per-frame range]), n of N" \
  -- --export-selftest tools/testdata/multi --stack always
# A19 Temporal x-axis paste parsing (newline / CRLF / mixed separators)
item A19  "Temporal x axis paste: comma/space/tab/newline/CRLF, positioned errors" \
  -- --export-tsv-selftest tools/testdata/multi --stack always --cfa bayer --bayer-pattern RGGB
# A20 frame-linearity panel sections and their stubs
item A20  "frame-lin panel: sections, 'set the x axis first' stub, both plots inside" \
  -- --frame-lin-selftest tools/testdata
# A21 new-window argv (placement itself is 実機確認)
item A21  "new window: argv shape, --secondary, refusal is one Messages line" \
  -- --newwin-selftest tools/testdata
# A22 derive dialog pairing counts
item A22  "derive dialog: by-name pairing counts" \
  -- --derive-selftest tools/testdata
# A23/A24 Browse row model, grouped vs flat, local browse identity
item A23  "Browse rows: grouped/flat parity, member cells blank" \
  -- --browse-selftest tools/testdata/rb
item A24  "local browse: title/tag/peer/header identity" \
  -- --localbrowse-selftest tools/testdata/rb

# ---- section B: 設計正典 expressible as UI state --------------------------
item B3   "CFA planes never mixed in the UI tables (one row per plane)" \
  -- --export-tsv-selftest tools/testdata/multi --stack always --cfa bayer --bayer-pattern RGGB

echo
echo "UI MATRIX pass=$pass fail=$fail   (traces in $OUT)"
[ $fail -eq 0 ]
