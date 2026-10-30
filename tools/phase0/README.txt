PHASE 0 KIT - Ryzen 5 8500G / Radeon 740M
Reads facts from the GPU from an Ubuntu live USB. Nothing is installed, the PC's disk is not touched,
and the only thing that "writes" to the GPU is a normal amdgpu driver load at the very end.

=== ON YOUR OTHER PC ===
1. Download Ubuntu 25.10 Desktop (amd64 .iso) from ubuntu.com/download/desktop.
2. Download Rufus from rufus.ie. Plug in a USB stick (8 GB or more; it will be ERASED).
3. In Rufus: Device = the stick, Boot selection = the Ubuntu .iso, Partition scheme = GPT,
   Target system = UEFI, File system = FAT32. Press START.
   If Rufus asks "ISO Image mode or DD Image mode": choose ISO Image mode (so the stick stays writable).
4. When Rufus is done, copy the whole folder  phase0-kit  to the root of the stick
   (so the stick has  X:\phase0-kit\RUN_ME.sh ).

=== BIOS ON THE 8500G (Del at power-on) ===
  Initial Display Output = IGD Video      Integrated Graphics = Forces
  UMA Frame Buffer Size  = 2G             Above 4G Decoding   = Enabled
  Re-Size BAR Support    = Disabled       CSM Support         = Disabled
  Secure Boot            = Disabled
Write down your BIOS version. Plug the monitor into the board port you want to use with macOS.

=== ONE BOOT ===
1. Power on, press F12, pick the USB stick (the "UEFI:" entry).
2. In the GRUB menu (tap Esc if it does not show) highlight "Try or Install Ubuntu" and press  e
3. On the line that starts with  linux  press End, type a space and:   modprobe.blacklist=amdgpu
   Do not press Enter. Press F10 (or Ctrl+X).
4. Choose "Try Ubuntu". Optional: connect Wi-Fi/Ethernet (only for two extra tools; works without).
5. Open Terminal (Ctrl+Alt+T) and run:
       sudo bash /cdrom/phase0-kit/RUN_ME.sh
   If "/cdrom/phase0-kit" is not found, run:   ls /media/*/*  and use the path where phase0-kit is.
   In step 2/3 the screen may flicker or go black for a few seconds: that is expected, wait.

=== SEND BACK ===
phase0-8500g.tar.xz: the script says where it saved it (the stick, another USB drive, or the Home folder).
If it is only in the Home folder, copy it to another USB stick or upload it with Firefox BEFORE shutting down.
Plus: BIOS version, the BIOS values you used, and which port the monitor was on (HDMI or DisplayPort).
