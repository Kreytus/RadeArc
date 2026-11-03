#!/bin/bash
# fetch_regs.sh: the Linux v6.17 sources gen_dcn314_regs.py reads, unmodified, into build/linux-src: DCN 3.1.4's register
# offset / shift-mask headers and the register lists (SRI macros) that place each register in its block.
set -e
cd "$(dirname "$0")/../.."
OUT=build/linux-src
R=https://raw.githubusercontent.com/torvalds/linux/v6.17/drivers/gpu/drm/amd
for f in include/asic_reg/dcn/dcn_3_1_4_offset.h include/asic_reg/dcn/dcn_3_1_4_sh_mask.h \
         display/dc/hubp/dcn10/dcn10_hubp.h display/dc/hubp/dcn20/dcn20_hubp.h display/dc/hubp/dcn21/dcn21_hubp.h \
         display/dc/hubp/dcn30/dcn30_hubp.h display/dc/optc/dcn314/dcn314_optc.h; do
    mkdir -p "$OUT/$(dirname "$f")"
    [ -s "$OUT/$f" ] || curl -sfSL --retry 3 -o "$OUT/$f" "$R/$f"
done
echo "Linux v6.17 register headers in $OUT"
