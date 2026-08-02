#!/bin/bash
# Diff two ab_capture.sh output dirs.
#   usage: ab_diff.sh <dirA> <dirB> [--raw]
# Default is the NORMALIZED diff; --raw skips normalization entirely so the
# unfiltered difference can be inspected before any rule is justified.
#
# Normalization rules (each one exists because the raw diff proved the text is
# run-specific, never because it was convenient - see docs/verify-functional.md):
#   N1  the --remote-exe path: each build must launch ITS OWN viewer-serve.exe,
#       so this argument is different by construction, not by behavior.
#   N2  the pinned per-pass APPDATA / TMP / capture directory paths.
#   N3  elapsed wall-clock readings (ms, "in <x> s"). NOT "%.2f MB on the wire" -
#       that is a data volume, deterministic, and must stay under comparison.
#   N4  raw heap addresses from %p (abstatsselftest CmpOff/closeStack lines,
#       verifyselftest V4 resolveB). ASLR makes the VALUE run-specific, but
#       null-vs-non-null is behavioral - so an all-zero pointer normalizes to
#       <NULLPTR> and any other to <PTR>, keeping that distinction under test.
# Anything else that differs is a real behavioral difference.
set -u
A="$1"; B="$2"; MODE="${3:-normalized}"

norm() {
  if [ "$MODE" = "--raw" ]; then cat "$1"; return; fi
  sed -E \
    -e 's#[A-Za-z]:[\\/][^ "]*viewer-serve\.exe#<SERVE_EXE>#g' \
    -e 's#[A-Za-z]:[\\/][^ "]*viewer-wt-verify(-base)?#<WORKTREE>#g' \
    -e 's#[A-Za-z]:[\\/][^ "]*verifyagent_ab/(capA|capB)#<CAPDIR>#g' \
    -e 's#(capA|capB)#<PASS>#g' \
    -e 's#([0-9]+\.[0-9]+|[0-9]+) ?(ms|MB/s)#<TIME>#g' \
    -e 's#in [0-9]+\.[0-9]+ s#in <TIME> s#g' \
    -e 's#generated: [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}#generated: <TIMESTAMP>#g' \
    -e 's#(img|resolveB|cmpB\(\)|held)=(0x)?0+\b#\1=<NULLPTR>#g' \
    -e 's#(img|resolveB|cmpB\(\))=(0x)?[0-9a-fA-F]+#\1=<PTR>#g' \
    -e 's#held (0x)?0+\b#held <NULLPTR>#g' \
    -e 's#held (0x)?[0-9a-fA-F]+#held <PTR>#g' \
    "$1"
}

same=0; differ=0; missing=0; rcdiff=0
: > "$A/../diff_report.txt"
R="$A/../diff_report.txt"
for f in "$A"/caps/*.out; do
  n=$(basename "$f" .out)
  if [ ! -f "$B/caps/$n.out" ]; then echo "MISSING IN B: $n" | tee -a "$R"; missing=$((missing+1)); continue; fi
  ra=$(cat "$A/caps/$n.rc" 2>/dev/null); rb=$(cat "$B/caps/$n.rc" 2>/dev/null)
  [ "$ra" != "$rb" ] && { echo "RC DIFF   $n : A=$ra B=$rb" | tee -a "$R"; rcdiff=$((rcdiff+1)); }
  norm "$A/caps/$n.out" > "$A/caps/$n.norm"
  norm "$B/caps/$n.out" > "$B/caps/$n.norm"
  if diff -q "$A/caps/$n.norm" "$B/caps/$n.norm" >/dev/null; then
    same=$((same+1))
  else
    differ=$((differ+1))
    { echo "=== OUTPUT DIFF  $n  (rc A=$ra B=$rb) ==="
      diff "$A/caps/$n.norm" "$B/caps/$n.norm" | head -40; } | tee -a "$R"
  fi
done
echo "AB SUMMARY identical=$same differing=$differ rc_differing=$rcdiff missing=$missing" | tee -a "$R"
[ $differ -eq 0 ] && [ $rcdiff -eq 0 ] && [ $missing -eq 0 ]
