// test_phx_dcn314_clk.cpp: Test L step 3's clock sequence (phx_dcn314_clk.h) against Linux's rules and a fake VBIOSSMC
// mailbox: khz_to_mhz_ceil, dccg31_update_dpp_dto's phase, dentist_get_divider_from_did, the order going up
// (DISPCLK, DPPCLK, DTO) and coming down (DISPCLK, DTO, DPPCLK), and a failing SMU stopping the raise.
#include "phx_dcn314_clk.h"
#include <stdio.h>

using namespace phxdcn314;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct Fake {
    uint32_t msg = 0, arg = 0, resp = kSmuOk, dtoParam = 0, dtoCtrl = 0;
    uint32_t answer = kSmuOk;
    char order[16]; unsigned n = 0;
};
static uint32_t frd(void *c, uint32_t a) {
    Fake *f = (Fake *)c;
    if (a == kSmuResp) return f->resp;
    if (a == kSmuArg) return f->arg;
    if (a == kDppDtoPhase0.addr) return f->dtoParam;
    if (a == kDppDtoEnable0.addr) return f->dtoCtrl;
    return 0;
}
static void fwr(void *c, uint32_t a, uint32_t v) {
    Fake *f = (Fake *)c;
    if (a == kSmuResp) f->resp = v;
    else if (a == kSmuArg) f->arg = v;
    else if (a == kSmuMsg) {   // the SMU answers at once and echoes the clock it set
        f->msg = v; f->resp = f->answer;
        if (f->n < 15) f->order[f->n++] = v == kMsgSetDispclkFreq ? 'D' : v == kMsgSetDppclkFreq ? 'P' : '?';
    } else if (a == kDppDtoPhase0.addr) { f->dtoParam = v; if (f->n < 15) f->order[f->n++] = 'T'; }
    else if (a == kDppDtoEnable0.addr) f->dtoCtrl = v;
}
static void fdelay(void *, unsigned) {}

int main() {
    unsigned disp144 = 0, dpp144 = 0, dpp60 = 0;
    CHECK(phxDmlGet(kDml144, "clk.dispclk_khz", &disp144) && disp144 == 342857);
    CHECK(phxDmlGet(kDml144, "clk.dppclk_khz", &dpp144) && dpp144 == 337478);
    CHECK(phxDmlGet(kDml60, "clk.dppclk_khz", &dpp60));
    CHECK(khzToMhzCeil(342857) == 343 && khzToMhzCeil(337478) == 338 && khzToMhzCeil(165000) == 165);
    CHECK(dppDtoPhase(337478, 337478) == 0xff);              // the pipe at the full global DPPCLK
    CHECK(dppDtoPhase(337478, 149411) == 113);               // ceil(255 * 149411 / 337478)
    CHECK(dentistKhz(2400000, 0x3a) == 165517);              // Test clk: GOP's DISPCLK, divider 14.5
    CHECK(dentistKhz(2400000, 0x1c) == 342857);              // divider 7.0

    Fake f; f.dtoCtrl = 0x00000000; f.dtoParam = 0x00000000;
    Io io{frd, fwr, fdelay, &f};
    ClkStep up = raise(io, disp144, dpp144, dpp144);
    CHECK(up.ok());
    CHECK(f.n == 3 && f.order[0] == 'D' && f.order[1] == 'P' && f.order[2] == 'T');
    CHECK(fieldGet(f.dtoParam, kDppDtoPhase0) == 0xff && fieldGet(f.dtoParam, kDppDtoModulo0) == 0xff);
    CHECK(fieldGet(f.dtoCtrl, kDppDtoEnable0) == 1);
    CHECK(up.dispMhz == 343 && up.dppMhz == 338);

    f.n = 0;
    ClkStep down = lower(io, 165517, dpp60, 0, 0);
    CHECK(down.ok());
    CHECK(f.n == 3 && f.order[0] == 'D' && f.order[1] == 'T' && f.order[2] == 'P');
    CHECK(f.dtoParam == 0 && f.dtoCtrl == 0);
    CHECK(down.dispMhz == 166);

    Fake g; g.answer = kSmuFailed;   // the SMU refuses DISPCLK: no DPPCLK message, no DTO write
    Io io2{frd, fwr, fdelay, &g};
    ClkStep bad = raise(io2, disp144, dpp144, dpp144);
    CHECK(!bad.ok() && bad.dispResp == kSmuFailed && g.n == 1);
    CHECK(g.resp == kSmuOk);   // the failed response is cleared, as Linux does

    if (fails) { fprintf(stderr, "test_phx_dcn314_clk: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_clk: OK\n");
    return 0;
}
