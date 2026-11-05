#!/bin/bash
# testl4.sh: Phase 3b Test L step 4a (docs/TEST_PHOENIX2.md). Runs from the unpacked install package.
#   bash testl4.sh          read-only: DMCUB state, inbox 1, the planned 144 Hz commands; nothing sent
#   bash testl4.sh --null   also one DMUB_CMD__NULL through inbox 1
# The driver's DMCUB must be running: `bash testj.sh run` first in the same boot (GPU up, kill switch off).
# Needs boot-args -phxp2 phxdebug=1. Output: ~/Desktop/testl4.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testl4.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testl4 $* ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sudo "$P/testl4" "$@"
    echo "testl4 exit $?"
} 2>&1 | tee "$OUT"
echo "done: $OUT"
