#!/bin/bash
# build.sh: compile Linux's DCN 3.1.4 DML (fetched by fetch_dml.sh, unmodified) into build/dml/libdml314.a.
set -e
cd "$(dirname "$0")/../.."
bash tools/dml/fetch_dml.sh >/dev/null
S=build/dml-src; O=build/dml
mkdir -p "$O"
CC=${CC:-cc}
for f in display_mode_vba.c display_rq_dlg_helpers.c dcn314/display_mode_vba_314.c dcn314/display_rq_dlg_calc_314.c \
         calcs/dcn_calc_math.c; do
    $CC -std=gnu11 -O2 -w -c -Itools/dml/shim -I$S -I$S/dcn314 -include tools/dml/shim/dm_services.h "$S/$f" \
        -o "$O/$(basename "$f" .c).o"
done
ar rcs "$O/libdml314.a" "$O"/*.o
# dcn3_14_ip and dcn3_14_soc, verbatim from dcn314_fpu.c (the soc one is static there)
{
    echo '#pragma once'
    echo '#define DCN3_14_DEFAULT_DET_SIZE 384'
    sed -n '/^struct _vcs_dpi_ip_params_st dcn3_14_ip = {/,/^};/p' "$S/dcn314/dcn314_fpu.c" | sed 's/^struct/static struct/'
    sed -n '/^static struct _vcs_dpi_soc_bounding_box_st dcn3_14_soc = {/,/^};/p' "$S/dcn314/dcn314_fpu.c"
} > "$O/dcn314_ipsoc.h"
$CC -std=gnu11 -O2 -Wall -Wno-unused-variable -Itools/dml/shim -I$S -I$S/dcn314 -I"$O" -include tools/dml/shim/dm_services.h \
    tools/dml/dml314_run.c "$O/libdml314.a" -lm -o "$O/dml314_run"
echo "built $O/libdml314.a $O/dml314_run"
