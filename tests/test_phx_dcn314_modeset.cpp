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

static const phxtiming::Mode m60 = {148500, 1920, 88, 44, 2200, 1080, 4, 5, 1125, true, true};   // EDID mode 0 (Test K)
static const uint32_t S = 0x34c0;

// GOP's state (Test K / testl / testl5 dumps) in a fresh fake engine
static Fake &freshGop() {
    Fake &f = *new Fake(); gF = &f;
    const phxtiming::OtgRegs t = phxtiming::otgRegs(m60);
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
    return f;
}

// a fake SCDC sink (SCDC version 1): records what is written, answers reads from its registers
static uint8_t gScdc[256];
static uint8_t gScdcWrites[8][2]; static unsigned gNScdcWrites;
static bool sRead(void *, uint8_t off, uint8_t *v) { *v = gScdc[off]; return true; }
static bool sWrite(void *, const uint8_t *d, unsigned n) {
    if (n == 2 && gNScdcWrites < 8) { gScdcWrites[gNScdcWrites][0] = d[0]; gScdcWrites[gNScdcWrites++][1] = d[1]; gScdc[d[0]] = d[1]; }
    return n == 2;
}

// 5d: GOP's DVI stream down, the same 60 Hz mode up as HDMI with the AVI InfoFrame and SCDC
static void testDviToHdmi() {
    Fake &f = freshGop();
    phxdmubinbox::Io dio{frd, fwr, fvr, fvw, fdelay, nullptr};
    phxdmubinbox::Inbox ib;
    CHECK(phxdmubinbox::locate(dio, 0x8000000000ull, 256ull << 20, &ib) == phxdmubinbox::Status::Ok);
    DpmsCtx c{Io{frd, fwr, fdelay, nullptr}, dio, &ib, phxdmubcmd::Signal::Dvi, 148500};
    gScdc[kScdcSinkVersion] = 1;
    const ScdcIo scdc{sRead, sWrite, nullptr};
    c.scdc = &scdc;
    c.avi = phxavi::avi(16, 0, 2200, 1125);
    const ModesetResult r = modeset(c, m60, kDml60, phxdmubcmd::Signal::Hdmi);
    CHECK(r.timingOk && r.frontEnd == Result::Ok && c.dmub == phxdmubinbox::Status::Ok && c.vpgIdle);
    // DMUB: the DVI transmitter off and its SYMCLK workaround as DVI, then everything as HDMI (ATOM mode 3)
    CHECK(f.ncmd == 5);
    if (f.ncmd == 5) {
        CHECK(f.cmds[0][1] == phxdmubcmd::kSubDig1TransmitterControl && f.cmds[0][4 + 2] == phxdmubcmd::kDigModeDvi);
        CHECK(f.cmds[1][1] == phxdmubcmd::kSubSetPixelClock && f.cmds[1][4 + 6] == phxdmubcmd::kDigModeDvi);
        CHECK(f.cmds[2][1] == phxdmubcmd::kSubSetPixelClock && f.cmds[2][4 + 6] == phxdmubcmd::kDigModeHdmi);
        CHECK(f.cmds[3][1] == phxdmubcmd::kSubDigxEncoderControl && f.cmds[3][4 + 2] == phxdmubcmd::kDigModeHdmi);
        CHECK(f.cmds[4][1] == phxdmubcmd::kSubDig1TransmitterControl && f.cmds[4][4 + 2] == phxdmubcmd::kDigModeHdmi);
    }
    CHECK(fieldGet(f.reg[kDigBeMode.addr], kDigBeMode) == kDigModeHdmi);
    // the HDMI engine: GEN_VERSION 1, KEEPOUT 1, NO_EXTRA_NULL 1, no scrambling below 340 MHz; GC every frame, not muted
    const uint32_t hc = f.reg[kHdmiPktGenVer.addr];
    CHECK(fieldGet(hc, kHdmiPktGenVer) == 1 && fieldGet(hc, kHdmiKeepout) == 1 && fieldGet(hc, kHdmiNoExtraNull) == 1 &&
          fieldGet(hc, kHdmiScramble) == 0 && fieldGet(hc, kHdmiClkChanRate) == 0 && fieldGet(hc, kHdmiDeepColorDepth) == 0);
    CHECK(fieldGet(f.reg[kHdmiGcCont.addr], kHdmiGcCont) == 1 && fieldGet(f.reg[kHdmiGcSend.addr], kHdmiGcSend) == 1);
    CHECK(fieldGet(f.reg[kHdmiGcAvmute.addr], kHdmiGcAvmute) == 0);
    // the AVI InfoFrame went into VPG packet 0 (header dword, then sb) and is sent every frame on line 2
    uint32_t vpg[9]; unsigned nv = 0;
    for (unsigned i = 0; i < f.nev && nv < 9; i++) if (f.ev[i].addr == kVpgData.addr) vpg[nv++] = f.ev[i].val;
    CHECK(nv == 9);
    CHECK(vpg[0] == (0x82u | 0x02u << 8 | 0x0du << 16));
    CHECK(vpg[1] == ((uint32_t)c.avi.sb[0] | (uint32_t)c.avi.sb[1] << 8 | (uint32_t)c.avi.sb[2] << 16 | (uint32_t)c.avi.sb[3] << 24));
    CHECK(fieldGet(f.reg[kHdmiGen0Cont.addr], kHdmiGen0Cont) == 1 && fieldGet(f.reg[kHdmiGen0Send.addr], kHdmiGen0Send) == 1);
    CHECK(fieldGet(f.reg[kHdmiGen0Line.addr], kHdmiGen0Line) == 2);
    CHECK(fieldGet(f.reg[kHdmiGen5Cont.addr], kHdmiGen5Cont) == 0);
    // SCDC on the way up: source version 1 (the sink is version 1), TMDS_CONFIG 0 below 340 MHz; none on the way down (DVI)
    CHECK(gNScdcWrites == 2 && gScdcWrites[0][0] == kScdcSourceVersion && gScdcWrites[0][1] == 1 &&
          gScdcWrites[1][0] == kScdcTmdsConfig && gScdcWrites[1][1] == 0);
    CHECK(c.scdcTmdsAfterOn == 0);
    CHECK(!isBlanked(c.io));
}

int main() {
    Fake &f = freshGop();
    const phxtiming::OtgRegs t = phxtiming::otgRegs(m60);
    phxdmubinbox::Io dio{frd, fwr, fvr, fvw, fdelay, nullptr};
    phxdmubinbox::Inbox ib;
    CHECK(phxdmubinbox::locate(dio, 0x8000000000ull, 256ull << 20, &ib) == phxdmubinbox::Status::Ok);
    DpmsCtx c{Io{frd, fwr, fdelay, nullptr}, dio, &ib, phxdmubcmd::Signal::Dvi, 148500};

    const ModesetResult r = modeset(c, m60, kDml60, phxdmubcmd::Signal::Dvi);
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

    testDviToHdmi();
    if (fails) { fprintf(stderr, "test_phx_dcn314_modeset: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_modeset: OK (%u register writes, %u DMUB commands)\n", f.nev, f.ncmd);
    return 0;
}
