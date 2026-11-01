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
