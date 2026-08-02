#!/bin/bash
# Section B: the measurement invariants that must hold at EVERY future stage.
# Asserted against a capture dir produced by ab_capture.sh, so every one is a
# statement about real captured output, not about the source.
#
# Each assertion quotes text that was FIRST read out of a real capture - an
# assertion aimed at a file that does not carry the marker would "fail" without
# telling you anything about the product, which proves nothing.
#   usage: invariants.sh <capdir>
set -u
C="$1/caps"
ok=0; bad=0
say() { if [ "$3" -eq 0 ]; then echo "INV $1 PASS  $2"; ok=$((ok+1));
        else echo "INV $1 FAIL  $2"; bad=$((bad+1)); fi; }

# B-1  sigma_t is a property of the STACK, never of a frame.
grep -q "temporal summary (stack statistics; sigma_t is a property of the stack, never of a frame)" \
     "$C/texport.out"; say B-1a "exported temporal summary is headed a STACK statistic" $?
# ...and the PER-FRAME table must not carry a sigma_t column, or it would be one.
if grep -qE "^frame	file.*sigma_t" "$C/framestats.out"; then r=1; else r=0; fi
say B-1b "per-frame table carries mean/sigma but NO sigma_t column" $r
grep -qE "^frame	file	mean \[DN\]	sigma \[DN\]" "$C/framestats.out"
say B-1c "per-frame table header is the expected mean/sigma form" $?

# B-2  CFA planes are never mixed: with a mosaic every column is per plane.
grep -qE "mean_R \[DN\].*mean_Gr \[DN\].*mean_Gb \[DN\].*mean_B \[DN\]" "$C/texport.out"
say B-2a "mosaiced per-frame table is per plane (R/Gr/Gb/B), never pooled" $?
grep -q "E9 B's sigma_t is temporal\[1\]'s number, not A's relabelled" "$C/texport.out"
say B-2b "each side's sigma_t is its own number, not a relabelled neighbour" $?

# B-3  partial loads say n of N - never a bare count that hides the shortfall.
grep -qE "resident [0-9]+ of [0-9]+ frame\(s\)" "$C/texport.out"
say B-3a "export provenance states residency as 'resident n of N frame(s)'" $?
grep -qE "^side	ch	n	N	sigma_t \[DN\]" "$C/texport.out"
say B-3b "temporal summary carries explicit n and N columns" $?

# B-4/B-5  a quantity carried into a label carries its unit; pixel value is [DN].
grep -qE "sigma_t \[DN\]	sigma_fpn \[DN\]	sigma_tot \[DN\]" "$C/texport.out"
say B-4a "stack noise quantities are labelled [DN]" $?
grep -qE "sigma_frame \[%\]|sigma_col \[%\]" "$C/texport.out"
say B-4b "relative quantities are labelled [%], distinct from [DN]" $?
grep -qE "mean \[DN\]" "$C/framestats.out"; say B-5 "pixel value is [DN]" $?

echo "INVARIANTS pass=$ok fail=$bad"
[ $bad -eq 0 ]
