//
//  test_rdna3_chip_profile.cpp — chip profiles against the real IP discovery tables of a Phoenix (Firebat U6C,
//  Radeon 780M) and a Phoenix 2 (Ryzen 5 8500G, Radeon 740M), both committed under hardware/dumps/.
//
#include "rdna3_chip_profile.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace rdna3;

static std::vector<uint8_t> read_dump(const char *path) {
    std::ifstream f(path, std::ios::binary);
    assert(f && "discovery dump missing: run make test from the repository root");
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void check_fw(const IpDiscoveryParser &d, HwId hw, const char *want) {
    char got[32];
    assert(fw_prefix(d, hw, got, sizeof got));
    if (strcmp(got, want) != 0) {
        std::cerr << "fw prefix " << got << " != " << want << std::endl;
        assert(false);
    }
}

static void test_phoenix1() {
    auto bin = read_dump("hardware/dumps/firebat_u6c/ip_discovery.bin");
    IpDiscoveryParser d;
    assert(d.init(bin.data(), bin.size()));
    const ChipProfile *p = chip_profile(d);
    assert(p && p->chip == PhxChip::Phoenix1);
    ChipRegs r;
    assert(chip_regs(d, *p, r));
    // MP0/MP1 base[1] = 0x16000: the absolute dwords upstream hardcodes in PhxNativeABI.h
    assert(r.psp_c2pmsg_64 == 0x16080 && r.psp_c2pmsg_67 == 0x16083 && r.psp_c2pmsg_81 == 0x16091);
    assert(r.psp_c2pmsg_69 == 0x16085 && r.psp_c2pmsg_70 == 0x16086 && r.psp_c2pmsg_71 == 0x16087);
    assert(r.psp_c2pmsg_58 == 0x1607a);
    assert(r.smu_msg == 0x16282 && r.smu_arg == 0x16292 && r.smu_resp == 0x1629a);
    assert(r.gb_addr_config == 0x263e);   // GC base[0] 0x1260 + 0x13de
    // the MMHUB FB and NBIO HDP-remap dwords upstream hardcodes (PhxNativeABI.h)
    assert(r.mmhub_fb_location_base == 0x1a8ec && r.mmhub_fb_location_top == 0x1a8ed && r.mmhub_fb_offset == 0x1a8d7);
    assert(r.nbio_remap_hdp_mem_flush_cntl == 0xe4d);
    check_fw(d, HwId::Gc, "gc_11_0_1");
    check_fw(d, HwId::Mp0, "psp_13_0_4");
    check_fw(d, HwId::Sdma0, "sdma_6_0_1");
    check_fw(d, HwId::Vcn, "vcn_4_0_2");
    assert(p->external_rev_add == 0x1);
    std::cout << "[PASS] test_phoenix1 (GC 11.0.1, MP 13.0.4, mailboxes at MP base[1])" << std::endl;
}

static void test_phoenix2() {
    auto bin = read_dump("hardware/dumps/b650m_8500g/ip_discovery.bin");
    IpDiscoveryParser d;
    assert(d.init(bin.data(), bin.size()));
    const ChipProfile *p = chip_profile(d);
    assert(p && p->chip == PhxChip::Phoenix2);
    ChipRegs r;
    assert(chip_regs(d, *p, r));
    // MP0/MP1 base[0] = 0x16000 on this chip (no 0x4000 segment), BASE_IDX 0: the same absolute dwords as Phoenix 1,
    // measured live on the 8500G (docs/PHOENIX2_FACTS.md: C2PMSG_81 0x001df3c3, SMU C2PMSG_90 = 1)
    assert(r.psp_c2pmsg_64 == 0x16080 && r.psp_c2pmsg_81 == 0x16091);
    assert(r.smu_msg == 0x16282 && r.smu_arg == 0x16292 && r.smu_resp == 0x1629a);
    assert(r.gb_addr_config == 0x263e);
    // read on the 8500G at these dwords: FB_LOCATION 0x8000..0x807f, FB_OFFSET 0x3e0 (docs/PHOENIX2_FACTS.md)
    assert(r.mmhub_fb_location_base == 0x1a8ec && r.mmhub_fb_location_top == 0x1a8ed && r.mmhub_fb_offset == 0x1a8d7);
    assert(r.nbio_remap_hdp_mem_flush_cntl == 0xe4d);   // NBIF 7.7.1: same header and bases as 7.7.0
    check_fw(d, HwId::Gc, "gc_11_0_4");
    check_fw(d, HwId::Mp0, "psp_13_0_11");
    check_fw(d, HwId::Sdma0, "sdma_6_0_1");
    check_fw(d, HwId::Vcn, "vcn_4_0_2");
    assert(p->external_rev_add == 0x80);   // rev_id 2 -> 0x82, as RADV reported on the 8500G
    std::cout << "[PASS] test_phoenix2 (GC 11.0.4, MP 13.0.11, mailboxes at MP base[0])" << std::endl;
}

// A GC version no profile lists must get no profile. Patch the Phoenix 2 table's GC entry (hw 11, inst 0, 3 bases,
// 11.0.4) to 11.0.3 and fix the binary checksum (16-bit byte sum from binary_size, discovery.h).
static void test_unknown_chip_refused() {
    auto bin = read_dump("hardware/dumps/b650m_8500g/ip_discovery.bin");
    const uint8_t gc[] = {0x0b, 0x00, 0x00, 0x03, 0x0b, 0x00, 0x04};
    size_t at = 0;
    for (size_t i = 0; i + sizeof gc <= bin.size(); i++)
        if (memcmp(&bin[i], gc, sizeof gc) == 0) { at = i; break; }
    assert(at);
    bin[at + 6] = 3;
    const size_t bin_size = bin[10] | (bin[11] << 8);
    uint16_t sum = 0;
    for (size_t i = 10; i < bin_size; i++) sum = static_cast<uint16_t>(sum + bin[i]);
    bin[8] = static_cast<uint8_t>(sum); bin[9] = static_cast<uint8_t>(sum >> 8);
    IpDiscoveryParser d;
    assert(d.init(bin.data(), bin.size()));
    IpEntry e;
    assert(d.find_ip(HwId::Gc, 0, e) && e.revision == 3);
    assert(chip_profile(d) == nullptr);
    IpDiscoveryParser empty;
    assert(chip_profile(empty) == nullptr);
    std::cout << "[PASS] test_unknown_chip_refused (GC 11.0.3 and an invalid table get no profile)" << std::endl;
}

static void test_fw_prefix_bounds() {
    auto bin = read_dump("hardware/dumps/b650m_8500g/ip_discovery.bin");
    IpDiscoveryParser d;
    assert(d.init(bin.data(), bin.size()));
    char small[11];   // "psp_13_0_11" needs 12 bytes
    assert(!fw_prefix(d, HwId::Mp0, small, sizeof small));
    char ok[12];
    assert(fw_prefix(d, HwId::Mp0, ok, sizeof ok) && strcmp(ok, "psp_13_0_11") == 0);
    assert(!fw_prefix(d, HwId::Dmu, ok, sizeof ok));   // no Linux prefix rule for DMU (dmcub is named in amdgpu_dm.c)
    std::cout << "[PASS] test_fw_prefix_bounds" << std::endl;
}

int main() {
    std::cout << "=== Running chip profile tests (Phoenix / Phoenix 2) ===" << std::endl;
    test_phoenix1();
    test_phoenix2();
    test_unknown_chip_refused();
    test_fw_prefix_bounds();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
