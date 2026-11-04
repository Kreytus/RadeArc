#!/bin/bash
# testl2.sh: Phase 3b Test L step 2 (docs/TEST_PHOENIX2.md). Runs from the unpacked install package.
#   bash testl2.sh           dry run: what would be written, nothing written
#   bash testl2.sh --apply   write Linux's 60 Hz values for 15 s, watch, restore everything
# Needs boot-args -phxp2 phxdebug=1. Output: ~/Desktop/testl2.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testl2.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testl2 $* ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sysctl -n kern.bootargs
    sudo "$P/testl2" "$@"
    echo "testl2 exit $?"
} 2>&1 | tee "$OUT"
echo "done: $OUT"
