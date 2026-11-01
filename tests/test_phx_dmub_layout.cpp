// tests/test_phx_dmub_layout.cpp: phxdmub::plan (src/amd/phx_dmub_layout.h) on the real dcn_3_1_4_dmcub.bin and the
// 8500G's VBIOS (hardware/dumps/b650m_8500g/vbios.rom): the PSP payload, the metadata and the window layout Linux would
// compute (dmub_srv_calc_region_info), checked against the values worked out by hand from the source.
#include "phx_dmub_layout.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static std::vector<uint8_t> load(const char *p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
    printf("=== Running DMUB layout tests (dcn_3_1_4_dmcub.bin) ===\n");
    using namespace phxdmub;
    auto fw = load("tests/fixtures/phoenix/dcn_3_1_4_dmcub.bin");
    auto vbios = load("hardware/dumps/b650m_8500g/vbios.rom");
    if (fw.empty()) { printf("[SKIP] dcn_3_1_4_dmcub.bin missing (tools/fetch_test_firmware.sh)\n"); return 0; }
    CHECK(vbios.size() == 16896);
    Layout L{};
    CHECK(plan(fw.data(), (uint32_t)fw.size(), (uint32_t)vbios.size(), &L));
    // linux-firmware's current dcn_3_1_4_dmcub.bin is 0x08006500 (common header ucode_version); Ubuntu 25.10 loaded
    // 0x08005200 on the 8500G (firmware_info.txt). Same layout (dmcub_firmware_header_v1_0, combined metadata).
    const uint32_t ver = fw[16] | fw[17] << 8 | fw[18] << 16 | (uint32_t)fw[19] << 24;
    CHECK(ver == 0x08006500);
    CHECK(L.ucodeOffset == 256 && L.instConstBytes == 350368 && L.bssDataBytes == 0);
    CHECK(L.pspOffset == 256 && L.pspSize == 350368);                       // PSP payload: header to footer
    CHECK(L.instConstOffset == 512 && L.instConstSize == 350368 - 512);     // the code proper
    printf("[PASS] test_header (PSP payload 256+%u, code %u bytes)\n", L.pspSize, L.instConstSize);

    CHECK(L.hasMeta);   // combined metadata at the end of inst_const (no bss)
    CHECK(L.fwVersion == ver);
    printf("[PASS] test_meta (fw 0x%08x, dal_fw %u, fw_state %u, trace %u, shared %u)\n", L.fwVersion, L.metaDalFw,
           L.metaFwStateSize, L.metaTraceSize, L.metaSharedSize);
    CHECK(L.metaDalFw == 1);

    // dmub_srv_calc_regions_for_memory_type: bases 256-aligned, sizes 64-aligned, in window order
    for (uint32_t i = 0; i < kWindowCount; i++) {
        CHECK(L.regions[i].base % 256 == 0);
        if (i) CHECK(L.regions[i].base >= L.regions[i - 1].top);
    }
    CHECK(L.regions[kInstConst].base == 0 && L.regions[kInstConst].top == align(350368 - 512, 64));
    CHECK(L.regions[kStack].top - L.regions[kStack].base == 128 * 1024 + 512 * 1024);
    CHECK(L.regions[kBssData].top == L.regions[kBssData].base);
    CHECK(L.regions[kVbios].top - L.regions[kVbios].base == 16896);
    CHECK(L.regions[kMailbox].top - L.regions[kMailbox].base == 16384);
    CHECK(L.fbSize % 4096 == 0 && L.fbSize < 2 * 1024 * 1024);
    printf("[PASS] test_regions (DMUB buffer %u bytes)\n", L.fbSize);
    for (uint32_t i = 0; i < kWindowCount; i++)
        printf("    window %2u: 0x%06x..0x%06x\n", i, L.regions[i].base, L.regions[i].top);

    // windows for a buffer at VRAM offset 208 MiB (MC 0x80_0000_0000 + 0xD000000)
    const uint64_t mc = 0x8000000000ull + 0xD000000;
    Windows w = windows(L, mc);
    CHECK(w.cw3.offset == mc + L.regions[kVbios].base && w.cw3.base == 0x63000000 && w.cw3.top == 0x63000000 + 16896);
    CHECK(w.cw4.base == 0x64000000 && w.inbox1Base == 0x64000000 && w.inbox1Size == 8192);
    CHECK(w.region5Top == w.cw5.top - w.cw5.base - 1);
    printf("[PASS] test_windows (cw3 0x%llx -> 0x%08x..0x%08x, inbox1 0x%08x+%u)\n", (unsigned long long)w.cw3.offset,
           w.cw3.base, w.cw3.top, w.inbox1Base, w.inbox1Size);
    printf("=== All tests PASSED successfully ===\n");
    return 0;
}
