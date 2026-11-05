/*
 * testl5.cpp - Phase 3b Test L step 5 (docs/TEST_PHOENIX2.md), the pieces of Linux's mode set, each proven on the
 * running 60 Hz picture before the 144 Hz switch uses it.
 *
 *   testl5            read-only: which OPP feeds OTG0, the DPG state
 *   testl5 --blank    5a: blank the picture for 3 s the way Linux blanks an HDMI/DVI stream (dcn20_blank_pixel_data:
 *                     OPP0's display pattern generator, phx_dcn314_blank.h), unblank, check the DPG is back as saved
 *   testl5 --dpms     5b: Linux's DPMS off and on for the live DVI stream (link_set_dpms_off / _on,
 *                     phx_dcn314_dpms.h): encoders down, DMUB transmitter disable; 3 s; encoder setup, DMUB
 *                     transmitter enable at the same 148.5 MHz. The OTG keeps running. Needs the driver's DMCUB
 *                     (`testj.sh run` in this boot).
 *
 *   testl5 --modeset  5c: Linux's whole mode set for the live pipe, at the same 60 Hz DVI mode (phx_dcn314_modeset.h):
 *                     link down, OTG down, OTG up with the 60 Hz timing, link up, front end with Linux's 60 Hz values,
 *                     unblank. The pipe stays on Linux's values (step 2 proved them). Needs `testj.sh run` too.
 *
 * --blank: the screen goes black for 3 s. --dpms: the monitor loses the signal for 3 s and syncs again.
 * --modeset: the monitor loses the signal for a moment, as at any mode change.
 * PhxBringup debug selectors (-phxp2, phxdebug=1, root).
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl5 testl5.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn314_modeset.h"
#include "phx_dcn_i2c.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_WRITEREG 108
#define SEL_DBG_VRAM_READ 102
#define SEL_DBG_VRAM_WRITE 109

using namespace phxdcn314;
static io_connect_t conn;
static bool gWriteFailed;

static int open_kext(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!svc) { fprintf(stderr, "PhxBringup service not found\n"); return -1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), UC_TYPE, &conn);
    IOObjectRelease(svc);
    if (kr) { fprintf(stderr, "IOServiceOpen: 0x%08x\n", kr); return -1; }
    uint64_t in[2] = {1, 0}, out[4];
    uint32_t n = 4;
    kr = IOConnectCallScalarMethod(conn, SEL_HELLO, in, 2, out, &n);
    if (kr) { fprintf(stderr, "Hello: 0x%08x\n", kr); return -1; }
    return 0;
}

static uint32_t rd(void *, uint32_t reg) {
    uint64_t in[3] = {reg, 1, 0xFFFFFFFFull};
    uint32_t v = 0;
    size_t sz = 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
    if (kr) { fprintf(stderr, "read 0x%05x: kr 0x%08x\n", reg, kr); exit(1); }
    return v;
}
static void wr(void *, uint32_t reg, uint32_t val) {
    uint64_t in[2] = {reg, val}, out[1] = {0};
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_WRITEREG, in, 2, out, &n);
    if (kr && !gWriteFailed) {
        gWriteFailed = true;
        fprintf(stderr, "write 0x%05x = 0x%08x: kr 0x%08x%s\n", reg, val, kr,
                kr == (kern_return_t)0xe00002c7 ? " (unsupported: boot without -phxp2 / phxdebug=1?)" :
                kr == (kern_return_t)0xe00002c1 ? " (not privileged: run with sudo)" : "");
    }
}
static void delayUs(void *, unsigned us) { usleep(us); }
static uint32_t vrd(void *, uint64_t off) {
    uint64_t in[2] = {off, 1};
    uint32_t v = 0;
    size_t sz = 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, &v, &sz);
    if (kr) { fprintf(stderr, "vram read 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); exit(1); }
    return v;
}
static void vwr(void *, uint64_t off, uint32_t val) {
    uint64_t in[2] = {off, val};
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
    if (kr && !gWriteFailed) { gWriteFailed = true; fprintf(stderr, "vram write 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); }
}
static uint64_t prop64(const char *key) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    uint64_t v = 0;
    if (!svc) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef p = IORegistryEntryCreateCFProperty(svc, k, kCFAllocatorDefault, 0);
    if (p && CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)p, kCFNumberSInt64Type, &v);
    if (p) CFRelease(p);
    CFRelease(k);
    IOObjectRelease(svc);
    return v;
}
static uint32_t frames(void) { return fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount); }

// 5b: the registers DPMS off/on touches, compared before and after (status bits masked: FIFO reset done / error,
// SYMCLK on, Dolby metadata missed)
struct Snap { const char *name; uint32_t addr, mask; };
static const Snap kSnap[] = {
    {"DIG0_DIG_BE_CNTL", kDigBeMode.addr, 0xFFFFFFFFu},
    {"DIG0_DIG_BE_EN_CNTL", kDigBeEnable.addr, 0x00000001u},
    // DIG_STEREOSYNC_GATE_EN: enc1_setup_stereo_sync sets it to 1 for a stream without 3D, GOP leaves 0 (Test L 5b): it is
    // checked separately against Linux's value below
    {"DIG0_DIG_FE_CNTL", kDigFeSourceSel.addr, ~(kDigFeSymclkOn.mask | kDigFeDolbyMissed.mask | kDigFeStereoGate.mask)},
    {"DIG0_DIG_FIFO_CTRL0", kDigFifoEnable.addr, ~(kDigFifoResetDone.mask | kDigFifoError.mask)},
    {"OTG0_OTG_CONTROL", kOtgOutMux.addr, kOtgOutMux.mask},
    {"OTG_PIXEL_RATE_DIV", kOtg0RateDivK1.addr, kOtg0RateDivK1.mask | kOtg0RateDivK2.mask},
    {"DME0_DME_CONTROL", kDmeEngineEn.addr, kDmeEngineEn.mask},
    {"DIG0_HDMI_METADATA_PACKET_CONTROL", kHdmiMetaPktEn.addr, kHdmiMetaPktEn.mask},
    {"OTG0_PIXEL_RATE_CNTL", kPipe0DtoSrcSel.addr, kPipe0DtoSrcSel.mask},
    {"PHYPLLA_PIXCLK_RESYNC_CNTL", kPhyPllADeepColor.addr, kPhyPllADeepColor.mask},
};
static const unsigned kNSnap = sizeof(kSnap) / sizeof(kSnap[0]);

static int runDpms(Io io) {
    phxdmubinbox::Io dio{rd, wr, vrd, vwr, delayUs, nullptr};
    phxdmubinbox::Inbox ib;
    const uint64_t mcBase = prop64("phx-fb-mc-base");
    const bool rdy = phxdmubinbox::ready(dio);
    const phxdmubinbox::Status loc = mcBase ? phxdmubinbox::locate(dio, mcBase, 256ull << 20, &ib) : phxdmubinbox::Status::BadWindow;
    const uint32_t be = rd(nullptr, kDigBeMode.addr), fe = rd(nullptr, kDigFeSourceSel.addr);
    const unsigned digMode = fieldGet(be, kDigBeMode), feSrc = fieldGet(be, kDigBeFeSrc), digEn = fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable);
    const unsigned otgSel = fieldGet(fe, kDigFeSourceSel), vpgPwr = fieldGet(rd(nullptr, kVpgMemPwrState.addr), kVpgMemPwrState);
    printf("== 5b preconditions: DMCUB %s, inbox 1 %s; DIG BE0 mode %u (2 = DVI) enable %u FE source 0x%x; DIG FE0 from OTG%u; VPG0 mem power state %u\n",
           rdy ? "driver firmware" : "NOT the driver's", phxdmubinbox::name(loc), digMode, digEn, feSrc, otgSel, vpgPwr);
    if (!rdy || loc != phxdmubinbox::Status::Ok || digMode != kDigModeDvi || !digEn || !(feSrc & kDigFeSourceDigA) || otgSel != 0 || vpgPwr != 0) {
        fprintf(stderr, "testl5: not the state 5b is written for (run testj.sh run first?): nothing written\n");
        return 3;
    }
    uint32_t before[kNSnap];
    for (unsigned i = 0; i < kNSnap; i++) before[i] = rd(nullptr, kSnap[i].addr);
    const unsigned uf0 = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);

    signal(SIGINT, SIG_IGN);   // DPMS on below must run
    DpmsCtx c{io, dio, &ib, phxdmubcmd::Signal::Dvi, 148500};
    dpmsOff(c);
    printf("== DPMS off (link_set_dpms_off): DMUB %s, DIG_ENABLE now %u%s\n", phxdmubinbox::name(c.dmub),
           fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable), gWriteFailed ? ", A WRITE FAILED" : "");
    fflush(stdout);
    const uint32_t a0 = frames();
    sleep(3);
    const uint32_t a1 = frames();
    printf("  3 s off: OTG0 frames +%u (the SYMCLK workaround keeps it running: ~180 expected)\n", (a1 - a0) & 0xFFFFFF);
    gWriteFailed = false;
    dpmsOn(c);
    printf("== DPMS on (link_set_dpms_on): DMUB %s, DIG_ENABLE %u, FIFO reset timeouts %u%s\n", phxdmubinbox::name(c.dmub),
           fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable), c.fifoResetTimeouts, gWriteFailed ? ", A WRITE FAILED" : "");
    fflush(stdout);
    const uint32_t b0 = frames();
    sleep(2);
    const uint32_t b1 = frames();
    const unsigned uf = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    unsigned diff = 0;
    for (unsigned i = 0; i < kNSnap; i++) {
        const uint32_t now = rd(nullptr, kSnap[i].addr);
        const bool same = ((now ^ before[i]) & kSnap[i].mask) == 0;
        diff += !same;
        printf("  %-34s [0x%05x] before 0x%08x after 0x%08x %s\n", kSnap[i].name, kSnap[i].addr, before[i], now, same ? "same" : "DIFFERS");
    }
    const unsigned gate = fieldGet(rd(nullptr, kDigFeStereoGate.addr), kDigFeStereoGate);
    printf("  DIG_STEREOSYNC_GATE_EN %u (Linux's enc1_setup_stereo_sync, no 3D: 1)\n", gate);
    diff += gate != 1;
    const unsigned fifoErr = fieldGet(rd(nullptr, kDigFifoError.addr), kDigFifoError);
    printf("  2 s on: frames +%u, underflow %u (was %u), DIG_FIFO_ERROR %u (was %u)\n", (b1 - b0) & 0xFFFFFF, uf, uf0, fifoErr,
           fieldGet(before[3], kDigFifoError));
    const bool pass = c.dmub == phxdmubinbox::Status::Ok && !gWriteFailed && fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable) &&
                      ((a1 - a0) & 0xFFFFFF) > 100 && ((b1 - b0) & 0xFFFFFF) > 100 && !diff && !fifoErr;
    printf("RESULT: %s\n", pass ? "PASS (Linux's DPMS off / on for the DVI link works; the encoders are back as GOP had them)"
                                : "CHECK (see above; if the picture did not come back, restart)");
    return 0;
}

static const uint32_t kDpgRegs[7] = {kDpgEn.addr, kDpgWidth.addr, kDpgXOffset.addr, kDpgColB1.addr, kDpgColG1.addr,
                                     kDpgColR1.addr, kDpgRamp0.addr};
static const char *const kDpgNames[7] = {"DPG_CONTROL", "DPG_DIMENSIONS", "DPG_OFFSET_SEGMENT", "DPG_COLOUR_B_CB",
                                         "DPG_COLOUR_G_Y", "DPG_COLOUR_R_CR", "DPG_RAMP_CONTROL"};

// DDC1 for SCDC through the display engine's I2C (phx_dcn_i2c.h, dce_i2c_hw), from user space
struct UHw {
    uint32_t rd(uint32_t r) { return ::rd(nullptr, r); }
    void wr(uint32_t r, uint32_t v) { ::wr(nullptr, r, v); }
    void delayUs(uint32_t us) { usleep(us); }
};
static UHw gHw;
static uint32_t gScdcLastErr;
static bool scdcRead(void *, uint8_t off, uint8_t *v) {
    phxi2c::Ddc1<UHw> ddc(gHw);
    gScdcLastErr = ddc.readAt(0x54, off, v, 1);
    return gScdcLastErr == phxi2c::kOk;
}
static bool scdcWrite(void *, const uint8_t *d, unsigned n) {
    phxi2c::Ddc1<UHw> ddc(gHw);
    gScdcLastErr = ddc.write(0x54, d, n);
    return gScdcLastErr == phxi2c::kOk;
}

// target: 0 keep the signal GOP / the last run left, 2 DVI, 3 HDMI
static int runModeset(Io io, unsigned target) {
    phxdmubinbox::Io dio{rd, wr, vrd, vwr, delayUs, nullptr};
    phxdmubinbox::Inbox ib;
    const uint64_t mcBase = prop64("phx-fb-mc-base");
    const bool rdy = phxdmubinbox::ready(dio);
    const phxdmubinbox::Status loc = mcBase ? phxdmubinbox::locate(dio, mcBase, 256ull << 20, &ib) : phxdmubinbox::Status::BadWindow;
    const phxtiming::Mode m60 = {148500, 1920, 88, 44, 2200, 1080, 4, 5, 1125, true, true};   // EDID mode 0 (Test K)
    auto rd0 = [](uint32_t r) { return rd(nullptr, r); };
    const phxtiming::OtgRegs live = phxtiming::readOtg(rd0, 0);
    const bool sameTiming = !phxtiming::otgDiff(phxtiming::otgRegs(m60), live);
    uint32_t timer = 0;
    for (unsigned i = 0; i < kNumExtraFields; i++)
        if (!strcmp(kExtraFields[i].reg, "DCHUBBUB_GLOBAL_TIMER_CNTL")) timer = rd(nullptr, kExtraFields[i].addr);
    const uint32_t be = rd(nullptr, kDigBeMode.addr);
    const unsigned digMode = fieldGet(be, kDigBeMode), digEn = fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable);
    const unsigned vpgPwr = fieldGet(rd(nullptr, kVpgMemPwrState.addr), kVpgMemPwrState);
    phxi2c::Ddc1<UHw> ddc(gHw);
    const uint32_t ddcOk = ddc.check();
    const unsigned to = target ? target : digMode;
    printf("== 5c/5d preconditions: DMCUB %s, inbox 1 %s; OTG0 timing %s; DLG refclk %u kHz; DIG BE0 mode %u (2 DVI, 3 HDMI) enable %u;"
           " VPG0 %u; DDC1 %s; going to %s\n", rdy ? "driver firmware" : "NOT the driver's", phxdmubinbox::name(loc),
           sameTiming ? "= EDID mode 0 (60 Hz)" : "NOT the 60 Hz mode", dchubRefKhz(timer), digMode, digEn, vpgPwr,
           phxi2c::resultName(ddcOk), to == kDigModeHdmi ? "HDMI" : "DVI");
    if (!rdy || loc != phxdmubinbox::Status::Ok || !sameTiming || dchubRefKhz(timer) != 24000 ||
        (digMode != kDigModeDvi && digMode != kDigModeHdmi) || !digEn || vpgPwr || (to == kDigModeHdmi && ddcOk != phxi2c::kOk)) {
        fprintf(stderr, "testl5: not the state this is written for (run testj.sh run first?): nothing written\n");
        return 3;
    }
    const unsigned uf0 = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    signal(SIGINT, SIG_IGN);
    const phxdmubcmd::Signal from = digMode == kDigModeHdmi ? phxdmubcmd::Signal::Hdmi : phxdmubcmd::Signal::Dvi;
    DpmsCtx c{io, dio, &ib, from, m60.pixelKhz};
    const ScdcIo scdc{scdcRead, scdcWrite, nullptr};
    c.scdc = &scdc;
    c.avi = phxavi::avi(16, 0, m60.hTotal, m60.vTotal);   // CTA VIC 16, no picture aspect (phx_hdmi_avi.h)
    const ModesetResult r = modeset(c, m60, kDml60, to == kDigModeHdmi ? phxdmubcmd::Signal::Hdmi : phxdmubcmd::Signal::Dvi);
    if (from == phxdmubcmd::Signal::Hdmi)
        printf("== SCDC going down (legacy, 165 MHz): read %s, sink version %u, writes %s\n", c.scdcOff.readOk ? "ok" : "FAILED",
               c.scdcOff.sinkVersion, c.scdcOff.writeOk ? "ok" : "FAILED");
    if (to == kDigModeHdmi)
        printf("== HDMI up: SCDC read %s, sink version %u, writes %s, TMDS_CONFIG after %u (last I2C result %s); VPG %s;"
               " DIG_MODE %u; AVI InfoFrame %02x %02x %02x, sending %u on line %u\n", c.scdcOn.readOk ? "ok" : "FAILED",
               c.scdcOn.sinkVersion, c.scdcOn.writeOk ? "ok" : "FAILED", c.scdcTmdsAfterOn, phxi2c::resultName(gScdcLastErr),
               c.vpgIdle ? "idle" : "CONFLICT wait timed out", fieldGet(rd(nullptr, kDigBeMode.addr), kDigBeMode), c.avi.hb0, c.avi.hb1,
               c.avi.hb2, fieldGet(rd(nullptr, kHdmiGen0Send.addr), kHdmiGen0Send), fieldGet(rd(nullptr, kHdmiGen0Line.addr), kHdmiGen0Line));
    printf("== mode set (60 Hz %s -> %s, Linux's order): ", from == phxdmubcmd::Signal::Hdmi ? "HDMI" : "DVI",
           to == kDigModeHdmi ? "HDMI" : "DVI");
    printf("DMUB %s; OTG_BUSY cleared %s, OPTC clock on %s, OTG clock on %s%s;"
           " timing %s; front end %s; FIFO reset timeouts %u%s\n", phxdmubinbox::name(c.dmub), r.otg.busyCleared ? "yes" : "NO",
           r.otg.inClkOn ? "yes" : "NO", r.otg.otgClkOn ? "yes" : "NO", r.otg.underflowCleared ? " (an OTG underflow was cleared)" : "",
           r.timingOk ? "ok" : "BAD", r.frontEnd == Result::Ok ? "ok" : "NOT ok", c.fifoResetTimeouts, gWriteFailed ? ", A WRITE FAILED" : "");
    fflush(stdout);
    const uint32_t f0 = frames();
    sleep(3);
    const uint32_t f1 = frames();
    const phxtiming::OtgRegs after = phxtiming::readOtg(rd0, 0);
    const unsigned uf = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    const unsigned fifoErr = fieldGet(rd(nullptr, kDigFifoError.addr), kDigFifoError);
    const unsigned master = fieldGet(rd(nullptr, kOtgMasterEn.addr), kOtgMasterEn), vtg = fieldGet(rd(nullptr, kVtgEnable.addr), kVtgEnable);
    printf("  3 s after: frames +%u, OTG_MASTER_EN %u, VTG0_ENABLE %u, timing %s, underflow %u (was %u), DIG_FIFO_ERROR %u, DIG_ENABLE %u\n",
           (f1 - f0) & 0xFFFFFF, master, vtg, phxtiming::otgDiff(phxtiming::otgRegs(m60), after) ? "CHANGED" : "the 60 Hz mode", uf, uf0,
           fifoErr, fieldGet(rd(nullptr, kDigBeEnable.addr), kDigBeEnable));
    const unsigned modeNow = fieldGet(rd(nullptr, kDigBeMode.addr), kDigBeMode);
    // HDMI: the DIG in HDMI mode, the AVI InfoFrame sent every frame. SCDC is reported, not required: a sink without
    // SCDC NACKs it, and Linux ignores that (link_query_ddc_data's result is not checked).
    const bool hdmiOk = to != kDigModeHdmi || (modeNow == kDigModeHdmi && fieldGet(rd(nullptr, kHdmiGen0Send.addr), kHdmiGen0Send) == 1 && c.vpgIdle);
    const bool pass = c.dmub == phxdmubinbox::Status::Ok && r.otg.busyCleared && r.otg.inClkOn && r.otg.otgClkOn && r.timingOk &&
                      r.frontEnd == Result::Ok && !gWriteFailed && ((f1 - f0) & 0xFFFFFF) > 170 && master && vtg && !fifoErr && uf == uf0 &&
                      modeNow == to && hdmiOk;
    printf("RESULT: %s\n", pass ? (to == kDigModeHdmi ? "PASS (Linux's mode set brought the pipe up as HDMI at 60 Hz)"
                                                       : "PASS (Linux's mode set brought the pipe down and up again at 60 Hz as DVI)")
                                : "CHECK (see above; if the picture did not come back, restart)");
    return 0;
}

int main(int argc, char **argv) {
    const bool doBlank = argc > 1 && !strcmp(argv[1], "--blank");
    const bool doDpms = argc > 1 && !strcmp(argv[1], "--dpms");
    const bool doModeset = argc > 1 && !strcmp(argv[1], "--modeset");
    const bool doHdmi = argc > 1 && !strcmp(argv[1], "--hdmi"), doDvi = argc > 1 && !strcmp(argv[1], "--dvi");
    if (argc > 1 && !doBlank && !doDpms && !doModeset && !doHdmi && !doDvi) {
        fprintf(stderr, "usage: testl5 [--blank | --dpms | --modeset | --hdmi | --dvi]\n");
        return 2;
    }
    if (open_kext()) return 1;
    if (doDpms) return runDpms(Io{rd, wr, delayUs, nullptr});
    if (doModeset || doHdmi || doDvi) return runModeset(Io{rd, wr, delayUs, nullptr}, doHdmi ? kDigModeHdmi : doDvi ? kDigModeDvi : 0);
    Io io{rd, wr, delayUs, nullptr};

    const unsigned opp = fieldGet(rd(nullptr, kOdmSeg0Src.addr), kOdmSeg0Src);
    const uint32_t f0 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    usleep(100000);
    const uint32_t f1 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    uint32_t saved[7];
    printf("== OTG0 is fed by OPP%u (OPTC_SEG0_SRC_SEL), frames +%u in 100 ms; OPP0 DPG:\n", opp, (f1 - f0) & 0xFFFFFF);
    for (int i = 0; i < 7; i++) { saved[i] = rd(nullptr, kDpgRegs[i]); printf("  %-20s [0x%05x] 0x%08x\n", kDpgNames[i], kDpgRegs[i], saved[i]); }
    printf("  blanked now: %s\n", isBlanked(io) ? "yes" : "no");
    if (opp != 0 || f1 == f0) { fprintf(stderr, "testl5: OTG0 is not running from OPP0: nothing written\n"); return 3; }
    if (!doBlank) { printf("Read-only: nothing written. testl5 --blank blanks the picture for 3 s.\n"); return 0; }

    signal(SIGINT, SIG_IGN);   // the unblank below must run
    applyBlank(io, 1920, 1080);
    const bool b = isBlanked(io);
    printf("== 5a blank (dcn20_blank_pixel_data true): DPG_CONTROL 0x%08x, blanked %s%s\n", rd(nullptr, kDpgEn.addr),
           b ? "yes" : "NO", gWriteFailed ? ", A WRITE FAILED" : "");
    fflush(stdout);
    const uint32_t g0 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    sleep(3);
    const uint32_t g1 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    const unsigned uf = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    gWriteFailed = false;
    applyUnblank(io, 1920, 1080);
    unsigned diff = 0;
    for (int i = 0; i < 7; i++) {
        const uint32_t now = rd(nullptr, kDpgRegs[i]);
        if (now != saved[i]) { diff++; printf("  %-20s 0x%08x, saved 0x%08x: writing the saved word back\n", kDpgNames[i], now, saved[i]); wr(nullptr, kDpgRegs[i], saved[i]); }
    }
    printf("== unblank (dcn20_blank_pixel_data false): blanked %s; %u frames while blank, underflow %u; %u DPG registers differed from GOP's%s\n",
           isBlanked(io) ? "STILL" : "no", (g1 - g0) & 0xFFFFFF, uf, diff, gWriteFailed ? ", A WRITE FAILED" : "");
    const bool pass = b && !isBlanked(io) && ((g1 - g0) & 0xFFFFFF) > 120 && !gWriteFailed;
    printf("RESULT: %s\n", pass ? "PASS (Linux's HDMI blank / unblank works; the screen was black for 3 s)" : "CHECK (see above)");
    return 0;
}
