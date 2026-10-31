//
//  test_rdna3_fw_plan.cpp — the LOAD_IP_FW plan built from the linux-firmware headers.
//  Phoenix (gc_11_0_1): must equal the list upstream tools/phxctl/bringup5.sh hardcoded and ran on the 780M.
//  Phoenix 2 (gc_11_0_4): same payload types and order, offsets from its own headers.
//  Needs `make fixtures` (tests/fixtures/phoenix); skipped without it.
//
#include "rdna3_fw_plan.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace rdna3;

static const std::string kDir = "tests/fixtures/phoenix/";

static std::vector<uint8_t> read_file(const std::string &name) {
    std::ifstream f(kDir + name, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Files {
    std::vector<uint8_t> data[kFwFileCount];
    const uint8_t *bins[kFwFileCount];
    uint64_t sizes[kFwFileCount];
};

static bool load(const char *gc, const char *sdma, Files &fs) {
    for (size_t i = 0; i < kFwFileCount; i++) {
        char name[48];
        assert(fw_file_name(static_cast<FwFile>(i), gc, sdma, name, sizeof name));
        fs.data[i] = read_file(name);
        if (fs.data[i].empty()) {
            std::cout << "[SKIP] " << gc << ": " << name << " not in " << kDir << " (run `make fixtures`)" << std::endl;
            return false;
        }
        fs.bins[i] = fs.data[i].data();
        fs.sizes[i] = fs.data[i].size();
    }
    return true;
}

struct Want {
    FwFile file;
    uint32_t offset, size, fw_type, dst;
};

// tools/phxctl/bringup5.sh (upstream nullarc cbb08dd), the "L <file> <offset> <size> <type> <dst>" lines in order
static const Want kBringup5[] = {
    {FwFile::CpPfp, 256, 263168, 2, 0x000000},   {FwFile::CpMe, 256, 263168, 1, 0x100000},
    {FwFile::CpMec, 256, 267008, 4, 0x200000},   {FwFile::Sdma, 256, 17408, 71, 0x380000},
    {FwFile::Sdma, 17664, 16896, 72, 0x3C0000},  {FwFile::Mes, 256, 135648, 33, 0x400000},
    {FwFile::Mes, 135904, 131072, 34, 0x440000}, {FwFile::MesKiq, 256, 104272, 81, 0x480000},
    {FwFile::MesKiq, 104528, 131072, 82, 0x4C0000}, {FwFile::Imu, 256, 66048, 68, 0x500000},
    {FwFile::Imu, 66304, 66048, 69, 0x600000},   {FwFile::Rlc, 25344, 2560, 20, 0x700000},
    {FwFile::Rlc, 27904, 21104, 21, 0x720000},   {FwFile::Rlc, 49008, 66048, 26, 0x800000},
    {FwFile::Rlc, 115056, 33280, 48, 0x840000},  {FwFile::Rlc, 148336, 8704, 25, 0x880000},
    {FwFile::Rlc, 256, 25088, 8, 0x300000},
};
constexpr uint32_t kBringup5Count = sizeof kBringup5 / sizeof kBringup5[0];

static void test_names() {
    char n[48];
    assert(fw_file_name(FwFile::Mes, "gc_11_0_4", "sdma_6_0_1", n, sizeof n) && !strcmp(n, "gc_11_0_4_mes_2.bin"));
    assert(fw_file_name(FwFile::MesKiq, "gc_11_0_4", "sdma_6_0_1", n, sizeof n) && !strcmp(n, "gc_11_0_4_mes1.bin"));
    assert(fw_file_name(FwFile::Sdma, "gc_11_0_4", "sdma_6_0_1", n, sizeof n) && !strcmp(n, "sdma_6_0_1.bin"));
    char small[10];
    assert(!fw_file_name(FwFile::Rlc, "gc_11_0_4", "sdma_6_0_1", small, sizeof small));
    std::cout << "[PASS] test_names" << std::endl;
}

static void test_phoenix1_equals_bringup5() {
    Files fs;
    if (!load("gc_11_0_1", "sdma_6_0_1", fs)) return;
    FwLoad plan[kMaxFwLoads];
    const uint32_t n = fw_plan(fs.bins, fs.sizes, plan);
    assert(n == kBringup5Count);
    for (uint32_t i = 0; i < n; i++) {
        const Want &w = kBringup5[i];
        if (plan[i].file != w.file || plan[i].offset != w.offset || plan[i].size != w.size ||
            plan[i].fw_type != w.fw_type || plan[i].dst != w.dst) {
            std::cerr << "entry " << i << ": got type " << plan[i].fw_type << " off " << plan[i].offset << " size "
                      << plan[i].size << " dst 0x" << std::hex << plan[i].dst << std::endl;
            assert(false);
        }
    }
    auto toc = read_file("psp_13_0_4_toc.bin");
    uint32_t off = 0, len = 0;
    assert(!toc.empty() && toc_payload(toc.data(), toc.size(), off, len));
    assert(off == 256 && len == 2304);   // bringup5.sh: stage psp_13_0_4_toc.bin 256 2304, LOAD_TOC size 0x900
    std::cout << "[PASS] test_phoenix1_equals_bringup5 (17 payloads + TOC, byte for byte)" << std::endl;
}

static void test_phoenix2_from_headers() {
    Files fs;
    if (!load("gc_11_0_4", "sdma_6_0_1", fs)) return;
    FwLoad plan[kMaxFwLoads];
    const uint32_t n = fw_plan(fs.bins, fs.sizes, plan);
    assert(n == kBringup5Count);
    for (uint32_t i = 0; i < n; i++) {
        // same payload types, order and fw_buf slots as on Phoenix ...
        assert(plan[i].file == kBringup5[i].file && plan[i].fw_type == kBringup5[i].fw_type &&
               plan[i].dst == kBringup5[i].dst);
        // ... and every payload inside its file
        assert(plan[i].size && uint64_t(plan[i].offset) + plan[i].size <= fs.sizes[static_cast<size_t>(plan[i].file)]);
    }
    // gc_11_0_4_mes1.bin: mes_firmware_header_v1_0 says ucode 103600 B at 256, data 131072 B at 103856 (bringup5.sh's
    // gc_11_0_1 offsets 104272 / 104528 would load the wrong bytes)
    assert(plan[7].fw_type == 81 && plan[7].offset == 256 && plan[7].size == 103600);
    assert(plan[8].fw_type == 82 && plan[8].offset == 103856 && plan[8].size == 131072);
    auto toc = read_file("psp_13_0_11_toc.bin");
    uint32_t off = 0, len = 0;
    assert(!toc.empty() && toc_payload(toc.data(), toc.size(), off, len) && off == 256 && len == 2304);
    std::cout << "[PASS] test_phoenix2_from_headers (17 payloads + TOC from the gc_11_0_4 / psp_13_0_11 headers)"
              << std::endl;
}

static void test_refusals() {
    Files fs;
    if (!load("gc_11_0_4", "sdma_6_0_1", fs)) return;
    FwLoad plan[kMaxFwLoads];
    const uint8_t *bins[kFwFileCount];
    uint64_t sizes[kFwFileCount];
    memcpy(bins, fs.bins, sizeof bins);
    memcpy(sizes, fs.sizes, sizeof sizes);
    bins[static_cast<size_t>(FwFile::Imu)] = nullptr;          // a missing file
    assert(fw_plan(bins, sizes, plan) == 0);
    bins[static_cast<size_t>(FwFile::Imu)] = fs.bins[static_cast<size_t>(FwFile::Imu)];
    sizes[static_cast<size_t>(FwFile::MesKiq)] = 64;           // a truncated file
    assert(fw_plan(bins, sizes, plan) == 0);
    std::vector<uint8_t> big = fs.data[static_cast<size_t>(FwFile::MesKiq)];
    auto *h = reinterpret_cast<mes_firmware_header_v1_0 *>(big.data());
    h->mes_ucode_data_size_bytes = 0x40001;                    // larger than its fw_buf slot (and than the file)
    big.resize(h->mes_ucode_data_offset_bytes + 0x40001);
    sizes[static_cast<size_t>(FwFile::MesKiq)] = big.size();
    bins[static_cast<size_t>(FwFile::MesKiq)] = big.data();
    assert(fw_plan(bins, sizes, plan) == 0);
    uint32_t off, len;
    const uint8_t junk[16] = {0};
    assert(!toc_payload(junk, sizeof junk, off, len));
    std::cout << "[PASS] test_refusals (missing, truncated, oversized payloads give no plan)" << std::endl;
}

int main() {
    std::cout << "=== Running firmware plan tests (gc_11_0_1 / gc_11_0_4) ===" << std::endl;
    test_names();
    test_phoenix1_equals_bringup5();
    test_phoenix2_from_headers();
    test_refusals();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
