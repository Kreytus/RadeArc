#!/bin/bash
# Phoenix GFX power-up in the Linux driver's order: PSP ring, TOC and TMR, the firmware of every engine through
# LOAD_IP_FW, RLC autoload, then the SMU messages that switch GFX on (EnableGfxImu with 1). Firmware from ~/gpu/fw; which
# files, offsets and sizes comes from phxfw (the chip's IP versions and the files' own headers, src/amd/rdna3_fw_plan),
# no longer from offsets hardcoded for gc_11_0_1. Run as root (phxboot) or as a user with sudo.
# A reboot is armed for 3 minutes in case the PSP hangs and cancelled at the end. Exit status 0 only when it got through.
set -u
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
P(){ $SUDO ~/gpu/phxctl/phxctl "$@"; }
# the plan first: an unknown chip or a missing/malformed firmware file loads nothing
PLAN=$(~/gpu/phxctl/phxfw ~/gpu/fw) || { echo "== no firmware plan (see phxfw above): nothing loaded"; exit 1; }
read -r _ CHIP _ <<<"$(grep '^CHIP ' <<<"$PLAN")"
echo "== chip $CHIP: $(grep '^CHIP ' <<<"$PLAN" | cut -d' ' -f3-)"
read -r _ TOCF TOCOFF TOCLEN <<<"$(grep '^TOC ' <<<"$PLAN")"
echo "== watchdog: reboot in 3 min unless cancelled =="
$SUDO bash -c 'nohup shutdown -r +3 >/dev/null 2>&1 &'
stop(){ echo "== $1: stopping, nothing more is sent"; $SUDO killall shutdown 2>/dev/null; exit 1; }
P psp destroy >/dev/null 2>&1
# phxctl always exits 0; the IOKit result is the "kr" it prints (PhxBringup refuses with 0xe00002c7 on a chip whose
# bring-up is not enabled, e.g. Phoenix 2 for now)
P psp create 0x1100000 | grep -q 'kr 0x00000000' || stop "PSP ring not created"
P stage "$TOCF" "$TOCOFF" "$TOCLEN" 0x0 >/dev/null
if [ "$CHIP" = phoenix ]; then
    # the 780M: exactly the LOAD_TOC and SETUP_TMR upstream ran (raw commands, 64 MiB TMR at VRAM 128 MiB)
    P raw 0x20 0x04000000 0x80 "$(printf '0x%x' "$TOCLEN")" 0 0 0 >/dev/null
    P raw 0x5 0x08000000 0x80 0x4000000 0x2 0x0C000000 0x6A >/dev/null
else
    # as Linux (amdgpu_psp.c psp_tmr_init / psp_load_toc / psp_prep_tmr_cmd_buf): LOAD_TOC gives the TMR size, SETUP_TMR
    # gets that size with the TMR's MC and real system addresses (the kext computes both from the FB layout)
    OUT=$(P toc "$TOCLEN"); echo "$OUT"
    grep -q 'kr 0x00000000  status 0x00000000' <<<"$OUT" || stop "LOAD_TOC failed"
    TMR=$(sed -n 's/.*tmr_size \(0x[0-9a-f]*\).*/\1/p' <<<"$OUT")
    [ -n "$TMR" ] && [ "$((TMR))" -gt 0 ] || stop "LOAD_TOC returned no TMR size"
    OUT=$(P tmrtoc "$TMR"); echo "$OUT"
    grep -q 'kr 0x00000000  status 0x00000000' <<<"$OUT" || stop "SETUP_TMR failed"
fi
echo "== load firmware =="
L(){ P loadfw "$@" 2>&1 | grep loadfw >> ~/gpu/fwaddr.log; }
: > ~/gpu/fwaddr.log
grep '^L ' <<<"$PLAN" | while read -r _ f off size type dst; do L "$f" "$off" "$size" "$type" "$dst"; done
NL=$(grep -c '^L ' <<<"$PLAN")
[ "$(grep -c 'kr 0x00000000' ~/gpu/fwaddr.log)" = "$NL" ] || stop "a firmware load was refused (~/gpu/fwaddr.log)"
grep -v 'status 0x00000000' ~/gpu/fwaddr.log | sed 's/^/   PSP status not 0 (as upstream, not fatal): /'

echo "== AUTOLOAD_RLC =="
P autoload
echo "== SMU 0x3, 0x2, EnableGfxImu(param=1) (linux order) =="
P smu 0x3 0 | tail -1
P smu 0x2 0 | tail -1
P smu 0x16 1 | tail -1
sleep 2
P regs 0xee7e 0x21a0 0x2004 0x263e
$SUDO killall shutdown 2>/dev/null; echo "watchdog cancelled rc=$?"
exit 0
