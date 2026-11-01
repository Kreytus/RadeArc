# First hardware test on the Ryzen 5 8500G (Phoenix 2)

What this tests: PhxBringup recognises the chip, then the GPU's firmware loading and power-up, then the MES scheduler and
one compute and one graphics queue. It does **not** touch the display: the screen stays on the normal firmware
framebuffer the whole time. Do the tests in order and send me the output of each before the next.

Safety nets: `bringup5.sh` and `full.sh` arm a reboot in 3-4 minutes and cancel it only when they finish, so a hung GPU
reboots by itself. Nothing runs at boot (you don't install phxboot), so a normal reboot without `-phxp2` is always clean.
If macOS ever fails to boot, add `-phxoff` to boot-args from the OpenCore picker.

**Do not** run nullarc's `install.sh`, and don't add PhxAccel.kext yet.

## 0. Prepare (once)

1. macOS Tahoe running on the 8500G with your OpenCore EFI, as before.
2. In the GitHub repo: **Actions → Build kexts → the latest run → Artifacts → kexts**: download and unzip it. It holds
   `PhxBringup.kext`, `PhxAccel.kext` and `phxctl/` (`phxctl`, `mesboot`, `phxfw`).
3. Copy `PhxBringup.kext` to `EFI/OC/Kexts/` and add it to `config.plist` → Kernel → Add:

   | Key | Value |
   |---|---|
   | Arch | `x86_64` |
   | BundlePath | `PhxBringup.kext` |
   | ExecutablePath | `Contents/MacOS/PhxBringup` |
   | PlistPath | `Contents/Info.plist` |
   | Enabled | `true` |

4. In Terminal, set up the tools and firmware (the repo's `tools/fetch_test_firmware.sh` downloads exactly the
   linux-firmware files needed, for both chips):

   ```bash
   mkdir -p ~/gpu/phxctl ~/gpu/fw
   ```

   ```bash
   cp <unzipped artifact>/phxctl/* ~/gpu/phxctl/ && chmod +x ~/gpu/phxctl/* && xattr -dr com.apple.quarantine ~/gpu/phxctl
   ```

   Get the repository (Code → Download ZIP on GitHub if `git` asks for a login), then from its folder:

   ```bash
   cp tools/phxctl/bringup5.sh tools/phxctl/full.sh ~/gpu/ && bash tools/fetch_test_firmware.sh ~/gpu/fw
   ```

## Test A: recognition only (nothing written to the GPU)

boot-args: add `phxdebug=1` (keep your others). Reboot, then:

```bash
log show --last boot --predicate 'eventMessage CONTAINS "PhxBringup:"'
```

Expected: `PhxBringup: service up (mmio mapped, bar0 mapped 256 MiB, debug 1, chip Phoenix 2, read-only (boot-arg -phxp2 enables Phoenix 2 bring-up))`

```bash
ioreg -rc PhxBringup | grep -E '"phx-(build|chip|gc-version|fb-size|fb-phys)"'
```

Expected: `phx-build` 35, `phx-chip` "Phoenix 2", `phx-gc-version` 720900, `phx-fb-size` 2147483648, `phx-fb-phys` 16642998272 (0x3E0000000).

```bash
~/gpu/phxctl/phxfw ~/gpu/fw
```

Expected: `CHIP phoenix2 gc_11_0_4 psp_13_0_11 sdma_6_0_1`, a `TOC` line and 17 `L` lines.

```bash
sudo ~/gpu/phxctl/phxctl psp status
```

Expected: `kr 0xe00002c7` (refused: still read-only, which is correct).

Send me all four outputs.

## Test B: firmware and GPU power-up

boot-args: `phxdebug=1 -phxp2`. Reboot, then:

```bash
bash ~/gpu/bringup5.sh 2>&1 | tee ~/gpu/test-b.log
```

It asks for your password (sudo). Expected, in order:
- `== chip phoenix2: gc_11_0_4 psp_13_0_11 sdma_6_0_1`
- `toc (size 0x900) -> kr 0x00000000  status 0x00000000 ... tmr_size 0x...`
- `tmrtoc (size 0x...) -> kr 0x00000000  status 0x00000000`
- no line containing `stopping`
- three SMU replies, and a last `regs` line where the first value (0xee7e, RLC bootload status) has **bit 31 set**
  (`0x0ee7e: 0x8...`): the RLC autoload finished, as Linux checks it (gfx_v11_0_wait_for_rlc_autoload_complete,
  RLC_RLCS_BOOTLOAD_STATUS.BOOTLOAD_COMPLETE).

Send me `~/gpu/test-b.log` and `~/gpu/fwaddr.log`. If the PC freezes, it reboots within 3 minutes: then send the
`log show --last boot` lines above from the boot before (`--predicate 'eventMessage CONTAINS "Phx"'`).

## Test C: MES scheduler and queues (only after B worked)

Reboot first (keep `phxdebug=1 -phxp2`): `full.sh` runs test B again itself. Then:

```bash
MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash ~/gpu/full.sh kiqsched 2>&1 | tee ~/gpu/test-c.log
```

Expected: a `mesboot: chip Phoenix 2, VRAM MC 0x8000000000 size 2048 MiB, FB phys 0x3e0000000 ...` line, then
`compute: scratch0=0xdeadbeef (want deadbeef) testbuf=0xfeedf00d (want feedf00d) ...` and
`gfx: scratch1=0xdeadbeef (want deadbeef) testbuf=0x6f7a6f7a (want 6f7a6f7a) ...`: the values must equal the "want" ones.

Send me `~/gpu/test-c.log`. After that, remove `-phxp2` again until we go on.

## Test D: the Vulkan driver (RADV) on the real GPU (Phase 2, only after C worked)

1. On any PC: GitHub → **Actions → Build Mesa RADV → the latest green run → Artifacts → mesa-radv**. Unzip it. It holds
   `libvulkan_radeon.dylib`, `libvulkan.1.dylib` (the Vulkan loader, so no Homebrew is needed), `vkprobe`,
   `radeon_icd.json`, `testd.sh` and `BUILD.txt`.
2. On the 8500G: copy that folder's files to `~/gpu/mesa/`. boot-args: `phxdebug=1 -phxp2`. Reboot, then:

```bash
bash ~/gpu/mesa/testd.sh
```

It asks for your password once and writes `~/Desktop/testd.txt`. Expected, after the Test C lines:
- `radv-darwin: native kext: ABI ..., canned phoenix2, GB_ADDR_CONFIG 0x00000142, ...`: RADV picked the Phoenix 2 table
  from the GC version the kext read (11.0.4), not from the PCI ID
- `[0] AMD Radeon 740M Graphics (RADV PHOENIX2)  vendor 0x1002 device 0x15c8 ...`
- `device created`, `compute pipeline compiled` (ACO compiled for gfx1103), `dispatch submitted and fence signalled`
- `results: 256/256 correct`
- `watchdog cancelled rc=0`

Send me `testd.txt`. After that, remove `-phxp2` again.

## Test E: read the monitor's EDID over DDC1 (Phase 3a, PhxBringup build 36)

What it does: PhxBringup drives the display engine's hardware I2C engine exactly as Linux does (dce_i2c_hw.c,
`src/amd/phx_dcn_i2c.h`) and reads your monitor's EDID from DDC1. It does not start the GPU, touch the display pipe or
change the picture. It refuses before sending anything if GOP did not leave DDC1 in hardware I2C mode, or if the display
firmware holds the engine.

1. GitHub → **Actions → Build kexts → the latest green run → Artifacts → kexts**. Unzip it.
2. Replace `EFI/OC/Kexts/PhxBringup.kext` with the new one (build 36), and copy the new `phxctl` folder's files over
   `~/gpu/phxctl/` (then `xattr -dr com.apple.quarantine ~/gpu/phxctl`).
3. boot-args: `phxdebug=1 -phxp2`. Reboot, then:

```bash
sudo ~/gpu/phxctl/phxctl edid ~/Desktop/edid.bin > ~/Desktop/teste.txt
```

Expected in `teste.txt`: `edid -> kr 0x00000000  result 0 (ok)  256 bytes`, a hex dump starting
`000: 00 ff ff ff ff ff ff 00`, then your monitor's manufacturer code and name. Send me `teste.txt` and `edid.bin`.
After that, remove `-phxp2` again.

## Test F: install, GPU up at every boot, Metal for apps (the screen is not taken over yet)

Package: GitHub → **Actions → Build install package → latest green run → Artifacts → radearc-package**. It holds
upstream nullarc 0.3.0-alpha (NullMoth's Metal plugin) with this repository's PhxBringup/PhxAccel, phxctl/mesboot/phxfw,
Mesa RADV (Phoenix 2 table), boot scripts, install.sh and the gc_11_0_4/psp_13_0_11 firmware.

EFI (config.plist): SIP `csr-active-config` 7F080000; boot-args `-v keepsyms=1 debug=0x100 npci=0x3000 phxdebug=1
-phxp2 amfi=0x80 amfi_get_out_of_my_way=0x1 -amfipassbeta`; AMFIPass.kext in Kernel → Add; PhxBringup.kext from the
package. Then `sudo ./install.sh --check`, `sudo ./install.sh` (without --windowserver: the screen stays on the firmware
framebuffer), reboot. Expected: `/var/log/phxboot.log` ends with `done` after the compute/gfx lines, and System
Information → Graphics shows the 740M with Metal support. Turn off: `sudo touch /Users/Shared/gpu/phxboot-off` and
reboot, or boot-arg `-phxoff`.

## Test J: the driver's DMCUB firmware (Phase 3b; no new kext, no EFI change)

From the newest install package (`radearc-package`, with `testj.sh` in its top folder). Unpack it on the Desktop.

```bash
cd ~/Desktop/radearc-package && bash testj.sh dry
```

Read-only. `~/Desktop/testj-dry.txt` shows the DMCUB state (`dal_fw 0`, GOP's firmware) and every register and VRAM
write the load would make (windows at VRAM 208 MiB). Send it before the real run.

```bash
cd ~/Desktop/radearc-package && bash testj.sh run
```

The PSP loads `dcn_3_1_4_dmcub.bin` (type 51), then `dmcubload` resets DMCUB, copies the VBIOS, sets up the windows
and mailbox, and starts it. Expected in `~/Desktop/testj-run.txt`:
- `loadfw ... fwtype 51 ... status 0x00000000`
- `auto-load (dmub_srv_wait_for_auto_load): OK, dal_fw 1 mailbox_rdy 1`
- `NULL command: OK, consumed`
- the picture unchanged throughout
- `watchdog cancelled rc=0`

If the PSP refuses (status ≠ 0), dmcubload still runs. Without the driver code DMCUB then stays in reset; the picture
does not depend on it. A reboot gives GOP's DMCUB back.

## Results on the B650M D3HP AX + Ryzen 5 8500G (build 35)

- **Test A, passed:** chip Phoenix 2, gc 11.0.4 (720900), UMA 2 GiB at 0x3E0000000; the PSP stays refused while read-only.
- **Test B, passed:** LOAD_TOC returned tmr_size 0x4a00000 (the same size Linux uses) and SETUP_TMR status 0. All 17
  LOAD_IP_FW loads returned status 0, as did AUTOLOAD_RLC. The SMU accepted 0x3, 0x2 (PMFW 0x004c6700) and
  EnableGfxImu. RLC_RLCS_BOOTLOAD_STATUS = 0xc000001f (complete), and GB_ADDR_CONFIG reads 0x142 live.
- **Test C, passed:** the MES SET_HW_RESOURCES, QUERY_SCHED_STATUS and both ADD_QUEUE calls returned OK. The compute
  queue gave scratch0=0xdeadbeef and testbuf=0xfeedf00d, the gfx queue gave scratch1=0xdeadbeef and testbuf=0x6f7a6f7a,
  and both queues were adopted (kr 0).
- **Test D, passed** (mesa-radv from RadeArc 3a1ba84): RADV chose `canned phoenix2` (GB_ADDR_CONFIG 0x142) on its own,
  from the kext's GC 11.0.4. The device was `AMD Radeon 740M Graphics (RADV PHOENIX2) 1002:15c8`. ACO compiled the
  compute pipeline, the dispatch ran on the GPU with its fence signalled, results were **256/256 correct**, and the
  watchdog was cancelled.
- **Test E, passed** (PhxBringup build 36): EDID read over DDC1 with the hardware I2C engine. `result 0 (ok)`,
  256 bytes, both block checksums good: **MSI G255F**, week 14 of 2024, EDID 1.3 with one CEA extension. Saved as
  `hardware/dumps/b650m_8500g/edid_msi_g255f.bin`. Detailed timings: 1920×1080 at 60 Hz (148.5 MHz, totals
  2200×1125: the mode GOP set), 144 Hz (335.77 MHz), 165 Hz (384.73 MHz) and **180 Hz (415.58 MHz, totals
  2080×1110)**. The range limits allow 48–180 Hz. Phase 3b note: 415.58 MHz is above 340 MHz, so 180 Hz over HDMI
  needs HDMI 2.0 TMDS scrambling (SCDC). The monitor's HF-VSDB advertises it (max TMDS 600 MHz, SCDC present).
- **Test F, passed** (radearc-package at f38b483: upstream 0.3.0-alpha, PhxBringup build 36). install.sh passed every
  check, and PhxAccel went into the Aux KC (`kmutil rc=0`). At boot, phxboot brought the GPU up and both queues
  answered, then `claim -> kr 0x0` and `done` 10 s after start. 60 s later the headless check restored MetalPluginName.
  System Information shows **AMD Radeon 740M, Metal Support: Metal 3**. RADV opened `canned phoenix2` through the
  plugin. The desktop stays on IONDRVFramebuffer (not armed). Logs: `hardware/dumps/b650m_8500g/testf_*.txt|log`.
- **Test G, passed** (package 3890276: PhxBringup build 37 with the PCI power hold, PhxAccel 91efb9a presenting the
  pipe built-in without EDID, Mesa signed ad-hoc): WindowServer composites the desktop on the Radeon 740M. Earlier tries
  failed in this order:
  1. CoreDisplay assert (display ID 0): NVK never came up in WindowServer, because the unsigned libvulkan_radeon.dylib
     was refused ("mapped file has no cdhash").
  2. A kernel panic (CPU stuck in IOPCIBridge::saveDeviceState's config read) during an M2 boot's bring-up: the GPU's
     PCI device had no power child.
- **Test H, passed** (boot-args `-phxfbext -phxfbedid` on package 3890276): the pipe was reported as external with its
  EDID. System Information names the display **MSI G255F** (1920×1080 at 60 Hz), and WindowServer composites on the GPU
  (`frames on the panel 1163`). This reporting is now the default for DVI/HDMI pipes; `-phxfbint` turns it off.
- **Test J, passed** (package 9c7e325, no kext change). The driver's DMCUB firmware runs:
  - The PSP accepted LOAD_IP_FW type 51 (`dcn_3_1_4_dmcub.bin`, 350368 bytes) with status 0, after the GFX bring-up.
    The PSP load itself stopped GOP's DMCUB firmware (afterwards `DMCUB_CNTL2` soft reset 1, `SCRATCH7` 0xDEADDEAD).
  - dmcubload then set up the windows at VRAM 208 MiB and started DMCUB. `SCRATCH0` became 0x3 (`dal_fw` 1,
    `mailbox_rdy` 1), and a `DMUB_CMD__NULL` through inbox 1 was consumed (RPTR 64).
  - The picture was not affected.

## Test K: the mode check (read-only, Phase 3b)

From the newest install package (with `testk.sh` in its top folder), unpacked on the Desktop:

```bash
cd ~/Desktop/radearc-package && bash testk.sh
```

Nothing is written to the GPU, and no reboot is needed. Needs `-phxp2 phxdebug=1` for the EDID read. Expected in
`~/Desktop/testk.txt`:
- `== EDID ...: 256 bytes, 4 detailed timings` (60, 144, 165, 180 Hz)
- `== pipe: OTG0 -> DIG FE 0 -> DIG BE 0, signal DVI` (GOP drives HDMI as DVI: DIG_MODE 2)
- eight `ok` lines, then `timing: MATCH mode 0 (60 Hz)`
- `refresh: measured 60.0xx Hz`
- the DIG / DCCG / HUBBUB register dump (the baseline for Tests L/M), then the planned 144/165/180 Hz OTG values
- `modecheck exit 0`

**Test K, passed** (package 9bdad1f, `hardware/dumps/b650m_8500g/testk.txt`):
- EDID: 4 detailed timings (60, 144.003, 165.001, 179.998 Hz). Pipe OTG0 → DIG FE 0 → BE 0, DVI.
- All eight OTG0 timing fields equal optc1_program_timing for the 60 Hz mode (`timing: MATCH mode 0`); refresh
  measured 59.7 Hz (60 frames in 1.005 s: one-frame quantisation of a 1 s window).
- Baseline decoded with dcn_3_1_4_sh_mask.h:
  - `DIG0_DIG_BE_CNTL` 0x00020100: DIG_MODE 2 (DVI), FE_SOURCE_SELECT FE0, DIG_HPD_SELECT 0 (= HPD1, as the VBIOS says)
  - `DIG0_DIG_FE_CNTL` 0x01000000: SOURCE OTG0, DIG_SYMCLK_FE_ON 1, TMDS_PIXEL_ENCODING 0 (RGB 4:4:4)
  - `DIG0_HDMI_CONTROL` 0x00010011: KEEPOUT_MODE 1, PACKET_GEN_VERSION 1, UNSCRAMBLED_CONTROL_LINE_NUM 1,
    **DATA_SCRAMBLE_EN 0, CLOCK_CHANNEL_RATE 0** (to be 1/1 above 340 MHz: 165/180 Hz)
  - `DIG0_DIG_BE_EN_CNTL` 0x101: DIG_ENABLE, SYMCLK_BE_ON; `DIG0_TMDS_CNTL` 1: TMDS_SYNC_PHASE
  - DCCG: `OTG0_PIXEL_RATE_CNTL` and the PHY/HDMI clock controls are 0: no DTO; the TMDS pixel clock comes from the
    PHY PLL, which the DMUB SET_PIXEL_CLOCK command programs
  - HUBBUB watermarks A: urgency 0x9c, SR enter 0x19b, SR exit 0x153, DRAM clock change 0x69c

## Test clk: display clocks and the SMU clock table (Phase 3b, DML inputs)

From the newest install package (with `testclk.sh` in its top folder), unpacked on the Desktop:

```bash
cd ~/Desktop/radearc-package && bash testclk.sh
```

No reboot. Needs `-phxp2 phxdebug=1`. The picture is not touched: the SMU answers four queries and copies its DPM
clock table into a free VRAM buffer at 210 MiB (Linux's dcn314 clock manager does the same with frame-buffer memory).
Expected in `~/Desktop/testclk.txt`:
- `DENTIST VCO ... kHz`, plus DISPCLK and DPPCLK in kHz (the clocks GOP left)
- `GetSmuVersion 0x...`, `GetDprefclkFreq` (Linux assumes 600 MHz), `GetFclkFrequency`
- the DPM table: `levels: dcf N disp/dpp N ...`, then DcfClocks / DispClocks / DppClocks / SocClocks in MHz and the
  DfPstate rows (FClk / MemClk for the DDR5)
- `clkinfo exit 0`
