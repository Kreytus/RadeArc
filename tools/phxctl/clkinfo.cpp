/*
 * clkinfo.cpp - Phase 3b (docs/PHASE3B_PLAN.md 4e, the DML inputs): the display clocks as DCN 3.1.4 runs them now,
 * and the SMU's DPM clock table, read the way Linux's dcn314 clock manager reads them. Nothing that drives the
 * picture is changed: the SMU only answers queries and copies its table into a free VRAM buffer.
 *
 *   clkinfo --dry-run   register reads only (DENTIST, the PLL, the SMU mailbox state); no SMU message, no write
 *   clkinfo             also: GetSmuVersion, GetDprefclkFreq, GetDtbclkFreq, GetFclkFrequency, and the DPM table
 *                       (SetVbiosDramAddrHigh/Low + TransferTableSmu2Dram TABLE_DPMCLOCKS)
 *
 * Sources (Linux v6.17):
 *  - clk_mgr/dcn314/dcn314_smu.c: the VBIOSSMC mailbox (MP1_SMN_C2PMSG_67 message, _83 argument, _91 response;
 *    dcn314_smu_wait_for_response: until _91 != 0, 10 us x 200000), message ids, MP1 segment 0 = 0x16000 (also the
 *    8500G's IP discovery: MP1 13.0.11 bases 0x00016000 ...); register offsets mp_13_0_5_offset.h
 *  - clk_mgr/dcn314/dcn314_clk_mgr.c: dcn314_get_dpm_table_from_smu (address high, low, transfer), the table in
 *    DC_MEM_ALLOC_TYPE_FRAME_BUFFER memory (so a VRAM MC address, as here); get_vco_frequency_from_reg
 *    (CLK1_CLK_PLL_REQ FbMult 8.16 x dfs_ref_freq_khz 48000; CLK_BASE seg 0 0x16C00 + 0x237)
 *  - clk_mgr/dcn314/dcn314_smu.h + pmfw_if/smu13_driver_if_v13_0_4.h: DpmClocks314_t, TABLE_DPMCLOCKS 4
 *  - clk_mgr/dce100/dce_clk_mgr.c dentist_get_divider_from_did + clk_mgr_internal.h ranges; DENTIST_DISPCLK_CNTL
 *    (dcn_3_1_4_offset.h 0x64, BASE_IDX 1 = 0xc0) WDIVIDER 6:0, DPPCLK_WDIVIDER 30:24
 *
 * build: clang++ -std=c++17 -O2 -o clkinfo clkinfo.cpp -framework IOKit -framework CoreFoundation
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_DBG_VRAM_READ 102
#define SEL_DBG_WRITE_REG 108
#define SEL_DBG_VRAM_WRITE 109

static const uint32_t MP1_C2PMSG_67 = 0x16000 + 0x283, MP1_C2PMSG_83 = 0x16000 + 0x293, MP1_C2PMSG_91 = 0x16000 + 0x29b;
static const uint32_t CLK1_CLK_PLL_REQ = 0x16C00 + 0x237;
static const uint32_t DENTIST_DISPCLK_CNTL = 0xc0 + 0x64;
// dcn314_smu.c
enum : uint32_t {
    MSG_GetSmuVersion = 0x2, MSG_GetFclkFrequency = 0xA, MSG_SetVbiosDramAddrHigh = 0xE, MSG_SetVbiosDramAddrLow = 0xF,
    MSG_TransferTableSmu2Dram = 0x10, MSG_GetDprefclkFreq = 0x13, MSG_GetDtbclkFreq = 0x14,
};
static const uint32_t TABLE_DPMCLOCKS = 4;
static const uint32_t RES_BUSY = 0x0, RES_OK = 0x1;

// DpmClocks314_t (8 levels each, NUM_DF_PSTATE_LEVELS 4)
struct DfPstate { uint32_t FClk, MemClk, Voltage; uint8_t WckRatio, Spare[3]; };
struct DpmClocks314 {
    uint32_t DcfClocks[8], DispClocks[8], DppClocks[8], SocClocks[8], VClocks[8], DClocks[8], SocVoltage[8];
    DfPstate DfPstateTable[4];
    uint8_t NumDcfClkLevelsEnabled, NumDispClkLevelsEnabled, NumSocClkLevelsEnabled, VcnClkLevelsEnabled,
        NumDfPstatesEnabled, spare[3];
    uint32_t MinGfxClk, MaxGfxClk;
};
static_assert(sizeof(DpmClocks314) == 304, "DpmClocks314_t layout");

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

static uint32_t rd(uint32_t reg) {
    uint64_t in[3] = {reg, 1, 0xFFFFFFFFull};
    uint32_t v = 0;
    size_t sz = 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, &v, &sz);
    if (kr) { fprintf(stderr, "read 0x%05x: kr 0x%08x\n", reg, kr); exit(1); }
    return v;
}

static void wr(uint32_t reg, uint32_t val) {
    uint64_t in[2] = {reg, val}, out[1] = {0};
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
    if (kr) { fprintf(stderr, "write 0x%05x: kr 0x%08x (needs -phxp2 and phxdebug=1)\n", reg, kr); exit(1); }
}

static void vram32(uint64_t off, uint32_t val) {
    uint64_t in[2] = {off, val};
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
    if (kr) { fprintf(stderr, "vram write 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); exit(1); }
}

static void vramRead(uint64_t off, uint32_t *buf, uint32_t count) {
    uint64_t in[2] = {off, count};
    size_t sz = count * 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, buf, &sz);
    if (kr) { fprintf(stderr, "vram read 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); exit(1); }
}

// dcn314_smu_wait_for_response: poll _91 until it is not BUSY, 10 us x 200000 (2 s)
static uint32_t waitResponse(void) {
    uint32_t r = RES_BUSY;
    for (int i = 0; i <= 200000; i++) {
        r = rd(MP1_C2PMSG_91);
        if (r != RES_BUSY) break;
        usleep(10);
    }
    return r;
}

// dcn314_smu_send_msg_with_param; returns false (and the response) when the SMU does not answer OK
static bool smuMsg(uint32_t msg, uint32_t param, uint32_t *out, uint32_t *resp) {
    uint32_t r = waitResponse();
    if (r == RES_BUSY) { *resp = r; return false; }
    wr(MP1_C2PMSG_91, RES_BUSY);
    wr(MP1_C2PMSG_83, param);
    wr(MP1_C2PMSG_67, msg);
    r = waitResponse();
    *resp = r;
    if (r != RES_OK) {
        if (r == 0xFF) wr(MP1_C2PMSG_91, RES_OK);   // as Linux does after VBIOSSMC_Result_Failed
        return false;
    }
    *out = rd(MP1_C2PMSG_83);
    return true;
}

// dentist_get_divider_from_did (x4 fixed point: DENTIST_DIVIDER_RANGE_SCALE_FACTOR)
static int dividerFromDid(int did) {
    if (did < 0x08) did = 0x08;
    if (did > 0x7f) did = 0x7f;
    if (did < 0x40) return 8 + 1 * (did - 0x08);
    if (did < 0x60) return 64 + 2 * (did - 0x40);
    if (did < 0x7e) return 128 + 4 * (did - 0x60);
    return 248 + 264 * (did - 0x7e);
}

static void printLevels(const char *name, const uint32_t *v, unsigned n) {
    printf("  %-11s", name);
    for (unsigned i = 0; i < n; i++) printf(" %5u", v[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    const bool dry = argc > 1 && !strcmp(argv[1], "--dry-run");
    if (open_kext()) return 1;

    // the clocks now (get_vco_frequency_from_reg, dentist)
    const uint32_t pll = rd(CLK1_CLK_PLL_REQ), dentist = rd(DENTIST_DISPCLK_CNTL);
    const uint32_t fbInt = pll & 0x1ff, fbFrac = (pll >> 16) & 0xffff;
    const uint64_t vcoKhz = ((uint64_t)fbInt * 65536 + fbFrac) * 48000 / 65536;
    const int dispDid = dentist & 0x7f, dppDid = (dentist >> 24) & 0x7f;
    const uint64_t dispKhz = vcoKhz * 4 / dividerFromDid(dispDid), dppKhz = vcoKhz * 4 / dividerFromDid(dppDid);
    printf("== clocks now\n");
    printf("  CLK1_CLK_PLL_REQ [0x%05x] = 0x%08x: FbMult %u + %u/65536 -> DENTIST VCO %llu kHz\n", CLK1_CLK_PLL_REQ, pll,
           fbInt, fbFrac, (unsigned long long)vcoKhz);
    printf("  DENTIST_DISPCLK_CNTL [0x%05x] = 0x%08x: DISPCLK DID 0x%02x -> %llu kHz, DPPCLK DID 0x%02x -> %llu kHz\n",
           DENTIST_DISPCLK_CNTL, dentist, dispDid, (unsigned long long)dispKhz, dppDid, (unsigned long long)dppKhz);
    printf("  VBIOSSMC mailbox: msg 0x%08x arg 0x%08x resp 0x%08x\n", rd(MP1_C2PMSG_67), rd(MP1_C2PMSG_83), rd(MP1_C2PMSG_91));
    if (dry) { printf("dry run: no SMU message sent\n"); return 0; }

    printf("== SMU queries\n");
    struct { const char *name; uint32_t msg; } q[] = {{"GetSmuVersion", MSG_GetSmuVersion},
                                                      {"GetDprefclkFreq (MHz)", MSG_GetDprefclkFreq},
                                                      {"GetDtbclkFreq (MHz)", MSG_GetDtbclkFreq},
                                                      {"GetFclkFrequency (MHz)", MSG_GetFclkFrequency}};
    for (auto &m : q) {
        uint32_t v = 0, resp = 0;
        if (smuMsg(m.msg, 0, &v, &resp)) printf("  %-24s 0x%08x (%u)\n", m.name, v, v);
        else printf("  %-24s FAILED resp 0x%02x\n", m.name, resp);
    }

    // DPM table: a zeroed 304-byte buffer at VRAM 210 MiB (free: just past dmcubload's DMUB buffer at 208 MiB)
    const uint64_t mcbase = prop64("phx-fb-mc-base");
    if (!mcbase) { fprintf(stderr, "clkinfo: no phx-fb-mc-base\n"); return 1; }
    const uint64_t off = 0xD200000ull, mc = mcbase + off;
    const uint32_t dw = sizeof(DpmClocks314) / 4;
    for (uint32_t i = 0; i < dw; i++) vram32(off + 4 * i, 0);
    printf("== DPM table (TransferTableSmu2Dram TABLE_DPMCLOCKS -> VRAM 0x%llx = MC 0x%llx)\n", (unsigned long long)off,
           (unsigned long long)mc);
    uint32_t v = 0, resp = 0;
    if (!smuMsg(MSG_SetVbiosDramAddrHigh, (uint32_t)(mc >> 32), &v, &resp) ||
        !smuMsg(MSG_SetVbiosDramAddrLow, (uint32_t)mc, &v, &resp) ||
        !smuMsg(MSG_TransferTableSmu2Dram, TABLE_DPMCLOCKS, &v, &resp)) {
        printf("  FAILED resp 0x%02x\n", resp);
        return 1;
    }
    DpmClocks314 t;
    vramRead(off, reinterpret_cast<uint32_t *>(&t), dw);
    const uint32_t *raw = reinterpret_cast<const uint32_t *>(&t);
    for (uint32_t i = 0; i < dw; i++) printf("%s%08x", i % 8 ? " " : (i ? "\n  " : "  "), raw[i]);
    printf("\n");
    printf("  levels: dcf %u disp/dpp %u soc %u vcn %u df %u; gfx %u..%u MHz\n", t.NumDcfClkLevelsEnabled,
           t.NumDispClkLevelsEnabled, t.NumSocClkLevelsEnabled, t.VcnClkLevelsEnabled, t.NumDfPstatesEnabled,
           t.MinGfxClk, t.MaxGfxClk);
    printLevels("DcfClocks", t.DcfClocks, 8);
    printLevels("DispClocks", t.DispClocks, 8);
    printLevels("DppClocks", t.DppClocks, 8);
    printLevels("SocClocks", t.SocClocks, 8);
    printLevels("SocVoltage", t.SocVoltage, 8);
    for (int i = 0; i < 4; i++)
        printf("  DfPstate %d: FClk %u MemClk %u Voltage %u WckRatio %u\n", i, t.DfPstateTable[i].FClk,
               t.DfPstateTable[i].MemClk, t.DfPstateTable[i].Voltage, t.DfPstateTable[i].WckRatio);
    return 0;
}
