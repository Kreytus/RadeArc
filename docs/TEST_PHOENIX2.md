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
