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

=== BOOT 1 ===
1. Power on, press F12, pick the USB stick (the "UEFI:" entry).
2. In the GRUB menu choose "Try or Install Ubuntu". When the installer window opens, close it / pick "Try Ubuntu".
3. Optional: connect Wi-Fi or Ethernet (only for two extra tools; works without).
4. Open Terminal (Ctrl+Alt+T) and run:
       sudo bash /cdrom/phase0-kit/RUN_ME.sh
   If "/cdrom/phase0-kit" is not found, run:   ls /media/*/*  and use the path where phase0-kit is.
5. It prints what to do for boot 2.

=== BOOT 2 ===
1. Reboot, F12, the stick again.
2. In GRUB highlight "Try or Install Ubuntu", press  e
3. Go to the line that starts with  linux  and add at its end:   modprobe.blacklist=amdgpu
4. Press F10 (or Ctrl+X). Try Ubuntu again, open Terminal, run the same command:
       sudo bash /cdrom/phase0-kit/RUN_ME.sh
   The screen may flicker or go black for a few seconds near the end: that is expected, wait.

=== SEND BACK ===
phase0-8500g.tar.xz from the root of the stick (it is next to phase0-kit),
plus: BIOS version, the BIOS values you used, and which port the monitor was on (HDMI or DisplayPort).
If the script said "the stick is read-only", the file is in the Home folder instead: upload it with Firefox.
