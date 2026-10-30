#!/usr/bin/env python3
"""
gop_dump.py: read-only snapshot of the display pipe the firmware (GOP) lit, plus the PSP/SMU mailbox
segments, on a Phoenix / Phoenix 2 APU. Run as root from a Linux live USB booted with
modprobe.blacklist=amdgpu, so no driver has touched the display engine yet.

  sudo python3 -I gop_dump.py --bdf 0000:XX:00.0 --disc <phase0-out>/driver/ip_discovery

--disc is the ip_discovery tree that `phase0.sh driver` saved from a boot with amdgpu loaded. It gives
the per-segment base addresses of DMU, MP0, MP1 and MMHUB for this exact chip, so nothing here assumes
the Phoenix 1 bases.

Only reads. It does not touch AUX or I2C data registers, any index/data pair, or any register that
clears when read.

Register names and offsets come from Linux drivers/gpu/drm/amd/include/asic_reg:
  dcn/dcn_3_1_4_offset.h   (DCN 3.1.4)
  mp/mp_13_0_4_offset.h    (MP0 C2PMSG_64/81 = 0x80/0x91, BASE_IDX 1)
  mp/mp_13_0_2_offset.h    (the same offsets with BASE_IDX 0, used by psp_v13_0.c and smu_v13_0.c, i.e. 13.0.11)
  mmhub/mmhub_3_0_1_offset.h (MMMC_VM_FB_* BASE_IDX 1)
"""
import argparse
import ctypes
import mmap
import os
import struct
import sys

# (name, dword offset, BASE_IDX) from dcn/dcn_3_1_4_offset.h. Absolute dword = DMU base[BASE_IDX] + offset.
DCN314 = [
    ("OTG0_OTG_CONTROL", 0x1b41, 2),
    ("OTG0_OTG_H_TOTAL", 0x1b2a, 2),
    ("OTG0_OTG_H_BLANK_START_END", 0x1b2b, 2),
    ("OTG0_OTG_H_SYNC_A", 0x1b2c, 2),
    ("OTG0_OTG_V_TOTAL", 0x1b2f, 2),
    ("OTG0_OTG_V_BLANK_START_END", 0x1b36, 2),
    ("OTG0_OTG_V_SYNC_A", 0x1b37, 2),
    ("OTG0_OTG_STATUS_POSITION", 0x1b4a, 2),
    ("OTG0_OTG_STATUS_FRAME_COUNT", 0x1b4c, 2),
    ("ODM0_OPTC_DATA_SOURCE_SELECT", 0x1acb, 2),
    ("HUBP0_DCSURF_SURFACE_CONFIG", 0x05e5, 2),
    ("HUBPREQ0_DCSURF_SURFACE_PITCH", 0x0607, 2),
    ("HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS", 0x060a, 2),
    ("HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0x060b, 2),
    ("OTG0_PIXEL_RATE_CNTL", 0x0080, 1),
    ("DP_DTO0_PHASE", 0x0081, 1),
    ("DP_DTO0_MODULO", 0x0082, 1),
    ("OTG1_OTG_CONTROL", 0x1bc1, 2),
    ("OTG1_OTG_H_TOTAL", 0x1baa, 2),
    ("OTG1_OTG_H_BLANK_START_END", 0x1bab, 2),
    ("OTG1_OTG_H_SYNC_A", 0x1bac, 2),
    ("OTG1_OTG_V_TOTAL", 0x1baf, 2),
    ("OTG1_OTG_V_BLANK_START_END", 0x1bb6, 2),
    ("OTG1_OTG_V_SYNC_A", 0x1bb7, 2),
    ("OTG1_OTG_STATUS_POSITION", 0x1bca, 2),
    ("OTG1_OTG_STATUS_FRAME_COUNT", 0x1bcc, 2),
    ("ODM1_OPTC_DATA_SOURCE_SELECT", 0x1adb, 2),
    ("HUBP1_DCSURF_SURFACE_CONFIG", 0x06c1, 2),
    ("HUBPREQ1_DCSURF_SURFACE_PITCH", 0x06e3, 2),
    ("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS", 0x06e6, 2),
    ("HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0x06e7, 2),
    ("OTG1_PIXEL_RATE_CNTL", 0x0084, 1),
    ("DP_DTO1_PHASE", 0x0085, 1),
    ("DP_DTO1_MODULO", 0x0086, 1),
    ("OTG2_OTG_CONTROL", 0x1c41, 2),
    ("OTG2_OTG_H_TOTAL", 0x1c2a, 2),
    ("OTG2_OTG_H_BLANK_START_END", 0x1c2b, 2),
    ("OTG2_OTG_H_SYNC_A", 0x1c2c, 2),
    ("OTG2_OTG_V_TOTAL", 0x1c2f, 2),
    ("OTG2_OTG_V_BLANK_START_END", 0x1c36, 2),
    ("OTG2_OTG_V_SYNC_A", 0x1c37, 2),
    ("OTG2_OTG_STATUS_POSITION", 0x1c4a, 2),
    ("OTG2_OTG_STATUS_FRAME_COUNT", 0x1c4c, 2),
    ("ODM2_OPTC_DATA_SOURCE_SELECT", 0x1aeb, 2),
    ("HUBP2_DCSURF_SURFACE_CONFIG", 0x079d, 2),
    ("HUBPREQ2_DCSURF_SURFACE_PITCH", 0x07bf, 2),
    ("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS", 0x07c2, 2),
    ("HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0x07c3, 2),
    ("OTG2_PIXEL_RATE_CNTL", 0x0088, 1),
    ("DP_DTO2_PHASE", 0x0089, 1),
    ("DP_DTO2_MODULO", 0x008a, 1),
    ("OTG3_OTG_CONTROL", 0x1cc1, 2),
    ("OTG3_OTG_H_TOTAL", 0x1caa, 2),
    ("OTG3_OTG_H_BLANK_START_END", 0x1cab, 2),
    ("OTG3_OTG_H_SYNC_A", 0x1cac, 2),
    ("OTG3_OTG_V_TOTAL", 0x1caf, 2),
    ("OTG3_OTG_V_BLANK_START_END", 0x1cb6, 2),
    ("OTG3_OTG_V_SYNC_A", 0x1cb7, 2),
    ("OTG3_OTG_STATUS_POSITION", 0x1cca, 2),
    ("OTG3_OTG_STATUS_FRAME_COUNT", 0x1ccc, 2),
    ("ODM3_OPTC_DATA_SOURCE_SELECT", 0x1afb, 2),
    ("HUBP3_DCSURF_SURFACE_CONFIG", 0x0879, 2),
    ("HUBPREQ3_DCSURF_SURFACE_PITCH", 0x089b, 2),
    ("HUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS", 0x089e, 2),
    ("HUBPREQ3_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", 0x089f, 2),
    ("OTG3_PIXEL_RATE_CNTL", 0x008c, 1),
    ("DP_DTO3_PHASE", 0x008d, 1),
    ("DP_DTO3_MODULO", 0x008e, 1),
    ("DIG0_DIG_FE_CNTL", 0x208b, 2),
    ("DIG0_DIG_BE_CNTL", 0x20b1, 2),
    ("DIG0_HDMI_CONTROL", 0x2094, 2),
    ("DIG0_TMDS_CNTL", 0x20d8, 2),
    ("DP0_DP_LINK_CNTL", 0x2108, 2),
    ("DP0_DP_CONFIG", 0x210b, 2),
    ("DP0_DP_VID_STREAM_CNTL", 0x210c, 2),
    ("DP_AUX0_AUX_CONTROL", 0x1f50, 2),
    ("DIG1_DIG_FE_CNTL", 0x218b, 2),
    ("DIG1_DIG_BE_CNTL", 0x21b1, 2),
    ("DIG1_HDMI_CONTROL", 0x2194, 2),
    ("DIG1_TMDS_CNTL", 0x21d8, 2),
    ("DP1_DP_LINK_CNTL", 0x2208, 2),
    ("DP1_DP_CONFIG", 0x220b, 2),
    ("DP1_DP_VID_STREAM_CNTL", 0x220c, 2),
    ("DP_AUX1_AUX_CONTROL", 0x1f6c, 2),
    ("DIG2_DIG_FE_CNTL", 0x228b, 2),
    ("DIG2_DIG_BE_CNTL", 0x22b1, 2),
    ("DIG2_HDMI_CONTROL", 0x2294, 2),
    ("DIG2_TMDS_CNTL", 0x22d8, 2),
    ("DP2_DP_LINK_CNTL", 0x2308, 2),
    ("DP2_DP_CONFIG", 0x230b, 2),
    ("DP2_DP_VID_STREAM_CNTL", 0x230c, 2),
    ("DP_AUX2_AUX_CONTROL", 0x1f88, 2),
    ("DIG3_DIG_FE_CNTL", 0x238b, 2),
    ("DIG3_DIG_BE_CNTL", 0x23b1, 2),
    ("DIG3_HDMI_CONTROL", 0x2394, 2),
    ("DIG3_TMDS_CNTL", 0x23d8, 2),
    ("DP3_DP_LINK_CNTL", 0x2408, 2),
    ("DP3_DP_CONFIG", 0x240b, 2),
    ("DP3_DP_VID_STREAM_CNTL", 0x240c, 2),
    ("DP_AUX3_AUX_CONTROL", 0x1fa4, 2),
    ("DIG4_DIG_FE_CNTL", 0x248b, 2),
    ("DIG4_DIG_BE_CNTL", 0x24b1, 2),
    ("DIG4_HDMI_CONTROL", 0x2494, 2),
    ("DIG4_TMDS_CNTL", 0x24d8, 2),
    ("DP4_DP_LINK_CNTL", 0x2508, 2),
    ("DP4_DP_CONFIG", 0x250b, 2),
    ("DP4_DP_VID_STREAM_CNTL", 0x250c, 2),
    ("DP_AUX4_AUX_CONTROL", 0x1fc0, 2),
    ("DIO_LINKA_CNTL", 0x1f04, 2),
    ("DIO_LINKB_CNTL", 0x1f05, 2),
    ("DIO_LINKC_CNTL", 0x1f06, 2),
    ("DIO_LINKD_CNTL", 0x1f07, 2),
    ("DIO_LINKE_CNTL", 0x1f08, 2),
    ("DIO_LINKF_CNTL", 0x1f09, 2),
    ("DC_I2C_DDC1_SETUP", 0x1ea3, 2),
    ("DC_I2C_DDC2_SETUP", 0x1ea5, 2),
    ("DC_I2C_DDC3_SETUP", 0x1ea7, 2),
    ("DC_I2C_DDC4_SETUP", 0x1ea9, 2),
    ("DC_I2C_DDC5_SETUP", 0x1eab, 2),
    ("SYMCLKA_CLOCK_ENABLE", 0x00a0, 1),
    ("SYMCLKB_CLOCK_ENABLE", 0x00a1, 1),
    ("SYMCLKC_CLOCK_ENABLE", 0x00a2, 1),
    ("SYMCLKD_CLOCK_ENABLE", 0x00a3, 1),
    ("SYMCLKE_CLOCK_ENABLE", 0x00a4, 1),
    ("PHYASYMCLK_CLOCK_CNTL", 0x0052, 2),
    ("PHYBSYMCLK_CLOCK_CNTL", 0x0053, 2),
    ("PHYCSYMCLK_CLOCK_CNTL", 0x0054, 2),
    ("PHYDSYMCLK_CLOCK_CNTL", 0x0055, 2),
    ("PHYESYMCLK_CLOCK_CNTL", 0x0056, 2),
    ("DPSTREAMCLK_CNTL", 0x004a, 1),
    ("HDMISTREAMCLK_CNTL", 0x0059, 2),
]

# MP0 (PSP) C2PMSG_64 (ring status), C2PMSG_81 (SOS sign of life); MP1 (SMU) C2PMSG_90 (response).
# Read at every segment base the discovery table lists: 13.0.4 uses BASE_IDX 1 (mp_13_0_4_offset.h),
# psp_v13_0.c / smu_v13_0.c (13.0.11) use BASE_IDX 0 (mp_13_0_2_offset.h). Which segment answers is the point.
MAILBOX = [("MP0", "C2PMSG_64", 0x0080), ("MP0", "C2PMSG_81", 0x0091), ("MP1", "C2PMSG_90", 0x029a)]

# mmhub/mmhub_3_0_1_offset.h, BASE_IDX 1 (what PhxBringup::readFbLayout reads at fixed 0x1A8xx).
MMHUB = [("MMMC_VM_FB_LOCATION_BASE", 0x08ec, 1), ("MMMC_VM_FB_LOCATION_TOP", 0x08ed, 1),
         ("MMMC_VM_FB_OFFSET", 0x08d7, 1)]


def bases(disc, ip):
    p = os.path.join(disc, "die", "0", ip, "0", "base_addr")
    with open(p) as f:
        return [int(l, 16) for l in f.read().split()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bdf", required=True, help="PCI address of the iGPU, e.g. 0000:0e:00.0")
    ap.add_argument("--disc", required=True, help="saved ip_discovery directory from `phase0.sh driver`")
    a = ap.parse_args()

    dev = "/sys/bus/pci/devices/" + a.bdf
    if os.path.exists(dev + "/driver"):
        sys.exit("a driver is bound to %s (%s): boot with modprobe.blacklist=amdgpu" %
                 (a.bdf, os.path.basename(os.readlink(dev + "/driver"))))
    with open(dev + "/config", "rb") as f:
        cfg = f.read(8)
    vid, did, cmd = struct.unpack_from("<HHH", cfg, 0)
    print("pci %s %04x:%04x command 0x%04x" % (a.bdf, vid, did, cmd))
    if vid != 0x1002 or not (cmd & 2):
        sys.exit("not an AMD device, or memory decoding is off: refusing to read")

    seg = {ip: bases(a.disc, ip) for ip in ("DMU", "MP0", "MP1", "MMHUB")}
    for ip, b in seg.items():
        print("%-5s bases %s" % (ip, " ".join("0x%x" % x for x in b)))

    # Mapped read-write only because ctypes needs a writable buffer; nothing is ever stored through it.
    # ctypes reads each register with one aligned 32-bit load (struct and slicing may read byte by byte).
    fd = os.open(dev + "/resource5", os.O_RDWR | os.O_SYNC)
    size = os.fstat(fd).st_size
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)

    def rd(dw):
        if dw * 4 + 4 > size:
            return None
        return ctypes.c_uint32.from_buffer(mm, dw * 4).value

    def show(name, dw):
        v = rd(dw)
        print("  %-46s [0x%05x] = %s" % (name, dw, "out of BAR" if v is None else "0x%08x" % v))

    print("\n# DCN 3.1.4 (dcn_3_1_4_offset.h), BAR5 size 0x%x" % size)
    for name, off, idx in DCN314:
        show(name, seg["DMU"][idx] + off)
    print("\n# PSP/SMU mailboxes at every segment (13.0.4: BASE_IDX 1, 13.0.11: BASE_IDX 0)")
    for ip, name, off in MAILBOX:
        for i, b in enumerate(seg[ip]):
            show("%s seg%d %s" % (ip, i, name), b + off)
    print("\n# MMHUB FB layout (mmhub_3_0_1_offset.h)")
    for name, off, idx in MMHUB:
        show(name, seg["MMHUB"][idx] + off)
    try:
        mm.close()
    except BufferError:
        pass
    os.close(fd)


if __name__ == "__main__":
    main()
