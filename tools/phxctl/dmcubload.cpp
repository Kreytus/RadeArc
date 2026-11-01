/*
 * dmcubload.cpp - load and start the driver's DMCUB firmware (dcn_3_1_4_dmcub.bin) the way Linux amdgpu does, through
 * PhxBringup's root-only debug calls (ReadRegs, WriteReg 108, VRAM write 109). Phase 3b, docs/PHASE3B_PLAN.md 4d.
 *
 *   dmcubload --dry-run <dmcub.bin> <vbios.rom>   read the DMCUB state and print every step; writes nothing
 *   dmcubload <dmcub.bin> <vbios.rom>             do it (the PSP part first: phxctl loadfw <dmcub.bin> 256 <size> 51 ...)
 *
 * Sequence (Linux v6.17): amdgpu_dm.c dm_dmub_hw_init -> dmub_srv_hw_reset + dmub_srv_hw_init, DCN 3.1 hooks in
 * dmub/src/dmub_dcn31.c (dmub_dcn31_reset, setup_windows, setup_outbox0, setup_mailbox, setup_out_mailbox,
 * enable_dmub_boot_options, skip_dmub_panel_power_sequence, reset_release), then dmub_srv_wait_for_auto_load.
 * The window layout is phx_dmub_layout.h (dmub_srv_calc_region_info). Registers: dcn_3_1_4_offset.h, DMU segment 2
 * (0x34c0), fields dcn_3_1_4_sh_mask.h. Every poll has a timeout (Linux's: 100000 x 1 us for the stop, 100 ms auto-load).
 *
 * If it fails: DMCUB stays in reset; the OTG/DIG (the picture) are not touched. A reboot gives GOP's DMCUB back.
 *
 * build: clang++ -std=c++17 -O2 -I../../src/amd -o dmcubload dmcubload.cpp -framework IOKit -framework CoreFoundation
 */
#include "phx_dmub_layout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_DBG_WRITE_REG 108
#define SEL_DBG_VRAM_WRITE 109

/* DMU segment 2 + dcn_3_1_4_offset.h */
#define S2 0x34c0u
#define DMCUB_REGION5_OFFSET (S2 + 0x198)
#define DMCUB_REGION5_OFFSET_HIGH (S2 + 0x199)
#define DMCUB_REGION5_TOP_ADDRESS (S2 + 0x1a2)
#define DMCUB_REGION3_CW3_BASE_ADDRESS (S2 + 0x1a8)
#define DMCUB_REGION3_CW4_BASE_ADDRESS (S2 + 0x1a9)
#define DMCUB_REGION3_CW5_BASE_ADDRESS (S2 + 0x1aa)
#define DMCUB_REGION3_CW6_BASE_ADDRESS (S2 + 0x1ab)
#define DMCUB_REGION3_CW3_TOP_ADDRESS (S2 + 0x1b0)
#define DMCUB_REGION3_CW4_TOP_ADDRESS (S2 + 0x1b1)
#define DMCUB_REGION3_CW5_TOP_ADDRESS (S2 + 0x1b2)
#define DMCUB_REGION3_CW6_TOP_ADDRESS (S2 + 0x1b3)
#define DMCUB_REGION3_CW3_OFFSET (S2 + 0x1bb)
#define DMCUB_REGION3_CW3_OFFSET_HIGH (S2 + 0x1bc)
#define DMCUB_REGION3_CW4_OFFSET (S2 + 0x1bd)
#define DMCUB_REGION3_CW4_OFFSET_HIGH (S2 + 0x1be)
#define DMCUB_REGION3_CW5_OFFSET (S2 + 0x1bf)
#define DMCUB_REGION3_CW5_OFFSET_HIGH (S2 + 0x1c0)
#define DMCUB_REGION3_CW6_OFFSET (S2 + 0x1c1)
#define DMCUB_REGION3_CW6_OFFSET_HIGH (S2 + 0x1c2)
#define DMCUB_INBOX1_BASE_ADDRESS (S2 + 0x1d4)
#define DMCUB_INBOX1_SIZE (S2 + 0x1d5)
#define DMCUB_INBOX1_WPTR (S2 + 0x1d6)
#define DMCUB_INBOX1_RPTR (S2 + 0x1d7)
#define DMCUB_OUTBOX0_BASE_ADDRESS (S2 + 0x1d8)
#define DMCUB_OUTBOX0_SIZE (S2 + 0x1d9)
#define DMCUB_OUTBOX0_WPTR (S2 + 0x1da)
#define DMCUB_OUTBOX0_RPTR (S2 + 0x1db)
#define DMCUB_OUTBOX1_BASE_ADDRESS (S2 + 0x1dc)
#define DMCUB_OUTBOX1_SIZE (S2 + 0x1dd)
#define DMCUB_OUTBOX1_WPTR (S2 + 0x1de)
#define DMCUB_OUTBOX1_RPTR (S2 + 0x1df)
#define DMCUB_SCRATCH0 (S2 + 0x1e3)
#define DMCUB_SCRATCH7 (S2 + 0x1ea)
#define DMCUB_SCRATCH14 (S2 + 0x1f1)
#define DMCUB_SCRATCH15 (S2 + 0x1f2)
#define DMCUB_CNTL (S2 + 0x1f6)
#define DMCUB_GPINT_DATAIN1 (S2 + 0x1f8)
#define DMCUB_CNTL2 (S2 + 0x200)
#define MMHUBBUB_SOFT_RESET (S2 + 0x342)

#define DMCUB_CNTL_ENABLE 0x00010000u          /* DMCUB_CNTL__DMCUB_ENABLE_MASK */
#define DMCUB_CNTL_TRACEPORT_EN 0x00080000u    /* DMCUB_CNTL__DMCUB_TRACEPORT_EN_MASK */
#define DMCUB_CNTL_PWAIT_MODE_STATUS 0x00100000u
#define DMCUB_CNTL2_SOFT_RESET 0x1u            /* DMCUB_CNTL2__DMCUB_SOFT_RESET_MASK */
#define DMUIF_SOFT_RESET 0x100u                /* MMHUBBUB_SOFT_RESET__DMUIF_SOFT_RESET_MASK */
#define CW_TOP_MASK 0x1FFFFFFFu                /* DMCUB_REGION3_CWn_TOP_ADDRESS / REGION5 */
#define CW_ENABLE 0x80000000u
#define GPINT_STOP_FW 2u                       /* DMUB_GPINT__STOP_FW */
#define GPINT_STOP_FW_RESPONSE 0xDEADDEADu     /* DMUB_GPINT__STOP_FW_RESPONSE */
#define BOOT_STATUS_DAL_FW 0x1u                /* dmub_fw_boot_status.dal_fw */
#define BOOT_STATUS_MAILBOX_RDY 0x2u           /* .mailbox_rdy */
/* dmub_dcn31_enable_dmub_boot_options for DCN 3.1.4 (dm_dmub_hw_init: dpia_supported = true, disable_dpia = 0):
 * enable_dpia (bit 7) | dpia_supported (bit 9); skip_phy_init_panel_sequence (bit 5) stays 0 */
#define BOOT_OPTIONS_DCN314 ((1u << 7) | (1u << 9))

static io_connect_t conn;
static int dry;
static uint64_t vram_base;   /* VRAM byte offset of the DMUB buffer */

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

static void wr(uint32_t reg, uint32_t val, const char *what) {
    printf("  wr 0x%05x = 0x%08x   %s\n", reg, val, what);
    if (dry) return;
    uint64_t in[2] = {reg, val}, out[1] = {0};
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
    if (kr) { fprintf(stderr, "write 0x%05x: kr 0x%08x (needs -phxp2 and phxdebug=1)\n", reg, kr); exit(1); }
}

static void upd(uint32_t reg, uint32_t clear, uint32_t set, const char *what) {   /* REG_UPDATE */
    wr(reg, (rd(reg) & ~clear) | set, what);
}

/* one dword at a VRAM byte offset (BAR0) */
static void vram32(uint64_t off, uint32_t val) {
    if (dry) return;
    uint64_t in[2] = {off, val};
    kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
    if (kr) { fprintf(stderr, "vram write 0x%llx: kr 0x%08x\n", (unsigned long long)off, kr); exit(1); }
}

static void vram_copy(uint32_t region_off, const uint8_t *src, uint32_t len, const char *what) {
    printf("  vram +0x%06x..+0x%06x  %s (%u bytes)\n", region_off, region_off + len, what, len);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        memcpy(&v, src + i, len - i >= 4 ? 4 : len - i);
        vram32(vram_base + region_off + i, v);
    }
}

static void vram_zero(uint32_t region_off, uint32_t len, const char *what) {
    printf("  vram +0x%06x..+0x%06x  zero %s\n", region_off, region_off + len, what);
    for (uint32_t i = 0; i < len; i += 4) vram32(vram_base + region_off + i, 0);
}

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* poll until (rd(reg) & mask) == want, at most timeout_us; returns 1 when met */
static int pollreg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t timeout_us, uint32_t *last) {
    const uint64_t end = now_us() + timeout_us;
    uint32_t v;
    do {
        v = rd(reg);
        if ((v & mask) == want) { if (last) *last = v; return 1; }
        usleep(10);
    } while (now_us() < end);
    if (last) *last = v;
    return 0;
}

static uint8_t *load(const char *p, uint32_t *len) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n + 4);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read %s failed\n", p); exit(1); }
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

/* dmub_dcn31_reset */
static void dmcub_reset(void) {
    printf("== reset (dmub_dcn31_reset)\n");
    if (!(rd(DMCUB_CNTL2) & DMCUB_CNTL2_SOFT_RESET)) {
        const uint32_t cmd = (1u << 28) | (GPINT_STOP_FW << 16);   /* status 1, command_code STOP_FW, param 0 */
        wr(DMCUB_GPINT_DATAIN1, cmd, "GPINT STOP_FW");
        if (!dry) {
            uint32_t v;
            /* is_gpint_acked: DATAIN1 reads back the command with status 0 */
            printf("  ack: %s\n", pollreg(DMCUB_GPINT_DATAIN1, 0xffffffffu, cmd & ~(0xfu << 28), 100000, &v) ? "yes" : "timeout");
            printf("  SCRATCH7 STOP_FW_RESPONSE: %s\n",
                   pollreg(DMCUB_SCRATCH7, 0xffffffffu, GPINT_STOP_FW_RESPONSE, 100000, &v) ? "yes" : "timeout");
            printf("  PWAIT_MODE_STATUS: %s\n",
                   pollreg(DMCUB_CNTL, DMCUB_CNTL_PWAIT_MODE_STATUS, DMCUB_CNTL_PWAIT_MODE_STATUS, 100000, &v) ? "yes" : "timeout");
        }
    } else {
        printf("  already in soft reset: no STOP_FW\n");
    }
    if (rd(DMCUB_CNTL) & DMCUB_CNTL_ENABLE) {
        upd(DMCUB_CNTL2, 0, DMCUB_CNTL2_SOFT_RESET, "DMCUB_SOFT_RESET = 1");
        upd(MMHUBBUB_SOFT_RESET, 0, DMUIF_SOFT_RESET, "DMUIF_SOFT_RESET = 1");
        upd(DMCUB_CNTL, DMCUB_CNTL_ENABLE, 0, "DMCUB_ENABLE = 0");
    }
    wr(DMCUB_INBOX1_RPTR, 0, "INBOX1_RPTR");
    wr(DMCUB_INBOX1_WPTR, 0, "INBOX1_WPTR");
    wr(DMCUB_OUTBOX1_RPTR, 0, "OUTBOX1_RPTR");
    wr(DMCUB_OUTBOX1_WPTR, 0, "OUTBOX1_WPTR");
    wr(DMCUB_OUTBOX0_RPTR, 0, "OUTBOX0_RPTR");
    wr(DMCUB_OUTBOX0_WPTR, 0, "OUTBOX0_WPTR");
    wr(DMCUB_SCRATCH0, 0, "SCRATCH0");
}

static void state(const char *when) {
    printf("== state %s: CNTL 0x%08x CNTL2 0x%08x SCRATCH0 0x%08x SCRATCH7 0x%08x SCRATCH14 0x%08x inbox1 %u/%u\n", when,
           rd(DMCUB_CNTL), rd(DMCUB_CNTL2), rd(DMCUB_SCRATCH0), rd(DMCUB_SCRATCH7), rd(DMCUB_SCRATCH14),
           rd(DMCUB_INBOX1_RPTR), rd(DMCUB_INBOX1_WPTR));
}

static void cw(uint32_t off_reg, uint32_t offh_reg, uint32_t base_reg, uint32_t top_reg, const phxdmub::Cw &c, const char *n) {
    char w[64];
    snprintf(w, sizeof w, "%s offset lo", n); wr(off_reg, (uint32_t)c.offset, w);
    snprintf(w, sizeof w, "%s offset hi", n); wr(offh_reg, (uint32_t)(c.offset >> 32), w);
    snprintf(w, sizeof w, "%s base", n); wr(base_reg, c.base, w);
    snprintf(w, sizeof w, "%s top | enable", n); wr(top_reg, (c.top & CW_TOP_MASK) | CW_ENABLE, w);
}

int main(int argc, char **argv) {
    int a = 1;
    if (argc > 1 && !strcmp(argv[1], "--dry-run")) { dry = 1; a++; }
    if (argc - a != 2) { fprintf(stderr, "usage: dmcubload [--dry-run] <dcn_3_1_4_dmcub.bin> <vbios.rom>\n"); return 2; }
    uint32_t fwlen, vblen;
    uint8_t *fw = load(argv[a], &fwlen), *vb = load(argv[a + 1], &vblen);
    phxdmub::Layout L;
    if (!phxdmub::plan(fw, fwlen, vblen, &L)) { fprintf(stderr, "dmcubload: %s is not a DMCUB firmware\n", argv[a]); return 1; }
    if (open_kext()) return 1;
    const uint64_t mcbase = prop64("phx-fb-mc-base");
    if (!mcbase) { fprintf(stderr, "dmcubload: no phx-fb-mc-base (PhxBringup chip unknown?)\n"); return 1; }
    /* VRAM 208 MiB: free in the bring-up's layout (TMR TOC ends at 202 MiB), inside BAR0's 256 MiB, 1 MiB aligned
     * (DMUB_FB_ALIGNMENT) */
    vram_base = 0xD000000ull;
    if (vram_base + L.fbSize > 0xE000000ull) { fprintf(stderr, "dmcubload: DMUB buffer too large\n"); return 1; }
    const uint64_t mc = mcbase + vram_base;
    const phxdmub::Windows W = phxdmub::windows(L, mc);
    printf("dmcubload%s: fw 0x%08x (dal_fw %u), PSP payload %s[256:+%u] type %u, buffer %u bytes at VRAM 0x%llx = MC 0x%llx\n",
           dry ? " (dry run)" : "", L.fwVersion, L.metaDalFw, argv[a], L.pspSize, phxdmub::kPspFwTypeDmub, L.fbSize,
           (unsigned long long)vram_base, (unsigned long long)mc);
    state("before");
    if ((rd(DMCUB_SCRATCH0) & (BOOT_STATUS_DAL_FW | BOOT_STATUS_MAILBOX_RDY)) == (BOOT_STATUS_DAL_FW | BOOT_STATUS_MAILBOX_RDY) &&
        (rd(DMCUB_CNTL) & DMCUB_CNTL_ENABLE)) {
        printf("dmcubload: the driver's DMCUB firmware already runs (dal_fw 1): nothing to do\n");
        return 0;
    }

    /* dm_dmub_hw_init: dmub_srv_hw_reset, then the copies, then dmub_srv_hw_init (which resets again) */
    dmcub_reset();
    printf("== copies (dm_dmub_hw_init)\n");
    const phxdmub::Region *R = L.regions;
    if (L.bssDataBytes) vram_copy(R[phxdmub::kBssData].base, fw + L.bssOffset, L.bssDataBytes, "bss/data");
    vram_copy(R[phxdmub::kVbios].base, vb, vblen, "VBIOS");
    vram_zero(R[phxdmub::kMailbox].base, R[phxdmub::kMailbox].top - R[phxdmub::kMailbox].base, "mailbox");
    vram_zero(R[phxdmub::kTraceBuff].base, R[phxdmub::kTraceBuff].top - R[phxdmub::kTraceBuff].base, "trace buffer");
    vram_zero(R[phxdmub::kFwState].base, R[phxdmub::kFwState].top - R[phxdmub::kFwState].base, "fw state");
    vram_zero(R[phxdmub::kSharedState].base, R[phxdmub::kSharedState].top - R[phxdmub::kSharedState].base, "shared state");

    printf("== dmub_srv_hw_init\n");
    dmcub_reset();
    /* PSP path: no backdoor load (cw0/cw1 are the PSP's) */
    cw(DMCUB_REGION3_CW3_OFFSET, DMCUB_REGION3_CW3_OFFSET_HIGH, DMCUB_REGION3_CW3_BASE_ADDRESS, DMCUB_REGION3_CW3_TOP_ADDRESS, W.cw3, "CW3 (VBIOS)");
    cw(DMCUB_REGION3_CW4_OFFSET, DMCUB_REGION3_CW4_OFFSET_HIGH, DMCUB_REGION3_CW4_BASE_ADDRESS, DMCUB_REGION3_CW4_TOP_ADDRESS, W.cw4, "CW4 (mailbox)");
    cw(DMCUB_REGION3_CW5_OFFSET, DMCUB_REGION3_CW5_OFFSET_HIGH, DMCUB_REGION3_CW5_BASE_ADDRESS, DMCUB_REGION3_CW5_TOP_ADDRESS, W.cw5, "CW5 (trace)");
    wr(DMCUB_REGION5_OFFSET, (uint32_t)W.cw5.offset, "REGION5 offset lo");
    wr(DMCUB_REGION5_OFFSET_HIGH, (uint32_t)(W.cw5.offset >> 32), "REGION5 offset hi");
    wr(DMCUB_REGION5_TOP_ADDRESS, (W.region5Top & CW_TOP_MASK) | CW_ENABLE, "REGION5 top | enable");
    cw(DMCUB_REGION3_CW6_OFFSET, DMCUB_REGION3_CW6_OFFSET_HIGH, DMCUB_REGION3_CW6_BASE_ADDRESS, DMCUB_REGION3_CW6_TOP_ADDRESS, W.cw6, "CW6 (fw state)");
    /* outbox0 = region 5 + TRACE_BUFFER_ENTRY_OFFSET, size = trace buffer - that offset (dmub_srv_hw_init) */
    const uint32_t traceSize = R[phxdmub::kTraceBuff].top - R[phxdmub::kTraceBuff].base;
    const uint32_t ob0 = phxdmub::kRegion5Base + phxdmub::kTraceBufferEntryOffset;
    wr(DMCUB_OUTBOX0_BASE_ADDRESS, ob0, "OUTBOX0 base (setup_outbox0)");
    wr(DMCUB_OUTBOX0_SIZE, traceSize - phxdmub::kTraceBufferEntryOffset, "OUTBOX0 size");
    wr(DMCUB_INBOX1_BASE_ADDRESS, W.inbox1Base, "INBOX1 base (setup_mailbox)");
    wr(DMCUB_INBOX1_SIZE, W.inbox1Size, "INBOX1 size");
    wr(DMCUB_OUTBOX1_BASE_ADDRESS, W.inbox1Base + phxdmub::kRbSize, "OUTBOX1 base (setup_out_mailbox)");
    wr(DMCUB_OUTBOX1_SIZE, phxdmub::kRbSize, "OUTBOX1 size");
    wr(DMCUB_SCRATCH14, BOOT_OPTIONS_DCN314, "boot options (enable_dpia | dpia_supported)");
    upd(DMCUB_SCRATCH14, 1u << 5, 0, "skip_phy_init_panel_sequence = 0");
    printf("== start (dmub_dcn31_reset_release)\n");
    upd(MMHUBBUB_SOFT_RESET, DMUIF_SOFT_RESET, 0, "DMUIF_SOFT_RESET = 0");
    wr(DMCUB_SCRATCH15, 0, "psp_version & 0x001100FF (no DMCU on DCN 3.1.4: 0)");
    upd(DMCUB_CNTL, 0, DMCUB_CNTL_ENABLE | DMCUB_CNTL_TRACEPORT_EN, "DMCUB_ENABLE | TRACEPORT_EN");
    upd(DMCUB_CNTL2, DMCUB_CNTL2_SOFT_RESET, 0, "DMCUB_SOFT_RESET = 0");
    if (dry) { printf("dry run: nothing written\n"); return 0; }

    uint32_t st = 0;
    const int ok = pollreg(DMCUB_SCRATCH0, BOOT_STATUS_DAL_FW | BOOT_STATUS_MAILBOX_RDY,
                        BOOT_STATUS_DAL_FW | BOOT_STATUS_MAILBOX_RDY, 100000, &st);
    state("after");
    printf("auto-load (dmub_srv_wait_for_auto_load): %s, SCRATCH0 0x%08x\n", ok ? "OK, dal_fw 1 mailbox_rdy 1" : "TIMEOUT", st);
    if (!ok) return 1;

    /* one DMUB_CMD__NULL through inbox 1: header type 0, payload 0; 64 bytes at the mailbox start, WPTR += 64 */
    for (uint32_t i = 0; i < 64; i += 4) vram32(vram_base + R[phxdmub::kMailbox].base + i, 0);
    wr(DMCUB_INBOX1_WPTR, 64, "INBOX1_WPTR (one NULL command)");
    uint32_t rp = 0;
    const int done = pollreg(DMCUB_INBOX1_RPTR, 0xffffffffu, 64, 100000, &rp);
    printf("NULL command: %s (INBOX1_RPTR %u)\n", done ? "OK, consumed" : "NOT consumed", rp);
    return done ? 0 : 1;
}
