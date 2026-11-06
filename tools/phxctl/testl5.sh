#!/bin/bash
# testl5.sh: Phase 3b Test L step 5 (docs/TEST_PHOENIX2.md). Runs from the unpacked install package.
#   bash testl5.sh           read-only: which OPP feeds OTG0, the DPG state
#   bash testl5.sh --blank   5a: blank the picture for 3 s the Linux way, unblank
#   bash testl5.sh --dpms    5b: Linux's DPMS off / on for the DVI link (the monitor loses the signal for 3 s);
#                            needs the driver's DMCUB: `bash testj.sh run` first in the same boot
#   bash testl5.sh --modeset 5c: Linux's whole mode set at the same 60 Hz (link, OTG, front end); also needs testj.sh run
#   bash testl5.sh --hdmi    5d: the same mode set, but the link comes up as HDMI (AVI InfoFrame, SCDC); testj.sh run first
#   bash testl5.sh --scdc    the monitor's SCDC registers (read-only); --scdc-write0 also writes TMDS_CONFIG 0 (DVI only)
#   bash testl5.sh --dvi     5d: back to DVI the same way (GOP's signal); a restart also brings DVI back
# Needs boot-args -phxp2 phxdebug=1. Output: ~/Desktop/testl5.txt
D=$(cd "$(dirname "$0")" && pwd)
P=$D/gpu/phxctl
OUT=$HOME/Desktop/testl5.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testl5 $* ($(head -1 "$D/RADEARC_BUILD.txt" 2>/dev/null))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    sysctl -n kern.bootargs
    sudo "$P/testl5" "$@"
    echo "testl5 exit $?"
} 2>&1 | tee "$OUT"
echo "done: $OUT"
