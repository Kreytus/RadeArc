#!/usr/bin/env python3
"""gen_dcn314_regs.py: the HUBP0 / OTG0 register fields Linux programs from DML for one pipe, with their addresses,
shifts and masks taken from Linux v6.17's own headers (tools/dml/fetch_regs.sh). Writes src/amd/phx_dcn314_prog_regs.h.

Which register sits in which block (HUBP / HUBPREQ / HUBPRET / OTG) comes from expanding dcn314_resource.c's lists,
HUBP_REG_LIST_DCN30(id) and OPTC_COMMON_REG_LIST_DCN3_14(inst), so the name resolves exactly as SRI() resolves it:
reg<block>0_<reg> + DCN_BASE segment <reg..._BASE_IDX>. Field shifts/masks: <block>0_<reg>__<FIELD>__SHIFT/_MASK,
the instance-0 names dcn314's hubp_shift/hubp_mask and optc_shift/optc_mask use for every instance.

The PROG table copies the order and the DML source of every field from the Linux functions DCN 3.1.4 calls:
  hubp3_setup (dcn30_hubp.c): hubp2_vready_at_or_After_vsync, hubp21_program_requestor, hubp3_program_deadline
  hubp2_setup_interdependent (dcn20_hubp.c)
  optc1_program_global_sync (dcn10_optc.c, dcn314_optc.c .program_global_sync)
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SRC = os.path.join(ROOT, "build", "linux-src")
OUT = os.path.join(ROOT, "src", "amd", "phx_dcn314_prog_regs.h")
# DMU 3.1.4 segment bases on the 8500G (IP discovery; the same bases modecheck/clkinfo use)
SEG = {0: 0x12, 1: 0xC0, 2: 0x34C0}

# (Linux function, register, kind, [(FIELD, DML source)])
#   kind SET    = REG_SET_n: the register is written whole, fields not named are 0
#   kind UPDATE = REG_UPDATE: read-modify-write of the named fields only
#   kind WRITE  = REG_WRITE of a constant (source "const:<value>", field None = the whole register)
# DML sources are dml314_run's names (dlg./ttu./rq./dest.); "calc.vready_after_vsync" is hubp2_vready_at_or_After_vsync's
# formula over dest.*.
PROG = [
    ("hubp2_vready_at_or_After_vsync", "HUBPREQ_DEBUG_DB", "WRITE", [(None, "const:256")]),
    ("hubp2_vready_at_or_After_vsync", "DCHUBP_CNTL", "UPDATE", [("HUBP_VREADY_AT_OR_AFTER_VSYNC", "calc.vready_after_vsync")]),
    ("hubp21_program_requestor", "HUBPRET_CONTROL", "UPDATE", [("DET_BUF_PLANE1_BASE_ADDRESS", "rq.plane1_base_address")]),
    ("hubp21_program_requestor", "DCN_EXPANSION_MODE", "SET", [
        ("DRQ_EXPANSION_MODE", "rq.drq_expansion_mode"), ("PRQ_EXPANSION_MODE", "rq.prq_expansion_mode"),
        ("MRQ_EXPANSION_MODE", "rq.mrq_expansion_mode"), ("CRQ_EXPANSION_MODE", "rq.crq_expansion_mode")]),
    ("hubp21_program_requestor", "DCHUBP_REQ_SIZE_CONFIG", "SET", [
        ("CHUNK_SIZE", "rq.rq_regs_l.chunk_size"), ("MIN_CHUNK_SIZE", "rq.rq_regs_l.min_chunk_size"),
        ("META_CHUNK_SIZE", "rq.rq_regs_l.meta_chunk_size"), ("MIN_META_CHUNK_SIZE", "rq.rq_regs_l.min_meta_chunk_size"),
        ("DPTE_GROUP_SIZE", "rq.rq_regs_l.dpte_group_size"), ("VM_GROUP_SIZE", "rq.rq_regs_l.mpte_group_size"),
        ("SWATH_HEIGHT", "rq.rq_regs_l.swath_height"), ("PTE_ROW_HEIGHT_LINEAR", "rq.rq_regs_l.pte_row_height_linear")]),
    ("hubp21_program_requestor", "DCHUBP_REQ_SIZE_CONFIG_C", "SET", [
        ("CHUNK_SIZE_C", "rq.rq_regs_c.chunk_size"), ("MIN_CHUNK_SIZE_C", "rq.rq_regs_c.min_chunk_size"),
        ("META_CHUNK_SIZE_C", "rq.rq_regs_c.meta_chunk_size"), ("MIN_META_CHUNK_SIZE_C", "rq.rq_regs_c.min_meta_chunk_size"),
        ("DPTE_GROUP_SIZE_C", "rq.rq_regs_c.dpte_group_size"), ("SWATH_HEIGHT_C", "rq.rq_regs_c.swath_height"),
        ("PTE_ROW_HEIGHT_LINEAR_C", "rq.rq_regs_c.pte_row_height_linear")]),
    # hubp3_program_deadline = hubp2_program_deadline + DCN_DMDATA_VM_CNTL
    ("hubp2_program_deadline", "BLANK_OFFSET_0", "SET", [
        ("REFCYC_H_BLANK_END", "dlg.refcyc_h_blank_end"), ("DLG_V_BLANK_END", "dlg.dlg_vblank_end")]),
    ("hubp2_program_deadline", "BLANK_OFFSET_1", "SET", [("MIN_DST_Y_NEXT_START", "dlg.min_dst_y_next_start")]),
    ("hubp2_program_deadline", "DST_DIMENSIONS", "SET", [("REFCYC_PER_HTOTAL", "dlg.refcyc_per_htotal")]),
    ("hubp2_program_deadline", "DST_AFTER_SCALER", "SET", [
        ("REFCYC_X_AFTER_SCALER", "dlg.refcyc_x_after_scaler"), ("DST_Y_AFTER_SCALER", "dlg.dst_y_after_scaler")]),
    ("hubp2_program_deadline", "REF_FREQ_TO_PIX_FREQ", "SET", [("REF_FREQ_TO_PIX_FREQ", "dlg.ref_freq_to_pix_freq")]),
    ("hubp2_program_deadline", "VBLANK_PARAMETERS_1", "SET", [("REFCYC_PER_PTE_GROUP_VBLANK_L", "dlg.refcyc_per_pte_group_vblank_l")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_0", "SET", [("DST_Y_PER_PTE_ROW_NOM_L", "dlg.dst_y_per_pte_row_nom_l")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_1", "SET", [("REFCYC_PER_PTE_GROUP_NOM_L", "dlg.refcyc_per_pte_group_nom_l")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_4", "SET", [("DST_Y_PER_META_ROW_NOM_L", "dlg.dst_y_per_meta_row_nom_l")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_5", "SET", [("REFCYC_PER_META_CHUNK_NOM_L", "dlg.refcyc_per_meta_chunk_nom_l")]),
    ("hubp2_program_deadline", "PER_LINE_DELIVERY", "SET", [
        ("REFCYC_PER_LINE_DELIVERY_L", "dlg.refcyc_per_line_delivery_l"),
        ("REFCYC_PER_LINE_DELIVERY_C", "dlg.refcyc_per_line_delivery_c")]),
    ("hubp2_program_deadline", "VBLANK_PARAMETERS_2", "SET", [("REFCYC_PER_PTE_GROUP_VBLANK_C", "dlg.refcyc_per_pte_group_vblank_c")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_2", "SET", [("DST_Y_PER_PTE_ROW_NOM_C", "dlg.dst_y_per_pte_row_nom_c")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_3", "SET", [("REFCYC_PER_PTE_GROUP_NOM_C", "dlg.refcyc_per_pte_group_nom_c")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_6", "SET", [("DST_Y_PER_META_ROW_NOM_C", "dlg.dst_y_per_meta_row_nom_c")]),
    ("hubp2_program_deadline", "NOM_PARAMETERS_7", "SET", [("REFCYC_PER_META_CHUNK_NOM_C", "dlg.refcyc_per_meta_chunk_nom_c")]),
    ("hubp2_program_deadline", "DCN_TTU_QOS_WM", "SET", [
        ("QoS_LEVEL_LOW_WM", "ttu.qos_level_low_wm"), ("QoS_LEVEL_HIGH_WM", "ttu.qos_level_high_wm")]),
    ("hubp2_program_deadline", "DCN_SURF0_TTU_CNTL0", "SET", [
        ("REFCYC_PER_REQ_DELIVERY", "ttu.refcyc_per_req_delivery_l"), ("QoS_LEVEL_FIXED", "ttu.qos_level_fixed_l"),
        ("QoS_RAMP_DISABLE", "ttu.qos_ramp_disable_l")]),
    ("hubp2_program_deadline", "DCN_SURF1_TTU_CNTL0", "SET", [
        ("REFCYC_PER_REQ_DELIVERY", "ttu.refcyc_per_req_delivery_c"), ("QoS_LEVEL_FIXED", "ttu.qos_level_fixed_c"),
        ("QoS_RAMP_DISABLE", "ttu.qos_ramp_disable_c")]),
    ("hubp2_program_deadline", "DCN_CUR0_TTU_CNTL0", "SET", [
        ("REFCYC_PER_REQ_DELIVERY", "ttu.refcyc_per_req_delivery_cur0"), ("QoS_LEVEL_FIXED", "ttu.qos_level_fixed_cur0"),
        ("QoS_RAMP_DISABLE", "ttu.qos_ramp_disable_cur0")]),
    ("hubp2_program_deadline", "FLIP_PARAMETERS_1", "SET", [("REFCYC_PER_PTE_GROUP_FLIP_L", "dlg.refcyc_per_pte_group_flip_l")]),
    ("hubp3_program_deadline", "DCN_DMDATA_VM_CNTL", "UPDATE", [("REFCYC_PER_VM_DMDATA", "dlg.refcyc_per_vm_dmdata")]),
    ("hubp2_setup_interdependent", "PREFETCH_SETTINGS", "SET", [
        ("DST_Y_PREFETCH", "dlg.dst_y_prefetch"), ("VRATIO_PREFETCH", "dlg.vratio_prefetch")]),
    ("hubp2_setup_interdependent", "PREFETCH_SETTINGS_C", "SET", [("VRATIO_PREFETCH_C", "dlg.vratio_prefetch_c")]),
    ("hubp2_setup_interdependent", "VBLANK_PARAMETERS_0", "SET", [
        ("DST_Y_PER_VM_VBLANK", "dlg.dst_y_per_vm_vblank"), ("DST_Y_PER_ROW_VBLANK", "dlg.dst_y_per_row_vblank")]),
    ("hubp2_setup_interdependent", "FLIP_PARAMETERS_0", "SET", [
        ("DST_Y_PER_VM_FLIP", "dlg.dst_y_per_vm_flip"), ("DST_Y_PER_ROW_FLIP", "dlg.dst_y_per_row_flip")]),
    ("hubp2_setup_interdependent", "VBLANK_PARAMETERS_3", "SET", [("REFCYC_PER_META_CHUNK_VBLANK_L", "dlg.refcyc_per_meta_chunk_vblank_l")]),
    ("hubp2_setup_interdependent", "VBLANK_PARAMETERS_4", "SET", [("REFCYC_PER_META_CHUNK_VBLANK_C", "dlg.refcyc_per_meta_chunk_vblank_c")]),
    ("hubp2_setup_interdependent", "FLIP_PARAMETERS_2", "SET", [("REFCYC_PER_META_CHUNK_FLIP_L", "dlg.refcyc_per_meta_chunk_flip_l")]),
    ("hubp2_setup_interdependent", "PER_LINE_DELIVERY_PRE", "SET", [
        ("REFCYC_PER_LINE_DELIVERY_PRE_L", "dlg.refcyc_per_line_delivery_pre_l"),
        ("REFCYC_PER_LINE_DELIVERY_PRE_C", "dlg.refcyc_per_line_delivery_pre_c")]),
    ("hubp2_setup_interdependent", "DCN_SURF0_TTU_CNTL1", "SET", [("REFCYC_PER_REQ_DELIVERY_PRE", "ttu.refcyc_per_req_delivery_pre_l")]),
    ("hubp2_setup_interdependent", "DCN_SURF1_TTU_CNTL1", "SET", [("REFCYC_PER_REQ_DELIVERY_PRE", "ttu.refcyc_per_req_delivery_pre_c")]),
    ("hubp2_setup_interdependent", "DCN_CUR0_TTU_CNTL1", "SET", [("REFCYC_PER_REQ_DELIVERY_PRE", "ttu.refcyc_per_req_delivery_pre_cur0")]),
    ("hubp2_setup_interdependent", "DCN_CUR1_TTU_CNTL1", "SET", [("REFCYC_PER_REQ_DELIVERY_PRE", "ttu.refcyc_per_req_delivery_pre_cur1")]),
    ("hubp2_setup_interdependent", "DCN_GLOBAL_TTU_CNTL", "SET", [
        ("MIN_TTU_VBLANK", "ttu.min_ttu_vblank"), ("QoS_LEVEL_FLIP", "ttu.qos_level_flip")]),
    ("optc1_program_global_sync", "OTG_VSTARTUP_PARAM", "SET", [("VSTARTUP_START", "dest.vstartup_start")]),
    ("optc1_program_global_sync", "OTG_VUPDATE_PARAM", "SET", [
        ("VUPDATE_OFFSET", "dest.vupdate_offset"), ("VUPDATE_WIDTH", "dest.vupdate_width")]),
    ("optc1_program_global_sync", "OTG_VREADY_PARAM", "SET", [("VREADY_OFFSET", "dest.vready_offset")]),
]

# Read-only context for the dry run (full Linux register names, no instance block): what hubbub31_get_dchub_ref_freq
# reads to pick DLG refclk 24 or 12 MHz (dcn31_hubbub.c), and the PHY PLL A resync register dce112_program_pixel_clk_resync
# / dcn31_program_pix_clk program (dce_clock_source.c; PLL0 = PHYPLLA, CS_SF list in dce_clock_source.h).
EXTRA = [
    ("DCHUBBUB_GLOBAL_TIMER_CNTL", ["DCHUBBUB_GLOBAL_TIMER_REFDIV", "DCHUBBUB_GLOBAL_TIMER_ENABLE"]),
    ("PHYPLLA_PIXCLK_RESYNC_CNTL", ["PHYPLLA_PIXCLK_RESYNC_ENABLE", "PHYPLLA_DEEP_COLOR_DTO_ENABLE_STATUS",
                                    "PHYPLLA_DCCG_DEEP_COLOR_CNTL", "PHYPLLA_PIXCLK_ENABLE"]),
    # the DET / compressed-buffer split (dcn31_init_crb, dcn31_program_det_size, dcn31_program_compbuf_size in
    # dcn31_hubbub.c; 64 KB segments) and the requesting mode hubp31_set_unbounded_requesting sets (dcn31_hubp.c)
    ("DCHUBBUB_DET0_CTRL", ["DET0_SIZE", "DET0_SIZE_CURRENT"]),
    ("DCHUBBUB_DET1_CTRL", ["DET1_SIZE", "DET1_SIZE_CURRENT"]),
    ("DCHUBBUB_DET2_CTRL", ["DET2_SIZE", "DET2_SIZE_CURRENT"]),
    ("DCHUBBUB_DET3_CTRL", ["DET3_SIZE", "DET3_SIZE_CURRENT"]),
    ("DCHUBBUB_COMPBUF_CTRL", ["COMPBUF_SIZE", "COMPBUF_SIZE_CURRENT", "CONFIG_ERROR"]),
    ("HUBP0_DCHUBP_CNTL", ["HUBP_UNBOUNDED_REQ_MODE", "HUBP_UNDERFLOW_STATUS"]),
    ("CURSOR0_0_CURSOR_CONTROL", ["CURSOR_REQ_MODE"]),   # SRI(CURSOR_CONTROL, CURSOR0_, 0), dcn20_hubp.h
] + [
    # which HUBP is live (the dry run expects HUBP0 feeding OTG0): DCHUBP_CNTL blank / TTU-disable of all four pipes
    ("HUBP%d_DCHUBP_CNTL" % i, ["HUBP_BLANK_EN", "HUBP_TTU_DISABLE"]) for i in range(4)
]

# Named single fields the Test L sequence drives (phx_dcn314_step2.h), each from the Linux function that writes or
# polls it, resolved like PROG through the HUBP / OPTC register lists. (register, field, constant name, Linux source)
NAMED = [
    ("OTG_GLOBAL_CONTROL2", "OTG_MASTER_UPDATE_LOCK_SEL", "kOtgLockSel", "optc3_lock (dcn30_optc.c)"),
    ("OTG_MASTER_UPDATE_LOCK", "OTG_MASTER_UPDATE_LOCK", "kOtgLock", "optc3_lock / optc1_unlock"),
    ("OTG_MASTER_UPDATE_LOCK", "UPDATE_LOCK_STATUS", "kOtgLockStatus", "optc3_lock REG_WAIT"),
    ("OTG_STATUS", "OTG_V_ACTIVE_DISP", "kOtgVActiveDisp", "optc1_wait_for_state(CRTC_STATE_VACTIVE)"),
    ("OTG_STATUS_FRAME_COUNT", "OTG_FRAME_COUNT", "kOtgFrameCount", "optc1_get_vblank_counter"),
    ("CONTROL", "VTG0_FP2", "kVtgFp2", "optc1_set_vtg_params"),
    ("CONTROL", "VTG0_VCOUNT_INIT", "kVtgVcountInit", "optc1_set_vtg_params"),
    ("DCHUBP_CNTL", "HUBP_VTG_SEL", "kHubpVtgSel", "hubp2_vtg_sel (dcn20_update_dchubp_dpp)"),
    ("DCHUBP_CNTL", "HUBP_UNDERFLOW_STATUS", "kHubpUnderflow", "hubp2_read_state_common"),
    # which OPP feeds OTG0 (optc314 ODM source select), and OPP0's display pattern generator that
    # dcn20_blank_pixel_data -> opp2_set_disp_pattern_generator / opp2_dpg_set_blank_color drive (dcn20_opp.c)
    ("OPTC_DATA_SOURCE_SELECT", "OPTC_SEG0_SRC_SEL", "kOdmSeg0Src", "optc314 set_odm_bypass / combine"),
    ("DPG_CONTROL", "DPG_EN", "kDpgEn", "opp2_set_disp_pattern_generator"),
    ("DPG_CONTROL", "DPG_MODE", "kDpgMode", "opp2_set_disp_pattern_generator"),
    ("DPG_DIMENSIONS", "DPG_ACTIVE_WIDTH", "kDpgWidth", "opp2_set_disp_pattern_generator"),
    ("DPG_DIMENSIONS", "DPG_ACTIVE_HEIGHT", "kDpgHeight", "opp2_set_disp_pattern_generator"),
    ("DPG_OFFSET_SEGMENT", "DPG_X_OFFSET", "kDpgXOffset", "opp2_set_disp_pattern_generator"),
    ("DPG_OFFSET_SEGMENT", "DPG_SEGMENT_WIDTH", "kDpgSegWidth", "opp2_set_disp_pattern_generator"),
    ("DPG_COLOUR_B_CB", "DPG_COLOUR1_B_CB", "kDpgColB1", "opp2_dpg_set_blank_color"),
    ("DPG_COLOUR_B_CB", "DPG_COLOUR0_B_CB", "kDpgColB0", "opp2_dpg_set_blank_color"),
    ("DPG_COLOUR_G_Y", "DPG_COLOUR1_G_Y", "kDpgColG1", "opp2_dpg_set_blank_color"),
    ("DPG_COLOUR_G_Y", "DPG_COLOUR0_G_Y", "kDpgColG0", "opp2_dpg_set_blank_color"),
    ("DPG_COLOUR_R_CR", "DPG_COLOUR1_R_CR", "kDpgColR1", "opp2_dpg_set_blank_color"),
    ("DPG_COLOUR_R_CR", "DPG_COLOUR0_R_CR", "kDpgColR0", "opp2_dpg_set_blank_color"),
    ("DPG_RAMP_CONTROL", "DPG_RAMP0_OFFSET", "kDpgRamp0", "opp2_set_disp_pattern_generator (video mode clears it)"),
]

# Named fields of registers without an instance block (full Linux names), for Test L step 3 (phx_dcn314_clk.h):
# dccg31_update_dpp_dto (dcn31_dccg.c; dcn314_dccg.c .update_dpp_dto) and the DISPCLK divider dccg314 reads back.
NAMED_FULL = [
    ("DPPCLK_DTO_CTRL", "DPPCLK0_DTO_ENABLE", "kDppDtoEnable0", "dccg31_update_dpp_dto"),
    ("DPPCLK0_DTO_PARAM", "DPPCLK0_DTO_PHASE", "kDppDtoPhase0", "dccg31_update_dpp_dto"),
    ("DPPCLK0_DTO_PARAM", "DPPCLK0_DTO_MODULO", "kDppDtoModulo0", "dccg31_update_dpp_dto"),
    ("DENTIST_DISPCLK_CNTL", "DENTIST_DISPCLK_RDIVIDER", "kDentistDispRdiv", "the running DISPCLK divider"),
    # DMCUB inbox 1 (dmub_dcn31.c: is_hw_init, setup_mailbox, get_inbox1_rptr, set_inbox1_wptr) and the CW4 window
    # that backs it (dmub_dcn31_setup_windows), for phx_dmub_inbox.h
    ("DMCUB_CNTL", "DMCUB_ENABLE", "kDmcubEnable", "dmub_dcn31_is_hw_init"),
    ("DMCUB_SCRATCH0", "DMCUB_SCRATCH0", "kDmcubScratch0", "dmub_fw_boot_status (dal_fw bit 0, mailbox_rdy bit 1)"),
    ("DMCUB_INBOX1_BASE_ADDRESS", "DMCUB_INBOX1_BASE_ADDRESS", "kInbox1Base", "dmub_dcn31_setup_mailbox"),
    ("DMCUB_INBOX1_SIZE", "DMCUB_INBOX1_SIZE", "kInbox1Size", "dmub_dcn31_setup_mailbox"),
    ("DMCUB_INBOX1_WPTR", "DMCUB_INBOX1_WPTR", "kInbox1Wptr", "dmub_dcn31_set_inbox1_wptr"),
    ("DMCUB_INBOX1_RPTR", "DMCUB_INBOX1_RPTR", "kInbox1Rptr", "dmub_dcn31_get_inbox1_rptr"),
    ("DMCUB_REGION3_CW4_OFFSET", "DMCUB_REGION3_CW4_OFFSET", "kCw4Offset", "dmub_dcn31_setup_windows"),
    ("DMCUB_REGION3_CW4_OFFSET_HIGH", "DMCUB_REGION3_CW4_OFFSET_HIGH", "kCw4OffsetHigh", "dmub_dcn31_setup_windows"),
    ("DMCUB_REGION3_CW4_BASE_ADDRESS", "DMCUB_REGION3_CW4_BASE_ADDRESS", "kCw4Base", "dmub_dcn31_setup_windows"),
]


def read(rel):
    with open(os.path.join(SRC, rel)) as f:
        return f.read()


def defines(text):
    out = {}
    for m in re.finditer(r"^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)L?\s*$", text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def macros(texts):
    """#define NAME(arg) bodies (backslash continuations joined)."""
    out = {}
    for t in texts:
        t = re.sub(r"\\\n", " ", t)
        for m in re.finditer(r"^#define\s+(\w+)\((\w+)\)\s*(.*)$", t, re.M):
            out[m.group(1)] = m.group(3)
    return out


def split_top(s):
    parts, depth, cur = [], 0, ""
    for c in s:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(cur.strip()); cur = ""
        else:
            cur += c
    if cur.strip():
        parts.append(cur.strip())
    return parts


def expand(name, mac, seen=None):
    """{register: block} from an SRI register list macro, following nested list macros."""
    regs = {}
    for item in split_top(mac[name]):
        m = re.match(r"(\w+)\s*\((.*)\)$", item, re.S)
        if not m:
            continue
        fn, args = m.group(1), [a.strip() for a in split_top(m.group(2))]
        if fn == "SRI":
            regs[args[0]] = args[1]
        elif fn in mac:
            regs.update(expand(fn, mac))
    return regs


def main():
    off = defines(read("include/asic_reg/dcn/dcn_3_1_4_offset.h"))
    sh = defines(read("include/asic_reg/dcn/dcn_3_1_4_sh_mask.h"))
    mac = macros([read(p) for p in ("display/dc/hubp/dcn10/dcn10_hubp.h", "display/dc/hubp/dcn20/dcn20_hubp.h",
                                     "display/dc/hubp/dcn21/dcn21_hubp.h", "display/dc/hubp/dcn30/dcn30_hubp.h",
                                     "display/dc/optc/dcn314/dcn314_optc.h", "display/dc/opp/dcn10/dcn10_opp.h",
                                     "display/dc/opp/dcn20/dcn20_opp.h", "display/dc/dcn30/dcn30_opp.h")])
    block = expand("HUBP_REG_LIST_DCN30", mac)
    block.update(expand("OPTC_COMMON_REG_LIST_DCN3_14", mac))
    block.update(expand("OPP_REG_LIST_DCN30", mac))   # dcn314_resource.c opp_regs

    lines = []
    regs_out = []
    for fn, reg, kind, fields in PROG:
        if reg not in block:
            sys.exit("%s is not in HUBP_REG_LIST_DCN30 / OPTC_COMMON_REG_LIST_DCN3_14: Linux skips it on DCN 3.1.4" % reg)
        name = "%s0_%s" % (block[reg], reg)
        o, b = off.get("reg" + name), off.get("reg" + name + "_BASE_IDX")
        if o is None or b is None:
            sys.exit("reg%s missing from dcn_3_1_4_offset.h" % name)
        addr = SEG[b] + o
        ri = len(regs_out)
        regs_out.append('    {"%s", 0x%05x, %d, %s, "%s"},  // reg%s 0x%04x, BASE_IDX %d' % (
            reg, addr, len(lines), "kSet" if kind == "SET" else "kUpdate" if kind == "UPDATE" else "kWrite", fn, name, o, b))
        for field, srcname in fields:
            if field is None:
                s_, m_ = 0, 0xFFFFFFFF
            else:
                k = "%s__%s" % (name, field)
                if k + "__SHIFT" not in sh or k + "_MASK" not in sh:
                    sys.exit("%s__SHIFT/_MASK missing from dcn_3_1_4_sh_mask.h" % k)
                s_, m_ = sh[k + "__SHIFT"], sh[k + "_MASK"]
            lines.append('    {%d, "%s", %d, 0x%08x, "%s"},' % (ri, field or "(whole)", s_, m_, srcname))

    extra = []
    for reg, fields in EXTRA:
        o, b = off.get("reg" + reg), off.get("reg" + reg + "_BASE_IDX")
        if o is None or b is None:
            sys.exit("reg%s missing from dcn_3_1_4_offset.h" % reg)
        for field in fields:
            k = "%s__%s" % (reg, field)
            if k + "__SHIFT" not in sh or k + "_MASK" not in sh:
                sys.exit("%s__SHIFT/_MASK missing from dcn_3_1_4_sh_mask.h" % k)
            extra.append('    {"%s", "%s", 0x%05x, %d, 0x%08x},  // reg%s 0x%04x, BASE_IDX %d' % (
                reg, field, SEG[b] + o, sh[k + "__SHIFT"], sh[k + "_MASK"], reg, o, b))

    named = []
    for reg, field, cname, src in NAMED:
        if reg not in block:
            sys.exit("%s is not in HUBP_REG_LIST_DCN30 / OPTC_COMMON_REG_LIST_DCN3_14" % reg)
        name = "%s0_%s" % (block[reg], reg)
        o, b = off.get("reg" + name), off.get("reg" + name + "_BASE_IDX")
        k = "%s__%s" % (name, field)
        if o is None or b is None or k + "__SHIFT" not in sh or k + "_MASK" not in sh:
            sys.exit("%s / %s missing from the Linux headers" % (name, k))
        named.append("static constexpr Fld %s = {0x%05x, %d, 0x%08x};  // reg%s.%s, %s" % (
            cname, SEG[b] + o, sh[k + "__SHIFT"], sh[k + "_MASK"], name, field, src))

    for reg, field, cname, src in NAMED_FULL:
        o, b = off.get("reg" + reg), off.get("reg" + reg + "_BASE_IDX")
        k = "%s__%s" % (reg, field)
        if o is None or b is None or k + "__SHIFT" not in sh or k + "_MASK" not in sh:
            sys.exit("%s / %s missing from the Linux headers" % (reg, k))
        named.append("static constexpr Fld %s = {0x%05x, %d, 0x%08x};  // reg%s.%s, %s" % (
            cname, SEG[b] + o, sh[k + "__SHIFT"], sh[k + "_MASK"], reg, field, src))

    hdr = """// phx_dcn314_prog_regs.h: GENERATED by tools/dml/gen_dcn314_regs.py from Linux v6.17's dcn_3_1_4_offset.h,
// dcn_3_1_4_sh_mask.h and the HUBP_REG_LIST_DCN30 / OPTC_COMMON_REG_LIST_DCN3_14 register lists. Do not edit.
//
// The HUBP0 and OTG0 registers Linux programs from DML for one pipe on DCN 3.1.4, in its order: hubp3_setup
// (hubp2_vready_at_or_After_vsync, hubp21_program_requestor, hubp3_program_deadline), hubp2_setup_interdependent,
// optc1_program_global_sync. Addresses include the 8500G's DMU 3.1.4 segment bases (0x12 0xc0 0x34c0).
// Each field names the DML value that feeds it (dml314_run --emit names, phx_dml314_plans.h).
#pragma once
#include <stdint.h>

namespace phxdcn314 {

enum ProgKind : uint8_t { kSet, kUpdate, kWrite };
struct ProgReg { const char *name; uint32_t addr; uint16_t firstField; ProgKind kind; const char *linuxFn; };
struct ProgField { uint16_t reg; const char *name; uint8_t shift; uint32_t mask; const char *dml; };

static const ProgReg kProgRegs[] = {
%s
};
static const ProgField kProgFields[] = {
%s
};
// Read-only context: DCHUBBUB_GLOBAL_TIMER_CNTL (DLG refclk), PHYPLLA_PIXCLK_RESYNC_CNTL (PLL0 resync / deep colour)
struct ExtraField { const char *reg; const char *name; uint32_t addr; uint8_t shift; uint32_t mask; };
static const ExtraField kExtraFields[] = {
%s
};
static constexpr unsigned kNumExtraFields = sizeof(kExtraFields) / sizeof(kExtraFields[0]);
// Single fields the Test L sequence drives (phx_dcn314_step2.h)
struct Fld { uint32_t addr; uint8_t shift; uint32_t mask; };
%s
static constexpr unsigned kNumProgRegs = sizeof(kProgRegs) / sizeof(kProgRegs[0]);
static constexpr unsigned kNumProgFields = sizeof(kProgFields) / sizeof(kProgFields[0]);

}  // namespace phxdcn314
""" % ("\n".join(regs_out), "\n".join(lines), "\n".join(extra), "\n".join(named))
    with open(OUT, "w") as f:
        f.write(hdr)
    print("wrote %s: %d registers, %d fields" % (OUT, len(regs_out), len(lines)))


if __name__ == "__main__":
    main()
