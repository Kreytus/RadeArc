/*
 * testl.cpp - Phase 3b Test L dry run (docs/PHASE3B_PLAN.md 4e, docs/TEST_PHOENIX2.md): read-only. Reads every HUBP0 /
 * OTG0 register the 144 Hz switch will write (the fields Linux programs from DML on DCN 3.1.4: hubp3_setup,
 * hubp2_setup_interdependent, optc1_program_global_sync) and prints, per field, the live value GOP left next to the
 * value Linux's DML gives for 60 Hz (GOP's mode) and 144 Hz, plus the register words the switch would write. Also the
 * DLG reference clock (DCHUBBUB_GLOBAL_TIMER_CNTL, which picks the 24 or 12 MHz plan), PHY PLL A's resync register and
 * which HUBPs are live.
 *
 *   testl
 *
 * Only PhxBringup's ReadRegs (selector 2) is used: nothing is written, no write gate is needed.
 * Register addresses and fields: src/amd/phx_dcn314_prog_regs.h, generated from Linux v6.17's dcn_3_1_4_offset.h /
 * dcn_3_1_4_sh_mask.h (tools/dml/gen_dcn314_regs.py). DML values: src/amd/phx_dml314_plans.h (tools/dml/gen_plans.sh).
 *
 * GOP is not Linux, so the 60 Hz column is a sanity check, not a pass/fail: the same order of magnitude in the timing
 * fields (refcyc_per_htotal, vstartup) says the field mapping and refclk are right.
 * Exit: 0 = done, 1 = error.
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl testl.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn314_plan.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2

static io_connect_t conn;

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

static uint32_t rd(uint32_t reg) {
    uint64_t in[3] = {reg, 1, 0xFFFFFFFFull};
    uint32_t v = 0;
    size_t sz = 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
    if (kr) { fprintf(stderr, "read 0x%05x: kr 0x%08x\n", reg, kr); exit(1); }
    return v;
}

int main(void) {
    using namespace phxdcn314;
    if (open_kext()) return 1;

    printf("== context (read-only)\n");
    const char *lastReg = "";
    uint32_t timerCntl = 0;
    for (unsigned i = 0; i < kNumExtraFields; i++) {
        const ExtraField &e = kExtraFields[i];
        const uint32_t v = rd(e.addr);
        if (strcmp(lastReg, e.reg)) { printf("  %-28s [0x%05x] = 0x%08x\n", e.reg, e.addr, v); lastReg = e.reg; }
        printf("      %-38s %u\n", e.name, (v & e.mask) >> e.shift);
        if (!strcmp(e.reg, "DCHUBBUB_GLOBAL_TIMER_CNTL")) timerCntl = v;
    }
    const unsigned refKhz = dchubRefKhz(timerCntl);
    printf("  DLG refclk (hubbub31_get_dchub_ref_freq): %u kHz -> DML plans for %s\n", refKhz,
           refKhz == 12000 ? "12 MHz (REFDIV 2)" : "24 MHz");
    const PhxDmlVal *p60 = planFor(60, refKhz), *p144 = planFor(144, refKhz);
    unsigned dc, dp, det, cb, ub;
    if (phxDmlGet(p144, "hubbub.det_kb", &det) && phxDmlGet(p144, "hubbub.compbuf_kb", &cb) && phxDmlGet(p144, "hubp.unbounded_req", &ub))
        // dcn31_program_det_size / dcn31_program_compbuf_size: segments = (KB + 63) / 64 (DCN31_CRB_SEGMENT_SIZE_KB)
        printf("  Linux plan (60 and 144 Hz alike): DET0 %u KB = %u segments, COMPBUF %u KB = %u segments, unbounded_req %u\n",
               det, (det + 63) / 64, cb, (cb + 63) / 64, ub);
    if (phxDmlGet(p144, "clk.dispclk_khz", &dc) && phxDmlGet(p144, "clk.dppclk_khz", &dp))
        printf("  144 Hz needs DISPCLK >= %u kHz, DPPCLK >= %u kHz (SMU SetDispclkFreq / SetDppclkFreq)\n", dc, dp);

    printf("== HUBP0 / OTG0: live (GOP, 60 Hz) | Linux DML 60 Hz | Linux DML 144 Hz\n");
    unsigned same = 0, total = 0, overflow = 0;
    for (unsigned r = 0; r < kNumProgRegs; r++) {
        const ProgReg &R = kProgRegs[r];
        const uint32_t live = rd(R.addr);
        uint32_t w60 = 0, w144 = 0;
        bool o60 = false, o144 = false;
        const bool ok60 = regWord(r, p60, live, &w60, &o60), ok144 = regWord(r, p144, live, &w144, &o144);
        printf("  %-26s [0x%05x] live 0x%08x | 60 %s0x%08x | 144 %s0x%08x  %s %s\n", R.name, R.addr, live,
               o60 ? "!" : "", ok60 ? w60 : 0, o144 ? "!" : "", ok144 ? w144 : 0,
               R.kind == kSet ? "SET" : R.kind == kUpdate ? "UPDATE" : "WRITE", R.linuxFn);
        overflow += o60 + o144;
        for (unsigned i = R.firstField; i < kNumProgFields && kProgFields[i].reg == r; i++) {
            const ProgField &f = kProgFields[i];
            unsigned v60 = 0, v144 = 0;
            fieldValue(f, p60, &v60);
            fieldValue(f, p144, &v144);
            const unsigned lv = (live & f.mask) >> f.shift;
            total++;
            same += lv == v60;
            printf("      %-34s %10u %c %10u   %10u   (%s)\n", f.name, lv, lv == v60 ? '=' : ' ', v60, v144, f.dml);
        }
    }
    printf("== summary: %u of %u fields equal Linux's 60 Hz plan; %u plan values spill out of their field (want 0)\n",
           same, total, overflow);
    printf("Nothing was written.\n");
    return 0;
}
