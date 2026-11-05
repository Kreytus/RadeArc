// test_phx_dcn314_blank.cpp: phx_dcn314_blank.h: the DPG writes of dcn20_blank_pixel_data for a 1920x1080 sRGB
// stream, the composed DPG_CONTROL (EN 1, mode 4, other bits kept), and unblank leaving the DPG all zero.
#include "phx_dcn314_blank.h"
#include <stdio.h>

using namespace phxdcn314;
static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static uint32_t regs[0x6000];
static uint32_t frd(void *, uint32_t a) { return regs[a]; }
static void fwr(void *, uint32_t a, uint32_t v) { regs[a] = v; }
static void fdelay(void *, unsigned) {}

int main() {
    Io io{frd, fwr, fdelay, nullptr};
    regs[kDpgEn.addr] = 0x00000100;   // an unrelated bit (e.g. DPG_BIT_DEPTH) that the REG_UPDATE keeps
    applyBlank(io, 1920, 1080);
    CHECK(isBlanked(io));
    CHECK(fieldGet(regs[kDpgEn.addr], kDpgEn) == 1 && fieldGet(regs[kDpgEn.addr], kDpgMode) == 4);
    CHECK((regs[kDpgEn.addr] & 0x100) == 0x100);
    CHECK(fieldGet(regs[kDpgWidth.addr], kDpgWidth) == 1920 && fieldGet(regs[kDpgWidth.addr], kDpgHeight) == 1080);
    CHECK(regs[kDpgColR1.addr] == 0 && regs[kDpgColG1.addr] == 0 && regs[kDpgColB1.addr] == 0);
    applyUnblank(io, 1920, 1080);
    CHECK(!isBlanked(io));
    CHECK(regs[kDpgEn.addr] == 0 && regs[kDpgRamp0.addr] == 0);
    if (fails) { fprintf(stderr, "test_phx_dcn314_blank: %d failure(s)\n", fails); return 1; }
    printf("test_phx_dcn314_blank: OK\n");
    return 0;
}
