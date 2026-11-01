/*
 * modecheck.cpp - Phase 3b Test K (docs/PHASE3B_PLAN.md 7): read-only. Compares the OTG timing Linux would program
 * for the monitor's EDID modes (phx_dcn_timing.h, optc1_program_timing) with the live registers GOP left, measures the
 * real refresh from OTG_FRAME_COUNT, and dumps the display registers the 144/165/180 Hz switch will touch (DIG / HDMI
 * stream encoder, DCCG clocks, HUBBUB watermarks) with their Linux names, as the baseline for Tests L and M.
 *
 *   modecheck <edid.bin>     (the EDID from `phxctl edid <file>`)
 *
 * Only PhxBringup's ReadRegs (selector 2) is used: nothing is written, no -phxp2/phxdebug write gate is needed.
 * Exit: 0 = the live 60 Hz timing equals the computed one, 3 = it differs, 1/2 = error / usage.
 *
 * Registers: Linux drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_4_offset.h. DMU segment bases from IP discovery on
 * the 8500G (DMU 3.1.4 bases 0x12 0xc0 0x34c0): BASE_IDX 1 = 0xc0, BASE_IDX 2 = 0x34c0. Instance strides from the
 * same header: OTG n +0x80, DIG n +0x100, OTGn_PIXEL_RATE_CNTL +4 (0x80, 0x84), DPPCLKn_DTO_PARAM +1 (0x99, 0x9a),
 * PHYxSYMCLK_CLOCK_CNTL +1 (0x52, 0x53).
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -I../../src/phxaccel/accel -o modecheck modecheck.cpp \
 *        -framework IOKit -framework CoreFoundation
 */
#include "phx_dcn_pipe.h"
#include "phx_dcn_timing.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2

static const uint32_t S1 = 0xc0, S2 = 0x34c0;
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

static void dump(const char *name, uint32_t addr) { printf("  [0x%05x] = 0x%08x  %s\n", addr, rd(addr), name); }

static void printMode(int i, const phxtiming::Mode &m) {
    const uint32_t mhz = m.refreshMilliHz();
    printf("  mode %d: %ux%u @ %u.%03u Hz, pixel %u kHz, h %u/%u/%u total %u, v %u/%u/%u total %u, sync %c%c\n", i,
           m.hActive, m.vActive, mhz / 1000, mhz % 1000, m.pixelKhz, m.hActive, m.hFrontPorch, m.hSync, m.hTotal,
           m.vActive, m.vFrontPorch, m.vSync, m.vTotal, m.hSyncPositive ? '+' : '-', m.vSyncPositive ? '+' : '-');
}

int main(int argc, char **argv) {
    using namespace phxtiming;
    if (argc != 2) { fprintf(stderr, "usage: modecheck <edid.bin>\n"); return 2; }
    uint8_t edid[512] = {0};
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "modecheck: cannot open %s\n", argv[1]); return 1; }
    const uint32_t elen = (uint32_t)fread(edid, 1, sizeof edid, f);
    fclose(f);
    Mode modes[16];
    const int nm = modesFromEdid(edid, elen, modes, 16);
    printf("== EDID %s: %u bytes, %d detailed timings\n", argv[1], elen, nm);
    for (int i = 0; i < nm; i++) printMode(i, modes[i]);
    if (!nm) { fprintf(stderr, "modecheck: no detailed timings in the EDID\n"); return 1; }

    if (open_kext()) return 1;
    const phxdcn::Pipe p = phxdcn::pipe(rd);
    if (p.otg < 0 || p.dig < 0) { fprintf(stderr, "modecheck: no lit pipe found (otg %d dig %d)\n", p.otg, p.dig); return 1; }
    const uint32_t be = (uint32_t)p.dig;
    const uint32_t fe = (uint32_t)__builtin_ctz((rd(phxdcn::digReg(be, phxdcn::kDigBeCntl)) & 0x7f00) >> 8);
    const uint32_t otg = (uint32_t)p.otg;
    printf("== pipe: OTG%u -> DIG FE %u -> DIG BE %u, signal %s\n", otg, fe, be, phxdcn::modeName(p.digMode));

    // the live timing against every EDID mode
    const OtgRegs live = readOtg(rd, otg);
    int match = -1;
    for (int i = 0; i < nm && match < 0; i++)
        if (!otgDiff(otgRegs(modes[i]), live)) match = i;
    printf("== OTG%u timing (live vs optc1_program_timing)\n", otg);
    const int ref = match >= 0 ? match : 0;
    const OtgRegs want = otgRegs(modes[ref]);
    const uint32_t lv[8] = {live.hTotal, live.hBlankStartEnd, live.hSyncA, live.hSyncPol,
                            live.vTotal, live.vBlankStartEnd, live.vSyncA, live.vSyncPol};
    const uint32_t wv[8] = {want.hTotal, want.hBlankStartEnd, want.hSyncA, want.hSyncPol,
                            want.vTotal, want.vBlankStartEnd, want.vSyncA, want.vSyncPol};
    for (int i = 0; i < 8; i++)
        printf("  [0x%05x] %-22s live 0x%08x  computed 0x%08x  %s\n", S2 + kOtgFieldRegs[i] + otg * 0x80,
               kOtgFieldNames[i], lv[i], wv[i], lv[i] == wv[i] ? "ok" : "DIFF");
    if (match >= 0) printf("timing: MATCH mode %d (%u Hz)\n", match, (modes[match].refreshMilliHz() + 500) / 1000);
    else printf("timing: NO MATCH (compared with mode 0)\n");

    // the real refresh: OTG_FRAME_COUNT (bits 23:0) over ~1 s, as PhxAMDFB's measureRefresh
    const uint32_t fcReg = phxdcn::otgReg(otg, phxdcn::kOtgFrameCount);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint32_t c0 = rd(fcReg) & 0xffffff;
    const uint64_t t0 = mach_absolute_time();
    usleep(1000000);
    const uint32_t c1 = rd(fcReg) & 0xffffff;
    const uint64_t t1 = mach_absolute_time();
    const uint64_t ns = (t1 - t0) * tb.numer / tb.denom;
    const uint32_t frames = (c1 - c0) & 0xffffff;
    const uint32_t mhz = ns ? (uint32_t)((uint64_t)frames * 1000000000000ull / ns) : 0;
    printf("refresh: measured %u.%03u Hz (%u frames in %llu us), EDID mode %u.%03u Hz\n", mhz / 1000, mhz % 1000, frames,
           (unsigned long long)(ns / 1000), modes[ref].refreshMilliHz() / 1000, modes[ref].refreshMilliHz() % 1000);

    // baseline for Tests L/M: what the HDMI mode change touches (docs/PHASE3B_PLAN.md 2-3)
    char n[96];
    printf("== OTG%u other state\n", otg);
    const struct { const char *name; uint32_t reg; } otgX[] = {
        {"OTG_CONTROL", 0x1b41}, {"OTG_H_TIMING_CNTL", 0x1b2e}, {"OTG_V_TOTAL_MIN", 0x1b30},
        {"OTG_V_TOTAL_MAX", 0x1b31}, {"OTG_V_TOTAL_CONTROL", 0x1b33}, {"OTG_INTERLACE_CONTROL", 0x1b44},
        {"OTG_STATUS_POSITION", 0x1b4a}};
    for (auto &r : otgX) { snprintf(n, sizeof n, "OTG%u_%s", otg, r.name); dump(n, S2 + r.reg + otg * 0x80); }
    printf("== DIG FE %u (stream encoder) / BE %u (link encoder)\n", fe, be);
    const struct { const char *name; uint32_t reg; bool backEnd; } dig[] = {
        {"DIG_FE_CNTL", 0x208b, false}, {"DIG_CLOCK_PATTERN", 0x208e, false}, {"HDMI_CONTROL", 0x2094, false},
        {"HDMI_ACR_PACKET_CONTROL", 0x2097, false}, {"HDMI_VBI_PACKET_CONTROL", 0x2098, false},
        {"HDMI_INFOFRAME_CONTROL0", 0x2099, false}, {"HDMI_GC", 0x209e, false}, {"DIG_BE_CNTL", 0x20b1, true},
        {"DIG_BE_EN_CNTL", 0x20b2, true}, {"TMDS_CNTL", 0x20d8, true}};
    for (auto &r : dig) {
        const uint32_t i = r.backEnd ? be : fe;
        snprintf(n, sizeof n, "DIG%u_%s", i, r.name);
        dump(n, S2 + r.reg + i * 0x100);
    }
    printf("== DCCG\n");
    snprintf(n, sizeof n, "OTG%u_PIXEL_RATE_CNTL", otg); dump(n, S1 + 0x80 + otg * 4);
    dump("DPPCLK_DTO_CTRL", S1 + 0xb6);
    for (uint32_t i = 0; i < 4; i++) { snprintf(n, sizeof n, "DPPCLK%u_DTO_PARAM", i); dump(n, S1 + 0x99 + i); }
    dump("DCCG_GATE_DISABLE_CNTL", S1 + 0x74);
    dump("DCCG_GATE_DISABLE_CNTL2", S1 + 0x7c);
    dump("PHYASYMCLK_CLOCK_CNTL", S2 + 0x52);
    dump("PHYBSYMCLK_CLOCK_CNTL", S2 + 0x53);
    dump("HDMICHARCLK0_CLOCK_CNTL", S2 + 0x4a);
    dump("HDMISTREAMCLK_CNTL", S2 + 0x59);
    dump("DIO_CLK_CNTL", S2 + 0x1ee0);
    printf("== HUBBUB watermarks set A\n");
    dump("DCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A", S2 + 0x4fd);
    dump("DCHUBBUB_ARB_ALLOW_SR_ENTER_WATERMARK_A", S2 + 0x4ff);
    dump("DCHUBBUB_ARB_ALLOW_SR_EXIT_WATERMARK_A", S2 + 0x501);
    dump("DCHUBBUB_ARB_ALLOW_DRAM_CLK_CHANGE_WATERMARK_A", S2 + 0x503);
    dump("DCHUBBUB_ARB_FRAC_URG_BW_NOM_A", S2 + 0x504);

    // what Test L/M would program (nothing is written here)
    printf("== planned OTG%u values for the other EDID modes (not written)\n", otg);
    for (int i = 0; i < nm; i++) {
        if (i == ref) continue;
        const OtgRegs r = otgRegs(modes[i]);
        printf("  mode %d %u Hz: H_TOTAL 0x%x H_BLANK 0x%08x H_SYNC_A 0x%08x V_TOTAL 0x%x V_BLANK 0x%08x V_SYNC_A 0x%08x\n",
               i, (modes[i].refreshMilliHz() + 500) / 1000, r.hTotal, r.hBlankStartEnd, r.hSyncA, r.vTotal,
               r.vBlankStartEnd, r.vSyncA);
    }
    return match >= 0 ? 0 : 3;
}
