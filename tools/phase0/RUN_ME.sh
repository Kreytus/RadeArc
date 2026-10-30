#!/usr/bin/env bash
# Phase 0 on the Ubuntu live USB, in ONE boot: boot the stick with  modprobe.blacklist=amdgpu  added to the GRUB
# "linux" line, then run   sudo bash RUN_ME.sh
#   1. reads the display pipe the firmware (GOP) lit, before any driver touches it
#   2. loads amdgpu once while recording its register writes (screen flickers for a few seconds)
#   3. collects everything amdgpu then reports (IP discovery, firmware, EDID, CU config)
# Nothing has to survive a reboot. The result is saved on the stick if it is writable, else on any other writable
# USB drive, else in the home folder (then upload it with Firefox BEFORE shutting down: the live session is in RAM).
set -u
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
HERE=$(cd "$(dirname "$0")" && pwd)
# Loading amdgpu restarts the desktop session (logout), which kills a script running in its Terminal: run detached
# from the session, log to a file and only follow the log here.
LOG=/root/phase0-run.log
if [ -z "${PHASE0_DETACHED:-}" ]; then
    PHASE0_DETACHED=1 setsid nohup bash "$0" "$@" > "$LOG" 2>&1 < /dev/null &
    echo "Running in the background; it keeps going even if you get logged out."
    echo "If that happens: log back in (user ubuntu, no password), open Terminal and run   sudo tail -f $LOG"
    echo
    sleep 1; exec tail --pid=$! -n +1 -f "$LOG"
fi
WORK=/root/phase0-out
BDF=$(lspci -D -d 1002: -n | awk '$2 ~ /^03(00|80):/ {print $1; exit}')
[ -n "$BDF" ] || { echo "no AMD GPU found: is Integrated Graphics set to Forces in the BIOS?"; exit 1; }

if [ -e "/sys/bus/pci/devices/$BDF/driver" ] && [ -s "$WORK/gop/gop_regs.txt" ]; then
    # an earlier run in this boot got as far as loading amdgpu (e.g. the session ended under it): finish that one
    echo "== finishing the earlier run of this boot"
    T=/sys/kernel/tracing
    if [ ! -s "$WORK/trace-dc1/regtrace.txt.xz" ] && [ -e $T/trace ]; then
        echo 0 > $T/tracing_on; mkdir -p "$WORK/trace-dc1"
        xz -T0 < $T/trace > "$WORK/trace-dc1/regtrace.txt.xz"; dmesg > "$WORK/trace-dc1/dmesg.txt"
    fi
    FINISH=1
elif [ -e "/sys/bus/pci/devices/$BDF/driver" ]; then
    cat <<'EOF'
amdgpu is already loaded, so the firmware display state is gone in this boot. Reboot and:
  1. F12 -> the "UEFI: <stick>" entry. In the GRUB menu (tap Esc if it does not show) highlight
     "Try or Install Ubuntu" and press  e
  2. On the line starting with  linux  press End, type a space and   modprobe.blacklist=amdgpu
  3. Press F10. Then run this command again.
EOF
    exit 1
fi

# optional tools; everything still works without them (no network = skipped)
if ! command -v vulkaninfo >/dev/null || ! command -v edid-decode >/dev/null; then
    echo "== installing vulkan-tools and edid-decode (needs internet, skipped otherwise)"
    timeout 30 add-apt-repository -y universe >/dev/null 2>&1
    timeout 120 apt-get update -qq >/dev/null 2>&1 && \
        timeout 300 apt-get install -y -qq vulkan-tools mesa-vulkan-drivers edid-decode >/dev/null 2>&1 || \
        echo "   (no network: skipped, not a problem)"
fi

if [ -z "${FINISH:-}" ]; then
    rm -rf "$WORK"
    echo "== 1/3 reading the firmware (GOP) display pipe"
    bash "$HERE/phase0.sh" gop "$WORK" > /dev/null
    grep -E 'OTG[0-3]_OTG_CONTROL|seg[01] C2PMSG_(81|90)|^pci' "$WORK/gop/gop_regs.txt"
    echo
    echo "== 2/3 loading amdgpu and recording it (about 40 s). The screen may go black and you may get logged out:"
    echo "   that is expected and the script keeps running. Log back in and run   sudo tail -f $LOG"
    sleep 3
    bash "$HERE/phase0.sh" trace "$WORK" 1
fi

echo "== 3/3 collecting what amdgpu reports"
bash "$HERE/phase0.sh" driver "$WORK" > /dev/null
python3 -I "$HERE/gop_dump.py" --bdf x --compare "$WORK/driver/ip_discovery" | tee "$WORK/gop/bases_check.txt"

# where to save: the stick itself (FAT32 from Rufus can be remounted read-write), any other writable USB drive, home
TAR=phase0-8500g.tar.xz
tar -C /root -cJf "/root/$TAR" phase0-out
MNT=$(findmnt -n -o TARGET --target "$HERE")
mount -o remount,rw "$MNT" 2>/dev/null
SAVED=""
for d in "$(dirname "$HERE")" /media/*/* /run/media/*/*; do
    [ -d "$d" ] && cp "/root/$TAR" "$d/$TAR" 2>/dev/null && sync && { SAVED="$d/$TAR"; break; }
done
HOME_DIR=$(getent passwd "${SUDO_USER:-ubuntu}" | cut -d: -f6)
cp "/root/$TAR" "${HOME_DIR:-/root}/$TAR" 2>/dev/null && chmod a+r "${HOME_DIR:-/root}/$TAR"
echo
if [ -n "$SAVED" ]; then
    echo "== All done: saved $SAVED ($(du -h "/root/$TAR" | cut -f1)). Shut down and send me that file."
else
    echo "== All done, but no writable USB drive was found."
    echo "   The file is $HOME_DIR/$TAR (Home folder). The live session forgets it at shutdown, so FIRST either"
    echo "   plug in another USB stick and copy it there, or upload it with Firefox (Google Drive, email, ...)."
fi
