#!/bin/bash
# bocheck.sh: the GPU buffer counters of PhxBringup build >= 38 (read-only; build 39 adds the host-import ones). Run it when the desktop flickers: a non-zero
# handles_full / maps_full / nomem says which per-process limit WindowServer hit; the peaks say how close it got.
#   bash bocheck.sh       -> ~/Desktop/bocheck.txt
OUT=$HOME/Desktop/bocheck.txt
{
    echo "== $(date '+%F %T')"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sysctl debug 2>/dev/null | grep phxbo
    L=$(log show --last boot --style compact --predicate 'process == "WindowServer" AND eventMessage CONTAINS "Failed to allocate"' 2>/dev/null | grep "Failed to allocate")
    echo "WindowServer allocation failures since this boot: $(printf '%s' "$L" | grep -c .)"
    printf '%s\n' "$L" | tail -3 | cut -c1-160
} > "$OUT" 2>&1
cat "$OUT"
