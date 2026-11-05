// test_phx_dmub_inbox.cpp: phx_dmub_inbox.h against Linux's dmub_srv rules with a fake DMCUB: the ring found from
// CW4 + INBOX1 registers, commands copied at wrpt and read back before WPTR is written, wrap at the capacity, a full
// ring refused (one slot unused), RPTR past the capacity a HW failure, and a DMCUB that never consumes a timeout.
#include "phx_dmub_inbox.h"
#include "phx_dmub_vbios_cmd.h"
#include <stdio.h>
#include <string.h>

using namespace phxdmubinbox;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const uint64_t kMcBase = 0x8000000000ull, kVramSize = 256ull << 20;
struct Fake {
    uint32_t reg[0x4000] = {};
    uint32_t vram[(16u << 20) / 4] = {};   // the 16 MiB from VRAM 0xD000000
    bool consumes = true;
    unsigned wptrWrites = 0;
};
static Fake gF;
static uint32_t frd(void *c, uint32_t a) { return ((Fake *)c)->reg[a]; }
static void fwr(void *c, uint32_t a, uint32_t v) {
    Fake *f = (Fake *)c;
    f->reg[a] = v;
    if (a == kInbox1Wptr.addr) { f->wptrWrites++; if (f->consumes) f->reg[kInbox1Rptr.addr] = v; }
}
static uint32_t fvr(void *c, uint64_t off) { return ((Fake *)c)->vram[(off - 0xD000000) / 4]; }
static void fvw(void *c, uint64_t off, uint32_t v) { ((Fake *)c)->vram[(off - 0xD000000) / 4] = v; }
static void fdelay(void *, unsigned) {}

int main() {
    Fake &f = gF;
    // as dmcubload set it up (Test J): the mailbox window at VRAM 0xD000000 + 0x800000, DMCUB sees it at 0x64000000
    f.reg[kDmcubEnable.addr] = kDmcubEnable.mask;
    f.reg[kDmcubScratch0.addr] = kBootDalFw | kBootMailboxRdy;
    f.reg[kCw4Base.addr] = 0x64000000;
    const uint64_t mc = kMcBase + 0xD000000 + 0x800000;
    f.reg[kCw4Offset.addr] = (uint32_t)mc;
    f.reg[kCw4OffsetHigh.addr] = (uint32_t)(mc >> 32);
    f.reg[kInbox1Base.addr] = 0x64000000;
    f.reg[kInbox1Size.addr] = 8192;
    f.reg[kInbox1Wptr.addr] = f.reg[kInbox1Rptr.addr] = 64;   // Test J's NULL command
    Io io{frd, fwr, fvr, fvw, fdelay, &f};

    CHECK(ready(io));
    Inbox ib;
    CHECK(locate(io, kMcBase, kVramSize, &ib) == Status::Ok);
    CHECK(ib.vramOff == 0xD000000 + 0x800000 && ib.capacity == 8192 && ib.wrpt == 64);

    uint8_t cmd[64];
    phxdmubcmd::transmitterEnable(cmd, phxdmubcmd::Signal::Hdmi, 335770);
    CHECK(send(io, ib, cmd) == Status::Ok);
    CHECK(ib.wrpt == 128 && f.reg[kInbox1Wptr.addr] == 128);
    CHECK(memcmp(&f.vram[(0x800000 + 64) / 4], cmd, 64) == 0);   // at the old wrpt

    // wrap: the last slot, then back to 0
    f.reg[kInbox1Wptr.addr] = f.reg[kInbox1Rptr.addr] = 8128;
    CHECK(locate(io, kMcBase, kVramSize, &ib) == Status::Ok);
    CHECK(send(io, ib, cmd) == Status::Ok);
    CHECK(ib.wrpt == 0 && f.reg[kInbox1Wptr.addr] == 0);
    CHECK(memcmp(&f.vram[(0x800000 + 8128) / 4], cmd, 64) == 0);

    // full: one slot always unused (rptr just ahead of wrpt)
    f.consumes = false;
    f.reg[kInbox1Wptr.addr] = 0; f.reg[kInbox1Rptr.addr] = 64;
    CHECK(locate(io, kMcBase, kVramSize, &ib) == Status::Ok);
    const unsigned w0 = f.wptrWrites;
    CHECK(queue(io, ib, cmd) == Status::QueueFull);
    CHECK(f.wptrWrites == w0);

    // a DMCUB that never consumes: timeout; RPTR past the capacity: HW failure
    f.reg[kInbox1Wptr.addr] = f.reg[kInbox1Rptr.addr] = 0;
    CHECK(locate(io, kMcBase, kVramSize, &ib) == Status::Ok);
    CHECK(send(io, ib, cmd, 50) == Status::Timeout);
    f.reg[kInbox1Rptr.addr] = 9000;
    CHECK(waitIdle(io, ib, 50) == Status::HwFailure);

    // GOP's firmware (dal_fw 0) is not ready; a ring outside CW4 is refused
    f.reg[kDmcubScratch0.addr] = kBootMailboxRdy;
    CHECK(!ready(io));
    f.reg[kInbox1Base.addr] = 0x63000000;
    CHECK(locate(io, kMcBase, kVramSize, &ib) == Status::BadWindow);

    if (fails) { fprintf(stderr, "test_phx_dmub_inbox: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dmub_inbox: OK\n");
    return 0;
}
