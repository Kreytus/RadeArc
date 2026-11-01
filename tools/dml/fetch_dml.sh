#!/bin/bash
# fetch_dml.sh: Linux v6.17's Display Mode Library (drivers/gpu/drm/amd/display/dc/dml, MIT-licensed AMD code) for
# DCN 3.1.4, unmodified, into build/dml-src. Phase 3b: the per-mode values Linux programs (vstartup/vupdate/vready,
# HUBP DLG/TTU registers, watermarks, DISPCLK) come from DML, not from fixed formulas, so they are computed by Linux's
# own code rather than re-derived. Built in userspace with tools/dml/shim.
set -e
cd "$(dirname "$0")/../.."
OUT=build/dml-src
TAG=v6.17
R=https://raw.githubusercontent.com/torvalds/linux/$TAG/drivers/gpu/drm/amd/display/dc
mkdir -p "$OUT/dcn314" "$OUT/dcn31" "$OUT/calcs"
for f in display_mode_lib.h display_mode_vba.c display_mode_vba.h display_mode_structs.h display_mode_enums.h \
         dml_inline_defs.h dml_logger.h display_rq_dlg_helpers.c display_rq_dlg_helpers.h dc_features.h \
         dcn314/display_mode_vba_314.c dcn314/display_mode_vba_314.h dcn314/display_rq_dlg_calc_314.c \
         dcn314/display_rq_dlg_calc_314.h dcn314/dcn314_fpu.c dcn314/dcn314_fpu.h \
         dcn31/display_mode_vba_31.h dcn31/display_rq_dlg_calc_31.h \
         calcs/dcn_calc_math.c; do
    [ -s "$OUT/$f" ] || curl -sfSL --retry 3 -o "$OUT/$f" "$R/dml/$f"
done
[ -s "$OUT/dcn_calc_math.h" ] || curl -sfSL --retry 3 -o "$OUT/dcn_calc_math.h" "$R/inc/dcn_calc_math.h"
echo "DML $TAG in $OUT"
