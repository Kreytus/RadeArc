/*
 * testl5.cpp - Phase 3b Test L step 5 (docs/TEST_PHOENIX2.md), the pieces of Linux's mode set, each proven on the
 * running 60 Hz picture before the 144 Hz switch uses it.
 *
 *   testl5            read-only: which OPP feeds OTG0, the DPG state
 *   testl5 --blank    5a: blank the picture for 3 s the way Linux blanks an HDMI/DVI stream (dcn20_blank_pixel_data:
 *                     OPP0's display pattern generator, phx_dcn314_blank.h), unblank, check the DPG is back as saved
 *
 * The screen goes black for 3 s, then the picture returns. PhxBringup debug selectors (-phxp2, phxdebug=1, root).
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl5 testl5.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn314_blank.h"

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

static const uint32_t kDpgRegs[7] = {kDpgEn.addr, kDpgWidth.addr, kDpgXOffset.addr, kDpgColB1.addr, kDpgColG1.addr,
                                     kDpgColR1.addr, kDpgRamp0.addr};
static const char *const kDpgNames[7] = {"DPG_CONTROL", "DPG_DIMENSIONS", "DPG_OFFSET_SEGMENT", "DPG_COLOUR_B_CB",
                                         "DPG_COLOUR_G_Y", "DPG_COLOUR_R_CR", "DPG_RAMP_CONTROL"};

int main(int argc, char **argv) {
    const bool doBlank = argc > 1 && !strcmp(argv[1], "--blank");
    if (argc > 1 && !doBlank) { fprintf(stderr, "usage: testl5 [--blank]\n"); return 2; }
    if (open_kext()) return 1;
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
