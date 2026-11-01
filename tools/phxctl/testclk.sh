#!/bin/bash
# testclk.sh: Phase 3b, the display clocks and the SMU's DPM clock table (the DML inputs, docs/PHASE3B_PLAN.md 4e).
# Runs from the unpacked install package. Read-only for the picture: the SMU answers queries and copies its clock
# table into a free VRAM buffer. No reboot. Needs -phxp2 phxdebug=1 (the SMU mailbox is written).
#
#   bash testclk.sh
# Output: ~/Desktop/testclk.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testclk.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testclk ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sudo "$P/clkinfo" --dry-run
    echo "== full"
    sudo "$P/clkinfo"
    echo "clkinfo exit $?"
} > "$OUT" 2>&1
echo "done: $OUT"
