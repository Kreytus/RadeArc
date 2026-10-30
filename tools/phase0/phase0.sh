#!/usr/bin/env bash
# Phase 0: collect the facts about a Phoenix / Phoenix 2 APU from a Linux live USB.
# Run as root. Everything is read-only except `trace`, which loads amdgpu (a normal driver load).
#
#   sudo bash phase0.sh driver  [out]   boot 1: normal boot, amdgpu loaded, monitor on the iGPU output
#   sudo bash phase0.sh gop     [out]   boot 2: kernel cmdline modprobe.blacklist=amdgpu (GOP still drives the monitor)
#   sudo bash phase0.sh trace   [out] [dc]  boot 2, after `gop`: MMIO trace of amdgpu bring-up (dc=0 GFX only, dc=1 with display)
#
# out defaults to ./phase0-out. Put it on the USB stick and send the whole directory back as a tarball.
set -u
CMD=${1:-}; OUT=$(realpath -m "${2:-./phase0-out}"); HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
# the AMD display-class device on bus 0 of the APU (class 0300 VGA or 0380 display)
BDF=$(lspci -D -d 1002: -n | awk '$2 ~ /^03(00|80):/ {print $1; exit}')
[ -n "$BDF" ] || { echo "no AMD display device found"; exit 1; }
DEV=/sys/bus/pci/devices/$BDF
run() { local f=$1; shift; echo "\$ $*" > "$f"; "$@" >> "$f" 2>&1 || echo "(exit $?)" >> "$f"; }

case "$CMD" in
driver)
    D=$OUT/driver; mkdir -p "$D"
    [ -e "$DEV/driver" ] || { echo "amdgpu is not bound to $BDF: boot without modprobe.blacklist=amdgpu"; exit 1; }
    mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
    echo "$BDF" > "$D/bdf"
    run "$D/uname.txt" uname -a
    cp /proc/cmdline "$D/cmdline.txt"
    run "$D/cpu.txt" grep -m1 "model name" /proc/cpuinfo
    run "$D/lspci-nn.txt" lspci -nn
    run "$D/lspci-gpu.txt" lspci -nnvvv -s "$BDF"
    cp "$DEV/resource" "$D/pci-resource.txt"
    run "$D/dmidecode.txt" dmidecode -t bios -t baseboard -t processor
    dmesg > "$D/dmesg.txt"
    grep -iE "amdgpu|drm|psp|smu|imu|rlc|mes|kiq|discovery|vram|gtt|uma" "$D/dmesg.txt" > "$D/dmesg-amdgpu.txt"
    # IP discovery as amdgpu parsed it (amdgpu_discovery.c sysfs: die/0/<IP>/<inst>/{major,minor,revision,base_addr,...})
    cp -rL "$DEV/ip_discovery" "$D/ip_discovery" 2>/dev/null
    ( cd "$DEV/ip_discovery/die/0" && for ip in *; do [ -L "$ip" ] || continue
        for i in "$ip"/*/; do printf "%-8s inst %s  v%s.%s.%s  harvest %s  bases %s\n" "$ip" "$(basename "$i")" \
          "$(cat "$i/major")" "$(cat "$i/minor")" "$(cat "$i/revision")" "$(cat "$i/harvest")" "$(tr '\n' ' ' < "$i/base_addr")"; done
      done ) > "$D/ip_discovery.txt" 2>&1
    for c in /sys/kernel/debug/dri/*/; do
        [ -e "$c/amdgpu_firmware_info" ] || continue
        n=$(basename "$c")
        cat "$c/amdgpu_firmware_info"   > "$D/dri$n-firmware_info.txt" 2>&1
        cat "$c/amdgpu_discovery"       > "$D/dri$n-ip_discovery.bin" 2>/dev/null   # raw table (amdgpu_debugfs.c blob)
        cat "$c/amdgpu_vbios"           > "$D/dri$n-vbios.rom" 2>/dev/null
        cat "$c/amdgpu_dm_dtn_log"      > "$D/dri$n-dm_dtn_log.txt" 2>&1           # DCN HW state as Linux programmed it
        cat "$c/state"                  > "$D/dri$n-state.txt" 2>&1                 # connector -> CRTC -> mode
        cat "$c/amdgpu_pm_info"         > "$D/dri$n-pm_info.txt" 2>&1
        for k in "$c"/*/; do            # per connector: DP link settings, DSC, HDMI/DP type
            [ -e "$k/link_settings" ] || continue
            kk=$(basename "$k"); mkdir -p "$D/dri$n-conn/$kk"
            for f in link_settings dp_dsc_fec_support output_bpc; do cat "$k/$f" > "$D/dri$n-conn/$kk/$f" 2>&1; done
        done
    done
    # every connector: status, modes, EDID (the monitor's real EDID over DDC or AUX)
    for k in /sys/class/drm/card*-*; do
        kk=$(basename "$k"); mkdir -p "$D/connectors/$kk"
        for f in status enabled modes dpms; do cat "$k/$f" > "$D/connectors/$kk/$f" 2>/dev/null; done
        [ -s "$k/edid" ] && cp "$k/edid" "$D/connectors/$kk/edid.bin" && \
            { command -v edid-decode >/dev/null && edid-decode "$k/edid" > "$D/connectors/$kk/edid.txt" 2>&1; }
    done
    for f in mem_info_vram_total mem_info_vis_vram_total mem_info_gtt_total; do
        echo "$f $(cat "$DEV/$f" 2>/dev/null)"; done > "$D/memory.txt"
    cp /sys/firmware/acpi/tables/VFCT "$D/VFCT.bin" 2>/dev/null   # the APU's VBIOS image (connector table)
    # CU/WGP/SE configuration and GB_ADDR_CONFIG as Mesa sees them (RADV prints its whole ac_gpu_info)
    if command -v vulkaninfo >/dev/null; then
        RADV_DEBUG=info vulkaninfo --summary > "$D/radv-info.txt" 2>&1
        vulkaninfo > "$D/vulkaninfo.txt" 2>&1
    else
        echo "vulkaninfo missing: apt install vulkan-tools mesa-vulkan-drivers" > "$D/radv-info.txt"
    fi
    echo "saved in $D"; ls "$D"
    ;;
gop)
    D=$OUT/gop; mkdir -p "$D"
    [ -e "$DEV/driver" ] && { echo "a driver is bound to $BDF: boot with modprobe.blacklist=amdgpu"; exit 1; }
    [ -d "$OUT/driver/ip_discovery" ] || { echo "run '$0 driver' first (in a normal boot)"; exit 1; }
    grep -q '\[none\]' /sys/kernel/security/lockdown 2>/dev/null || \
        echo "warning: kernel lockdown is on (Secure Boot?): PCI BAR reads will be refused"
    run "$D/lspci-gpu.txt" lspci -nnvvv -s "$BDF"
    cp "$DEV/resource" "$D/pci-resource.txt"
    cp /proc/cmdline "$D/cmdline.txt"
    run "$D/fb.txt" cat /proc/fb
    python3 -I "$HERE/gop_dump.py" --bdf "$BDF" --disc "$OUT/driver/ip_discovery" > "$D/gop_regs.txt" 2>&1
    cat "$D/gop_regs.txt"
    ;;
trace)
    # Same method as nullarc tools/linux-trace/trace.sh (amdgpu_device_wreg/rreg tracepoints), version-independent.
    DC=${3:-0}; D=$OUT/trace-dc$DC; mkdir -p "$D"
    [ -e "$DEV/driver" ] && { echo "amdgpu already bound: boot with modprobe.blacklist=amdgpu"; exit 1; }
    mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
    T=/sys/kernel/tracing
    echo 0 > $T/tracing_on; echo 524288 > $T/buffer_size_kb; : > $T/trace
    echo 1 > $T/events/amdgpu/amdgpu_device_wreg/enable
    echo 1 > $T/events/amdgpu/amdgpu_device_rreg/enable
    echo 1 > $T/tracing_on
    modprobe amdgpu dc="$DC" > "$D/modprobe.txt" 2>&1
    sleep 25
    echo 0 > $T/tracing_on
    xz -T0 < $T/trace > "$D/regtrace.txt.xz"
    dmesg > "$D/dmesg.txt"
    echo "saved in $D ($(xz -l "$D/regtrace.txt.xz" | tail -1))"
    ;;
*)
    sed -n '2,10p' "$0"; exit 1 ;;
esac
