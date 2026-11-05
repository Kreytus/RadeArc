// test_phx_hdmi_avi.cpp: phx_hdmi_avi.h against Linux's own AVI InfoFrame construction. The union below is copied
// verbatim from Linux v6.17 drivers/gpu/drm/amd/display/include/set_mode_types.h (MIT, AMD), and fill() repeats
// set_avi_info_frame's assignments for the stream the 8500G drives. Both must give the same bytes for the 60 Hz
// (VIC 16) and 144 Hz (VIC 0) modes, and the checksum must make the packet sum to 0.
#include "phx_hdmi_avi.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- verbatim from include/set_mode_types.h ----
struct hdmi_info_frame_header {
	uint8_t info_frame_type;
	uint8_t version;
	uint8_t length;
};
#pragma pack(push)
#pragma pack(1)
struct info_packet_raw_data {
	uint8_t hb0;
	uint8_t hb1;
	uint8_t hb2;
	uint8_t sb[28]; /* sb0~sb27 */
};
union hdmi_info_packet {
	struct avi_info_frame {
		struct hdmi_info_frame_header header;
		uint8_t CHECK_SUM:8;
		uint8_t S0_S1:2;
		uint8_t B0_B1:2;
		uint8_t A0:1;
		uint8_t Y0_Y1_Y2:3;
		uint8_t R0_R3:4;
		uint8_t M0_M1:2;
		uint8_t C0_C1:2;
		uint8_t SC0_SC1:2;
		uint8_t Q0_Q1:2;
		uint8_t EC0_EC2:3;
		uint8_t ITC:1;
		uint8_t VIC0_VIC7:8;
		uint8_t PR0_PR3:4;
		uint8_t CN0_CN1:2;
		uint8_t YQ0_YQ1:2;
		uint16_t bar_top;
		uint16_t bar_bottom;
		uint16_t bar_left;
		uint16_t bar_right;
		uint8_t FR0_FR3:4;
		uint8_t ACE0_ACE3:4;
		uint8_t RID0_RID5:6;
		uint8_t FR4:1;
		uint8_t F157:1;
		uint8_t reserved[12];
	} bits;
	struct info_packet_raw_data packet_raw_data;
};
#pragma pack(pop)
// ---- end verbatim ----

// set_avi_info_frame's assignments for RGB / sRGB / no 3D / no RID / content No Data / qs_bit 0 / no borders
static void fill(union hdmi_info_packet *h, unsigned vic, unsigned aspect, unsigned hTotal, unsigned vTotal) {
    memset(h, 0, sizeof *h);
    h->bits.header.info_frame_type = 0x82;   // HDMI_INFOFRAME_TYPE_AVI (include/linux/hdmi.h)
    h->bits.header.version = 2;
    h->bits.header.length = 13;              // HDMI_AVI_INFOFRAME_SIZE
    h->bits.Y0_Y1_Y2 = 0;
    h->bits.A0 = 1;                          // ACTIVE_FORMAT_VALID (dc/inc/hw/transform.h)
    h->bits.B0_B1 = 3;                       // BAR_INFO_BOTH_VALID
    h->bits.SC0_SC1 = 0;                     // PICTURE_SCALING_UNIFORM
    h->bits.S0_S1 = 2;                       // SCANNING_TYPE_UNDERSCAN (dc_hw_types.h)
    h->bits.C0_C1 = 0;                       // COLORIMETRY_NO_DATA
    h->bits.M0_M1 = (aspect == 1 || aspect == 2) ? aspect : 0;
    h->bits.R0_R3 = 8;                       // ACTIVE_FORMAT_ASPECT_RATIO_SAME_AS_PICTURE
    h->bits.CN0_CN1 = 0;
    h->bits.ITC = 1;
    h->bits.Q0_Q1 = 0;                       // RGB_QUANTIZATION_DEFAULT_RANGE
    h->bits.YQ0_YQ1 = 0;                     // YYC_QUANTIZATION_LIMITED_RANGE
    h->bits.VIC0_VIC7 = vic;
    if (vic >= 128) h->bits.header.version = 3;
    h->bits.PR0_PR3 = 0;
    h->bits.bar_top = 0;
    h->bits.bar_bottom = vTotal - 0 + 1;
    h->bits.bar_left = 0;
    h->bits.bar_right = hTotal - 0 + 1;
    h->bits.ACE0_ACE3 = 0;
    uint8_t *cs = &h->packet_raw_data.sb[0];
    *cs = 0x82 + h->bits.header.length + h->bits.header.version;
    for (unsigned i = 1; i <= h->bits.header.length; i++) *cs += h->packet_raw_data.sb[i];
    *cs = (uint8_t)(0x100 - *cs);
}

static void compare(unsigned vic, unsigned aspect, unsigned ht, unsigned vt) {
    union hdmi_info_packet h;
    fill(&h, vic, aspect, ht, vt);
    const phxavi::InfoPacket p = phxavi::avi(vic, aspect, ht, vt);
    CHECK(p.valid && p.hb0 == h.packet_raw_data.hb0 && p.hb1 == h.packet_raw_data.hb1 && p.hb2 == h.packet_raw_data.hb2 && p.hb3 == 0);
    CHECK(memcmp(p.sb, h.packet_raw_data.sb, 28) == 0);
    uint8_t sum = (uint8_t)(p.hb0 + p.hb1 + p.hb2);
    for (unsigned i = 0; i <= p.hb2; i++) sum = (uint8_t)(sum + p.sb[i]);
    CHECK(sum == 0);
}

int main() {
    static_assert(sizeof(union hdmi_info_packet) == 31, "Linux's union: 3 header bytes + 28 data bytes");
    compare(16, 0, 2200, 1125);   // 1920x1080@60, EDID mode 0 = CTA VIC 16, aspect from the mode: none
    compare(0, 0, 2080, 1121);    // 1920x1080@144, EDID mode 1: no CTA VIC
    compare(16, 2, 2200, 1125);   // the same with a 16:9 picture aspect (a client with the aspect-ratio cap)
    const phxavi::InfoPacket p = phxavi::avi(16, 0, 2200, 1125);
    printf("test_phx_hdmi_avi: %s (60 Hz AVI: %02x %02x %02x |", fails ? "FAILED" : "OK", p.hb0, p.hb1, p.hb2);
    for (int i = 0; i < 14; i++) printf(" %02x", p.sb[i]);
    printf(")\n");
    return fails ? 1 : 0;
}
