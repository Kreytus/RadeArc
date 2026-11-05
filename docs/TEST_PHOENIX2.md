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

**Test clk, passed** (package 01b37b5, `hardware/dumps/b650m_8500g/testclk.txt`):
- DENTIST VCO 2400 MHz (FbMult 50 × 48 MHz). `DENTIST_DISPCLK_CNTL` 0x00193a00: RDIVIDER DID 0x3a = divider 14.5,
  so **DISPCLK 165.5 MHz** now (GOP, for 148.5 MHz pixels). The WDIVIDER fields read 0: the SMU sets the clocks.
  The first build printed 1200 MHz from WDIVIDER; fixed to read RDIVIDER.
- The SMU answers on the VBIOSSMC mailbox:
  - version 0x004c6700
  - DPREFCLK 600 MHz (Linux assumes the same), DTBCLK 600 MHz
  - FCLK 2000 MHz
- DPM table (TransferTableSmu2Dram into frame-buffer memory):
  - DCFCLK 200/400/436/533/600/685/800/960 MHz
  - DISPCLK = DPPCLK 600/685/800/800/960/960/960/1200 MHz
  - SOCCLK 400…1200 MHz
  - 2 DF P-states: FCLK 2000 / MemClk 2600, and 500 / 1000
- So 144 Hz (335.77 MHz pixels) needs a higher DISPCLK, set through SMU `SetDispclkFreq`. The value comes from DML.

## Test L, step 1: dry run of the 144 Hz switch (read-only, Phase 3b)

From the newest install package (with `testl.sh` in its top folder):

```bash
cd ~/Desktop/radearc-package && bash testl.sh
```

No reboot, nothing is written. `testl` reads every HUBP0 / OTG0 register the 144 Hz switch will write: the fields
Linux programs from DML on DCN 3.1.4 (`hubp3_setup`, `hubp2_setup_interdependent`, `optc1_program_global_sync`).
Per field it prints the live value GOP left, Linux's DML value for 60 Hz and Linux's DML value for 144 Hz. Addresses
and fields come from `src/amd/phx_dcn314_prog_regs.h`, generated from Linux's headers by
`tools/dml/gen_dcn314_regs.py`. The DML values come from `src/amd/phx_dml314_plans.h` (`tools/dml/gen_plans.sh`).
CI regenerates both files and fails if they differ from the committed ones.

Expected in `~/Desktop/testl.txt`:
- `DCHUBBUB_GLOBAL_TIMER_CNTL`: REFDIV and ENABLE, then `DLG refclk ... kHz`. This picks the 24 or 12 MHz plan.
- `PHYPLLA_PIXCLK_RESYNC_CNTL`: `DCCG_DEEP_COLOR_CNTL` 0 at 8 bpc.
- `HUBP0..3_DCHUBP_CNTL`: HUBP0 not blanked (`HUBP_BLANK_EN 0`), the others blanked. HUBP0 feeds OTG0.
- 44 registers, each with its fields:
  - `live | 60 | 144` columns
  - `=` where GOP's value equals Linux's 60 Hz value
- `summary: N of 76 fields equal Linux's 60 Hz plan; 0 plan values spill out of their field`
- `testl exit 0`

GOP is not Linux, so not every field will match. The check is that the timing fields have the same order of
magnitude: `REFCYC_PER_HTOTAL` (DLG refclk cycles per line), `VSTARTUP_START`, `REF_FREQ_TO_PIX_FREQ`. If they are
about 2× off, the refclk is wrong. If they are unrelated, the field mapping is wrong. Both get fixed before step 2,
which writes.

**Test L step 1, passed** (package bb5dccd, `hardware/dumps/b650m_8500g/testl.txt`):
- `DCHUBBUB_GLOBAL_TIMER_CNTL` 0x1001: REFDIV 1, enabled → **DLG refclk 24 MHz**. The 24 MHz plans apply; the
  12 MHz ones are not needed.
- HUBP0 is the live pipe (`HUBP_BLANK_EN 0`); HUBP1..3 are blanked. HUBP0 has no underflow
  (`DCHUBP_CNTL` 0x000f1002: `UNDERFLOW_STATUS 0`).
- `PHYPLLA_PIXCLK_RESYNC_CNTL` 0x103: resync on, `DCCG_DEEP_COLOR_CNTL` 0 (8 bpc), as `dcn31_program_pix_clk` sets.
- 39 of 76 fields equal Linux's 60 Hz plan, and no plan value spills out of its field.
- The field mapping is right. GOP wrote exactly what Linux writes in:
  - `HUBPREQ_DEBUG_DB` = 0x100 (`hubp2_vready_at_or_After_vsync`'s constant)
  - `DLG_V_BLANK_END` 41
  - `VUPDATE_OFFSET` 550
  - `VRATIO_PREFETCH` 0x80000 (1.0)
  - the QoS levels (8, 14)
  - `DCN_DMDATA_VM_CNTL` 0x80000000
- GOP leaves most DLG / TTU fields at 0: `REFCYC_PER_HTOTAL`, `REF_FREQ_TO_PIX_FREQ`, `MIN_DST_Y_NEXT_START`, the TTU
  delivery rates. It also runs HUBP0 with `HUBP_TTU_DISABLE 1`, which Linux clears when it unblanks
  (`hubp2_set_blank`: `HUBP_TTU_DISABLE` 0). So GOP's pipe runs without the TTU deadline logic, and those fields are
  unused.
- Linux's own values check out by hand at 24 MHz:
  - `REFCYC_PER_HTOTAL` 91022 / 256 = 355.6 refclk cycles = 2200 px / 148.5 MHz
  - `REF_FREQ_TO_PIX_FREQ` 84733 / 2^19 = 24 / 148.5
- GOP and Linux differ in:
  - `VSTARTUP` 13 vs 44
  - `VREADY` 300 vs 328, `VUPDATE_WIDTH` 320 vs 247
  - `DST_Y_PREFETCH` 40 vs 28
  - the request sizes: GOP sets only `CHUNK_SIZE`; Linux also sets min chunk 3, meta 1, DPTE/VM group 3,
    PTE row height 4
- DML plans the pipe with a 192 KB DET and unbounded requesting. GOP has `HUBP_UNBOUNDED_REQ_MODE` 0. testl now also
  reads `DCHUBBUB_DET0..3_CTRL`, `DCHUBBUB_COMPBUF_CTRL` and `CURSOR_REQ_MODE`, and prints Linux's plan (DET0
  3 segments, COMPBUF 25 segments, unbounded 1). Step 2 has to program those too, or the DML request sizes don't fit
  the buffer.

**Step 2 (next, writes):** reprogram the *same* 60 Hz mode with Linux's values. No clock, PLL or PHY change:
- save every register first
- program them under `OTG_MASTER_UPDATE_LOCK` (`optc1_lock`), in Linux's order
- check `HUBP_UNDERFLOW_STATUS` and that `OTG_FRAME_COUNT` keeps running
- restore the saved words automatically after 15 s

This proves the write path and Linux's HUBP values on a mode that already works. The clock and timing change for
144 Hz comes after.

## Test L, step 2: Linux's values on the running 60 Hz pipe (writes, restores itself)

From the newest install package. Needs the boot-args `-phxp2 phxdebug=1`, as Test clk did.

Dry run first. Nothing is written; it lists the 46 planned writes next to the live values:
```bash
cd ~/Desktop/radearc-package && bash testl2.sh
```
Then the real run. It writes, watches for 15 s, and puts every saved register back:
```bash
cd ~/Desktop/radearc-package && bash testl2.sh --apply
```

What it does (`src/amd/phx_dcn314_step2.h`), in the order of Linux v6.17's `dcn20_program_front_end_for_ctx` /
`dcn20_program_pipe` for DCN 3.1.4:
1. `optc3_lock`: `OTG_MASTER_UPDATE_LOCK_SEL` 0, `OTG_MASTER_UPDATE_LOCK` 1, then wait for `UPDATE_LOCK_STATUS`
   (10 × 1 µs). If the lock never reports, nothing else is written.
2. `optc1_program_global_sync` (VSTARTUP 44, VUPDATE 550/247, VREADY 328), then wait for VACTIVE.
3. `optc1_set_vtg_params`: `VTG0_FP2` 2, `VTG0_VCOUNT_INIT` 1121.
4. `hubp2_vtg_sel`, `hubp3_setup`, `hubp2_setup_interdependent`: Linux's DML values for 60 Hz.
5. `optc1_unlock`.

It refuses to run unless the DLG refclk is 24 MHz, HUBP0 is live, `VTG0_VCOUNT_INIT` is 1121 and OTG0's frame
counter moves. Ctrl-C restores at once. It doesn't change the clocks, PLL, PHY or timing, the DET / compressed-buffer
split, unbounded requesting, the watermarks or `HUBP_TTU_DISABLE`; those are later steps.

Expected in `~/Desktop/testl2.txt`:
- `apply: ok`
- every 2 s: `frames +N` (about 120 per 2 s) and `underflow status 0`
- `watched: ~900 frames, underflow status max 0`
- `restore: ok, 0 of 45 registers differ`
- `RESULT: PASS`

The picture should look the same throughout. If the screen breaks up or goes black, wait: it restores after 15 s.
If it doesn't come back, restart; GOP sets the pipe up again at boot.

**Test L step 2, passed** (package 08cb06b, build 46, `hardware/dumps/b650m_8500g/testl2.txt`; the GPU desktop was
off through the kill switch, so the panel showed GOP's framebuffer):
- Preconditions held: DLG refclk 24 MHz, HUBP0 live, `VTG0_VCOUNT_INIT` 1121, frames moving.
- `optc3_lock` reported at once, so all 46 writes went in under the lock.
- For 15 s with Linux's 60 Hz values (VSTARTUP 44, `VTG0_FP2` 2, the full DLG/TTU/RQ set from DML):
  - 907 frames (60.5 Hz)
  - `HUBP_UNDERFLOW_STATUS` 0 throughout
  - no visible change
- The restore put all 45 registers back exactly (0 differ).
- So the write path, Linux's lock / global-sync / VTG / HUBP order and DML's per-mode values all work on the 8500G.
  The 144 Hz switch writes the same registers with the 144 Hz plan, plus the clocks, the PLL, the PHY and the timing.

## Test L, step 3: the 144 Hz display clocks at 60 Hz (SMU messages, comes back down)

From the newest install package; same boot-args as step 2. Dry run, then the real one:
```bash
cd ~/Desktop/radearc-package && bash testl3.sh
```
```bash
cd ~/Desktop/radearc-package && bash testl3.sh --apply
```

What it does (`src/amd/phx_dcn314_clk.h`), the way Linux v6.17's `dcn314_update_clocks` raises clocks before a mode
switch:
1. `VBIOSSMC_MSG_SetDispclkFreq` 343 MHz: DML's 342.857 MHz for 144 Hz, rounded up by `khz_to_mhz_ceil`.
2. `VBIOSSMC_MSG_SetDppclkFreq` 338 MHz.
3. `dccg31_update_dpp_dto` for pipe 0: phase 255 / modulo 255 (the pipe at the full DPPCLK), enabled.

It watches for 15 s (frames, underflow, the DISPCLK the DENTIST divider reports), then comes down the way Linux lowers:
1. DISPCLK back to GOP's (from `DENTIST_DISPCLK_RDIVIDER`).
2. The DTO registers as saved.
3. DPPCLK to Linux's 60 Hz value. GOP's DPPCLK can't be read on DCN 3.1.4.

It refuses to run unless the pipe is the 60 Hz one and the SMU mailbox is idle.

Expected in `~/Desktop/testl3.txt`:
- `up: SetDispclkFreq OK (343 MHz), SetDppclkFreq OK (338 MHz)` and `DISPCLK now` ≥ 342857 kHz
  (342857 = 2400 MHz / 7)
- every 2 s: about 120 frames and `underflow status 0`
- `down: … OK …, DTO as saved; DISPCLK now ~165517 kHz`
- `RESULT: PASS`

The picture doesn't change; the panel stays at 60 Hz.

**Test L step 3, passed** (package a526db9, build 46, `hardware/dumps/b650m_8500g/testl3.txt`):
- Up: `SetDispclkFreq 343` and `SetDppclkFreq 338` both answered OK. The DENTIST divider then gave
  **DISPCLK 355.555 MHz** (2400 / 6.75), the first divider step at or above DML's 342.857 MHz.
- For 15 s: 903 frames (60.2 Hz), underflow 0, DISPCLK steady.
- Down: both messages OK, the DTO registers back as saved. DISPCLK 168.421 MHz (2400 / 14.25), one divider step
  above GOP's 165.517 for the 166 MHz request.
- The argument register after each message read 400 / 400 going up and 200 / 150 coming down, not the DENTIST
  clock. `dcn314_smu_set_dispclk` returns that value as "actual_dispclk_set_mhz", so it is the SMU's own figure (which
  DPM level it picked, perhaps), and the DENTIST divider is the clock to trust.
- So the SMU path Linux uses for DCN 3.1.4 clocks works on the 8500G, and the pipe takes the 144 Hz clocks without
  underflow.
