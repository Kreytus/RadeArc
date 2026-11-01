// dml314_run.c: run Linux's DCN 3.1.4 Display Mode Library (unmodified, tools/dml/fetch_dml.sh) for one HDMI display
// on the Ryzen 5 8500G, the way amdgpu's DC drives it, and print what DC would program: DISPCLK/DPPCLK/DCFCLK, the
// OTG vstartup/vupdate/vready, the watermarks and the HUBP DLG/TTU/RQ register values. Phase 3b, Test L.
//
// Call chain mirrored (Linux v6.17), each step commented where it is reproduced below:
//   dcn314_clk_mgr_helper_populate_bw_params (clk_mgr/dcn314/dcn314_clk_mgr.c)  - the SMU DPM table -> clk_table
//   dcn314_update_bw_bounding_box_fpu (dml/dcn314/dcn314_fpu.c)                 - clk_table -> soc clock_limits
//   dml_init_instance (dml/display_mode_lib.c), DML_PROJECT_DCN314 -> dml314_funcs
//   dcn314_validate_bandwidth -> dcn30_internal_validate_bw (resource/dcn30/dcn30_resource.c):
//     dcn31_update_soc_for_wm_a, dcn314_populate_dml_pipes_from_context_fpu (-> dcn31x_ -> dcn20_populate_...),
//     dml_get_voltage_level, dcn20_validate_apply_pipe_split_flags (checked: one unsplit pipe, else refused)
//   dcn31_calculate_wm_and_dlg_fp (dml/dcn31/dcn31_fpu.c) -> dcn20_calculate_dlg_params (dml/dcn20/dcn20_fpu.c)
//
// Machine inputs (not Linux constants), each from a dump of the 8500G:
//   - the SMU DPM table: hardware/dumps/b650m_8500g/testclk.txt (TransferTableSmu2Dram TABLE_DPMCLOCKS)
//   - VBIOS integratedsysteminfo v2.2: memorytype 0x22 (DDR5), umachannelnumber 0; dce_info v4.4: dce_refclk 24 MHz
//   - the plane: GOP's/macOS's scanout, 1920x1080 ARGB8888 linear (no DCC); the timing: the EDID DTD
//   - DCHUB refclk: hubbub31_get_dchub_ref_freq gives 24000 kHz, or 12000 if DCHUBBUB_GLOBAL_TIMER_REFDIV == 2
//     (--refdiv2); Test L's dry run reads the register.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display_mode_lib.h"
#include "display_mode_vba.h"
#include "dcn314/display_mode_vba_314.h"
#include "dcn314/display_rq_dlg_calc_314.h"
#include "dcn314_ipsoc.h"   // dcn3_14_ip and dcn3_14_soc, extracted verbatim from dcn314_fpu.c by build.sh

// ---- the SMU DPM table (DpmClocks314_t) from testclk.txt ----
static const unsigned DcfClocks[8] = {200, 400, 436, 533, 600, 685, 800, 960};
static const unsigned DispClocks[8] = {600, 685, 800, 800, 960, 960, 960, 1200};
static const unsigned DppClocks[8] = {600, 685, 800, 800, 960, 960, 960, 1200};
static const unsigned SocClocks[8] = {400, 600, 720, 800, 900, 1028, 1028, 1200};
static const unsigned SocVoltage[8] = {3000, 3080, 3120, 3159, 3260, 3360, 3600, 3960};
static const struct { unsigned FClk, MemClk, Voltage, WckRatio; } DfPstate[4] = {
    {2000, 2600, 3960, 0}, {500, 1000, 3000, 0}, {500, 1000, 3000, 0}, {0, 0, 3000, 0}};
static const unsigned NumDcfClkLevelsEnabled = 8, NumDispClkLevelsEnabled = 8, NumDfPstatesEnabled = 2;

struct clk_entry { unsigned dcfclk, fclk, memclk, voltage, socclk, dispclk, dppclk, phyclk, phyclk_d18, dtbclk, wck; };

static int valid_clk(unsigned v) { return v > 1 && v < 100000; }                          // is_valid_clock_value
static unsigned max_clk(const unsigned *c, unsigned n) { unsigned m = 0; for (unsigned i = 0; i < n; i++) if (c[i] > m) m = c[i]; return m; }
static unsigned wck_ratio(unsigned r) { return r == 1 ? 2 : r == 2 ? 4 : 1; }              // convert_wck_ratio

// dcn314_clk_mgr_helper_populate_bw_params. dcn314_bw_params starts with clk_table.num_entries = 4 and zeroed
// entries, so def_max is all zero and the defaults searched below are zero too.
static int populate_bw_params(struct clk_entry *e) {
    const unsigned def_entries = 4;
    unsigned max_pstate = 0, max_fclk = 0, min_pstate = 0, i;
    memset(e, 0, sizeof(struct clk_entry) * 8);
    for (i = 0; i < NumDfPstatesEnabled; i++)
        if (valid_clk(DfPstate[i].FClk) && DfPstate[i].FClk > max_fclk) { max_fclk = DfPstate[i].FClk; max_pstate = i; }
    const unsigned max_dispclk = max_clk(DispClocks, NumDispClkLevelsEnabled), max_dppclk = max_clk(DppClocks, NumDispClkLevelsEnabled);
    for (i = 0; i < NumDcfClkLevelsEnabled; i++) {
        unsigned min_fclk = DfPstate[0].FClk;
        for (unsigned j = 1; j < NumDfPstatesEnabled; j++)
            if (valid_clk(DfPstate[j].FClk) && DfPstate[j].FClk < min_fclk && DfPstate[j].Voltage <= SocVoltage[i]) {
                min_fclk = DfPstate[j].FClk;
                min_pstate = j;
            }
        // phyclk/phyclk_d18/dtbclk from the (zero) defaults
        e[i].fclk = min_fclk;
        e[i].memclk = DfPstate[min_pstate].MemClk;
        e[i].voltage = DfPstate[min_pstate].Voltage;
        e[i].dcfclk = DcfClocks[i];
        e[i].socclk = SocClocks[i];
        e[i].dispclk = max_dispclk;
        e[i].dppclk = max_dppclk;
        e[i].wck = wck_ratio(DfPstate[min_pstate].WckRatio);
    }
    (void)def_entries;
    if (max_pstate != min_pstate || i == 0) {
        if (i > 8 - 1) i = 8 - 1;   // MAX_NUM_DPM_LVL
        e[i].fclk = max_fclk;
        e[i].memclk = DfPstate[max_pstate].MemClk;
        e[i].voltage = DfPstate[max_pstate].Voltage;
        e[i].dcfclk = max_clk(DcfClocks, 8);
        e[i].socclk = max_clk(SocClocks, 8);
        e[i].dispclk = max_dispclk;
        e[i].dppclk = max_dppclk;
        e[i].wck = wck_ratio(DfPstate[max_pstate].WckRatio);
        i++;
    }
    const unsigned n = i--;
    e[i].socclk = max_clk(SocClocks, 8);
    e[i].dispclk = max_clk(DispClocks, 8);
    e[i].dppclk = max_clk(DppClocks, 8);
    // def_max is zero: phyclk/phyclk_d18/dtbclk stay 0, and the "set any 0 clocks to max default" loop sets 0s to 0
    return (int)n;
}

int main(int argc, char **argv) {
    // the mode: EDID DTD numbers (phx_dcn_timing.h prints them; modecheck's "mode N" lines)
    if (argc < 10) {
        fprintf(stderr, "usage: dml314_run pixel_khz h_active h_fp h_sync h_total v_active v_fp v_sync v_total [--refdiv2]\n");
        return 2;
    }
    const unsigned pix_khz = atoi(argv[1]), ha = atoi(argv[2]), hfp = atoi(argv[3]), hs = atoi(argv[4]), ht = atoi(argv[5]);
    const unsigned va = atoi(argv[6]), vfp = atoi(argv[7]), vs = atoi(argv[8]), vt = atoi(argv[9]);
    const int refdiv2 = argc > 10 && !strcmp(argv[10], "--refdiv2");
    const double refclk_mhz = (refdiv2 ? 12000 : 24000) / 1000.0;

    static struct display_mode_lib dml;
    struct _vcs_dpi_soc_bounding_box_st soc = dcn3_14_soc;
    struct _vcs_dpi_ip_params_st ip = dcn3_14_ip;

    // ---- dcn314_update_bw_bounding_box_fpu ----
    struct clk_entry ct[8];
    const int nent = populate_bw_params(ct);
    ip.max_num_otg = 4;    // dcn314 res_cap num_timing_generator
    ip.max_num_dpp = 4;    // pipe_count
    soc.dram_channel_width_bytes = 8;   // bw_params: memory_type 0x22 ? 8 : 4
    soc.num_chans = 4;                  // ma_channel_number 0 -> 4
    {
        int max_dispclk_mhz = 0, max_dppclk_mhz = 0;
        struct _vcs_dpi_voltage_scaling_st cl[DC__VOLTAGE_STATES];
        memcpy(cl, soc.clock_limits, sizeof(cl));
        for (int i = 0; i < nent; i++) {
            if ((int)ct[i].dispclk > max_dispclk_mhz) max_dispclk_mhz = ct[i].dispclk;
            if ((int)ct[i].dppclk > max_dppclk_mhz) max_dppclk_mhz = ct[i].dppclk;
        }
        for (int i = 0; i < nent; i++) {
            unsigned closest = 0;
            for (int j = soc.num_states - 1; j >= 0; j--)
                if ((unsigned)soc.clock_limits[j].dcfclk_mhz <= ct[i].dcfclk) { closest = j; break; }
            if (nent == 1) closest = soc.num_states - 1;
            cl[i].state = i;
            cl[i].dcfclk_mhz = ct[i].dcfclk;
            cl[i].fabricclk_mhz = ct[i].fclk;
            cl[i].socclk_mhz = ct[i].socclk;
            if (ct[i].memclk && ct[i].wck) cl[i].dram_speed_mts = ct[i].memclk * 2 * ct[i].wck;
            cl[i].dispclk_mhz = max_dispclk_mhz ? max_dispclk_mhz : soc.clock_limits[closest].dispclk_mhz;
            cl[i].dppclk_mhz = max_dppclk_mhz ? max_dppclk_mhz : soc.clock_limits[closest].dppclk_mhz;
            cl[i].dram_bw_per_chan_gbps = soc.clock_limits[closest].dram_bw_per_chan_gbps;
            cl[i].dscclk_mhz = soc.clock_limits[closest].dscclk_mhz;
            cl[i].dtbclk_mhz = soc.clock_limits[closest].dtbclk_mhz;
            cl[i].phyclk_d18_mhz = soc.clock_limits[closest].phyclk_d18_mhz;
            cl[i].phyclk_mhz = soc.clock_limits[closest].phyclk_mhz;
        }
        for (int i = 0; i < nent; i++) soc.clock_limits[i] = cl[i];
        soc.num_states = nent;
        if (max_dispclk_mhz) soc.dispclk_dppclk_vco_speed_mhz = max_dispclk_mhz * 2;
        // dcn20_patch_bounding_box: no bb_overrides -> no change
    }
    // dml_init_instance(DML_PROJECT_DCN314)
    dml.soc = soc;
    dml.ip = ip;
    dml.project = DML_PROJECT_DCN314;
    dml.funcs.validate = dml314_ModeSupportAndSystemConfigurationFull;
    dml.funcs.recalculate = dml314_recalculate;
    dml.funcs.rq_dlg_get_dlg_reg = dml314_rq_dlg_get_dlg_reg;
    dml.funcs.rq_dlg_get_rq_reg = dml314_rq_dlg_get_rq_reg;

    printf("== soc: %d states (from the SMU DPM table)\n", soc.num_states);
    for (int i = 0; i < soc.num_states; i++)
        printf("  state %d: dcfclk %.0f fclk %.0f socclk %.0f dram %.0f MT/s dispclk %.0f dppclk %.0f phyclk %.0f dtbclk %.0f\n", i,
               soc.clock_limits[i].dcfclk_mhz, soc.clock_limits[i].fabricclk_mhz, soc.clock_limits[i].socclk_mhz,
               soc.clock_limits[i].dram_speed_mts, soc.clock_limits[i].dispclk_mhz, soc.clock_limits[i].dppclk_mhz,
               soc.clock_limits[i].phyclk_mhz, soc.clock_limits[i].dtbclk_mhz);

    // ---- dcn30_internal_validate_bw ----
    dml.vba.maxMpcComb = 0;
    dml.vba.VoltageLevel = 0;
    dml.vba.DRAMClockChangeSupport[0][0] = dm_dram_clock_change_vactive;
    // dcn31_update_soc_for_wm_a: ddr5_wm_table WM_A (memory type 0x22 is not LpDdr5MemType)
    dml.soc.dram_clock_change_latency_us = 11.72;
    dml.soc.sr_enter_plus_exit_time_us = 14.5;
    dml.soc.sr_exit_time_us = 12.5;

    // ---- populate pipes: dcn20_populate_dml_pipes_from_context, then dcn31x_, then dcn314_ ----
    static display_e2e_pipe_params_st pipes[4];
    memset(pipes, 0, sizeof(pipes));
    display_e2e_pipe_params_st *p = &pipes[0];
    p->clks_cfg.refclk_mhz = refclk_mhz;
    p->pipe.dest.use_maximum_vstartup = 0;            // DCN_VERSION_2_01 only
    p->dout.dsc_enable = 0;
    p->dout.dsc_slices = 0;
    p->pipe.src.dcc = false;
    p->pipe.src.dcc_rate = 1;
    p->pipe.dest.synchronized_vblank_all_planes = true;   // one stream
    p->pipe.dest.synchronize_timings = true;
    p->pipe.dest.hblank_start = ht - hfp;
    p->pipe.dest.hblank_end = p->pipe.dest.hblank_start - ha;
    p->pipe.dest.vblank_start = vt - vfp;
    p->pipe.dest.vblank_end = p->pipe.dest.vblank_start - va;
    p->pipe.dest.htotal = ht;
    p->pipe.dest.vtotal = vt;
    p->pipe.dest.hactive = ha;
    p->pipe.dest.vactive = va;
    p->pipe.dest.interlaced = 0;
    p->pipe.dest.pixel_rate_mhz = pix_khz * 10 / 10000.0;   // pix_clk_100hz / 10000.0
    p->pipe.dest.otg_inst = 0;
    p->dout.dp_lanes = 4;
    p->dout.dp_rate = dm_dp_rate_na;
    p->dout.is_virtual = 0;
    p->pipe.dest.vtotal_min = 0;    // stream->adjust (no VRR)
    p->pipe.dest.vtotal_max = 0;
    p->pipe.dest.odm_combine = dm_odm_combine_mode_disabled;
    p->pipe.src.hsplit_grp = 0;
    p->dout.output_type = dm_hdmi;                     // SIGNAL_TYPE_HDMI_TYPE_A
    p->dout.output_format = dm_444;                    // RGB, 8 bpc
    p->dout.output_bpp = 8 * 3;
    p->dout.dsc_input_bpc = 12;
    p->dout.max_audio_sample_rate = 0;                 // no audio
    p->pipe.src.num_cursors = dml.ip.number_of_cursors;    // graphics plane: dc->dml.ip.number_of_cursors
    p->pipe.src.cur0_src_width = 256;
    p->pipe.src.cur0_bpp = dm_cur_32bit;
    // the plane (plane_state branch): full-screen, unscaled, not rotated, linear ARGB8888
    p->pipe.src.immediate_flip = false;                // pln->flip_immediate (dcn314 sets true below)
    p->pipe.src.is_hsplit = false;
    p->pipe.src.source_scan = dm_horz;
    p->pipe.src.source_rotation = dm_rotation_0;
    p->pipe.src.viewport_width = ha;
    p->pipe.src.viewport_height = va;
    p->pipe.src.viewport_width_max = ha;
    p->pipe.src.viewport_height_max = va;
    p->pipe.src.surface_width_y = ha;
    p->pipe.src.surface_height_y = va;
    p->pipe.src.data_pitch = ha;
    p->pipe.dest.recout_width = ha;
    p->pipe.dest.recout_height = va;
    p->pipe.dest.full_recout_width = ha;
    p->pipe.dest.full_recout_height = va;
    p->pipe.scale_ratio_depth.lb_depth = dm_lb_16;
    p->pipe.scale_ratio_depth.hscl_ratio = 1.0;
    p->pipe.scale_ratio_depth.vscl_ratio = 1.0;
    p->pipe.scale_ratio_depth.scl_enable = 0;
    p->pipe.scale_taps.htaps = 1;
    p->pipe.scale_taps.vtaps = 1;
    p->pipe.src.macro_tile_size = dm_4k_tile;          // swizzle_mode_to_macro_tile_size(DC_SW_LINEAR)
    p->pipe.src.sw_mode = dm_sw_linear;                // swizzle_to_dml_params(DC_SW_LINEAR)
    p->pipe.src.source_format = dm_444_32;
    // dcn31x_populate_dml_pipes_from_context
    p->pipe.src.gpuvm = 1;
    p->pipe.src.hostvm = 1;   // vm_pa_config.is_hvm_enabled = gpu_vm_support: amdgpu_sg_display -1 and an APU
    // dcn314_populate_dml_pipes_from_context_fpu
    {
        // micro_sec_to_vert_lines(VBlankNomDefaultUS, timing), dcn314_fpu.c
        const unsigned lines_time_in_ns = 1000.0 * (((float)ht * 1000.0) / ((float)(pix_khz * 10) / 10.0));
        const unsigned num_lines = (unsigned)ceil(1000.0 * ip.VBlankNomDefaultUS / lines_time_in_ns);
        const unsigned v_back_porch = vt - va - vfp - vs;   // get_vertical_back_porch
        p->pipe.dest.vtotal = vt;
        unsigned vbn = vt - p->pipe.dest.vactive;
        if (vbn > num_lines) vbn = num_lines;
        if (vbn < vs + v_back_porch + 2) vbn = vs + v_back_porch + 2;
        if (vbn > 1023) vbn = 1023;
        p->pipe.dest.vblank_nom = vbn;
        p->pipe.src.immediate_flip = true;
        p->pipe.src.unbounded_req_mode = false;
        p->pipe.src.dcc_fraction_of_zs_req_luma = 0;
        p->pipe.src.dcc_fraction_of_zs_req_chroma = 0;
        p->pipe.dest.vfront_porch = vfp;
        p->pipe.src.dcc_rate = 3;
        p->dout.dsc_input_bpc = 0;
        dml.ip.det_buffer_size_kbytes = 384;   // DCN3_14_DEFAULT_DET_SIZE
        // one pipe, rotation 0, not dual-plane, src width <= 5120 (disable_z9_mpc unset)
        dml.ip.det_buffer_size_kbytes = 192;
        p->pipe.src.unbounded_req_mode = true;
        printf("== pipe: %ux%u, htotal %u vtotal %u, %.3f MHz, vblank_nom %u, refclk %.0f MHz, DET %u KB unbounded\n", ha, va, ht,
               vt, p->pipe.dest.pixel_rate_mhz, vbn, refclk_mhz, dml.ip.det_buffer_size_kbytes);
    }
    const int pipe_cnt = 1;

    // dml_get_voltage_level with prefetch capped (validate_mode AND_PROGRAMMING)
    dml.soc.allow_dram_self_refresh_or_dram_clock_change_in_vblank = dm_allow_self_refresh_and_mclk_switch;
    int vlevel = dml_get_voltage_level(&dml, pipes, pipe_cnt);
    printf("== validate: vlevel %d of %d\n", vlevel, dml.soc.num_states);
    if (vlevel >= dml.soc.num_states) { printf("MODE NOT SUPPORTED by DML\n"); return 3; }
    // dcn20_validate_apply_pipe_split_flags with MPC_SPLIT_DYNAMIC: a split only if DML wants >1 DPP
    printf("  NoOfDPP %u, ODMCombine %d, maxMpcComb %d\n", (unsigned)dml.vba.NoOfDPP[vlevel][dml.vba.maxMpcComb][0],
           (int)dml.vba.ODMCombineEnabled[0], dml.vba.maxMpcComb);
    if (dml.vba.NoOfDPP[vlevel][dml.vba.maxMpcComb][0] != 1 || dml.vba.ODMCombineEnabled[0] != dm_odm_combine_mode_disabled) {
        printf("DML wants a pipe split or ODM combine: not supported by the single-pipe takeover\n");
        return 4;
    }

    // ---- dcn31_calculate_wm_and_dlg_fp ----
    double dcfclk = dml.vba.DCFCLKState[vlevel][dml.vba.maxMpcComb];
    if (dml.soc.min_dcfclk > dcfclk) dcfclk = dml.soc.min_dcfclk;
    pipes[0].clks_cfg.voltage = vlevel;
    pipes[0].clks_cfg.dcfclk_mhz = dcfclk;
    pipes[0].clks_cfg.socclk_mhz = dml.soc.clock_limits[vlevel].socclk_mhz;
    unsigned z8_ns = get_wm_z8_stutter_enter_exit(&dml, pipes, pipe_cnt) * 1000;
    if (get_stutter_period(&dml, pipes, pipe_cnt) < 2100 && z8_ns < 2100 * 1000) z8_ns = 2100 * 1000;   // minimum_z8_residency_time
    // update_soc_for_wm_a again (same values)
    const double wm_urgent = get_wm_urgent(&dml, pipes, pipe_cnt) * 1000;
    const double wm_sr_enter = get_wm_stutter_enter_exit(&dml, pipes, pipe_cnt) * 1000;
    const double wm_sr_exit = get_wm_stutter_exit(&dml, pipes, pipe_cnt) * 1000;
    const double wm_pstate = get_wm_dram_clock_change(&dml, pipes, pipe_cnt) * 1000;
    const double wm_z8_exit = get_wm_z8_stutter_exit(&dml, pipes, pipe_cnt) * 1000;
    const double wm_pte = get_wm_memory_trip(&dml, pipes, pipe_cnt) * 1000;
    const double frac_nom = get_fraction_of_urgent_bandwidth(&dml, pipes, pipe_cnt) * 1000;
    const double frac_flip = get_fraction_of_urgent_bandwidth_imm_flip(&dml, pipes, pipe_cnt) * 1000;
    const double urg_lat = get_urgent_latency(&dml, pipes, pipe_cnt) * 1000;
    pipes[0].clks_cfg.dispclk_mhz = get_dispclk_calculated(&dml, pipes, pipe_cnt);
    pipes[0].clks_cfg.dppclk_mhz = get_dppclk_calculated(&dml, pipes, pipe_cnt, 0);
    // dc->debug.min_disp_clk_khz / min_dpp_clk_khz: unset for dcn314

    // ---- dcn20_calculate_dlg_params ----
    const unsigned dispclk_khz = dml.vba.DISPCLK * 1000, dcfclk_khz = dml.vba.DCFCLK * 1000, socclk_khz = dml.vba.SOCCLK * 1000;
    const unsigned dramclk_khz = dml.vba.DRAMSpeed * 1000 / 16, ds_khz = dml.vba.DCFCLKDeepSleep * 1000, fclk_khz = dml.vba.FabricClock * 1000;
    const int pstate = dml.vba.DRAMClockChangeSupport[vlevel][dml.vba.maxMpcComb] != dm_dram_clock_change_unsupported;
    pipes[0].pipe.dest.vstartup_start = get_vstartup(&dml, pipes, pipe_cnt, 0);
    pipes[0].pipe.dest.vupdate_offset = get_vupdate_offset(&dml, pipes, pipe_cnt, 0);
    pipes[0].pipe.dest.vupdate_width = get_vupdate_width(&dml, pipes, pipe_cnt, 0);
    pipes[0].pipe.dest.vready_offset = get_vready_offset(&dml, pipes, pipe_cnt, 0);
    const unsigned dppclk_khz = pipes[0].clks_cfg.dppclk_mhz * 1000;
    const int cstate_en = dml.vba.PrefetchMode[vlevel][dml.vba.maxMpcComb] != 2;
    display_dlg_regs_st dlg;
    display_ttu_regs_st ttu;
    display_rq_regs_st rq;
    memset(&dlg, 0, sizeof(dlg)); memset(&ttu, 0, sizeof(ttu)); memset(&rq, 0, sizeof(rq));
    // dcn31: p_state_change_support = DRAMClockChangeSupport == vactive (after dcn20_calculate_dlg_params, but the
    // dlg call inside uses dcn20's value: != unsupported)
    dml.funcs.rq_dlg_get_dlg_reg(&dml, &dlg, &ttu, pipes, pipe_cnt, 0, cstate_en, pstate, false, false, true);
    dml.funcs.rq_dlg_get_rq_reg(&dml, &rq, &pipes[0].pipe);

    printf("== clocks (dcn20_calculate_dlg_params)\n");
    printf("  dispclk_khz %u  dppclk_khz %u  dcfclk_khz %u  dcfclk_deep_sleep_khz %u  socclk_khz %u  fclk_khz %u  dramclk_khz %u\n",
           dispclk_khz, dppclk_khz, dcfclk_khz, ds_khz, socclk_khz, fclk_khz, dramclk_khz);
    printf("  p_state_change_support %d (dcn31: vactive only -> %d), PrefetchMode %d\n", pstate,
           dml.vba.DRAMClockChangeSupport[vlevel][dml.vba.maxMpcComb] == dm_dram_clock_change_vactive,
           (int)dml.vba.PrefetchMode[vlevel][dml.vba.maxMpcComb]);
    printf("== OTG (pipe_dlg_param)\n");
    printf("  vstartup_start %u  vupdate_offset %u  vupdate_width %u  vready_offset %u  pstate_keepout %u\n",
           pipes[0].pipe.dest.vstartup_start, pipes[0].pipe.dest.vupdate_offset, pipes[0].pipe.dest.vupdate_width,
           pipes[0].pipe.dest.vready_offset, pipes[0].pipe.dest.pstate_keepout);
    printf("== watermarks set A (ns)\n");
    printf("  urgent %.0f  sr_enter_plus_exit %.0f  sr_exit %.0f  pstate_change %.0f  z8_enter_plus_exit %u  z8_exit %.0f\n",
           wm_urgent, wm_sr_enter, wm_sr_exit, wm_pstate, z8_ns, wm_z8_exit);
    printf("  pte_meta_urgent %.0f  frac_urg_bw_nom %.0f  frac_urg_bw_flip %.0f  urgent_latency %.0f\n", wm_pte, frac_nom,
           frac_flip, urg_lat);
    printf("== DLG regs\n");
#define D(f) printf("  dlg.%-40s %u\n", #f, (unsigned)dlg.f)
    D(refcyc_h_blank_end); D(dlg_vblank_end); D(min_dst_y_next_start); D(min_dst_y_next_start_us); D(refcyc_per_htotal);
    D(refcyc_x_after_scaler); D(dst_y_after_scaler); D(dst_y_prefetch); D(dst_y_per_vm_vblank); D(dst_y_per_row_vblank);
    D(dst_y_per_vm_flip); D(dst_y_per_row_flip); D(ref_freq_to_pix_freq); D(vratio_prefetch); D(vratio_prefetch_c);
    D(refcyc_per_pte_group_vblank_l); D(refcyc_per_pte_group_vblank_c); D(refcyc_per_meta_chunk_vblank_l);
    D(refcyc_per_meta_chunk_vblank_c); D(refcyc_per_pte_group_flip_l); D(refcyc_per_pte_group_flip_c);
    D(refcyc_per_meta_chunk_flip_l); D(refcyc_per_meta_chunk_flip_c); D(dst_y_per_pte_row_nom_l);
    D(dst_y_per_pte_row_nom_c); D(refcyc_per_pte_group_nom_l); D(refcyc_per_pte_group_nom_c);
    D(dst_y_per_meta_row_nom_l); D(dst_y_per_meta_row_nom_c); D(refcyc_per_meta_chunk_nom_l);
    D(refcyc_per_meta_chunk_nom_c); D(refcyc_per_line_delivery_pre_l); D(refcyc_per_line_delivery_pre_c);
    D(refcyc_per_line_delivery_l); D(refcyc_per_line_delivery_c); D(refcyc_per_vm_group_vblank);
    D(refcyc_per_vm_group_flip); D(refcyc_per_vm_req_vblank); D(refcyc_per_vm_req_flip); D(dst_y_offset_cur0);
    D(chunk_hdl_adjust_cur0); D(dst_y_delta_drq_limit); D(xfc_reg_transfer_delay); D(xfc_reg_precharge_delay);
    D(xfc_reg_remote_surface_flip_latency); D(xfc_reg_prefetch_margin);
#undef D
    printf("== TTU regs\n");
#define T(f) printf("  ttu.%-40s %u\n", #f, (unsigned)ttu.f)
    T(qos_level_low_wm); T(qos_level_high_wm); T(min_ttu_vblank); T(qos_level_flip); T(refcyc_per_req_delivery_l);
    T(refcyc_per_req_delivery_c); T(refcyc_per_req_delivery_cur0); T(refcyc_per_req_delivery_cur1);
    T(refcyc_per_req_delivery_pre_l); T(refcyc_per_req_delivery_pre_c); T(refcyc_per_req_delivery_pre_cur0);
    T(refcyc_per_req_delivery_pre_cur1); T(qos_level_fixed_l); T(qos_level_fixed_c); T(qos_level_fixed_cur0);
    T(qos_level_fixed_cur1); T(qos_ramp_disable_l); T(qos_ramp_disable_c); T(qos_ramp_disable_cur0);
    T(qos_ramp_disable_cur1);
#undef T
    printf("== RQ regs\n");
    printf("  rq.drq_expansion_mode %u prq %u mrq %u crq %u plane1_base_address %u\n", rq.drq_expansion_mode,
           rq.prq_expansion_mode, rq.mrq_expansion_mode, rq.crq_expansion_mode, rq.plane1_base_address);
#define R(s, f) printf("  rq.%s.%-28s %u\n", #s, #f, (unsigned)rq.s.f)
    R(rq_regs_l, chunk_size); R(rq_regs_l, min_chunk_size); R(rq_regs_l, meta_chunk_size); R(rq_regs_l, min_meta_chunk_size);
    R(rq_regs_l, dpte_group_size); R(rq_regs_l, mpte_group_size); R(rq_regs_l, swath_height); R(rq_regs_l, pte_row_height_linear);
#undef R
    return 0;
}
