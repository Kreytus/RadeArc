// test_phx_dcn314_modeset.cpp: Test L 5c's same-mode re-set (phx_dcn314_modeset.h) over a fake display engine seeded
// with GOP's 60 Hz state: the OTG goes down after the link and comes up before it, the clocks are waited for, the
// timing registers end where Test K found them, VTG and OTG are enabled in optc314_enable_crtc's order, the DMUB
// commands come in Linux's order, the DIO FIFO resync copies the DISPCLK read divider, and the picture is unblanked.
#include "phx_dcn314_modeset.h"
#include <stdio.h>
#include <string.h>

using namespace phxdcn314;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Fake {
    uint32_t reg[0xA000] = {};
    uint32_t vram[8192 / 4] = {};
    uint8_t cmds[16][64]; unsigned ncmd = 0;
    struct Ev { uint32_t addr, val; } ev[4096]; unsigned nev = 0;
};
static Fake *gF;
static uint32_t frd(void *, uint32_t a) {
    uint32_t v = gF->reg[a];
    if (a == kOtgBusy.addr) v = fieldSet(fieldSet(v, kOtgBusy, 0), kOtgClockOn, fieldGet(v, kOtgClockEn));
    if (a == kOptcInClkOn.addr) v = fieldSet(v, kOptcInClkOn, fieldGet(v, kOptcInClkEn));
    if (a == kDigFifoResetDone.addr) v = fieldSet(v, kDigFifoResetDone, fieldGet(v, kDigFifoReset));
    if (a == kOtgLockStatus.addr) v = fieldSet(v, kOtgLockStatus, fieldGet(v, kOtgLock));
    if (a == kOtgVActiveDisp.addr) v |= kOtgVActiveDisp.mask;
    return v;
}
static void fwr(void *, uint32_t a, uint32_t v) {
    gF->reg[a] = v;
    if (gF->nev < 4096) gF->ev[gF->nev++] = {a, v};
    if (a == phxdmubinbox::kInbox1Wptr.addr) {
        const uint32_t prev = gF->reg[phxdmubinbox::kInbox1Rptr.addr];
        if (gF->ncmd < 16) memcpy(gF->cmds[gF->ncmd++], (uint8_t *)gF->vram + prev, 64);
        gF->reg[phxdmubinbox::kInbox1Rptr.addr] = v;
    }
}
static uint32_t fvr(void *, uint64_t off) { return gF->vram[(off - 0xd0f9900) / 4]; }
static void fvw(void *, uint64_t off, uint32_t v) { gF->vram[(off - 0xd0f9900) / 4] = v; }
static void fdelay(void *, unsigned) {}

int main() {
    Fake &f = *new Fake(); gF = &f;
    const phxtiming::Mode m60 = {148500, 1920, 88, 44, 2200, 1080, 4, 5, 1125, true, true};   // EDID mode 0 (Test K)
    const phxtiming::OtgRegs t = phxtiming::otgRegs(m60);
    const uint32_t S = 0x34c0;
    // GOP's state (Test K / testl / testl5 dumps)
    f.reg[S + phxtiming::kOtgHTotal] = t.hTotal; f.reg[S + phxtiming::kOtgHBlankStartEnd] = t.hBlankStartEnd;
    f.reg[S + phxtiming::kOtgHSyncA] = t.hSyncA; f.reg[S + phxtiming::kOtgVTotal] = t.vTotal;
    f.reg[S + phxtiming::kOtgVBlankStartEnd] = t.vBlankStartEnd; f.reg[S + phxtiming::kOtgVSyncA] = t.vSyncA;
    f.reg[kOtgMasterEn.addr] = 0x00010301;
    f.reg[kVtgEnable.addr] = 0x84610000;
    f.reg[kOtgClockEn.addr] = kOtgClockEn.mask | kOtgClockGateDis.mask;
    f.reg[kOptcInClkEn.addr] = kOptcInClkEn.mask | kOptcInClkGateDis.mask;
    f.reg[kDigBeMode.addr] = 0x00020100; f.reg[kDigBeEnable.addr] = 0x101;
    f.reg[kDigFeSourceSel.addr] = 0x01000000; f.reg[kDigFifoEnable.addr] = 0x1d;
    f.reg[kOtg0RateDivK1.addr] = 0xdb6;
    f.reg[kDentistDispRdiv.addr] = 0x00193a00;   // RDIVIDER 0x3a, WDIVIDER 0 (Test clk)
    f.reg[phxdmubinbox::kCw4Base.addr] = 0x04000000; f.reg[phxdmubinbox::kInbox1Base.addr] = 0x64000000;
    f.reg[phxdmubinbox::kInbox1Size.addr] = 8192;
    f.reg[phxdmubinbox::kCw4Offset.addr] = 0x0d0f9900; f.reg[phxdmubinbox::kCw4OffsetHigh.addr] = 0x80;
    f.reg[phxdmubinbox::kInbox1Wptr.addr] = f.reg[phxdmubinbox::kInbox1Rptr.addr] = 128;

    phxdmubinbox::Io dio{frd, fwr, fvr, fvw, fdelay, nullptr};
    phxdmubinbox::Inbox ib;
    CHECK(phxdmubinbox::locate(dio, 0x8000000000ull, 256ull << 20, &ib) == phxdmubinbox::Status::Ok);
    DpmsCtx c{Io{frd, fwr, fdelay, nullptr}, dio, &ib, phxdmubcmd::Signal::Dvi, 148500};

    const ModesetResult r = modeset(c, m60, kDml60);
    CHECK(r.otg.busyCleared && r.otg.inClkOn && r.otg.otgClkOn && r.timingOk && r.frontEnd == Result::Ok);
    CHECK(c.dmub == phxdmubinbox::Status::Ok);
    // DMUB: transmitter disable, SET_PIXEL_CLOCK (SYMCLK workaround), SET_PIXEL_CLOCK (enable_stream_timing),
    // encoder STREAM_SETUP, transmitter enable
    CHECK(f.ncmd == 5);
    const uint8_t want[5] = {phxdmubcmd::kSubDig1TransmitterControl, phxdmubcmd::kSubSetPixelClock, phxdmubcmd::kSubSetPixelClock,
                             phxdmubcmd::kSubDigxEncoderControl, phxdmubcmd::kSubDig1TransmitterControl};
    for (unsigned i = 0; i < 5 && i < f.ncmd; i++) CHECK(f.cmds[i][1] == want[i]);
    // OTG_MASTER_EN: off once, on once, in that order; VTG0_ENABLE 1 written before the master enable
    int off = -1, on = -1, vtgOn = -1;
    for (unsigned i = 0; i < f.nev; i++) {
        if (f.ev[i].addr == kOtgMasterEn.addr && !fieldGet(f.ev[i].val, kOtgMasterEn) && off < 0) off = (int)i;
        if (f.ev[i].addr == kOtgMasterEn.addr && fieldGet(f.ev[i].val, kOtgMasterEn) && off >= 0 && on < 0) on = (int)i;
        if (f.ev[i].addr == kVtgEnable.addr && fieldGet(f.ev[i].val, kVtgEnable) && on < 0) vtgOn = (int)i;
    }
    CHECK(off >= 0 && on > off && vtgOn > off && vtgOn < on);
    CHECK(fieldGet(f.reg[kOtgDisablePoint.addr], kOtgDisablePoint) == 2);
    // the timing ends as GOP had it (same mode); global sync and FP2 are Linux's 60 Hz (step 2)
    CHECK(f.reg[S + phxtiming::kOtgHTotal] == t.hTotal && f.reg[S + phxtiming::kOtgVTotal] == t.vTotal);
    CHECK((f.reg[S + phxtiming::kOtgHBlankStartEnd] & 0x7fff7fff) == t.hBlankStartEnd);
    CHECK((f.reg[S + phxtiming::kOtgVBlankStartEnd] & 0x7fff7fff) == t.vBlankStartEnd);
    CHECK(f.reg[0x05046] == 44 && fieldGet(f.reg[kVtgFp2.addr], kVtgFp2) == 2);
    CHECK(fieldGet(f.reg[kVtgEnable.addr], kVtgEnable) == 1);
    CHECK(f.reg[kOtgVTotalMin.addr] == 0 && f.reg[kOtgVTotalMax.addr] == 0);   // set_drr without DRR
    // clocks back on, DIO FIFO resync copied the read divider, unblanked, link up
    CHECK(fieldGet(f.reg[kOtgClockEn.addr], kOtgClockEn) == 1 && fieldGet(f.reg[kOptcInClkEn.addr], kOptcInClkEn) == 1);
    CHECK(fieldGet(f.reg[kDentistDispWdiv.addr], kDentistDispWdiv) == 0x3a);
    CHECK(!isBlanked(c.io));
    CHECK(fieldGet(f.reg[kDigBeFeSrc.addr], kDigBeFeSrc) == 1 && fieldGet(f.reg[kDigFifoEnable.addr], kDigFifoEnable) == 1);
    CHECK(frameWaitUs(m60) == 1125u * 14u);   // Linux's integer arithmetic: 2200 * 10000 / 1485000 = 14

    if (fails) { fprintf(stderr, "test_phx_dcn314_modeset: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_modeset: OK (%u register writes, %u DMUB commands)\n", f.nev, f.ncmd);
    return 0;
}
