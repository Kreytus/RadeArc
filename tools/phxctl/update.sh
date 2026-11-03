#!/bin/bash
# update.sh: install the newest RadeArc package on the 8500G in one go, with the previous one kept for rollback.
#
#   bash update.sh              download the newest package (CI release package-latest); if it is newer than the
#                               installed one: update PhxBringup.kext in OpenCore's EFI, run install.sh
#                               --windowserver, arm the GPU desktop, offer a reboot
#   bash update.sh --force      the same even if that package is already installed
#   bash update.sh --rollback   reinstall the package that was installed before the current one
#   bash update.sh --status     what is installed and what is newest, nothing changed
#
# Asks before changing anything and before rebooting. The kill switch /Users/Shared/gpu/phxboot-off is only removed
# if you say so. Packages are kept in ~/RadeArc/packages (the newest 3), EFI kext backups in
# /Users/Shared/gpu/efi-backup. Run from a package folder elsewhere (e.g. ~/Desktop/radearc-package), that folder is
# replaced with the package just installed, so its scripts match what is installed; the old one goes to the Trash.
set -uo pipefail
SELF=$(cd "$(dirname "$0")" && pwd)
REPO=Kreytus/RadeArc
REL=https://github.com/$REPO/releases/download/package-latest
BASE=$HOME/RadeArc; PKGS=$BASE/packages
G=/Users/Shared/gpu
STATE=$G/radearc-installed.txt     # line 1: package folder, then its RADEARC_BUILD.txt
MODE=${1:-update}

say() { printf '%s\n' "$*"; }
die() { say "✗ $*"; exit 1; }
ask() { local a; read -r -p "$1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]; }
kexts_of() { grep -m1 '^kexts:' "$1" 2>/dev/null | awk '{print $NF}' | cut -c1-7; }

installed_dir() { [ -f "$STATE" ] && head -1 "$STATE"; }
show_status() {
    say "running kext: $(ioreg -rc PhxBringup 2>/dev/null | grep -o '"phx-build" = [0-9]*' || echo 'PhxBringup not loaded')"
    if [ -f "$STATE" ]; then say "installed package:"; tail -n +2 "$STATE" | sed 's/^/  /'; else say "installed package: unknown (not installed with update.sh yet)"; fi
    say "newest package:"
    curl -fsSL "$REL/RADEARC_BUILD.txt" 2>/dev/null | sed 's/^/  /' || say "  (could not reach GitHub)"
    [ -e "$G/phxboot-off" ] && say "kill switch: ON ($G/phxboot-off): the GPU driver is off"
}

# Where OpenCore's PhxBringup.kext lives: EFI partitions (mounted if needed) and every mounted volume (an OpenCore USB
# drive such as "INST") that holds EFI/OC/Kexts/PhxBringup.kext. With more than one, the one OpenCore booted from wins
# (its NVRAM boot-path names the partition's UUID), else you choose.
find_efi() {
    local cands=() d mp v bp u
    for d in $(diskutil list | awk '/EFI/ {print $NF}'); do
        mp=$(diskutil info "$d" 2>/dev/null | awk -F': *' '/Mount Point/ {print $2}')
        [ -n "$mp" ] || { sudo diskutil mount "$d" >/dev/null 2>&1; mp=$(diskutil info "$d" | awk -F': *' '/Mount Point/ {print $2}'); }
        [ -n "$mp" ] && [ -d "$mp/EFI/OC/Kexts/PhxBringup.kext" ] && cands+=("$mp")
    done
    for v in /Volumes/*; do
        [ -d "$v/EFI/OC/Kexts/PhxBringup.kext" ] || continue
        case " ${cands[*]-} " in *" $v "*) ;; *) cands+=("$v") ;; esac
    done
    [ ${#cands[@]} -gt 0 ] || return 1
    [ ${#cands[@]} -eq 1 ] && { echo "${cands[0]}"; return 0; }
    bp=$(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path 2>/dev/null | tr 'a-f' 'A-F')
    for v in "${cands[@]}"; do
        u=$(diskutil info "$v" 2>/dev/null | awk -F': *' '/Partition UUID/ {print $2}' | tr 'a-f' 'A-F')
        [ -n "$u" ] && case "$bp" in *"$u"*) echo "$v"; return 0 ;; esac
    done
    say "PhxBringup.kext is in more than one OpenCore EFI:" >&2
    local i=1; for v in "${cands[@]}"; do say "  $i) $v" >&2; i=$((i + 1)); done
    read -r -p "Which one does this Mac boot from? (number) " i
    [ -n "${cands[$((i - 1))]:-}" ] || return 1
    echo "${cands[$((i - 1))]}"
}

# Replace the package folder this script runs from with $1. Renames only: bash keeps reading this script from the open
# file, so the rest of the run is unaffected.
refresh_self() {
    local P=$1 tr
    [ -f "$SELF/install.sh" ] && [ -f "$SELF/update.sh" ] || return 0
    case "$SELF/" in "$PKGS"/*|"$P"/*) return 0 ;; esac
    [ "$SELF" = "$P" ] && return 0
    rm -rf "$SELF.new" && ditto "$P" "$SELF.new" || { say "  (could not refresh $SELF: kept as it was)"; rm -rf "$SELF.new"; return 0; }
    tr="$HOME/.Trash/$(basename "$SELF")-$(date +%Y%m%d-%H%M%S)"
    mv "$SELF" "$tr" 2>/dev/null || { rm -rf "$SELF.new"; say "  (could not move $SELF to the Trash: kept as it was)"; return 0; }
    mv "$SELF.new" "$SELF" || { mv "$tr" "$SELF"; say "  (could not refresh $SELF: kept as it was)"; return 0; }
    say "  ✓ $SELF now holds this package (the old one is in the Trash)"
}

install_pkg() {   # $1 = unpacked package folder
    local P=$1 ts efi
    [ -f "$P/install.sh" ] && [ -d "$P/EFI/OC/Kexts/PhxBringup.kext" ] || die "$P is not a RadeArc package"
    say "== package"; sed 's/^/  /' "$P/RADEARC_BUILD.txt"
    ask "Install it (EFI kext + install.sh --windowserver)?" || die "nothing changed"
    sudo -v || die "sudo needed"
    xattr -dr com.apple.quarantine "$P" 2>/dev/null
    ts=$(date +%Y%m%d-%H%M%S)

    say "== PhxBringup.kext in OpenCore's EFI"
    efi=$(find_efi) || die "no EFI partition with EFI/OC/Kexts/PhxBringup.kext found: copy it by hand"
    sudo mkdir -p "$G/efi-backup/$ts"
    sudo ditto "$efi/EFI/OC/Kexts/PhxBringup.kext" "$G/efi-backup/$ts/PhxBringup.kext" || die "EFI backup failed: nothing changed"
    sudo rm -rf "$efi/EFI/OC/Kexts/PhxBringup.kext.new"
    sudo ditto "$P/EFI/OC/Kexts/PhxBringup.kext" "$efi/EFI/OC/Kexts/PhxBringup.kext.new" || die "copy to EFI failed: old kext kept"
    sudo rm -rf "$efi/EFI/OC/Kexts/PhxBringup.kext" && sudo mv "$efi/EFI/OC/Kexts/PhxBringup.kext.new" "$efi/EFI/OC/Kexts/PhxBringup.kext" \
        || die "EFI swap failed: restore from $G/efi-backup/$ts"
    say "  ✓ $efi/EFI/OC/Kexts/PhxBringup.kext (backup $G/efi-backup/$ts)"

    say "== install.sh --windowserver"
    (cd "$P" && sudo bash install.sh --windowserver) || die "install.sh failed (see above); EFI kext backup in $G/efi-backup/$ts"

    if [ -e "$G/phxboot-off" ]; then
        if ask "The kill switch $G/phxboot-off is on (GPU driver off). Turn the GPU back on?"; then sudo rm -f "$G/phxboot-off"; fi
    fi
    if [ ! -e "$G/phxboot-off" ]; then
        sudo touch "$G/phxboot-m2" && sudo nvram phxfb-boot=1 && say "  ✓ GPU desktop armed for the next boot"
    fi
    { echo "$P"; cat "$P/RADEARC_BUILD.txt"; } | sudo tee "$STATE" >/dev/null
    [ -n "${PREV:-}" ] && echo "$PREV" | sudo tee "$G/radearc-previous.txt" >/dev/null
    refresh_self "$P"
    say "== done. After the reboot check:  ioreg -rc PhxBringup | grep phx-build"
    ask "Reboot now?" && sudo reboot
}

case "$MODE" in
--status) show_status; exit 0 ;;
--rollback)
    p=$(cat "$G/radearc-previous.txt" 2>/dev/null)
    [ -n "$p" ] && [ -d "$p" ] || die "no previous package recorded"
    PREV=$(installed_dir); install_pkg "$p"; exit 0 ;;
update|--force) ;;
*) sed -n 2,14p "$0"; exit 2 ;;
esac

mkdir -p "$PKGS"
NEW=$(curl -fsSL "$REL/RADEARC_BUILD.txt") || die "could not reach $REL (network, or CI has not published yet)"
K=$(printf '%s\n' "$NEW" > "$PKGS/.newest.txt"; kexts_of "$PKGS/.newest.txt")
CUR=$(installed_dir)
if [ "$MODE" != --force ] && [ -n "$CUR" ] && [ "$(kexts_of "$STATE")" = "$K" ] && \
   cmp -s <(tail -n +2 "$STATE") "$PKGS/.newest.txt"; then
    say "✓ up to date (kexts $K)"; show_status; exit 0
fi
D=$PKGS/$(date +%Y%m%d-%H%M%S)-$K
say "== downloading the newest package (kexts $K)"
mkdir -p "$D" && curl -fL --progress-bar -o "$D/radearc-package.zip" "$REL/radearc-package.zip" || die "download failed"
ditto -x -k "$D/radearc-package.zip" "$D" && rm -f "$D/radearc-package.zip" || die "unzip failed"
P=$D/radearc-package
PREV=$CUR
# keep the newest 3 packages (and the installed one, which becomes --rollback's)
ls -1dt "$PKGS"/*/ 2>/dev/null | tail -n +4 | while read -r old; do [ "${old%/}" = "${PREV%/radearc-package}" ] || rm -rf "$old"; done
install_pkg "$P"
