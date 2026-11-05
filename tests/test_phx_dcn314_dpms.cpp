// test_phx_dcn314_dpms.cpp: Test L step 5b's DPMS off / on (phx_dcn314_dpms.h) over a fake register file and a fake
// DMCUB: the DMUB commands in Linux's order (transmitter disable; encoder STREAM_SETUP, transmitter enable), the
// BE<->FE connection and FIFO going down and up, the FIFO reset handshake while SYMCLK runs, no transmitter disable
// when the DIG is already off, and a full off/on cycle ending with the encoder registers as GOP left them.
#include "phx_dcn314_dpms.h"
#include <stdio.h>
#include <string.h>

using namespace phxdcn314;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Fake {
    uint32_t reg[0x6000] = {};
    uint32_t vram[8192 / 4] = {};
    uint8_t cmds[8][64]; unsigned ncmd = 0;
};
static Fake *gF;
static uint32_t frd(void *, uint32_t a) {
    uint32_t v = gF->reg[a];
    if (a == kDigFifoResetDone.addr)   // the FIFO reports its reset state as soon as it is asked
        v = fieldSet(v, kDigFifoResetDone, fieldGet(gF->reg[kDigFifoReset.addr], kDigFifoReset));
    return v;
}
static void fwr(void *, uint32_t a, uint32_t v) {
    gF->reg[a] = v;
    if (a == phxdmubinbox::kInbox1Wptr.addr) {   // DMCUB consumes the command: record it, move RPTR
        const uint32_t prev = gF->reg[phxdmubinbox::kInbox1Rptr.addr];
        if (gF->ncmd < 8) memcpy(gF->cmds[gF->ncmd++], (uint8_t *)gF->vram + prev, 64);
        gF->reg[phxdmubinbox::kInbox1Rptr.addr] = v;
    }
}
static uint32_t fvr(void *, uint64_t off) { return gF->vram[(off - 0xd0f9900) / 4]; }
static void fvw(void *, uint64_t off, uint32_t v) { gF->vram[(off - 0xd0f9900) / 4] = v; }
static void fdelay(void *, unsigned) {}

static void seedGop(Fake &f) {   // the 8500G as GOP leaves it (Test K): DVI on DIG BE 0, FE 0 from OTG0, FIFO running
    f.reg[kDigBeMode.addr] = fieldSet(fieldSet(0, kDigBeMode, 2), kDigBeFeSrc, 1);
    f.reg[kDigBeEnable.addr] = kDigBeEnable.mask;
    f.reg[kDigFeSourceSel.addr] = fieldSet(fieldSet(0, kDigFeSymclkOn, 1), kDigFeStereoGate, 1);
    f.reg[kDigFifoEnable.addr] = fieldSet(fieldSet(0, kDigFifoEnable, 1), kDigFifoStartLvl, 7);
    f.reg[kOtg0RateDivK1.addr] = fieldSet(0, kOtg0RateDivK2, kRateDivBy4);
    // inbox 1 as dmcubload / Test L 4a found it
    f.reg[phxdmubinbox::kCw4Base.addr] = 0x04000000; f.reg[phxdmubinbox::kInbox1Base.addr] = 0x64000000;
    f.reg[phxdmubinbox::kInbox1Size.addr] = 8192;
    f.reg[phxdmubinbox::kCw4Offset.addr] = 0x0d0f9900; f.reg[phxdmubinbox::kCw4OffsetHigh.addr] = 0x80;
    f.reg[phxdmubinbox::kInbox1Wptr.addr] = f.reg[phxdmubinbox::kInbox1Rptr.addr] = 128;
}

int main() {
    Fake &f = *new Fake(); gF = &f;
    seedGop(f);
    const uint32_t be0 = f.reg[kDigBeMode.addr], fe0 = f.reg[kDigFeSourceSel.addr], fifo0 = f.reg[kDigFifoEnable.addr];
    phxdmubinbox::Inbox ib;
    phxdmubinbox::Io dio{frd, fwr, fvr, fvw, fdelay, nullptr};
    CHECK(phxdmubinbox::locate(dio, 0x8000000000ull, 256ull << 20, &ib) == phxdmubinbox::Status::Ok);
    DpmsCtx c{Io{frd, fwr, fdelay, nullptr}, dio, &ib, phxdmubcmd::Signal::Dvi, 148500};

    dpmsOff(c);
    CHECK(c.dmub == phxdmubinbox::Status::Ok);
    CHECK(f.ncmd == 1);
    uint8_t want[64];
    phxdmubcmd::transmitterDisable(want, phxdmubcmd::Signal::Dvi);
    CHECK(memcmp(f.cmds[0], want, 64) == 0);
    CHECK(fieldGet(f.reg[kDigBeFeSrc.addr], kDigBeFeSrc) == 0);       // BE disconnected from FE 0
    CHECK(fieldGet(f.reg[kDigFifoEnable.addr], kDigFifoEnable) == 0);

    dpmsOn(c);
    CHECK(c.dmub == phxdmubinbox::Status::Ok && c.fifoResetTimeouts == 0);
    CHECK(f.ncmd == 3);
    phxdmubcmd::encoderStreamSetup(want, phxdmubcmd::Signal::Dvi, 148500);
    CHECK(memcmp(f.cmds[1], want, 64) == 0);
    phxdmubcmd::transmitterEnable(want, phxdmubcmd::Signal::Dvi, 148500);
    CHECK(memcmp(f.cmds[2], want, 64) == 0);
    // back to GOP's encoder state
    CHECK(f.reg[kDigBeMode.addr] == be0);
    CHECK(f.reg[kDigFeSourceSel.addr] == fe0);
    CHECK(fieldGet(f.reg[kDigFifoEnable.addr], kDigFifoEnable) == 1 && fieldGet(f.reg[kDigFifoEnable.addr], kDigFifoStartLvl) == 7);
    CHECK(fieldGet(f.reg[kDigFifoReset.addr], kDigFifoReset) == 0);
    CHECK((f.reg[kDigFifoEnable.addr] & ~kDigFifoResetDone.mask) == (fifo0 & ~kDigFifoResetDone.mask));
    CHECK(fieldGet(f.reg[kOtg0RateDivK2.addr], kOtg0RateDivK2) == kRateDivBy4);

    // a DIG that is already off gets no transmitter disable
    f.reg[kDigBeEnable.addr] = 0; f.ncmd = 0;
    dpmsOff(c);
    CHECK(f.ncmd == 0);

    if (fails) { fprintf(stderr, "test_phx_dcn314_dpms: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_dpms: OK\n");
    return 0;
}
