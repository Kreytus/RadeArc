// phx_dcn_pipe.h: which display pipe the firmware (GOP) lit on a DCN 3.1.4 display engine (Phoenix 1 and Phoenix 2),
// read-only. Shared by PhxAMDFB (phxfb.cpp) and tests/test_phx_dcn_pipe.cpp, which runs it on the Ryzen 5 8500G's dump.
//
// Registers: dword offsets in the DMU's segment 2, base 0x34c0 on both chips (IP discovery DMU 3.1.4 bases 0x12 0xc0
// 0x34c0: hardware/dumps/b650m_8500g/ip_discovery.txt), from Linux drivers/gpu/drm/amd/include/asic_reg/dcn/
// dcn_3_1_4_offset.h and dcn_3_1_4_sh_mask.h. Instances: OTG n at +0x80 * n (regOTG1_OTG_H_TOTAL 0x1baa =
// regOTG0_OTG_H_TOTAL 0x1b2a + 0x80), DIG n at +0x100 * n (regDIG1_DIG_FE_CNTL 0x218b = regDIG0_DIG_FE_CNTL 0x208b + 0x100).
#pragma once
#include <stdint.h>

namespace phxdcn {

static constexpr uint32_t kDmuSeg2 = 0x34c0;
static constexpr uint32_t kOtgHTotal = 0x1b2a, kOtgVTotal = 0x1b2f, kOtgControl = 0x1b41, kOtgFrameCount = 0x1b4c,
                          kOtgStride = 0x80, kOtgCount = 4;
static constexpr uint32_t kDigFeCntl = 0x208b, kDigBeCntl = 0x20b1, kDigBeEnCntl = 0x20b2, kDigStride = 0x100,
                          kDigCount = 5;
static inline uint32_t otgReg(uint32_t otg, uint32_t reg) { return kDmuSeg2 + reg + otg * kOtgStride; }
static inline uint32_t digReg(uint32_t dig, uint32_t reg) { return kDmuSeg2 + reg + dig * kDigStride; }
// the highest dword pipe() reads (DIG4's DIG_BE_EN_CNTL): the MMIO view must reach it
static constexpr uint32_t kHighestReg = kDmuSeg2 + kDigBeEnCntl + (kDigCount - 1) * kDigStride;

// DIG_BE_CNTL.DIG_MODE as Linux decodes it (dcn10_get_dig_mode, dc/dio/dcn10/dcn10_link_encoder.c)
enum DigMode : uint32_t { kModeNone = 0, kModeDp = 1, kModeDvi = 2, kModeHdmi = 3, kModeDpMst = 5 };

struct Pipe {
    int otg = -1, dig = -1;
    uint32_t digMode = kModeNone;
    // TMDS (DVI, HDMI) is always an external connector. DP may be eDP or an external DP port, which only the VBIOS
    // connector table tells: callers keep the built-in panel the 780M machines have.
    bool external() const { return digMode == kModeDvi || digMode == kModeHdmi; }
};

// rd(dword) reads one register. The running OTG: OTG_CONTROL.OTG_MASTER_EN (bit 0). Its DIG: the enabled back end
// (DIG_BE_EN_CNTL.DIG_ENABLE, bit 0: dcn10_is_dig_enabled) whose front end (DIG_BE_CNTL.DIG_FE_SOURCE_SELECT, bits 14:8,
// one bit per front end: dcn10_get_dig_frontend) takes that OTG (DIG_FE_CNTL.DIG_SOURCE_SELECT, bits 2:0); its
// DIG_MODE (bits 18:16) is the signal.
template <typename Rd> Pipe pipe(Rd rd) {
    Pipe p;
    for (uint32_t o = 0; o < kOtgCount && p.otg < 0; o++)
        if (rd(otgReg(o, kOtgControl)) & 0x1) p.otg = (int)o;
    if (p.otg < 0) return p;
    for (uint32_t d = 0; d < kDigCount; d++) {
        if (!(rd(digReg(d, kDigBeEnCntl)) & 0x1)) continue;
        const uint32_t be = rd(digReg(d, kDigBeCntl));
        const uint32_t feMask = (be & 0x7f00) >> 8;
        if (!feMask || (feMask & (feMask - 1))) continue;   // exactly one front end
        const uint32_t fe = (uint32_t)__builtin_ctz(feMask);
        if (fe >= kDigCount || (rd(digReg(fe, kDigFeCntl)) & 0x7) != (uint32_t)p.otg) continue;
        p.dig = (int)d;
        p.digMode = (be & 0x70000) >> 16;
        break;
    }
    return p;
}

static inline const char *modeName(uint32_t m) {
    switch (m) {
    case kModeDp: return "DP";
    case kModeDvi: return "DVI";
    case kModeHdmi: return "HDMI";
    case kModeDpMst: return "DP MST";
    default: return "?";
    }
}

} // namespace phxdcn
