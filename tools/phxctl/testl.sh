#!/bin/bash
# testl.sh: Phase 3b Test L dry run, read-only (docs/TEST_PHOENIX2.md). Runs from the unpacked install package. Reads
# the HUBP0 / OTG0 registers the 144 Hz switch will write and compares them with Linux's DML values for 60 and 144 Hz.
# Writes nothing to the GPU, needs no reboot.
#
#   bash testl.sh
# Output: ~/Desktop/testl.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testl.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testl ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sudo "$P/testl"
    echo "testl exit $?"
} > "$OUT" 2>&1
echo "done: $OUT"
