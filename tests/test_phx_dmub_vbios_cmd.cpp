// tests/test_phx_dmub_vbios_cmd.cpp: phxdmubcmd (src/amd/phx_dmub_vbios_cmd.h) against Linux's own structures. The
// structs below are copied verbatim from Linux v6.17 dmub/inc/dmub_cmd.h and include/atomfirmware.h; each command is
// built a second time by filling them the way command_table2.c does (memset, header, payload) and the bytes must be
// identical. Values: the 8500G's HDMI port at 144/165/180 Hz (EDID DTDs) and GOP's 60 Hz.
#include "phx_dmub_vbios_cmd.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #c);       \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

namespace linux_ref {
// ---- dmub_cmd.h ----
struct dmub_cmd_header {
    unsigned int type : 8;
    unsigned int sub_type : 8;
    unsigned int ret_status : 1;
    unsigned int multi_cmd_pending : 1;
    unsigned int is_reg_based : 1;
    unsigned int reserved0 : 5;
    unsigned int payload_bytes : 6;
    unsigned int reserved1 : 2;
};
// ---- atomfirmware.h ----
struct dig_transmitter_control_parameters_v1_6 {
    uint8_t phyid;
    uint8_t action;
    union { uint8_t digmode; uint8_t dplaneset; } mode_laneset;
    uint8_t lanenum;
    uint32_t symclk_10khz;
    uint8_t hpdsel;
    uint8_t digfe_sel;
    uint8_t connobj_id;
    uint8_t reserved;
    uint32_t reserved1;
};
struct set_pixel_clock_parameter_v1_7 {
    uint32_t pixclk_100hz;
    uint8_t pll_id;
    uint8_t encoderobjid;
    uint8_t encoder_mode;
    uint8_t miscinfo;
    uint8_t crtc_id;
    uint8_t deep_color_ratio;
    uint8_t reserved1[2];
    uint32_t reserved2;
};
struct dig_encoder_stream_setup_parameters_v1_5 {
    uint8_t digid;
    uint8_t action;
    uint8_t digmode;
    uint8_t lanenum;
    uint32_t pclk_10khz;
    uint8_t bitpercolor;
    uint8_t dplinkrate_270mhz;
    uint8_t reserved[2];
};
struct dig_encoder_generic_cmd_parameters_v1_5 { uint8_t digid; uint8_t action; uint8_t reserved1[2]; uint32_t reserved2[2]; };
struct dig_encoder_link_setup_parameters_v1_5 {
    uint8_t digid; uint8_t action; uint8_t digmode; uint8_t lanenum; uint8_t symclk_10khz; uint8_t hpd_sel;
    uint8_t digfe_sel; uint8_t reserved[2];
};
struct dp_panel_mode_set_parameters_v1_5 { uint8_t digid; uint8_t action; uint8_t panelmode; uint8_t reserved1; uint32_t reserved2[2]; };
union dig_encoder_control_parameters_v1_5 {
    struct dig_encoder_generic_cmd_parameters_v1_5 cmd_param;
    struct dig_encoder_stream_setup_parameters_v1_5 stream_param;
    struct dig_encoder_link_setup_parameters_v1_5 link_param;
    struct dp_panel_mode_set_parameters_v1_5 dppanel_param;
};
// ---- dmub_cmd.h ----
struct dmub_cmd_digx_encoder_control_data { union dig_encoder_control_parameters_v1_5 dig; };
struct dmub_rb_cmd_digx_encoder_control {
    struct dmub_cmd_header header;
    struct dmub_cmd_digx_encoder_control_data encoder_control;
};
struct dmub_dig_transmitter_control_data_v1_7 {
    uint8_t phyid;
    uint8_t action;
    union { uint8_t digmode; uint8_t dplaneset; } mode_laneset;
    uint8_t lanenum;
    union { uint32_t symclk_10khz; } symclk_units;
    uint8_t hpdsel;
    uint8_t digfe_sel;
    uint8_t connobj_id;
    uint8_t HPO_instance;
    uint8_t reserved1;
    uint8_t skip_phy_ssc_reduction;
    uint8_t reserved2[2];
    uint32_t reserved3[11];
};
union dmub_cmd_dig1_transmitter_control_data {
    struct dig_transmitter_control_parameters_v1_6 dig;
    struct dmub_dig_transmitter_control_data_v1_7 dig_v1_7;
};
struct dmub_rb_cmd_dig1_transmitter_control {
    struct dmub_cmd_header header;
    union dmub_cmd_dig1_transmitter_control_data transmitter_control;
};
struct dmub_cmd_set_pixel_clock_data { struct set_pixel_clock_parameter_v1_7 clk; };
struct dmub_rb_cmd_set_pixel_clock {
    struct dmub_cmd_header header;
    struct dmub_cmd_set_pixel_clock_data pixel_clock;
};
union dmub_rb_cmd {
    struct dmub_rb_cmd_digx_encoder_control digx_encoder_control;
    struct dmub_rb_cmd_dig1_transmitter_control dig1_transmitter_control;
    struct dmub_rb_cmd_set_pixel_clock set_pixel_clock;
    uint8_t raw[64];
};
} // namespace linux_ref

using namespace phxdmubcmd;

// transmitter_control_dmcub_v1_7 + transmitter_control_v1_7, for the cntl dcn10_link_encoder_enable_tmds_output fills
static void refEnable(uint8_t *out, uint8_t digmode, uint32_t phyPixKhz) {
    linux_ref::dmub_rb_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.dig1_transmitter_control.header.type = 128;
    cmd.dig1_transmitter_control.header.sub_type = 1;
    cmd.dig1_transmitter_control.header.payload_bytes =
        sizeof(cmd.dig1_transmitter_control) - sizeof(cmd.dig1_transmitter_control.header);
    auto &d = cmd.dig1_transmitter_control.transmitter_control.dig_v1_7;
    d.phyid = 0;
    d.action = 1;
    d.mode_laneset.digmode = digmode;
    d.lanenum = 4;
    d.hpdsel = 1;
    d.digfe_sel = 0;
    d.connobj_id = 0;
    d.HPO_instance = (uint8_t)(0 - 10);
    d.symclk_units.symclk_10khz = phyPixKhz / 10;
    memcpy(out, &cmd, 64);
}

static void refPixelClock(uint8_t *out, uint8_t mode, uint32_t pixelKhz) {
    linux_ref::dmub_rb_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.set_pixel_clock.header.type = 128;
    cmd.set_pixel_clock.header.sub_type = 2;
    cmd.set_pixel_clock.header.payload_bytes = sizeof(cmd.set_pixel_clock) - sizeof(cmd.set_pixel_clock.header);
    auto &c = cmd.set_pixel_clock.pixel_clock.clk;
    c.crtc_id = 0;
    c.pll_id = 20;
    c.encoderobjid = 0x1E;
    c.encoder_mode = mode;
    c.pixclk_100hz = pixelKhz * 10;
    c.deep_color_ratio = 0;
    memcpy(out, &cmd, 64);
}

int main() {
    printf("=== Running DMUB VBIOS command tests (dmub_cmd.h layouts) ===\n");
    using namespace linux_ref;
    CHECK(sizeof(dmub_cmd_header) == 4);
    CHECK(sizeof(dmub_rb_cmd_dig1_transmitter_control) == 64);
    CHECK(sizeof(dmub_rb_cmd_dig1_transmitter_control) - sizeof(dmub_cmd_header) == kPayloadTransmitter);
    CHECK(sizeof(dmub_rb_cmd_set_pixel_clock) - sizeof(dmub_cmd_header) == kPayloadPixelClock);
    CHECK(offsetof(dmub_dig_transmitter_control_data_v1_7, symclk_units) == 4);
    CHECK(offsetof(dmub_dig_transmitter_control_data_v1_7, HPO_instance) == 11);
    CHECK(offsetof(set_pixel_clock_parameter_v1_7, crtc_id) == 8);
    printf("[PASS] test_layout (transmitter 4+%u, pixel clock 4+%u)\n", kPayloadTransmitter, kPayloadPixelClock);

    // the EDID's modes (kHz): GOP's 60 Hz, then 144, 165, 180 Hz
    const uint32_t clocks[4] = {148500, 335770, 384730, 415580};
    for (uint32_t k : clocks) {
        uint8_t a[64], b[64];
        transmitterEnable(a, Signal::Hdmi, k);
        refEnable(b, 3, k);
        CHECK(!memcmp(a, b, 64));
        setPixelClock(a, Signal::Hdmi, k);
        refPixelClock(b, 3, k);
        CHECK(!memcmp(a, b, 64));
        transmitterEnable(a, Signal::Dvi, k);
        refEnable(b, 2, k);
        CHECK(!memcmp(a, b, 64));
    }
    uint8_t c[64];
    transmitterEnable(c, Signal::Hdmi, 335770);
    CHECK(c[0] == 0x80 && c[1] == 0x01 && c[2] == 0x00 && c[3] == 60);   // header 0x3c000180
    CHECK(c[4] == 0 && c[5] == 1 && c[6] == 3 && c[7] == 4);              // UNIPHY A, ENABLE, HDMI, 4 lanes
    CHECK((c[8] | c[9] << 8 | c[10] << 16) == 33577 && c[12] == 1 && c[15] == 0xF6);
    printf("[PASS] test_transmitter_enable (144 Hz: symclk 33577 x 10 kHz, HPD1, HPO 0xF6)\n");

    setPixelClock(c, Signal::Hdmi, 415580);
    CHECK(c[0] == 0x80 && c[1] == 0x02 && c[3] == 16);
    CHECK((c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24) == 4155800 && c[8] == 20 && c[9] == 0x1E && c[10] == 3);
    printf("[PASS] test_set_pixel_clock (180 Hz: 4155800 x 100 Hz, COMBOPHY_PLL0, UNIPHY, HDMI, CRTC1)\n");

    {
        uint8_t a[64];
        transmitterDisable(a, Signal::Dvi);
        linux_ref::dmub_rb_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.dig1_transmitter_control.header.type = 128;
        cmd.dig1_transmitter_control.header.sub_type = 1;
        cmd.dig1_transmitter_control.header.payload_bytes = 60;
        auto &d = cmd.dig1_transmitter_control.transmitter_control.dig_v1_7;
        d.action = 0;
        d.mode_laneset.digmode = 2;
        d.hpdsel = 1;
        d.connobj_id = 0x0C;
        d.HPO_instance = 0xF6;
        CHECK(!memcmp(a, &cmd, 64));
    }
    printf("[PASS] test_transmitter_disable (DVI, connector 0x0C)\n");

    // encoder_control_dmcub + encoder_control_digx_v1_5 for enc314_stream_encoder_hdmi_set_stream_attribute's cntl
    for (uint32_t k : clocks) {
        uint8_t a[64];
        encoderStreamSetup(a, Signal::Hdmi, k);
        linux_ref::dmub_rb_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.digx_encoder_control.header.type = 128;
        cmd.digx_encoder_control.header.sub_type = 0;
        cmd.digx_encoder_control.header.payload_bytes =
            sizeof(cmd.digx_encoder_control) - sizeof(cmd.digx_encoder_control.header);
        dig_encoder_stream_setup_parameters_v1_5 p{};
        p.digid = 0;
        p.action = 0x0F;
        p.pclk_10khz = k / 10;
        p.digmode = 3;
        p.lanenum = 4;
        cmd.digx_encoder_control.encoder_control.dig.stream_param = p;
        CHECK(!memcmp(a, &cmd, 64));
    }
    CHECK(sizeof(dmub_rb_cmd_digx_encoder_control) - sizeof(dmub_cmd_header) == kPayloadEncoder);
    printf("[PASS] test_encoder_stream_setup (HDMI, DIG0, STREAM_SETUP 0x0F, 4 lanes)\n");
    printf("=== All tests PASSED successfully ===\n");
    return 0;
}
