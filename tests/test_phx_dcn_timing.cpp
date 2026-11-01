// tests/test_phx_dcn_timing.cpp: phxtiming::otgRegs (src/amd/phx_dcn_timing.h, Linux optc1_program_timing) against the
// OTG0 registers GOP left on the 8500G (hardware/dumps/b650m_8500g/gop_regs.txt) for the mode in the MSI G255F's EDID
// (edid_msi_g255f.bin), then the 144/165/180 Hz values it computes from the same EDID.
#include "phx_dcn_timing.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

using namespace phxtiming;

static std::map<uint32_t, uint32_t> gop() {
    std::map<uint32_t, uint32_t> r;
    std::ifstream f("hardware/dumps/b650m_8500g/gop_regs.txt");
    std::string l;
    while (std::getline(f, l)) {
        const size_t a = l.find("[0x"), e = l.find("] = 0x");
        if (a != std::string::npos && e != std::string::npos)
            r[(uint32_t)strtoul(l.c_str() + a + 1, nullptr, 16)] = (uint32_t)strtoul(l.c_str() + e + 4, nullptr, 16);
    }
    return r;
}

int main() {
    printf("=== Running OTG timing tests (optc1_program_timing vs the 8500G) ===\n");
    std::ifstream f("hardware/dumps/b650m_8500g/edid_msi_g255f.bin", std::ios::binary);
    std::vector<uint8_t> e((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(e.size() == 256);

    // block 0's first DTD: the 1080p60 mode GOP set
    Mode m60{};
    CHECK(modeFromDtd(&e[54], &m60));
    CHECK(m60.hActive == 1920 && m60.vActive == 1080 && m60.hTotal == 2200 && m60.vTotal == 1125);
    CHECK(m60.refreshMilliHz() == 60000 && m60.hSyncPositive && m60.vSyncPositive);
    const OtgRegs r = otgRegs(m60);
    auto g = gop();
    const uint32_t S = 0x34c0;   // DMU segment 2
    CHECK(g.at(S + kOtgHTotal) == r.hTotal);                 // 0x897
    CHECK(g.at(S + kOtgHBlankStartEnd) == r.hBlankStartEnd); // 0x00c00840
    CHECK(g.at(S + kOtgHSyncA) == r.hSyncA);                 // 0x002c0000
    CHECK(g.at(S + kOtgVTotal) == r.vTotal);                 // 0x464
    CHECK(g.at(S + kOtgVBlankStartEnd) == r.vBlankStartEnd); // 0x00290461
    CHECK(g.at(S + kOtgVSyncA) == r.vSyncA);                 // 0x00050000
    printf("[PASS] test_gop_60hz (6 OTG0 registers equal what GOP programmed)\n");

    // the CEA block's DTDs: 144, 165, 180 Hz
    const uint32_t dtd = 128 + e[130];
    struct { uint32_t mhz, ht, hbse, hs, vt, vbse, vs; } want[3] = {
        { 144000, 2079, (112u << 16) | 2032, 32u << 16, 1120, (37u << 16) | 1117, 5u << 16 },
        { 165000, 2079, (112u << 16) | 2032, 32u << 16, 1120, (38u << 16) | 1118, 5u << 16 },
        { 180000, 2079, (112u << 16) | 2032, 32u << 16, 1109, (27u << 16) | 1107, 4u << 16 },
    };
    for (int i = 0; i < 3; i++) {
        Mode m{};
        CHECK(modeFromDtd(&e[dtd + 18 * i], &m));
        const uint32_t mhz = m.refreshMilliHz();
        CHECK(mhz > want[i].mhz - 100 && mhz < want[i].mhz + 100);
        const OtgRegs o = otgRegs(m);
        CHECK(o.hTotal == want[i].ht && o.hBlankStartEnd == want[i].hbse && o.hSyncA == want[i].hs);
        CHECK(o.vTotal == want[i].vt && o.vBlankStartEnd == want[i].vbse && o.vSyncA == want[i].vs);
        CHECK(o.hSyncPol == 0 && o.vSyncPol == 0);
        printf("[PASS] test_edid_%u_hz (clock %u kHz: H_TOTAL 0x%x H_BLANK 0x%08x V_TOTAL 0x%x V_BLANK 0x%08x V_SYNC 0x%08x)\n",
               want[i].mhz / 1000, m.pixelKhz, o.hTotal, o.hBlankStartEnd, o.vTotal, o.vBlankStartEnd, o.vSyncA);
    }
    printf("=== All tests PASSED successfully ===\n");
    return 0;
}
