# Phase 3b plan: real mode setting, 1920×1080 at 180 Hz (Ryzen 5 8500G, MSI G255F over HDMI)

Status: **plan only.** Nothing in this document is implemented yet. No code touches the display engine beyond what
Phase 3a reads (the GOP pipe, the EDID over DDC1).

Rules carried over from the project brief:
- Every register, field, firmware command and sequence step comes from Linux `drivers/gpu/drm/amd` for the exact IP
  versions here (DCN 3.1.4, DMUB for DCN 3.1.4, SMU 13.0.11 / VBIOSSMC). Each one gets a comment naming the file and
  function.
- Anything that can only be learned from the hardware is listed under "Facts to dump first", with the exact command.
- The GOP mode stays the fallback. A failed mode set must leave a usable screen or restore one by itself.

## 1. Scope

The resolution stays the same: the scan-out surface (1920×1080 ARGB8888 at VRAM offset 0, pitch as GOP left it), HUBP
and the framebuffer PhxAMDFB publishes do not change. Only the timing, the pixel clock and the link change:

| Mode (EDID) | Pixel clock | Totals | Horizontal (fp / sync / bp) | Vertical (fp / sync / bp) | Sync |
|---|---|---|---|---|---|
| 60 Hz (GOP, DTD 1 of block 0) | 148.50 MHz | 2200×1125 | 88 / 44 / 148 | 4 / 5 / 36 | H+ V+ |
| 144 Hz (CEA block DTD) | 335.77 MHz | 2080×1121 | 48 / 32 / 80 | 4 / 5 / 32 | H+ V+ |
| 165 Hz (CEA block DTD) | 384.73 MHz | 2080×1121 | 48 / 32 / 80 | 3 / 5 / 33 | H+ V+ |
| **180 Hz** (CEA block DTD) | **415.58 MHz** | **2080×1110** | 48 / 32 / 80 | 3 / 4 / 23 | H+ V+ |

Source: `hardware/dumps/b650m_8500g/edid_msi_g255f.bin`, read under macOS by Test E. Range limits: 48–180 Hz. The
HF-VSDB advertises max TMDS 600 MHz and SCDC present.

## 2. What changes on the wire, and why it is more than new timing numbers

1. **The signal type.** GOP drives the monitor as DVI (DIG0 `DIG_MODE` 2, `dcn10_get_dig_mode`). DVI single link stops
   at 165 MHz, so every target mode needs HDMI mode (`DIG_MODE` 3).
2. **HDMI 2.0 scrambling at 165 Hz and 180 Hz.** Both are above 340 MHz. Linux handles this in
   `enc314_stream_encoder_hdmi_set_stream_attribute` (`dc/dio/dcn314/dcn314_dio_stream_encoder.c`): at or above
   `HDMI_CLOCK_CHANNEL_RATE_MORE_340M` (340000 kHz) it sets `HDMI_CONTROL.HDMI_DATA_SCRAMBLE_EN = 1` and
   `HDMI_CLOCK_CHANNEL_RATE = 1` (clock channel at 1/4 of the character rate). The sink is told over SCDC by
   `write_scdc_data` (`dc/link/protocols/link_ddc.c`): it reads the sink version at `HDMI_SCDC_SINK_VERSION`, writes
   source version 1 when the sink reports 1, then writes `HDMI_SCDC_TMDS_CONFIG` = **3** (scrambling + 1/40 clock
   ratio) when the pixel clock is above 340 MHz. Those are I2C transfers to address 0x54 on DDC1, with the same engine
   `src/amd/phx_dcn_i2c.h` already drives (Test E).
3. **The PHY and its PLL.** On DCN 3.1.4 Linux does not program the PHY directly. `dcn314_resource.c` sets
   `.dmub_command_table = true`, so `transmitter_control_dmcub` / `transmitter_control_dmcub_v1_7` and the encoder
   control in `dc/bios/command_table2.c` send `DMUB_CMD__VBIOS_DIG1_TRANSMITTER_CONTROL` and
   `DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL` to the **DMCUB** microcontroller, which runs the VBIOS command tables.
   **Mode setting therefore needs a running DMCUB** (see 4a).
4. **Display clocks.** 415.58 MHz needs a higher DISPCLK/DPPCLK than GOP's 148.5 MHz mode. Linux requests them from the
   SMU in `dcn314_smu.c`: `VBIOSSMC_MSG_SetDispclkFreq` (0x4) and `VBIOSSMC_MSG_SetDppclkFreq` (0x6), called from
   `dcn314_clk_mgr.c` (`dcn314_update_clocks`). The values come from DC's bandwidth calculation for the mode, not from a
   table: they are computed the way `dcn314_update_clocks` / DML compute them and checked against the 60 Hz values
   live on the 8500G (4b).
5. **Watermarks and DCHUB arbitration.** A higher refresh rate shortens the time HUBP has to fetch each line. Linux
   programs the watermarks from DML (`dcn314_hwseq.c` and the `hubbub31` functions). Wrong values risk underflow: a
   flickering or black screen, not damage. They are computed as DML computes them and checked against the live 60 Hz
   values (4b).
6. **The OTG timing.** `optc31` / `optc1_program_timing` (`dc/optc/dcn31/dcn31_optc.c` and the DCN10 base) programs
   H/V total, blank, sync and polarity. The register offsets already used read-only by `phx_dcn_pipe.h`
   (`regOTG0_OTG_H_TOTAL` …) come from `dcn_3_1_4_offset.h`.

## 3. Linux's order for a mode change on an already lit HDMI link

From `dc/link/link_dpms.c` (`link_set_dpms_off` / `link_set_dpms_on`) and `dcn314_hwseq.c`. The exact sequence is
checked against the live 60 Hz state (4b) before any write:

1. Blank and stop the stream: OTG blank, stream encoder off (`enc314_stream_encoder_*`), transmitter disable through
   DMUB.
2. Raise the clocks: SMU `SetDispclkFreq` / `SetDppclkFreq`, and the DCCG pixel-rate setup for OTG0
   (`dcn314_dccg.c`).
3. Program the PHY for the new TMDS rate: DMUB VBIOS transmitter control with the new pixel clock (and the HDMI
   signal type).
4. Program the OTG timing (`optc31`), watermarks (`hubbub31`) and ODM (one segment).
5. Stream encoder in HDMI mode: `enc314_stream_encoder_hdmi_set_stream_attribute`. At 165/180 Hz this includes
   scrambling, the AVI infoframe and the general control packet.
6. SCDC write to the monitor (`write_scdc_data`), at 165/180 Hz.
7. Transmitter enable through DMUB, stream enable, OTG unblank.

## 4. Facts to dump first (hardware only; nothing is assumed)

### 4a. Is DMCUB running under macOS after GOP? (read-only, no reboot, PhxBringup build ≥ 36)

`dmub_dcn31_is_hw_init` (`dmub/src/dmub_dcn31.c`) calls DMCUB ready when `DMCUB_CNTL.DMCUB_ENABLE` (mask 0x10000) is
set and the `dal_fw` bit of `DMCUB_SCRATCH0` is set. Absolute dwords are DMU segment 2 (0x34c0) plus
`dcn_3_1_4_offset.h`: `regDMCUB_CNTL` 0x1f6 → 0x36b6, `regDMCUB_SCRATCH0` 0x1e3 → 0x36a3, `regDMCUB_SCRATCH7` 0x1ea →
0x36aa, `regDMCUB_INBOX1_WPTR` 0x1d6 → 0x3696, `regDMCUB_INBOX1_RPTR` 0x1d7 → 0x3697.

```bash
sudo ~/gpu/phxctl/phxctl regs 0x36b6 0x36a3 0x36aa 0x3696 0x3697 > ~/Desktop/dmcub.txt
```

- **Running:** the macOS side can send the same DMUB commands Linux does (inbox 1, the `dmub_rb_cmd` layouts in
  `dmub/inc/dmub_cmd.h`).
- **Not running:** DMCUB has to be loaded first (`dcn_3_1_4_dmcub.bin`, Linux `dm_dmub_hw_init` / `dmub_srv_hw_init`).
  That becomes its own step with its own test, before any mode setting.

**Result (Test I, 2026-11-01, `hardware/dumps/b650m_8500g/dmcub_after_gop.txt`):** `DMCUB_CNTL` = 0x001900c6
(`DMCUB_ENABLE` set), `DMCUB_SCRATCH0` = 0x2 (`mailbox_rdy` 1, **`dal_fw` 0**: `union dmub_fw_boot_status`,
`dmub_cmd.h`), inbox 1 read and write pointers both 0x940 (idle). DMCUB runs the firmware GOP/VBIOS started, not the
driver's. `dmub_dcn31_is_hw_init` therefore returns false. Linux then resets DMCUB and loads `dcn_3_1_4_dmcub.bin`
itself (`dm_dmub_hw_init` → `dmub_srv_hw_init`) before it sends display commands, so the macOS side does the same.
**New first step of Phase 3b: load and start the driver's DMCUB firmware as Linux does**, with its own test. The test
checks that `dal_fw` = 1, that the GOP picture stays up, and that a DMUB no-op command completes. Only then comes any
mode setting. (No Linux trace exists: the load follows `dm_dmub_hw_init` / `dmub_srv_hw_init` from the source.)

### 4b. No Linux trace (decision 2026-11-01): verification from macOS instead

The user will not boot Linux again. The Phase 0 register trace (`trace-dc1`) came out empty because of the early kit's
tracepoint bug, so no recording of Linux programming the display exists. The sequence is therefore implemented from
the Linux source alone, and checked on the 8500G without writing:
- **Encoding check (read-only):** the code computes the full register set (OTG timing, DIG/HDMI, DCCG pixel rate,
  HUBP/watermark values it would program) for the **current** 1080p60 mode and compares it with the live registers
  under macOS (GOP's state) and with Linux's own 60 Hz state in Phase 0's `dm_dtn_log`
  (`hardware/dumps/b650m_8500g/linux_dm_dtn_log.txt`). Any field that differs is explained from the source before
  anything is written.
- **DMUB command check:** the `dmub_rb_cmd` structures (transmitter control, encoder control) are built from the
  Linux structures in `dmub_cmd.h` and the VBIOS data in Phase 0's `vbios.rom` (`hardware/dumps/b650m_8500g/`), and
  host-tested.
- **The first real switch is 144 Hz** (below 340 MHz: no scrambling, no SCDC), with the automatic revert.

### 4c. Confirm the monitor takes 180 Hz

The EDID lists 180 Hz with the range limits allowing it; the monitor's OSD confirms the rate during Test M.

## 4d. DMCUB load: Linux's exact sequence (implementation recipe for Test J)

From `amdgpu_dm/amdgpu_dm.c` `dm_dmub_hw_init` and `dmub/src/dmub_srv.c` `dmub_srv_hw_init`, with the DCN 3.1 hooks in
`dmub/src/dmub_dcn31.c` (DCN 3.1.4 uses them):

1. **Stop and reset the running (GOP) firmware:** `dmub_dcn31_reset`. If `DMCUB_CNTL2.DMCUB_SOFT_RESET` is 0, send GPINT
   `DMUB_GPINT__STOP_FW`, then wait (each wait at most 100000 × 1 µs) for the GPINT ack, for `DMCUB_SCRATCH7 ==
   DMUB_GPINT__STOP_FW_RESPONSE` and for `DMCUB_CNTL.DMCUB_PWAIT_MODE_STATUS` bit 0. Then, if `DMCUB_ENABLE`: set
   `DMCUB_CNTL2.DMCUB_SOFT_RESET`, `MMHUBBUB_SOFT_RESET.DMUIF_SOFT_RESET`, clear `DMCUB_ENABLE`, and zero the inbox 1,
   outbox 1 and outbox 0 pointers. The OTG keeps scanning out during this (on HDMI, DMCUB does not drive the picture),
   as it does when Linux loads.
2. **Memory windows in the UMA carve-out:** the sizes from `dmub_srv.c`, each window 1 MiB aligned
   (`DMUB_FB_ALIGNMENT`):
   - inst_const (cw0, from the firmware header's `inst_const_bytes` minus PSP header/footer)
   - stack 128 KiB (`DMUB_STACK_SIZE`), plus context 512 KiB (`DMUB_CONTEXT_SIZE`)
   - bss/data (`bss_data_bytes`)
   - VBIOS (cw3, `hardware/dumps/b650m_8500g/vbios.rom` size)
   - mailbox 2 × `DMUB_RB_SIZE` (cw4)
   - trace buffer 64 KiB (cw5, `DMUB_TRACE_BUFFER_SIZE`)
   - fw state 64 KiB (cw6), scratch 1 KiB, IB 1280 B

   Window bases: CW0 0x60000000, CW1 0x61000000, CW3 0x63000000, CW4 0x64000000, CW5 0x65000000, CW6 0x66000000,
   region 5 0xA0000000.
3. **Code:** Linux's APUs use `AMDGPU_FW_LOAD_PSP`. The PSP loads `inst_const` of `dcn_3_1_4_dmcub.bin` (the
   `dmcub_firmware_header_v1_0` gives the offsets), through the same LOAD_IP_FW path PhxBringup already drives for the 17
   GFX payloads (Test B). The DMUB firmware type comes from `psp_gfx_if.h` when this step is written. The bss/data part
   and the VBIOS are copied into their windows by the CPU. Mailbox, trace buffer and fw state are zeroed.
4. **Windows, mailbox, boot options:** `dmub_srv_hw_init` → `dmub_dcn31_setup_windows`
   (`DMCUB_REGION3_CW3..CW6_{OFFSET,OFFSET_HIGH,BASE_ADDRESS,TOP_ADDRESS}`, `DMCUB_REGION5_*`) and
   `dmub_dcn31_setup_mailbox` (`DMCUB_INBOX1_BASE_ADDRESS`, `DMCUB_INBOX1_SIZE`). Then
   `dmub_dcn31_enable_dmub_boot_options` (`DMCUB_SCRATCH14`; DCN 3.1.4: `dpia_supported` = 1 per `dm_dmub_hw_init`).
   cw0/cw1 are programmed by `dmub_dcn31_backdoor_load` only in the non-PSP path. With the PSP path the PSP sets them;
   checked from the source when written.
5. **Start:** `dmub_dcn31_reset_release`: clear `DMUIF_SOFT_RESET`, `DMCUB_SCRATCH15` = `psp_version & 0x001100FF`, set
   `DMCUB_ENABLE` and `DMCUB_TRACEPORT_EN`, clear `DMCUB_SOFT_RESET`.
6. **Wait** for the auto-load (`dmub_srv_wait_for_auto_load`, 100 ms) and check `dal_fw` = 1 in `DMCUB_SCRATCH0`
   (`dmub_dcn31_is_hw_init`).

Test J then checks three things: `dal_fw` = 1, the picture unchanged, and one DMUB no-op command
(`DMUB_CMD__NULL`, `dmub_cmd.h`) completed through inbox 1. If it fails: DMCUB stays in reset and the GOP picture keeps
running, because the OTG/DIG are untouched. A reboot restores GOP's DMCUB.

## 5. macOS implementation design (after 4a–4c)

- **Where:** a header-only `src/amd/phx_dcn_modeset.h` (pure sequence over `rd`/`wr`/`delayUs`/`smu`/`dmub` callbacks,
  like `phx_dcn_i2c.h`), so a host test can check it against the 60 Hz state recorded on the 8500G (4b) and require the same values in the
  same order. The kernel side calls it from PhxAMDFB (`setDisplayMode`), in thread context.
- **Modes offered:** PhxAMDFB's mode list grows from the one GOP mode to the EDID DTDs it can drive (60, 144, 165,
  180 Hz at 1920×1080). Each mode reports its real timing (`getTimingInfoForDisplayMode`), the AGDC link config
  (`phxagdc.cpp`) gets the same numbers, and the VBL timer is re-measured from the OTG frame counter after the switch.
- **Order of delivery:**
  1. **144 Hz first.** At 335.77 MHz it stays below 340 MHz: HDMI mode, new clock and timing, but no scrambling and no
     SCDC.
  2. **Then 165/180 Hz,** adding scrambling and SCDC.

## 6. Fail-safes

- **Opt-in:** mode setting runs only with the boot-arg `-phxmodeset`. Without it, PhxAMDFB keeps the GOP mode exactly
  as today. `-phxoff` turns everything off as before.
- **Saved state:** every register the sequence will touch is read and saved before the first write, and so are the
  DMUB and SMU settings it changes. The GOP 60 Hz state can then be restored by replaying the saved values in Linux's
  order.
- **Automatic revert:**
  - if the OTG frame counter does not advance, or the DIG/PHY does not report enabled, within a bounded time after the
    switch, the saved state is restored at once;
  - after every successful switch, a 15 s timer reverts to the previous mode unless `phxctl modeok` (or the macOS
    display panel's "keep") confirms it, so a monitor that shows no picture always gets its old mode back.
- **Armed per boot,** like `phxfb-boot`: a boot that hangs or panics during a mode switch disables mode setting on the
  next boot.
- **Kernel rules:** no blocking in interrupt context (mode setting runs from `setDisplayMode` or a thread call, never
  from the VBL path), a timeout on every poll loop (DMUB inbox, SMU response, OTG lock, I2C), MMIO only through the
  existing accessors, logging only behind `phxdebug`.

## 7. Tests on the 8500G (each one small, each with expected log lines)

| Test | What | Writes to the GPU? |
|---|---|---|
| I | 4a: DMCUB state (`dmcub.txt`) | no |
| J | DMCUB: load and start the driver's DMUB firmware as Linux does (`dal_fw` = 1, GOP picture stays) | yes |
| K | Encoding check under macOS: computed 60 Hz register set vs live registers and Phase 0's Linux `dm_dtn_log` | no |
| L | 144 Hz switch with automatic revert (`-phxmodeset`) | yes |
| M | 165 Hz and 180 Hz with scrambling + SCDC | yes |

## 8. Open questions (answered by the dumps, not guessed)

- Is DMCUB already running after GOP (4a)? If not: loading it is an extra step.
- Which DISPCLK/DPPCLK and watermark values Linux uses for each mode (4b).
- Whether the DCCG pixel clock source for OTG0/PHY A needs programming outside the DMUB command (4b).
- The GOP surface pitch field (1023) seen in Phase 0. It stays as GOP set it, since the resolution does not change.
  It is recorded for completeness.
