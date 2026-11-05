/*
 * testl4.cpp - Phase 3b Test L step 4a (docs/TEST_PHOENIX2.md): the DMUB command path the HDMI link change will use.
 * Finds inbox 1 from the registers (src/amd/phx_dmub_inbox.h), checks that DMCUB runs the driver's firmware, prints
 * the four DMUB_CMD__VBIOS commands Linux sends to bring the link up at 144 Hz (phx_dmub_vbios_cmd.h, byte-for-byte
 * Linux's structs), and with --null sends one DMUB_CMD__NULL the way Linux sends any command.
 *
 *   testl4           read-only: DMCUB state, where inbox 1 is, the planned commands; nothing sent
 *   testl4 --null    also: one DMUB_CMD__NULL through inbox 1 (queue, read back, WPTR, wait for RPTR)
 *
 * DMCUB must run the driver's firmware (dal_fw 1): after `testj.sh run` in this boot. GOP's firmware (dal_fw 0) is
 * not what Linux talks to, so then nothing is sent. PhxBringup debug selectors (-phxp2, phxdebug=1, root).
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o testl4 testl4.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dmub_inbox.h"
#include "phx_dmub_vbios_cmd.h"

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
#define SEL_WRITEREG 108
#define SEL_DBG_VRAM_WRITE 109

using namespace phxdmubinbox;
static io_connect_t conn;
static bool gFailed;

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
    if (kr && !gFailed) { gFailed = true; fprintf(stderr, "write 0x%05x: kr 0x%08x (needs -phxp2, phxdebug=1, sudo)\n", reg, kr); }
}
static uint32_t vrd(void *, uint64_t off) {
    uint64_t in[2] = {off, 1};
    uint32_t v = 0;
    size_t sz = 4;
    kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, &v, &sz);
    if (kr) { fprintf(stderr, "vram read 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); exit(1); }
    return v;
}
static void vwr(void *, uint64_t off, uint32_t val) {
    uint64_t in[2] = {off, val};
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
    if (kr && !gFailed) { gFailed = true; fprintf(stderr, "vram write 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); }
}
static void delayUs(void *, unsigned us) { usleep(us); }

static void dump(const char *what, const uint8_t c[64]) {
    printf("  %s\n", what);
    for (int i = 0; i < 64; i += 16) {
        printf("    +%02x:", i);
        for (int j = 0; j < 16; j++) printf(" %02x", c[i + j]);
        printf("\n");
    }
}

int main(int argc, char **argv) {
    const bool sendNull = argc > 1 && !strcmp(argv[1], "--null");
    if (argc > 1 && !sendNull) { fprintf(stderr, "usage: testl4 [--null]\n"); return 2; }
    if (open_kext()) return 1;
    Io io{rd, wr, vrd, vwr, delayUs, nullptr};

    const uint64_t mcBase = prop64("phx-fb-mc-base");
    uint32_t s0 = 0;
    const bool rdy = ready(io, &s0);
    printf("== DMCUB: CNTL 0x%08x (enable %u), SCRATCH0 0x%08x (dal_fw %u, mailbox_rdy %u), frame buffer MC base 0x%llx\n",
           rd(nullptr, kDmcubEnable.addr), (rd(nullptr, kDmcubEnable.addr) & kDmcubEnable.mask) ? 1 : 0, s0, s0 & kBootDalFw,
           (s0 & kBootMailboxRdy) >> 1, (unsigned long long)mcBase);
    Inbox ib;
    const Status loc = mcBase ? locate(io, mcBase, 256ull << 20, &ib) : Status::BadWindow;
    printf("== inbox 1: base 0x%08x size %u, CW4 base 0x%08x -> MC 0x%llx, so VRAM 0x%llx; RPTR %u WPTR %u: %s\n", ib.base,
           ib.capacity, ib.cw4Base, (unsigned long long)ib.cw4Mc, (unsigned long long)ib.vramOff, ib.rptr, ib.wrpt, name(loc));

    // the 144 Hz HDMI link bring-up as Linux sends it (docs/PHASE3B_PLAN.md 4e): PLL, encoder setup, transmitter on
    using namespace phxdmubcmd;
    uint8_t c[64];
    printf("== the 144 Hz commands (DMUB_CMD__VBIOS, Linux v6.17 command_table2.c fallbacks; not sent here)\n");
    transmitterDisable(c, Signal::Dvi);              dump("DIG1_TRANSMITTER_CONTROL disable (the link GOP runs, DVI)", c);
    setPixelClock(c, Signal::Hdmi, 335770);          dump("SET_PIXEL_CLOCK 335770 kHz, PLL0, HDMI", c);
    encoderStreamSetup(c, Signal::Hdmi, 335770);     dump("DIGX_ENCODER_CONTROL STREAM_SETUP, HDMI", c);
    transmitterEnable(c, Signal::Hdmi, 335770);      dump("DIG1_TRANSMITTER_CONTROL enable, HDMI, 4 lanes", c);

    if (!rdy || loc != Status::Ok) {
        printf("RESULT: %s\n", !rdy ? "DMCUB runs GOP's firmware (dal_fw 0): run `bash testj.sh run` first, then this again"
                                    : "inbox 1 not usable (see above): nothing sent");
        return rdy ? 4 : 3;
    }
    if (!sendNull) { printf("RESULT: READY (the driver's DMCUB is up, inbox 1 found). testl4 --null sends a no-op.\n"); return 0; }

    uint8_t nul[64] = {0};   // DMUB_CMD__NULL: header type 0, payload 0
    const uint32_t w0 = ib.wrpt;
    const Status st = send(io, ib, nul);
    printf("== DMUB_CMD__NULL: %s (WPTR %u -> %u, RPTR %u)%s\n", name(st), w0, ib.wrpt, ib.rptr, gFailed ? ", A WRITE FAILED" : "");
    printf("RESULT: %s\n", st == Status::Ok && !gFailed ? "PASS (a command went through inbox 1 the way Linux sends one)" : "CHECK (see above)");
    return 0;
}
