/*
 * testl3.cpp - Phase 3b Test L step 3 (docs/TEST_PHOENIX2.md): raise DISPCLK and DPPCLK to Linux's DML values for
 * 144 Hz while the panel keeps running 60 Hz, as Linux's clock manager raises them before a mode switch
 * (src/amd/phx_dcn314_clk.h), watch for 15 s, and come back down. Nothing else changes.
 *
 *   testl3            dry run: the clocks now and the SMU messages it would send; nothing sent or written
 *   testl3 --apply    raise, watch 15 s (OTG0 frames, HUBP0 underflow, the DISPCLK divider), lower, check
 *
 * Coming down: DISPCLK to GOP's (read from DENTIST_DISPCLK_RDIVIDER), the DPP DTO registers as saved, DPPCLK to Linux's
 * 60 Hz DML value. GOP's DPPCLK can't be read back on DCN 3.1.4 (DENTIST_DPPCLK_WDIVIDER reads 0 and there is no DPPCLK
 * read divider), and Test L step 2 ran this pipe at Linux's 60 Hz DPPCLK requirement without underflow.
 * Refuses unless: the DLG refclk is 24 MHz, HUBP0 is live, VTG0_VCOUNT_INIT is 1121, OTG0's frame counter moves and
 * the SMU mailbox is idle. Ctrl-C during the watch comes down at once. PhxBringup debug selectors (-phxp2, phxdebug=1,
 * root), as clkinfo and testl2.
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl3 testl3.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn314_clk.h"

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

using namespace phxdcn314;
static io_connect_t conn;
static volatile sig_atomic_t gStop;
static bool gWriteFailed;
// get_vco_frequency_from_reg (dcn314_clk_mgr.c): CLK1_CLK_PLL_REQ FbMult 8.16 x 48 MHz; CLK seg 0 0x16C00 + 0x237
static const uint32_t kClkPllReq = 0x16C00 + 0x237;

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
static void onSig(int) { gStop = 1; }

static unsigned vcoKhz() {
    const uint32_t pll = rd(nullptr, kClkPllReq);
    return (unsigned)(((uint64_t)(pll & 0x1ff) * 65536 + ((pll >> 16) & 0xffff)) * 48000 / 65536);
}
static unsigned dispclkNow(unsigned vco) { return dentistKhz(vco, fieldGet(rd(nullptr, kDentistDispRdiv.addr), kDentistDispRdiv)); }
static const char *resp(uint32_t r) { return r == kSmuOk ? "OK" : r == kSmuFailed ? "FAILED" : r == kSmuBusy ? "NO ANSWER" : "?"; }

int main(int argc, char **argv) {
    const bool doApply = argc > 1 && !strcmp(argv[1], "--apply");
    if (argc > 1 && !doApply) { fprintf(stderr, "usage: testl3 [--apply]\n"); return 2; }
    if (open_kext()) return 1;
    Io io{rd, wr, delayUs, nullptr};

    uint32_t timer = 0, blank = 1;
    for (unsigned i = 0; i < kNumExtraFields; i++) {
        if (!strcmp(kExtraFields[i].reg, "DCHUBBUB_GLOBAL_TIMER_CNTL")) timer = rd(nullptr, kExtraFields[i].addr);
        if (!strcmp(kExtraFields[i].reg, "HUBP0_DCHUBP_CNTL") && !strcmp(kExtraFields[i].name, "HUBP_BLANK_EN"))
            blank = (rd(nullptr, kExtraFields[i].addr) & kExtraFields[i].mask) >> kExtraFields[i].shift;
    }
    const unsigned vinit = fieldGet(rd(nullptr, kVtgVcountInit.addr), kVtgVcountInit);
    const uint32_t f0 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    usleep(100000);
    const uint32_t f1 = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    const uint32_t mbox = rd(nullptr, kSmuResp);
    printf("== preconditions: DLG refclk %u kHz, HUBP0 blank %u, VTG0_VCOUNT_INIT %u, OTG0 frames +%u in 100 ms, SMU response 0x%x\n",
           dchubRefKhz(timer), blank, vinit, (f1 - f0) & 0xFFFFFF, mbox);
    if (dchubRefKhz(timer) != 24000 || blank || vinit != 1121 || f1 == f0 || mbox == kSmuBusy) {
        fprintf(stderr, "testl3: not the 60 Hz pipe this is for, or the SMU is busy: nothing sent\n");
        return 3;
    }

    unsigned disp144 = 0, dpp144 = 0, dpp60 = 0;
    if (!phxDmlGet(kDml144, "clk.dispclk_khz", &disp144) || !phxDmlGet(kDml144, "clk.dppclk_khz", &dpp144) ||
        !phxDmlGet(kDml60, "clk.dppclk_khz", &dpp60)) { fprintf(stderr, "testl3: no DML clocks\n"); return 1; }
    const unsigned vco = vcoKhz(), dispGop = dispclkNow(vco);
    const uint32_t dtoParam = rd(nullptr, kDppDtoPhase0.addr), dtoCtrl = rd(nullptr, kDppDtoEnable0.addr);
    printf("== clocks now: DENTIST VCO %u kHz, DISPCLK %u kHz (RDIVIDER), DPPCLK0 DTO param 0x%08x ctrl 0x%08x (enable %u)\n",
           vco, dispGop, dtoParam, dtoCtrl, fieldGet(dtoCtrl, kDppDtoEnable0));
    printf("== up (dcn314_update_clocks, Linux DML 144 Hz):\n");
    printf("  SetDispclkFreq %u MHz (DISPCLK >= %u kHz)\n", khzToMhzCeil(disp144), disp144);
    printf("  SetDppclkFreq %u MHz (DPPCLK >= %u kHz)\n", khzToMhzCeil(dpp144), dpp144);
    printf("  DPPCLK0_DTO_PARAM phase %u modulo 255, DPPCLK0_DTO_ENABLE 1\n", dppDtoPhase(dpp144, dpp144));
    printf("== down: SetDispclkFreq %u MHz (GOP's DISPCLK), DTO registers as saved, SetDppclkFreq %u MHz (Linux 60 Hz)\n",
           khzToMhzCeil(dispGop), khzToMhzCeil(dpp60));
    if (!doApply) { printf("Dry run: nothing was sent or written. testl3 --apply raises the clocks for 15 s.\n"); return 0; }

    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);
    const unsigned uf0 = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    const ClkStep up = raise(io, disp144, dpp144, dpp144);
    printf("== up: SetDispclkFreq %s (%u MHz), SetDppclkFreq %s (%u MHz)%s; DISPCLK now %u kHz\n", resp(up.dispResp), up.dispMhz,
           up.dispResp == kSmuOk ? resp(up.dppResp) : "not sent", up.dppMhz, gWriteFailed ? ", A WRITE FAILED" : "",
           dispclkNow(vco));
    fflush(stdout);

    unsigned ufMax = uf0, frames = 0, dispMin = ~0u, dispMax = 0;
    uint32_t last = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    for (unsigned t = 0; t < 30 && !gStop && up.ok() && !gWriteFailed; t++) {
        usleep(500000);
        const uint32_t fc = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
        const unsigned uf = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow), d = dispclkNow(vco);
        frames += (fc - last) & 0xFFFFFF; last = fc;
        if (uf > ufMax) ufMax = uf;
        if (d < dispMin) dispMin = d;
        if (d > dispMax) dispMax = d;
        if (t % 4 == 3) { printf("  %4.1f s: frames +%u, underflow status %u, DISPCLK %u kHz\n", (t + 1) / 2.0, frames, uf, d); fflush(stdout); }
    }
    if (up.ok()) printf("  watched: %u frames, underflow status max %u, DISPCLK %u..%u kHz%s\n", frames, ufMax, dispMin, dispMax,
                        gStop ? " (stopped early)" : "");

    gWriteFailed = false;
    const ClkStep down = lower(io, dispGop, dpp60, dtoParam, dtoCtrl);
    const unsigned dispAfter = dispclkNow(vco);
    const bool dtoBack = rd(nullptr, kDppDtoPhase0.addr) == dtoParam && rd(nullptr, kDppDtoEnable0.addr) == dtoCtrl;
    printf("== down: SetDispclkFreq %s (%u MHz), SetDppclkFreq %s (%u MHz), DTO %s%s; DISPCLK now %u kHz (was %u)\n",
           resp(down.dispResp), down.dispMhz, resp(down.dppResp), down.dppMhz, dtoBack ? "as saved" : "DIFFERS",
           gWriteFailed ? ", A WRITE FAILED" : "", dispAfter, dispGop);
    const bool pass = up.ok() && down.ok() && frames > 600 && ufMax == uf0 && dispMin >= disp144 && dtoBack && !gWriteFailed;
    printf("RESULT: %s\n", pass ? "PASS (144 Hz clocks held 15 s at 60 Hz without underflow, and came back down)" : "CHECK (see above)");
    return 0;
}
