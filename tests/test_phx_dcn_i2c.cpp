// tests/test_phx_dcn_i2c.cpp: phxi2c::readEdid (src/amd/phx_dcn_i2c.h) against a simulated DCN hardware I2C engine.
// The model follows the register semantics dce_i2c_hw.c relies on: arbitration grant on SW_USE_I2C_REG_REQ, the
// DC_I2C_DATA index/auto-increment buffer, transactions run on DC_I2C_GO, SW_STATUS DONE/NACK.
#include "phx_dcn_i2c.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

using namespace phxi2c;

struct SimEngine {
    std::map<uint32_t, uint32_t> r;
    uint8_t buf[1024] = {};
    uint32_t index = 0;
    std::vector<uint8_t> edid;     // what the monitor at 0x50 returns (empty: nothing answers)
    uint32_t edidOff = 0;
    bool denyArbitration = false;
    int goCount = 0, dataReadsWhileWriting = 0;
    bool lastDataRw = false;

    uint32_t rd(uint32_t reg) {
        if (reg == kData) {
            if (!lastDataRw) dataReadsWhileWriting++;   // Linux never reads DATA while filling the buffer
            return (uint32_t)buf[index++ & 1023] << kDataDataShift;
        }
        return r.count(reg) ? r[reg] : 0;
    }
    void wr(uint32_t reg, uint32_t v) {
        if (reg == kData) {
            lastDataRw = v & kDataRw;
            if (v & kDataIndexWrite) index = (v & kDataIndexMask) >> kDataIndexShift;
            if (!(v & kDataRw)) buf[index++ & 1023] = (uint8_t)((v & kDataDataMask) >> kDataDataShift);
            return;
        }
        if (reg == kArbitration) {
            uint32_t st = (r[reg] & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift;
            if ((v & kArbSwUseReq) && !denyArbitration) st = kStatusUsedBySw;
            if (v & kArbSwDone) st = kStatusIdle;
            r[reg] = (v & ~(kArbRwCntlStatusMask | kArbSwUseReq | kArbSwDone)) | (st << kArbRwCntlStatusShift);
            return;
        }
        if (reg == kControl) {
            if (v & kCtlSwStatusReset) r[kSwStatus] = 0;
            const bool go = (v & kCtlGo) && !(r[reg] & kCtlGo);
            r[reg] = v;
            if (go) run();
            return;
        }
        r[reg] = v;
    }
    void delayUs(uint32_t) {}

    // run the programmed transactions over the buffer: address byte, then data (write) or room for data (read)
    void run() {
        goCount++;
        const uint32_t n = ((r[kControl] & kCtlTransCountMask) >> kCtlTransCountShift) + 1;
        uint32_t p = 0;
        for (uint32_t t = 0; t < n; t++) {
            const uint32_t tr = r[kTransaction0 + t];
            const uint32_t count = (tr & kTrCountMask) >> kTrCountShift;
            const uint8_t addr = buf[p++];
            if ((addr >> 1) != kEdidAddr || edid.empty()) {
                r[kSwStatus] = kSwDone | kSwStoppedOnNack;
                return;
            }
            if (tr & kTrRw) {
                for (uint32_t i = 0; i < count; i++) buf[p++] = edid[(edidOff + i) % edid.size()];
                edidOff += count;
            } else {
                if (count >= 1) edidOff = buf[p];   // the offset byte
                p += count;
            }
        }
        r[kSwStatus] = kSwDone;
    }
};

static std::vector<uint8_t> makeEdid(bool extension) {
    std::vector<uint8_t> e(extension ? 256 : 128, 0);
    const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    memcpy(e.data(), hdr, 8);
    e[8] = 0x10; e[9] = 0xac;   // "DEL"
    e[18] = 1; e[19] = 4;
    e[126] = extension ? 1 : 0;
    uint8_t s = 0;
    for (int i = 0; i < 127; i++) s = (uint8_t)(s + e[i]);
    e[127] = (uint8_t)(0x100 - s);
    if (extension) {
        e[128] = 0x02; e[129] = 0x03;   // CEA-861 block
        s = 0;
        for (int i = 128; i < 255; i++) s = (uint8_t)(s + e[i]);
        e[255] = (uint8_t)(0x100 - s);
    }
    return e;
}

static SimEngine engine() {
    SimEngine s;
    s.r[kMicrosecondTimeBaseDiv] = 0x00120264;   // MICROSECOND_TIME_BASE_DIV 100, XTAL_REF_DIV 2
    s.r[kDdc1Setup] = 0x30000040;                // as GOP left it on the 8500G (gop_regs.txt)
    return s;
}

int main() {
    printf("=== Running DCN 3.1.4 hardware I2C / EDID tests ===\n");
    // absolute addresses: the dump names DC_I2C_DDC1_SETUP at 0x05363
    CHECK(kDdc1Setup == 0x05363 && kMicrosecondTimeBaseDiv == 0x13b && kHighestReg == 0x5d90);

    SimEngine s = engine();
    s.edid = makeEdid(true);
    uint8_t out[256];
    uint32_t len = 0;
    CHECK(readEdid(s, out, &len) == kOk);
    CHECK(len == 256 && memcmp(out, s.edid.data(), 256) == 0);
    CHECK(s.goCount == 2 && s.dataReadsWhileWriting == 0);
    // the engine is handed back: arbitration released, light sleep forced again, DDC1 still enabled (keep_power_up)
    CHECK(((s.r[kArbitration] & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift) == kStatusIdle);
    CHECK(s.r[kDioMemPwrCtrl] & kI2cLightSleepForce);
    CHECK(s.r[kDdc1Setup] & kSetEnable);
    // programmed as Linux: time limit 3, send-reset-length bit, prescale 100*1000/2/100 = 500, threshold 2, timing 2
    CHECK((s.r[kDdc1Setup] >> kSetTimeLimitShift) == 3 && (s.r[kDdc1Setup] & kSetSendResetLength));
    CHECK((s.r[kDdc1Speed] >> kSpdPrescaleShift) == 500 && (s.r[kDdc1Speed] & kSpdThresholdMask) == 2 &&
          ((s.r[kDdc1Speed] & kSpdStartStopMask) >> kSpdStartStopShift) == 2);
    printf("[PASS] test_edid_two_blocks (256 bytes, engine released as Linux leaves it)\n");

    s = engine();
    s.edid = makeEdid(false);
    CHECK(readEdid(s, out, &len) == kOk && len == 128);
    printf("[PASS] test_edid_one_block\n");

    s = engine();   // nothing at 0x50
    CHECK(readEdid(s, out, &len) == kNack && len == 0);
    CHECK(((s.r[kArbitration] & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift) == kStatusIdle);
    printf("[PASS] test_no_monitor (NACK, engine released)\n");

    s = engine();
    s.edid = makeEdid(false);
    s.edid[127] ^= 1;
    CHECK(readEdid(s, out, &len) == kBadChecksum);
    s = engine();
    s.edid = makeEdid(false);
    s.edid[0] = 1;
    CHECK(readEdid(s, out, &len) == kBadHeader);
    printf("[PASS] test_bad_edid (header, checksum)\n");

    s = engine();
    s.edid = makeEdid(false);
    s.r[kGpioDdc1Mask] = kGpioAuxPadMode;
    CHECK(readEdid(s, out, &len) == kPadNotI2c && s.goCount == 0 && !s.r.count(kControl));
    s = engine();
    s.edid = makeEdid(false);
    s.r[kArbitration] = kRwCntlDmcuOnly << kArbRwCntlStatusShift;
    CHECK(readEdid(s, out, &len) == kEngineUnavailable && s.goCount == 0);
    s = engine();
    s.edid = makeEdid(false);
    s.denyArbitration = true;
    CHECK(readEdid(s, out, &len) == kNotAcquired && s.goCount == 0);
    s = engine();
    s.edid = makeEdid(false);
    s.r[kMicrosecondTimeBaseDiv] = 0;
    CHECK(readEdid(s, out, &len) == kNoTimeBase && s.goCount == 0);
    printf("[PASS] test_refusals (AUX pad, DMCU-held, not granted, no time base: nothing sent)\n");

    printf("=== All tests PASSED successfully ===\n");
    return 0;
}
