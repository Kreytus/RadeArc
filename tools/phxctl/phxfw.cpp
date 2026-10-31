// phxfw: prints the firmware load plan for the GPU PhxBringup found, from the firmware files' own headers.
//
//   phxfw <fwdir>                      chip from PhxBringup's IP discovery table (ioreg phx-discovery-bin), macOS only
//   phxfw --disc <ip_discovery.bin> <fwdir>   chip from a saved table (tests, Linux)
//
// Output, one command per line, read by bringup5.sh:
//   CHIP <name> <gc prefix> <psp prefix> <sdma prefix>
//   TOC <file> <offset> <size>
//   L <file> <offset> <size> <psp fw type> <fw_buf offset>     (the arguments of `phxctl loadfw`)
// Exit status 1 (and nothing on stdout) when the chip is unknown or a file is missing or malformed: load nothing then.
//
// build (macOS, from tools/phxctl): clang++ -std=c++17 -O2 -I../../src/amd -o phxfw phxfw.cpp
//   ../../src/amd/rdna3_ip_discovery.cpp ../../src/amd/rdna3_chip_profile.cpp ../../src/amd/rdna3_ucode_extract.cpp
//   ../../src/amd/rdna3_fw_plan.cpp -framework IOKit -framework CoreFoundation
#include "rdna3_chip_profile.h"
#include "rdna3_fw_plan.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

using namespace rdna3;

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#ifdef __APPLE__
static std::vector<uint8_t> discovery_from_ioreg() {
    std::vector<uint8_t> out;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!svc) return out;
    CFTypeRef p = IORegistryEntryCreateCFProperty(svc, CFSTR("phx-discovery-bin"), kCFAllocatorDefault, 0);
    if (p && CFGetTypeID(p) == CFDataGetTypeID()) {
        const UInt8 *b = CFDataGetBytePtr((CFDataRef)p);
        out.assign(b, b + CFDataGetLength((CFDataRef)p));
    }
    if (p) CFRelease(p);
    IOObjectRelease(svc);
    return out;
}
#endif

int main(int argc, char **argv) {
    std::vector<uint8_t> disc;
    std::string dir;
    if (argc == 4 && !strcmp(argv[1], "--disc")) {
        disc = read_file(argv[2]);
        dir = argv[3];
    } else if (argc == 2) {
#ifdef __APPLE__
        disc = discovery_from_ioreg();
#endif
        dir = argv[1];
    } else {
        fprintf(stderr, "usage: phxfw <fwdir> | phxfw --disc <ip_discovery.bin> <fwdir>\n");
        return 2;
    }
    if (disc.empty()) {
        fprintf(stderr, "phxfw: no IP discovery table (PhxBringup not loaded, or no phx-discovery-bin)\n");
        return 1;
    }
    IpDiscoveryParser d;
    const ChipProfile *chip = d.init(disc.data(), disc.size()) ? chip_profile(d) : nullptr;
    char gc[32], psp[32], sdma[32];
    if (!chip || !fw_prefix(d, HwId::Gc, gc, sizeof gc) || !fw_prefix(d, HwId::Mp0, psp, sizeof psp) ||
        !fw_prefix(d, HwId::Sdma0, sdma, sizeof sdma)) {
        fprintf(stderr, "phxfw: unknown chip (IP versions not in rdna3_chip_profile): not loading anything\n");
        return 1;
    }
    if (!dir.empty() && dir.back() != '/') dir += '/';

    std::vector<uint8_t> data[kFwFileCount];
    const uint8_t *bins[kFwFileCount];
    uint64_t sizes[kFwFileCount];
    std::string names[kFwFileCount];
    for (size_t i = 0; i < kFwFileCount; i++) {
        char n[64];
        if (!fw_file_name(static_cast<FwFile>(i), gc, sdma, n, sizeof n)) return 1;
        names[i] = dir + n;
        data[i] = read_file(names[i]);
        if (data[i].empty()) {
            fprintf(stderr, "phxfw: missing %s\n", names[i].c_str());
            return 1;
        }
        bins[i] = data[i].data();
        sizes[i] = data[i].size();
    }
    FwLoad plan[kMaxFwLoads];
    const uint32_t n = fw_plan(bins, sizes, plan);
    const std::string toc_path = dir + psp + "_toc.bin";
    std::vector<uint8_t> toc = read_file(toc_path);
    uint32_t toc_off = 0, toc_len = 0;
    if (!n || toc.empty() || !toc_payload(toc.data(), toc.size(), toc_off, toc_len)) {
        fprintf(stderr, "phxfw: a firmware file is malformed or a payload does not fit (or %s is missing)\n",
                toc_path.c_str());
        return 1;
    }
    printf("CHIP %s %s %s %s\n", chip->chip == PhxChip::Phoenix2 ? "phoenix2" : "phoenix", gc, psp, sdma);
    printf("TOC %s %u %u\n", toc_path.c_str(), toc_off, toc_len);
    for (uint32_t i = 0; i < n; i++)
        printf("L %s %u %u %u 0x%06x\n", names[static_cast<size_t>(plan[i].file)].c_str(), plan[i].offset, plan[i].size,
               plan[i].fw_type, plan[i].dst);
    return 0;
}
