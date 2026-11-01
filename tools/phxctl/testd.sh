#!/bin/bash
# testd.sh: Phase 2 hardware test (docs/TEST_PHOENIX2.md, Test D). Shipped in the "mesa-radv" CI artifact; copy that
# folder to ~/gpu/mesa and run:  bash ~/gpu/mesa/testd.sh
# 1. GPU bring-up, MES and the two queues handed to PhxBringup (Test C, full.sh: its own 4 min reboot watchdog).
# 2. vkprobe on the real kext with this folder's RADV: the device RADV picked, then a compute dispatch (fill.comp).
#    A 3 min reboot watchdog covers it too, and vkprobe is killed after 60 s.
# Everything goes to ~/Desktop/testd.txt. Needs boot-arg -phxp2 (Phoenix 2) and ~/gpu/full.sh, bringup5.sh, phxctl/.
D=$(cd "$(dirname "$0")" && pwd)
OUT=$HOME/Desktop/testd.txt
{
    echo "== $(date '+%F %T') testd, folder $D"
    cat "$D/BUILD.txt" 2>/dev/null
    xattr -dr com.apple.quarantine "$D" 2>/dev/null
    chmod +x "$D/vkprobe"
    echo "== bring-up and queues (Test C)"
    if ! MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash ~/gpu/full.sh kiqsched; then
        echo "full.sh failed: vkprobe not run"
        exit 1
    fi
    echo "== vkprobe on the real kext"
    sudo bash -c "nohup shutdown -r +3 >/dev/null 2>&1 &"
    cd "$D" && VK_ICD_FILENAMES="$D/radeon_icd.json" perl -e 'alarm 60; exec @ARGV' ./vkprobe 2>&1
    echo "vkprobe exit $?"
    sudo killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
} > "$OUT" 2>&1
echo "done: $OUT"
