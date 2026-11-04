/*
 * testl2.cpp - Phase 3b Test L step 2 (docs/TEST_PHOENIX2.md): reprogram the live 60 Hz pipe (OTG0 / HUBP0) with
 * Linux's DML values for that same mode, in Linux's DCN 3.1.4 order (src/amd/phx_dcn314_step2.h), watch it, and put
 * every saved register back. No clock, PLL, PHY or timing change.
 *
 *   testl2            dry run: the planned writes next to the live values, nothing written
 *   testl2 --apply    write, watch for 15 s (HUBP0 underflow, OTG0 frame counter), restore, verify the restore
 *
 * Refuses unless: the DLG refclk is 24 MHz (the plan's), HUBP0 is live, VTG0_VCOUNT_INIT says the 1125-line mode
 * (1121) and OTG0's frame counter moves. Ctrl-C during the watch restores at once. Writes go through PhxBringup's
 * debug register write (selector 108: boot-arg -phxp2, phxdebug=1, root).
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl2 testl2.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn314_step2.h"

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

static const char *res(Result r) {
    return r == Result::Ok ? "ok" : r == Result::LockTimeout ? "LOCK TIMEOUT (nothing written)" :
           r == Result::VactiveTimeout ? "VACTIVE TIMEOUT (written anyway, as Linux does)" : "BAD PLAN";
}

int main(int argc, char **argv) {
    const bool doApply = argc > 1 && !strcmp(argv[1], "--apply");
    if (argc > 1 && !doApply) { fprintf(stderr, "usage: testl2 [--apply]\n"); return 2; }
    if (open_kext()) return 1;
    Io io{rd, wr, delayUs, nullptr};

    // preconditions: the plan is Linux's 60 Hz, 24 MHz refclk, for the pipe GOP lit
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
    printf("== preconditions: DLG refclk %u kHz, HUBP0 blank %u, VTG0_VCOUNT_INIT %u, OTG0 frames +%u in 100 ms\n",
           dchubRefKhz(timer), blank, vinit, (f1 - f0) & 0xFFFFFF);
    if (dchubRefKhz(timer) != 24000 || blank || vinit != 1121 || f1 == f0) {
        fprintf(stderr, "testl2: the live pipe is not the 60 Hz one this plan is for: nothing written\n");
        return 3;
    }

    const VTiming t60 = {1125, 4, 1080};   // EDID mode 0 (testk.txt), the one OTG0 runs
    const Plan pl = buildPlan(io, kDml60, t60);
    if (!pl.ok) { fprintf(stderr, "testl2: the plan could not be built: nothing written\n"); return 1; }
    const Saved sv = save(io, pl);
    printf("== plan: %u writes (Linux DML, 60 Hz), %u registers saved\n", pl.n, sv.n);
    for (unsigned i = 0; i < pl.n; i++) {
        uint32_t was = 0;
        for (unsigned j = 0; j < sv.n; j++) if (sv.w[j].addr == pl.w[i].addr) was = sv.w[j].val;
        printf("  %2u [0x%05x] 0x%08x -> 0x%08x%s\n", i, pl.w[i].addr, was, pl.w[i].val, i + 1 == pl.firstAfterVactive ? "   (then wait for VACTIVE)" : "");
    }
    if (!doApply) { printf("Dry run: nothing was written. testl2 --apply writes it for 15 s.\n"); return 0; }

    signal(SIGINT, onSig);
    signal(SIGTERM, onSig);
    const unsigned uf0 = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
    printf("== apply (underflow status before: %u)\n", uf0);
    fflush(stdout);
    const Result ra = applyPlan(io, pl);
    printf("  apply: %s%s\n", res(ra), gWriteFailed ? ", A WRITE FAILED" : "");
    if (ra == Result::LockTimeout) return 4;

    unsigned ufMax = uf0, frames = 0;
    uint32_t last = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
    for (unsigned t = 0; t < 30 && !gStop && !gWriteFailed; t++) {   // 30 x 0.5 s
        usleep(500000);
        const uint32_t fc = fieldGet(rd(nullptr, kOtgFrameCount.addr), kOtgFrameCount);
        const unsigned uf = fieldGet(rd(nullptr, kHubpUnderflow.addr), kHubpUnderflow);
        frames += (fc - last) & 0xFFFFFF; last = fc;
        if (uf > ufMax) ufMax = uf;
        if (t % 4 == 3) { printf("  %4.1f s: frames +%u, underflow status %u\n", (t + 1) / 2.0, frames, uf); fflush(stdout); }
    }
    printf("  watched: %u frames, underflow status max %u%s\n", frames, ufMax, gStop ? " (stopped early)" : "");

    gWriteFailed = false;
    const Result rr = restore(io, sv, pl.firstAfterVactive);
    unsigned bad = 0;
    for (unsigned i = 0; i < sv.n; i++) {
        const uint32_t now = rd(nullptr, sv.w[i].addr);
        // DCHUBP_CNTL carries live status bits (NO_OUTSTANDING_REQ, XRQ, timeouts): compare its control fields only
        // (control fields per dcn_3_1_4_sh_mask.h: BLANK_EN, SOFT_RESET, VTG_SEL, VREADY_AT_OR_AFTER_VSYNC,
        // DISABLE_STOP_DATA_DURING_VM, UNBOUNDED_REQ_MODE, TTU_DISABLE, TTU_MODE, TIMEOUT_THRESHOLD, TIMEOUT_INTERRUPT_EN)
        const uint32_t mask = sv.w[i].addr == kHubpVtgSel.addr ? 0x0B00F7F5u : 0xFFFFFFFFu;
        if ((now & mask) != (sv.w[i].val & mask)) { bad++; printf("  restore: [0x%05x] 0x%08x, saved 0x%08x\n", sv.w[i].addr, now, sv.w[i].val); }
    }
    printf("== restore: %s, %u of %u registers differ%s\n", res(rr), bad, sv.n, gWriteFailed ? ", A WRITE FAILED" : "");
    printf("RESULT: %s\n", ra == Result::Ok && frames > 600 && ufMax == uf0 && !bad ? "PASS (Linux's 60 Hz values ran 15 s without underflow)" :
           "CHECK (see above)");
    return 0;
}
