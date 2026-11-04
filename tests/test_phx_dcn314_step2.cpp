// test_phx_dcn314_step2.cpp: Test L step 2's sequence (phx_dcn314_step2.h) over a fake register file seeded with the
// 8500G's live 60 Hz state (hardware/dumps/b650m_8500g/testl.txt): the writes sit inside optc3_lock / optc1_unlock,
// the global sync comes first and VACTIVE is waited for after it, VTG0_FP2 follows optc1_set_vtg_params, UPDATE
// registers keep their other bits, a lock that never reports refuses to write, and restore() puts every word back.
#include "phx_dcn314_step2.h"
#include <stdio.h>
#include <string.h>

using namespace phxdcn314;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Fake {
    uint32_t addr[256], val[256];
    unsigned n = 0;
    struct Ev { char op; uint32_t addr, val; } ev[4096];
    unsigned nev = 0;
    bool lockReports = true;
    uint32_t &at(uint32_t a) {
        for (unsigned i = 0; i < n; i++) if (addr[i] == a) return val[i];
        addr[n] = a; val[n] = 0; return val[n++];
    }
};
static uint32_t frd(void *c, uint32_t a) {
    Fake *f = (Fake *)c;
    uint32_t v = f->at(a);
    if (a == kOtgLockStatus.addr) {   // the lock reports once requested, if this fake lets it
        v = (v & ~kOtgLockStatus.mask) | ((f->lockReports && (v & kOtgLock.mask)) ? kOtgLockStatus.mask : 0);
    }
    if (a == kOtgVActiveDisp.addr) v |= kOtgVActiveDisp.mask;
    return v;
}
static void fwr(void *c, uint32_t a, uint32_t v) {
    Fake *f = (Fake *)c;
    f->at(a) = v;
    if (f->nev < 4096) f->ev[f->nev++] = {'w', a, v};
}
static void fdelay(void *, unsigned) {}

static void seed(Fake &f) {
    // live words from testl.txt (2026-11-04)
    f.at(0x03ab6) = 0x00000100; f.at(0x03ab3) = 0x000f1002; f.at(0x03b2c) = 0x00e40000; f.at(0x03ae9) = 0x00000055;
    f.at(0x03ab1) = 0x00000300; f.at(0x03b04) = 0x00290040; f.at(0x03b08) = 0x28080000; f.at(0x03aeb) = 0xe3000a64;
    f.at(0x05046) = 0x0000000d; f.at(0x05047) = 0x01400226; f.at(0x05048) = 0x0000012c; f.at(0x03af4) = 0x80000000;
    f.at(kVtgFp2.addr) = (1121u << 16) | 0;   // GOP: VCOUNT_INIT 1121, FP2 0 (vstartup 13)
}

int main() {
    const VTiming t60 = {1125, 4, 1080};
    Fake f; seed(f);
    Io io{frd, fwr, fdelay, &f};

    // optc1_set_vtg_params by hand: v_init 1121, blank end 41, vstartup 44 -> line start -2 -> FP2 2
    unsigned fp2, vinit;
    vtgParams(t60, 44, &fp2, &vinit);
    CHECK(fp2 == 2 && vinit == 1121);
    vtgParams(t60, 13, &fp2, &vinit);   // GOP's vstartup: no FP2
    CHECK(fp2 == 0);

    Plan pl = buildPlan(io, kDml60, t60);
    CHECK(pl.ok);
    CHECK(pl.firstAfterVactive == 3);   // OTG_VSTARTUP_PARAM, OTG_VUPDATE_PARAM, OTG_VREADY_PARAM
    CHECK(pl.w[0].addr == 0x05046 && pl.w[0].val == 44);
    CHECK(pl.w[3].addr == kVtgFp2.addr && fieldGet(pl.w[3].val, kVtgFp2) == 2 && fieldGet(pl.w[3].val, kVtgVcountInit) == 1121);
    // DCHUBP_CNTL: hubp2_vtg_sel then the vready UPDATE; the live bits outside both fields stay
    unsigned cntl = 0;
    for (unsigned i = 0; i < pl.n; i++) if (pl.w[i].addr == kHubpVtgSel.addr) cntl++;
    CHECK(cntl == 2);
    for (unsigned i = 0; i < pl.n; i++)
        if (pl.w[i].addr == kHubpVtgSel.addr) CHECK((pl.w[i].val & ~(kHubpVtgSel.mask | 0x100u)) == (0x000f1002u & ~(kHubpVtgSel.mask | 0x100u)));

    const Saved sv = save(io, pl);
    CHECK(sv.n > 0 && sv.n <= pl.n);

    f.nev = 0;
    CHECK(applyPlan(io, pl) == Result::Ok);
    // bracket: lock select, lock 1 first; lock 0 last
    CHECK(f.nev >= pl.n + 3);
    CHECK(f.ev[0].addr == kOtgLockSel.addr);
    CHECK(f.ev[1].addr == kOtgLock.addr && f.ev[1].val == 1);
    CHECK(f.ev[f.nev - 1].addr == kOtgLock.addr && f.ev[f.nev - 1].val == 0);
    CHECK(f.at(0x05046) == 44);

    CHECK(restore(io, sv, pl.firstAfterVactive) == Result::Ok);
    for (unsigned i = 0; i < sv.n; i++) {
        if (f.at(sv.w[i].addr) != sv.w[i].val) fprintf(stderr, "  restore: 0x%05x = 0x%08x, saved 0x%08x\n", sv.w[i].addr, f.at(sv.w[i].addr), sv.w[i].val);
        CHECK(f.at(sv.w[i].addr) == sv.w[i].val);
    }

    // a lock that never reports: nothing but the lock request and its release is written
    Fake g; seed(g); g.lockReports = false;
    Io io2{frd, fwr, fdelay, &g};
    Plan p2 = buildPlan(io2, kDml60, t60);
    g.nev = 0;
    CHECK(applyPlan(io2, p2) == Result::LockTimeout);
    CHECK(g.nev == 3);
    CHECK(g.at(0x05046) == 0x0000000d);

    if (fails) { fprintf(stderr, "test_phx_dcn314_step2: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_step2: OK (%u writes, %u saved registers)\n", pl.n, sv.n);
    return 0;
}
