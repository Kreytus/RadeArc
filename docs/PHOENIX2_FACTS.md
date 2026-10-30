# Phoenix 2 facts: Ryzen 5 8500G on Gigabyte B650M D3HP AX

Measured 2026-10-30 from an Ubuntu 25.10 live USB (kernel 6.17.0-5), one boot with `modprobe.blacklist=amdgpu`:
the GOP registers were read first, then amdgpu was loaded and queried. BIOS FA9a (2026-06-09), UMA 2 GiB, ReBAR off.
Raw files: [`hardware/dumps/b650m_8500g/`](../hardware/dumps/b650m_8500g/).

## Identity
| | Phoenix 2 (this machine) | Phoenix 1 reference (Firebat U6C) | Source |
|---|---|---|---|
| PCI | **1002:15C8 rev C5**, subsystem 1458:D000 | 1002:1900 rev BA | `lspci-gpu.txt` |
| amdgpu `rev_id` / external rev | 2 / **0x82** | 0x81 | `radv-info.txt` (`chip_rev`, `chip_external_rev`); soc21.c `rev_id + 0x80` |
| Mesa name / family_id | **PHOENIX2**, family_id 148 (GC_11_0_1 family, same as Phoenix 1) | PHOENIX | `radv-info.txt` |
| CPU brand | "AMD Ryzen 5 8500G w/ Radeon 740M Graphics" | | |
| VBIOS | 113-PHXGENERIC-001 | | `firmware_info.txt` |

## IP versions and bases (differences from Phoenix 1 in bold)
| IP | Version | Bases | vs Phoenix 1 |
|---|---|---|---|
| GC | **11.0.4** | 0x1260 0xA000 0x2402C00 | same bases |
| MP0 (PSP) | **13.0.11** | **0x16000** 0xDC0000 0xE00000 0xE40000 0x243FC00 | Phoenix 1 lists 0x4000 first; this chip has no 0x4000 segment |
| MP1 (SMU) | **13.0.11** | **0x16000** … (as MP0) | as MP0 |
| DMU (DCN) | 3.1.4 | 0x12 0xC0 0x34C0 0x9000 0x2403C00 | same |
| MMHUB | 3.0.1 | 0x13200 0x1A000 0x2408800 | same |
| SDMA0 | 6.0.1 | 0x1260 0xA000 0x2402C00 | same |
| VCN (UVD) | 4.0.2 | 0x7800 0x7E00 | same |
| OSSSYS | 6.0.1 | 0x10A0 0x240A000 | same |
| SMUIO | 13.0.7 | 0x16800 0x16A00 0x440000 0x2401000 | same (GOLDEN_TSC 0x16a2d stays valid) |
| NBIF | **7.7.1** | 0x0 0x14 0xD20 0x10400 0x241B000 0x4040000 | Phoenix 1: 7.7.0, same bases |
| DF | **4.0.2** | | Phoenix 1: 4.0.0 |
| LSDMA | 6.0.1 | 0x11400 **0x2456800** | extra base |

**PSP/SMU mailboxes:** Linux on 13.0.11 uses BASE_IDX 0 (`psp_v13_0.c` with `mp_13_0_2_offset.h`; `smu_v13_0_init_msg_ctl`).
Here base[0] = 0x16000, so the absolute dwords are the **same as upstream's constants**: C2PMSG_64 = 0x16080,
C2PMSG_81 = 0x16091, SMU msg/arg/resp = 0x16282/0x16292/0x1629A. Measured live before any driver: C2PMSG_81 =
0x001DF3C3 (SOS alive), C2PMSG_64 = 0x80C00000 (ready flag), SMU C2PMSG_90 = 1 (last response OK).
The code must still compute them as `MP base[BASE_IDX] + offset` per version, not hardcode them.

## Shader configuration (Linux + RADV)
1 SE, 1 SH per SE, 4 CUs per SH = **4 CUs (2 WGPs)**, CU mask SE0/SA0 = 0xF, 1 RB (mask 0x1), **GB_ADDR_CONFIG = 0x00000142**
(Phoenix 1 / upstream `kGbAddrConfig`: 0x242).

## Memory
| | |
|---|---|
| UMA carve-out | 2 GiB: MMMC_VM_FB_LOCATION_BASE/TOP = 0x8000/0x807F → MC 0x80_0000_0000..0x80_7FFF_FFFF |
| Carve-out system address | MMMC_VM_FB_OFFSET = 0x3E0 → **0x3_E000_0000** (Firebat: 0x6_A000_0000; `mesboot.c FB_PA` is wrong here) |
| Linux PSP TMR | 0x4A00000 bytes at MC 0x80_7000_0000 (dmesg "reserve 0x4a00000 … for PSP TMR") |
| BARs | BAR0 256 MiB @ 0xFC_F000_0000, BAR2 2 MiB, BAR5 512 KiB @ 0xDF70_0000 |
| GTT (Linux) | 6791 MiB |

## Firmware Linux loaded (linux-firmware in Ubuntu 25.10)
ME 0x07, PFP 0x0E, MEC 0x0F (feature 35, F32/legacy), RLC 0x22, RLCP 0x04, IMU 0x0B012E00, MES 0x7E, MES_KIQ 0x73,
SDMA0 0x17, TOC 0x0B, SMC 76.103.0, DMCUB 0x08005200, VCN 0x09118011, ASD 0x210000F3, TA HDCP 0x17000046, TA DTM 0x12000019.

## The pipe GOP lit (read before amdgpu loaded; fields decoded with `dcn_3_1_4_sh_mask.h`)
| | Value | Meaning |
|---|---|---|
| OTG | **OTG0** only: OTG_CONTROL 0x00010301 (MASTER_EN=1) | OTG1-3 off |
| Timing | H_TOTAL 2199, V_TOTAL 1124, H blank 192..2112, V blank 41..1121, HSYNC 0..44, VSYNC 0..5 | **1920×1080, 2200×1125 total**: the CEA-861 1080p timing (60 Hz at 148.5 MHz; the pixel clock was not read) |
| Surface | HUBP0, address MC 0x80_0000_0000 (**VRAM offset 0**), SURFACE_PIXEL_FORMAT 8 = ARGB8888 (`dcn20_hubp.c`), PITCH field 1023 | GOP framebuffer at the start of VRAM. Pitch 1024 px vs 1920 active is unexplained (scaled mode or a different pitch unit): **open** |
| Back end | **DIG0**: DIG_BE_CNTL DIG_MODE=2 = TMDS-DVI (`dcn10_link_encoder.c`), FE source = FE0; DIG_FE_CNTL SYMCLK_FE_ON; SYMCLKA enabled | HDMI port driven as DVI (no HDMI packets), on PHY A |
| DDC | DC_I2C_DDC1_SETUP ENABLE=1 (others idle) | the HDMI port's EDID is on **DDC1** |
| Linux connector | HDMI-A-1 connected (1920×1080 modes); DP-1..7 disconnected (DP-3..7 are USB-C/MST entries) | `linux_drm_state.txt` |

## Not captured (and how we get them instead)
- **EDID**: script bug (sysfs reports the file as size 0), now fixed. PhxFB will read it over DDC1 in Phase 3a.
- **MMIO trace**: script bug (amdgpu's tracepoints only exist after the module loads), now fixed. Phase 1 uses the Linux
  source per register instead of a copied trace.
- **Refresh rate**: PhxFB measures it from OTG0's frame counter at start.

## What this changes in the plan
- PSP/SMU: no address change needed on this chip, but key the segment choice on the MP version (BASE_IDX 0 for 13.0.11).
- GB_ADDR_CONFIG 0x142, 4 CUs, external rev 0x82: PhxBringup must read GB_ADDR_CONFIG live, and Mesa needs a Phoenix 2 canned entry.
- Firmware: `gc_11_0_4_*` + `psp_13_0_11_*`; sdma_6_0_1, dcn_3_1_4_dmcub and vcn_4_0_2 are shared with Phoenix 1.
- VRAM layout: the GOP framebuffer sits at VRAM offset 0 (≈8 MiB at 1080p), so upstream's fixed scratch layout at
  17 MiB and up does not overlap it. TMR size must come from the TOC (Linux: 0x4A00000), not upstream's fixed 64 MiB.
- Display 3a: take over OTG0 → DIG0 (TMDS) → PHY A, EDID on DDC1, report an external (not built-in) HDMI connection.
