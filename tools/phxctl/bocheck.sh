#!/bin/bash
# bocheck.sh: the GPU buffer counters of PhxBringup build 38 (read-only). Run it when the desktop flickers: a non-zero
# handles_full / maps_full / nomem says which per-process limit WindowServer hit; the peaks say how close it got.
#   bash bocheck.sh       -> ~/Desktop/bocheck.txt
OUT=$HOME/Desktop/bocheck.txt
{
    echo "== $(date '+%F %T')"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sysctl debug.phxbo_handles_full debug.phxbo_maps_full debug.phxbo_nomem debug.phxbo_peak_handles \
        debug.phxbo_peak_maps 2>&1
    log show --last 10m --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS "Failed to allocate"' \
        2>/dev/null | wc -l | sed 's/^/WindowServer allocation failures in the last 10 min: /'
} > "$OUT" 2>&1
cat "$OUT"
