#!/bin/sh
# Fuzz test for runtime/nvn/maxwell/translate.c (see tools/test_translate.c):
#   MSL output byte-identical to the reference MSL-only translator (REF, default: the commit before the GLSL
#   backend), and every GLSL translation compiles (shaderc, Vulkan 1.3) and validates (SPIRV-Tools).
# usage: tools/test_translate.sh [count] [jobs] [mode msl|glsl|both]
# Needs clang + shaderc + SPIRV-Tools for GLSL (Windows: MSYS2 ucrt64; macOS: brew install shaderc spirv-tools).
# Only msl and both modes need the reference revision (REF). GLSL=0 selects MSL-only for the default both mode.
set -e
cd "$(dirname "$0")/.."
COUNT=${1:-20000}
JOBS=${2:-8}
MODE=${3:-both}
if [ "${GLSL:-1}" = 0 ]; then
  case "$MODE" in
    both) MODE=msl;;
    glsl) echo "GLSL-only mode requires GLSL to be enabled." >&2; exit 2;;
  esac
fi
REF=${REF:-652e234}
OUT=${OUT:-build/test_translate}
mkdir -p "$OUT/dump"
CC=${CC:-clang}
RENAME="-Dmaxwell_translate=old_maxwell_translate -Dmaxwell_free=old_maxwell_free
 -Dmaxwell_disassemble=old_maxwell_disassemble -Dmaxwell_gmem_base=old_maxwell_gmem_base
 -Dmaxwell_gmem_size=old_maxwell_gmem_size -Dmaxwell_dbg_code=old_maxwell_dbg_code -Dmaxwell_dbg_pc=old_maxwell_dbg_pc
 -Dmaxwell_dbg_reg=old_maxwell_dbg_reg -Dmaxwell_dbg_vs=old_maxwell_dbg_vs"
PRE=""
case "$(uname -s)" in MINGW*|MSYS*) PRE="-include malloc.h";; esac
set --
case "$MODE" in
  glsl) ;;
  msl|both)
    git cat-file -e "$REF:runtime/nvn/maxwell/translate.c" 2>/dev/null || {
      echo "MSL reference $REF is unavailable. Supply REF or select GLSL-only mode." >&2
      exit 2
    }
    mkdir -p "$OUT/old"
    git show "$REF:runtime/nvn/maxwell/translate.c" > "$OUT/old/translate.c"
    git show "$REF:runtime/nvn/maxwell/shader.h" > "$OUT/old/shader.h"
    $CC -O2 -w $PRE $RENAME -c "$OUT/old/translate.c" -o "$OUT/old.o"
    set -- -DWITH_MSL_REFERENCE "$OUT/old.o"
    ;;
  *) echo "Unknown mode: $MODE" >&2; exit 2;;
esac
$CC -O2 -Wall -c runtime/nvn/maxwell/translate.c -o "$OUT/new.o"
if [ "$MODE" != msl ]; then
  set -- "$@" -DWITH_GLSL -lshaderc_shared -lSPIRV-Tools-shared
fi
$CC -O2 -Wall tools/test_translate.c "$OUT/new.o" "$@" -o "$OUT/test_translate"
rm -f "$OUT"/dump/msl_diff_*.txt "$OUT"/dump/glsl_compile_*.txt "$OUT"/dump/glsl_validate_*.txt "$OUT"/dump/glsl_ok_*.txt
PER=$(( (COUNT + JOBS - 1) / JOBS ))
i=0
pids=""
while [ $i -lt "$JOBS" ]; do
  "$OUT/test_translate" runtime/nvn/maxwell/translate.c "$PER" $((i * 7919 + ${SEED:-1})) "$MODE" "$OUT/dump" > "$OUT/log_$i.txt" 2>&1 &
  pids="$pids $!"
  i=$((i + 1))
done
status=0
i=0
for pid in $pids; do
  wait "$pid" || status=1
  cat "$OUT/log_$i.txt"
  i=$((i + 1))
done
exit "$status"
