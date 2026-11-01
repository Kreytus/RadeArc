#!/bin/bash
# testj.sh: Phase 3b Test J, the driver's DMCUB firmware (docs/PHASE3B_PLAN.md 4d, docs/TEST_PHOENIX2.md).
# Runs from the unpacked install package (it uses the package's gpu/phxctl and gpu/fw), nothing is installed.
#
#   bash testj.sh dry    read-only: the DMCUB state and every step it would take; writes nothing
#   bash testj.sh run    the PSP loads dcn_3_1_4_dmcub.bin (type 51, as amdgpu), then dmcubload resets DMCUB, sets up
#                        its windows and starts it; checks dal_fw = 1 and one NULL command. Needs -phxp2 phxdebug=1 and
#                        the GPU up (phxboot). A 3 min reboot watchdog covers a hang.
# Output: ~/Desktop/testj-<dry|run>.txt
D=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-dry}
P=$D/gpu/phxctl; F=$D/gpu/fw
FW=$F/dcn_3_1_4_dmcub.bin; VB=$F/vbios_b650m_8500g.rom
OUT=$HOME/Desktop/testj-$MODE.txt
xattr -dr com.apple.quarantine "$D/gpu" 2>/dev/null
chmod +x "$P"/* 2>/dev/null
{
    echo "== $(date '+%F %T') testj $MODE ($(cat "$D/RADEARC_BUILD.txt" 2>/dev/null | head -1))"
    ioreg -rc PhxBringup | grep -E '"phx-(build|chip)"'
    case "$MODE" in
    dry)
        sudo "$P/dmcubload" --dry-run "$FW" "$VB"
        ;;
    run)
        sudo bash -c "nohup shutdown -r +3 >/dev/null 2>&1 &"
        size=$(python3 -c "import struct,sys;print(struct.unpack_from('<I',open(sys.argv[1],'rb').read(),32)[0])" "$FW")
        echo "== PSP: load DMUB (inst_const_bytes $size from offset 256, type 51)"
        sudo "$P/phxctl" loadfw "$FW" 256 "$size" 51 0x0
        echo "== dmcubload"
        sudo "$P/dmcubload" "$FW" "$VB"
        echo "dmcubload exit $?"
        sudo killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
        ;;
    *) echo "usage: bash testj.sh dry|run" ;;
    esac
} > "$OUT" 2>&1
echo "done: $OUT"
