#!/usr/bin/env bash
# One command for Phase 0 on the Ubuntu live USB. Run it twice:
#   boot 1 (normal "Try Ubuntu"):                     sudo bash RUN_ME.sh   -> collects with amdgpu loaded
#   boot 2 (GRUB line + modprobe.blacklist=amdgpu):   sudo bash RUN_ME.sh   -> GOP register dump, then the MMIO trace
# Results go next to this folder on the stick (phase0-out/ and phase0-8500g.tar.xz), or to ~/ if the stick is read-only.
set -u
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
HERE=$(cd "$(dirname "$0")" && pwd)

# the live system mounts the stick read-only; FAT32 (Rufus "ISO image" mode) can be remounted read-write
if ! touch "$HERE/.w" 2>/dev/null; then
    mount -o remount,rw "$(findmnt -n -o TARGET --target "$HERE")" 2>/dev/null
fi
if touch "$HERE/.w" 2>/dev/null; then
    rm -f "$HERE/.w"; BASE=$(dirname "$HERE")
else
    BASE=$(getent passwd "${SUDO_USER:-ubuntu}" | cut -d: -f6); BASE=${BASE:-/root}
    echo "!! the stick is read-only: saving to $BASE (upload phase0-8500g.tar.xz from there with Firefox)"
fi
OUT=$BASE/phase0-out
BDF=$(lspci -D -d 1002: -n | awk '$2 ~ /^03(00|80):/ {print $1; exit}')
[ -n "$BDF" ] || { echo "no AMD GPU found: is Integrated Graphics set to Forces in the BIOS?"; exit 1; }

# optional tools; everything still works without them (no network = skipped)
if ! command -v vulkaninfo >/dev/null || ! command -v edid-decode >/dev/null; then
    echo "== installing vulkan-tools and edid-decode (needs internet, skipped otherwise)"
    timeout 30 add-apt-repository -y universe >/dev/null 2>&1
    timeout 120 apt-get update -qq >/dev/null 2>&1 && \
        timeout 300 apt-get install -y -qq vulkan-tools mesa-vulkan-drivers edid-decode >/dev/null 2>&1 || \
        echo "   (no network: skipped, not a problem)"
fi

pack() {
    tar -C "$BASE" -cJf "$BASE/phase0-8500g.tar.xz" phase0-out && sync
    echo; echo "== saved $BASE/phase0-8500g.tar.xz ($(du -h "$BASE/phase0-8500g.tar.xz" | cut -f1)): send me this file"
}

if [ -e "/sys/bus/pci/devices/$BDF/driver" ]; then
    echo "== boot 1: amdgpu is loaded, collecting"
    bash "$HERE/phase0.sh" driver "$OUT" | tail -3
    pack
    cat <<'EOF'

== Boot 1 done. Now boot 2:
   1. Reboot, boot the stick again (F12).
   2. In the GRUB menu, highlight "Try or Install Ubuntu" and press  e
   3. On the line starting with  linux  add a space and   modprobe.blacklist=amdgpu   at the end
      (after "quiet splash" or "---"), then press  F10  (or Ctrl+X).
   4. Open Terminal and run the same command again:   sudo bash /cdrom/phase0-kit/RUN_ME.sh
EOF
else
    [ -d "$OUT/driver/ip_discovery" ] || { echo "boot 1 has not run yet: boot WITHOUT modprobe.blacklist=amdgpu first"; exit 1; }
    echo "== boot 2: amdgpu is not loaded, reading the firmware (GOP) display pipe"
    bash "$HERE/phase0.sh" gop "$OUT" > /dev/null
    grep -E 'OTG[0-3]_OTG_CONTROL|seg[0-9] C2PMSG_(81|90)|^pci' "$OUT/gop/gop_regs.txt"
    pack
    echo
    echo "== last step: recording how Linux brings the GPU up. The screen flickers or goes black for a few"
    echo "   seconds while amdgpu takes over, then comes back. Takes about 40 s. Do not touch anything."
    sleep 5
    bash "$HERE/phase0.sh" trace "$OUT" 1
    pack
    echo "== All done. Shut down, and send me phase0-8500g.tar.xz from the stick."
fi
