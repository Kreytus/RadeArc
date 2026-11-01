#!/bin/bash
# testk.sh: Phase 3b Test K, read-only (docs/PHASE3B_PLAN.md 7, docs/TEST_PHOENIX2.md). Runs from the unpacked install
# package. Reads the monitor's EDID (phxctl edid), then modecheck compares the OTG timing Linux would program for the
# EDID's 60 Hz mode with the live registers, measures the refresh, and dumps the registers the 144/180 Hz switch will
# touch. Writes nothing to the GPU, needs no reboot.
#
#   bash testk.sh
# Output: ~/Desktop/testk.txt (and ~/Desktop/testk-edid.bin)
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testk.txt; EDID=$HOME/Desktop/testk-edid.bin
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testk ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sudo "$P/phxctl" edid "$EDID" | head -3
    sudo "$P/modecheck" "$EDID"
    echo "modecheck exit $?"
} > "$OUT" 2>&1
echo "done: $OUT"
