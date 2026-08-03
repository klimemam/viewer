#!/bin/sh
# median / min / max of the ms= field, so timings are never reported as
# a single laptop number. Reads bench lines on stdin.
awk '
match($0, /ms=[0-9.]+/) {
  v = substr($0, RSTART+3, RLENGTH-3) + 0
  # first run of each file touches cold disk on a 100 MB fixture; label it
  # but keep it out of the median
  key = $1 " " $2
  n[key]++
  if (n[key] == 1) { first[key] = v; next }
  a[key, ++m[key]] = v
}
END {
  for (k in m) {
    cnt = m[k]
    for (i = 1; i <= cnt; i++) for (j = i+1; j <= cnt; j++)
      if (a[k,j] < a[k,i]) { t = a[k,i]; a[k,i] = a[k,j]; a[k,j] = t }
    med = (cnt % 2) ? a[k, (cnt+1)/2] : (a[k, cnt/2] + a[k, cnt/2+1]) / 2
    printf "%s  n=%d  median=%.1f ms  min=%.1f  max=%.1f  spread=%.1f%%  [first(cold)=%.1f]\n",
           k, cnt, med, a[k,1], a[k,cnt], (a[k,cnt]-a[k,1])/med*100, first[k]
  }
}' "$@"
