// tests/test_phx_dcn_pipe.cpp: phxdcn::pipe (src/phxaccel/accel/phx_dcn_pipe.h) on the display registers the Ryzen 5
// 8500G's GOP left (hardware/dumps/b650m_8500g/gop_regs.txt, absolute BAR5 dword addresses).
#include "phx_dcn_pipe.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

using Regs = std::map<uint32_t, uint32_t>;

// lines like "  DIG0_DIG_BE_CNTL   [0x05571] = 0x00020100"
static Regs load(const char *path) {
    Regs r;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t lb = line.find("[0x"), eq = line.find("] = 0x");
        if (lb == std::string::npos || eq == std::string::npos) continue;
        r[(uint32_t)strtoul(line.c_str() + lb + 1, nullptr, 16)] = (uint32_t)strtoul(line.c_str() + eq + 4, nullptr, 16);
    }
    return r;
}

static phxdcn::Pipe run(const Regs &r) {
    return phxdcn::pipe([&r](uint32_t reg) {
        auto it = r.find(reg);
        return it == r.end() ? 0u : it->second;
    });
}

int main() {
    printf("=== Running DCN 3.1.4 GOP pipe tests (8500G dump) ===\n");
    using namespace phxdcn;
    // the addresses the dump names agree with dcn_3_1_4_offset.h + DMU segment 2
    CHECK(otgReg(0, kOtgControl) == 0x05001 && otgReg(1, kOtgControl) == 0x05081 && otgReg(0, kOtgHTotal) == 0x04fea);
    CHECK(digReg(0, kDigFeCntl) == 0x0554b && digReg(0, kDigBeCntl) == 0x05571 && digReg(4, kDigFeCntl) == 0x0594b);
    CHECK(kHighestReg == 0x5972);

    Regs r = load("hardware/dumps/b650m_8500g/gop_regs.txt");
    CHECK(r.size() > 100);
    CHECK(r[0x05001] == 0x00010301 && r[0x05571] == 0x00020100);

    // The dump has no DIG_BE_EN_CNTL. GOP drives the monitor through DIG0, so DIG_ENABLE is set there (Linux tells a
    // firmware-lit link by it, dcn10_is_dig_enabled); PhxAMDFB's log shows the live value.
    Regs on = r;
    on[digReg(0, kDigBeEnCntl)] = 0x101;
    Pipe p = run(on);
    CHECK(p.otg == 0 && p.dig == 0 && p.digMode == kModeDvi && p.external());
    printf("[PASS] test_8500g_gop (OTG0 -> DIG0, DVI: external display)\n");

    // DIG_ENABLE clear: no DIG found, the 780M's built-in behaviour stays
    p = run(r);
    CHECK(p.otg == 0 && p.dig < 0 && p.digMode == kModeNone && !p.external());
    printf("[PASS] test_no_enabled_dig (built-in panel kept)\n");

    // the same pipe in DP mode (eDP or DP: built-in kept) and HDMI mode (external)
    Regs dp = on;
    dp[digReg(0, kDigBeCntl)] = (0x00020100 & ~0x70000u) | (kModeDp << 16);
    p = run(dp);
    CHECK(p.dig == 0 && p.digMode == kModeDp && !p.external());
    Regs hdmi = on;
    hdmi[digReg(0, kDigBeCntl)] = (0x00020100 & ~0x70000u) | (kModeHdmi << 16);
    CHECK(run(hdmi).external());
    printf("[PASS] test_signal_modes (DP built-in, HDMI external)\n");

    // a DIG whose front end takes another OTG is not this pipe; no OTG running: nothing
    Regs other = on;
    other[digReg(0, kDigFeCntl)] = 0x01000001;   // front end A takes OTG1
    CHECK(run(other).dig < 0);
    Regs off = on;
    off[otgReg(0, kOtgControl)] = 0x00010300;
    CHECK(run(off).otg < 0 && !run(off).external());
    printf("[PASS] test_mismatches (other OTG, OTG stopped)\n");

    printf("=== All tests PASSED successfully ===\n");
    return 0;
}
