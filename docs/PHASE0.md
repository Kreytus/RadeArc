# Phase 0: facts from the Ryzen 5 8500G (Radeon 740M)

Nothing here writes to the GPU except the optional `trace` step, which is an ordinary amdgpu driver load.

## BIOS (Gigabyte B650M D3HP AX), check before booting anything

| Setting (Gigabyte name, usually under Settings → IO Ports / Miscellaneous) | Set to | Why |
|---|---|---|
| Initial Display Output | **IGD Video** | GOP lights the iGPU outputs |
| Integrated Graphics | **Forces** (not Auto) | iGPU stays on even with a dGPU present or no monitor detected |
| UMA Mode / UMA Frame Buffer Size | **UMA Specified, 2G** to start (write down the value) | the VRAM carve-out; bring-up and PhxFB place data in it |
| Above 4G Decoding | **Enabled** | BARs above 4 GiB, as on the reference laptop |
| Re-Size BAR Support | **Disabled** for the first runs | upstream was proven with a 256 MiB BAR0; we'll try ReBAR later |
| CSM Support | **Disabled** | pure UEFI GOP (no legacy VBIOS mode) |
| Secure Boot | **Disabled** | otherwise Linux lockdown refuses the PCI BAR reads in step 2 |
| IOMMU | leave as is, and note the value | |

Write down every value; they go into the report.

## Without Linux: native Windows + macOS (the default path)

WSL2 can't be used for the hardware reads: it is a Hyper-V VM and sees the GPU only as Microsoft's
paravirtual `/dev/dxg`, with no PCI device, no MMIO and no amdgpu. WSL is fine for the offline work: `make test`,
`tools/parse_discovery.py`, `edid-decode`.

### Windows (PowerShell as Administrator; nothing here touches the GPU)

PCI ID, revision, subsystem:

```powershell
Get-PnpDevice -Class Display | Format-List FriendlyName,InstanceId
```

```powershell
Get-CimInstance Win32_VideoController | Format-List Name,PNPDeviceID,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate
```

Every monitor EDID Windows has seen, saved as `.bin` in the current folder:

```powershell
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY' -Recurse -ErrorAction SilentlyContinue | Where-Object PSChildName -eq 'Device Parameters' | ForEach-Object { $e = (Get-ItemProperty $_.PSPath).EDID; if ($e) { $n = (($_.PSPath -split '\\')[-3..-2] -join '_') -replace '[^\w]', '_'; [IO.File]::WriteAllBytes("$PWD\edid_$n.bin", $e) } }
```

The VBIOS (connector table) from ACPI VFCT, with ACPICA's `acpidump.exe` (Intel "ACPI Component Architecture"
Windows binaries):

```powershell
.\acpidump.exe -n VFCT -b
```

GPU-Z: a screenshot of the Graphics Card tab (device ID, revision, subvendor, shaders, BIOS version, Resizable
BAR). If its BIOS save arrow works on the APU, also the saved `.rom`.

Then decode in WSL:

```bash
sudo apt install -y edid-decode && edid-decode edid_*.bin
```

### macOS on the 8500G (with the GOP/firmware framebuffer, before any bring-up)

Gets what only the hardware can tell: the IP discovery table (PhxProbe.kext), which PSP/SMU segment answers,
the GOP display pipe and the UMA layout (PhxBringup.kext `ReadRegs` via `phxctl regs`, root plus `phxdebug=1`).
Both kexts match only 1002:1900 upstream, so this needs a small reviewed diff first (adding the Phoenix 2 ID to
their `Info.plist`). **Do not run `install.sh` or `phxboot`**: they would run the Phoenix 1 bring-up on this chip.

OpenCore can also dump all ACPI tables (VFCT included) to the EFI partition: DEBUG build, `Misc → Debug →
SysReport = true`.

Without Linux there are no amdgpu MMIO traces, so the GART/MES/PSP values come from the Linux source (cited per
line) instead of a copied trace.

## Linux live USB (optional, only if one becomes possible)

Ubuntu 25.10 or newer (kernel 6.17+) or a current Arch ISO. Connect the 180 Hz monitor to the board output
you'll use with macOS. If you want to compare, run step 1 once per port (HDMI, then DP).

```bash
sudo apt update && sudo apt install -y vulkan-tools mesa-vulkan-drivers edid-decode pciutils dmidecode xz-utils git
```

```bash
git clone -b claude/amazing-wright-juncqb https://github.com/kreytus/radearc && cd radearc/tools/phase0
```

### Step 1: normal boot (amdgpu loaded)

```bash
sudo bash phase0.sh driver /media/$USER/<usbstick>/phase0-out
```

Collects `lspci -nn` and `-vvv`, the dmesg amdgpu lines (IP discovery, PSP/SMU/IMU/MES messages), the parsed IP
discovery tree with base addresses (`ip_discovery.txt`), the raw table (`dri*/amdgpu_discovery` →
`ip_discovery.bin`), `amdgpu_firmware_info`, the VBIOS (debugfs and ACPI VFCT), `amdgpu_dm_dtn_log` (DCN state:
OTG/HUBP/DIG per pipe), DRM `state` (connector → CRTC → mode), DP `link_settings`, every connector's
`status`/`modes`/EDID (`edid-decode`), VRAM/GTT sizes, and `RADV_DEBUG=info vulkaninfo` (CU/WGP/SE counts,
CU bitmap, GB_ADDR_CONFIG, family, external rev).

Quick look while you're there:

```bash
lspci -nn | grep -iE 'vga|display'
```

```bash
sudo dmesg | grep -iE 'amdgpu.*(discovery|ip block|psp|smu|mes|imu|vram|gtt)'
```

```bash
cat /media/$USER/<usbstick>/phase0-out/driver/ip_discovery.txt
```

```bash
grep -iE 'cu|wgp|se|gb_addr|family|chip|rev' /media/$USER/<usbstick>/phase0-out/driver/radv-info.txt | head -40
```

### Step 2: boot with amdgpu blacklisted (GOP still drives the monitor)

At the GRUB menu press `e`, add `modprobe.blacklist=amdgpu` to the `linux` line, and boot (F10). The picture
stays at the firmware resolution. Then:

```bash
sudo bash phase0.sh gop /media/$USER/<usbstick>/phase0-out
```

This runs `gop_dump.py`. It reads (32-bit MMIO reads only, from BAR5) the DCN 3.1.4 registers that describe the
pipe GOP lit: OTG0-3 control and timing and frame counter, ODM source, HUBP0-3 surface address and pitch,
DIG0-4 FE/BE control, HDMI/TMDS/DP config, DIO link and AUX/DDC setup, and the pixel-clock DTOs. It also reads
the PSP/SMU mailboxes at **every** MP0/MP1 segment, and the MMHUB FB layout. Offsets come from Linux
`dcn_3_1_4_offset.h`, `mp_13_0_{2,4}_offset.h` and `mmhub_3_0_1_offset.h`; bases come from step 1's discovery tree.

### Step 3 (optional, same boot as step 2, after it): MMIO traces of Linux bringing the GPU up

```bash
sudo bash phase0.sh trace /media/$USER/<usbstick>/phase0-out 0
```

Then reboot into the step 2 entry again, and:

```bash
sudo bash phase0.sh trace /media/$USER/<usbstick>/phase0-out 1
```

`dc=0` records GFX/PSP/SMU/MES only (the Phoenix 2 version of the trace upstream built `mesboot` from). `dc=1`
also records how amdgpu takes over the GOP pipe and reads the EDID over DDC/AUX, which we need for Phase 3a.

### Send back

```bash
tar -C /media/$USER/<usbstick> -cJf phase0-8500g.tar.xz phase0-out
```

Also send: the BIOS version, the BIOS values from the table above, and which rear port the monitor was on in each step.

## What I'll check in the result

- `lspci`: 1002:15c8 and its revision (soc21.c: external_rev = rev + 0x80 on GC 11.0.4).
- `ip_discovery.txt`: GC 11.0.4, MP0/MP1 13.0.11, DCN 3.1.4, SDMA 6.0.1, VCN 4.0.2, MMHUB/OSSSYS/SMUIO/NBIF versions, and all bases against the Phoenix 1 table in `hardware/dumps/firebat_u6c/ip_discovery.txt`.
- `gop_regs.txt`: which MP0 segment shows a non-zero C2PMSG_81 (SOS alive) and which MP1 segment shows C2PMSG_90 = 1; which OTG has bit 0 of OTG_CONTROL set and a running frame counter; which DIG's FE selects that OTG and what mode its BE is in (HDMI vs DP); where the HUBP surface sits relative to `MMMC_VM_FB_LOCATION_BASE`.
- `firmware_info`: the versions to pin in the fixtures.
- `radv-info.txt`: num CUs (expect 4 = 2 WGP), SE/SA/RB counts, GB_ADDR_CONFIG, the CU bitmap.
