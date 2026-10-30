# Phoenix 2 (Radeon 740M) port: audit of upstream nullarc

Base: [luuumi-dev/nullarc](https://github.com/luuumi-dev/nullarc) at `cbb08dd` (2026-10-21).
Linux references: torvalds/linux `master`, `drivers/gpu/drm/amd` (fetched 2026-10-30).

How bring-up works upstream matters for the port. `PhxBringup.kext` only exposes primitives: the PSP ring,
LOAD_IP_FW, SETUP_TMR, the SMU mailbox, register and VRAM peek/poke, and queue adoption. The **sequence** runs
in user space: `phxboot.sh` → `full.sh` → `bringup5.sh` (PSP, firmware, SMU), then `mesboot` (GART, MES, queues),
using values copied from a Linux MMIO trace taken on the Firebat U6C (a Ryzen 7 H 255, 1002:1900, GC 11.0.1).
Most Phoenix 1 assumptions therefore live in shell scripts and `mesboot.c`, not in the kext.

## (a) Every place that hardcodes Phoenix 1, 780M or eDP

### A. PCI ID and GPU identity (1002:1900, "780M")
| Where | What |
|---|---|
| `src/phxbringup/Info.plist:32` | `IOPCIMatch 0x19001002` only |
| `src/phxaccel/Info.plist:35,45` | `IOPCIPrimaryMatch 0x19001002` for both PhxAMDFB and NMXAccel |
| `src/phxprobe/Info.plist:32` | PhxProbe (reads the discovery table under macOS) matches 1900 only |
| `src/phxbringup/PhxBringup.cpp:385` | `model` = "AMD Radeon 780M" (the 760M fork's CPU-SKU naming is not in upstream) |
| `mesa-patches-phoenix/0002` | device_id 0x1900, rev 0xBA, subsystem 8001:2014, name "AMD Radeon 780M Graphics" |
| `mesa-patches-phoenix/0001` | canned device defaults to `"phoenix"`; `acd_canned_gc_version()` only knows `AMDGPU_FAMILY_GC_11_0_1` → 11.0.1 |
| `install/install.sh:5,48-49` | accepts 15c8 already, but says "780M/760M" |
| `src/kext/*`, `src/uapi/r3n_*`, `src/winsys/*` | old scaffold (not shipped; only `test_r3n_abi` uses it): chip_id 0x1900, rev 0xBA, 12 CU, 2700 MHz, 120 Hz |

### B. GC 11.0.1 constants and CU configuration
| Where | What |
|---|---|
| `PhxBringup.cpp:51-52` (sent to RADV in QueryInfo, `:1339-1340`) | `kGbAddrConfig = 0x242`, `kGcVersion = 11.0.1`. Linux reads GB_ADDR_CONFIG from the hardware (`gfx_v11_0.c`, `gfx_v11_0_gpu_early_init`: `RREG32_SOC15(GC, 0, regGB_ADDR_CONFIG)`). |
| Mesa 0001 `acd_native_handshake` | refuses a live GB_ADDR_CONFIG that differs from the canned table (unless `RADV_DARWIN_ALLOW_ADDRCFG=1`), so the canned table must match Phoenix 2 |
| Mesa canned `"phoenix"` entry (`amdgpu_devices.c`) | CU count, CU bitmap, SE/SA/RB counts, external rev: all Phoenix 1 / 12 CU |
| `PhxBringup.cpp:56-60` | `kVisTotal` 256 MiB and `kHiTotal` 1.5 GiB: "BAR0 is 256 MiB, UMA 6 GiB on this machine" |
| `PhxBringup.cpp:489-490, 407`; `mesboot.c` `SEG0/SEG1` (0x1260 / 0xA000); `full.sh` 0xDE0/0xA802/0xA803; `rb0.sh`; `phxctl.c gc` | GC registers as absolute dwords, which is only valid if Phoenix 2 reports the same GC bases |
| `mesboot.c:564` (`SET_HW_RESOURCES`) | GC/MMHUB/OSSSYS base tables `{0x1260,0xA000,0x2402c00}`, `{0x13200,0x1a000,0x2408800}`, `{0x10a0,0x240a000}` copied from the Firebat discovery table |
| `PhxNativeABI.h:68-70` | MMHUB FB_LOCATION/OFFSET at fixed 0x1A8EC/0x1A8ED/0x1A8D7 (MMHUB 3.0.1 base[1] 0x1a000 + `mmhub_3_0_1_offset.h`) |
| `PhxNativeABI.h:114` | NBIO HDP flush remap at 0xE4D (NBIF 7.7.0 assumption) |
| `PhxBringup.cpp:1362`, Mesa 0004 | SMUIO GOLDEN_TSC at 0x16a2d (SMUIO 13.0.7 base 0x16a00) |
| `tools/phxctl/*` | the golden registers are never programmed on either chip (Linux does it in `gfx_v11_0_init_golden_registers`) |

### C. PSP 13.0.4 / SMU 13.0.4
| Where | What |
|---|---|
| `PhxNativeABI.h:72-82` | PSP C2PMSG_58/64/67/69/70/71/81 at **0x16000 + offset**. That is MP0 base[1], `mp_13_0_4_offset.h` (BASE_IDX 1, used by `psp_v13_0_4.c`). |
| `PhxNativeABI.h:119-122` | SMU msg/arg/resp at 0x16282/0x16292/0x1629A: MP1 base[1] (`smu_v13_0_4_ppt.c`, `smu_v13_0_4_init_msg_ctl`, BASE_IDX 1) |
| `PhxBringup.cpp:869, 1066, 1119` | comments and flow are written against psp_v13_0_4 and smu 13.0.4 |

**Verified difference for 13.0.11:** `amdgpu_psp.c psp_early_init` sends MP0 13.0.11 to `psp_v13_0_set_psp_funcs`
(`psp_v13_0.c`, which includes `mp_13_0_2_offset.h`, **BASE_IDX 0**). For MP1 13.0.11, `smu_v13_0_4_set_ppt_funcs`
calls `smu_v13_0_init_msg_ctl` (`smu_v13_0.c`, also **BASE_IDX 0**) instead of `smu_v13_0_4_init_msg_ctl`. On
Phoenix 1, MP0/MP1 base[0] is 0x4000 and base[1] is 0x16000. If Phoenix 2 reports the same bases, the mailboxes
move to 0x4080 / 0x4282. Only the discovery table and a live read can settle this; `gop_dump.py` reads both segments.

Verified **unchanged** for 13.0.11: the message map (`smu_v13_0_4_message_map`; EnableGfxImu = 0x16,
`smu_v13_0_4_ppsmc.h:76`, argument `ENABLE_IMU_ARG_GFXOFF_ENABLE`); the non-SR-IOV ring create sequence
(C2PMSG_69/70/71 then 64, `psp_v13_0_ring_create`); `boot_time_tmr = false`, so SETUP_TMR is still sent
(`psp_skip_tmr`); firmware comes from a TOC plus a TA (`psp_v13_0_init_microcode`).

### D. Firmware names and load sequence
| Where | What |
|---|---|
| `tools/phxctl/bringup5.sh:13-35` (and `bringup3.sh`) | `gc_11_0_1_*`, `psp_13_0_4_toc.bin`, and a **hardcoded byte offset and size for every payload**. For example MES KIQ `gc_11_0_1_mes1.bin 256 104272` / `104528 131072`; linux-firmware's `gc_11_0_4_mes1.bin` header says ucode 103600 B at 256 and data 131072 B at 103856, so the Phoenix 1 offsets load garbage. |
| `bringup5.sh:14` | LOAD_TOC with a fixed size 0x900 |
| `bringup5.sh:15` | SETUP_TMR with a fixed TMR MC address, size 64 MiB and system address `0x6A_0C000000` = Firebat FB_OFFSET (0x6A0000000) + 0xC000000. Machine-specific. |
| `tools/fetch_test_firmware.sh`, `Makefile fixtures`, `install/install.sh:51` | only the 11.0.1 / 13.0.4 file set |
| `src/amd/rdna3_ucode_extract.cpp` | already parses the headers correctly, but the scripts don't use it |

Linux names for Phoenix 2 (verified from `MODULE_FIRMWARE`): `gc_11_0_4_{pfp,me,mec,rlc,imu,mes_2,mes1}.bin`
(`gfx_v11_0.c:108-111`, `imu_v11_0.c:39`, `mes_v11_0.c:48-50`), `psp_13_0_11_{toc,ta}.bin` (`psp_v13_0.c:52-53`).
SDMA, DMCUB and VCN names come from their discovered versions (`sdma_6_0_1`, `dcn_3_1_4_dmcub`, `vcn_4_0_2` only
if the dump says 6.0.1 / 3.1.4 / 4.0.2).

### E. GART and MES (`tools/phxctl/mesboot.c`), all from the Firebat trace
| Line | What |
|---|---|
| 73-77 | `VRAM_MC 0x8000000000`, `FB_PA 0x6A0000000` (Firebat `GCMC_VM_FB_OFFSET << 24`), GART at VRAM 0x2400000 |
| 106-112 | SYSTEM_APERTURE 0x00200000..0x00205fff (6 GiB UMA), DEFAULT_ADDR 0x006a0f9f, fault default 0xa23c |
| 80-89, bringup5/full/rb0 | fixed VRAM scratch layout: PSP ring 0x1100000, TMR 20 MiB, fw_buf 64 MiB, GART 0x2400000, rings 0x25xxxxx, test 0x26/0x27xxxxx. It assumes nothing else lives there, in particular **not the GOP scan-out**, and that UMA is large enough. |
| `mes_start` | MES `uc_start` 0xF0005000 fixed (the 11.0.4 header carries the same value, but it should be read from the header) |

Verified **unchanged** for GC 11.0.4: `gfx_v11_0_init_golden_registers` applies `golden_settings_gc_11_0_1` to
both 11.0.1 and 11.0.4; `imu_v11_0_program_rlc_ram` has no 11.0.1/11.0.4 case; `gmc_v11_0_set_gfxhub_funcs`
uses `gfxhub_v3_0` for both; RLC bootload status uses `regRLC_RLCS_BOOTLOAD_STATUS_gc_11_0_1` for both
(`gfx_v11_0.c:3159-3161`). `soc21.c` sets `external_rev_id = rev_id + 0x80` for 11.0.4 (11.0.1: `+ 0x1`); Mesa
tells R1 from R2 by that value.

### F. Display (PhxAccel): eDP and internal-panel assumptions
| Where | What |
|---|---|
| `phxfb.cpp:234-243` | `measureRefresh` reads **OTG0 only**, at fixed DMU base 0x34c0. GOP on HDMI/DP may use another OTG. |
| `phxfb.cpp:356` | `kConnectionFlags = kIOConnectionBuiltIn` |
| `phxfb.cpp:526` | `built-in = true` |
| `phxfb.cpp` (no `getDDCBlock` / `kConnectionSupportsHLDDCSense`) | no EDID at all, so IODisplay has nothing to identify the monitor by |
| `phxfb.cpp:75, 249` | 60 Hz fallback; the rounding table has no 180000 (a measured 180 Hz stays as measured, which is fine) |
| `phxagdc.cpp:56` | default 2560×1600 @ 60 Hz when PhxFB properties are missing |
| `phxagdc.cpp:53-73`, `nvrm-agdc-k5.h:65-84` | the AGDC link config is a synthetic CVT-RB timing with fixed sync polarity, not the timing GOP actually set |
| `nvrm-accel.cpp:696-699` | the accelerator assumes it drives "the 780M panel" (comments and logs only) |
| `phxboot.sh` | WindowServer arming assumes the panel PhxAMDFB took is the one display |

Upstream has **no backlight, OLED or lid code**: that was added by the 760M fork. Nothing to turn off, as long
as none of it is brought over.

### G. Tests and fixtures
| Where | What |
|---|---|
| `tests/test_rdna3_ucode.cpp:31-155` | only `gc_11_0_1_*`, `sdma_6_0_1`, `dcn_3_1_4_dmcub` |
| `tests/test_rdna3_discovery_uma.cpp:59-72` | synthetic table claims GC **11.0.3** plus MP0 13.0.4, which is not a real Phoenix |
| `src/amd/rdna3_ip_discovery.cpp:170-191` | `is_phoenix_apu()` accepts any GC 11.0.x / PSP 13.0.x / DCN 3.1.x / SDMA 6.0.x (it would match Navi3x dGPUs) |
| `src/amd/rdna3_dcn314_scanout.cpp:9-20` | OTG/HUBP offsets (0x20, 0x110, …) do **not** match `dcn_3_1_4_offset.h` (`regOTG0_OTG_H_TOTAL` = 0x1b2a, BASE_IDX 2). Scaffold, unused by PhxAccel: don't build on it. |
| `tests/test_rdna3_kext_bundle.py:54` | tests the legacy RDNA3Driver bundle (1900) |
| `hardware/dumps/firebat_u6c/ip_discovery.bin` | real Phoenix 1 table: the reference for a Phoenix 2 fixture |

### Fail-safes that already exist
`-phxbringupoff` (PhxBringup declines to start), the `phxboot-off` kill-switch file, `/var/db/phxboot.pending`
(an unfinished boot disables the next attempt), NVRAM `phxfb-boot` (PhxAMDFB takes the GOP display only once
per arming), WindowServer crash and hang counting with fallback, and 3-4 minute `shutdown -r` watchdogs around
bring-up. Missing: one boot-arg that also stops PhxAMDFB and phxboot. `phxboot.sh` currently just logs
"PhxBringup not loaded".

## (b) Phoenix 2 facts needed from the hardware

1. **PCI ID and revision**: Linux matches by PCI class, so the source can't confirm 15C8 (`lspci -nn`).
2. **Full IP discovery table** (binary and parsed): versions and **every base address** of GC, MP0, MP1, MMHUB,
   OSSSYS, SMUIO, NBIF, HDP, DMU, SDMA0, VCN, plus the `gc_info` block (SE/SA/WGP counts, which only exist in the binary).
3. **Which MP0/MP1 segment the PSP and SMU answer on** (see C): `gop_dump.py` reads C2PMSG_64/81/90 at every base.
4. **Firmware versions** actually loaded (`amdgpu_firmware_info`), so we test with the same linux-firmware blobs.
5. **CU/WGP harvest, GB_ADDR_CONFIG, external rev, family** as RADV sees them (`RADV_DEBUG=info`).
6. **UMA layout**: MMMC_VM_FB_LOCATION_BASE/TOP and FB_OFFSET, BAR0/BAR2/BAR5 sizes with your BIOS settings.
7. **The GOP display pipe**: which OTG runs, which HUBP and surface address, which DIG/DP/HDMI back end and
   PHY, its timing and pixel-clock source, and where in VRAM the GOP framebuffer sits.
8. **Connectors**: which rear port is which DIG/PHY/DDC/AUX (Linux `dm_dtn_log`, `state`, VBIOS/VFCT object
   table), and whether the HDMI port is native TMDS or a DP-to-HDMI converter (PCON/LSPCON).
9. **The monitor's EDID** over HDMI and over DP.
10. **An MMIO trace of Linux bringing GC 11.0.4 up** (dc=0) and one with the display (dc=1): the Phoenix 1
    GART/MES values were copied from such a trace, and we need the Phoenix 2 equivalents.

Not needed from the hardware: golden registers, the SMU message ids and the firmware file names. Those are
verified from the Linux source above.
