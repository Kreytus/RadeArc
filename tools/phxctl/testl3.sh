#!/bin/bash
# testl3.sh: Phase 3b Test L step 3 (docs/TEST_PHOENIX2.md). Runs from the unpacked install package.
#   bash testl3.sh           dry run: the clocks now and the planned SMU messages, nothing sent
#   bash testl3.sh --apply   raise DISPCLK/DPPCLK to the 144 Hz values for 15 s, watch, lower them
# Needs boot-args -phxp2 phxdebug=1. Output: ~/Desktop/testl3.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testl3.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testl3 $* ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sysctl -n kern.bootargs
    sudo "$P/testl3" "$@"
    echo "testl3 exit $?"
} 2>&1 | tee "$OUT"
echo "done: $OUT"
