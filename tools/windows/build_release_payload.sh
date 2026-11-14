#!/bin/bash
# Create the audited Windows payload and its corresponding source archive.
set -euo pipefail
cd "$(dirname "$0")/../.."

if [[ $# != 2 || ${MSYSTEM:-} != UCRT64 ]]; then
    echo 'Usage in MSYS2 UCRT64: bash tools/windows/build_release_payload.sh <new-output-directory> <native-python.exe>' >&2
    exit 2
fi
output=$(realpath -m "$1")
python=$2
test -f "$python"
test -d /ucrt64/share/licenses
if [[ "$output" =~ [[:space:]] || "$PWD" =~ [[:space:]] ]]; then
    echo 'Use a checkout and output path without whitespace for Make and FFmpeg.' >&2
    exit 2
fi
if [[ -e "$output" || -L "$output" ]]; then
    echo "Release output already exists: $output" >&2
    exit 2
fi

mkdir "$output"
stage="$output/work"
mkdir "$stage" "$stage/runtime" "$stage/empty-generated" "$stage/tmp"
export TMPDIR="$stage/tmp"
export TEMP="$(cygpath -am "$TMPDIR")"
export TMP="$TEMP"
export LC_ALL=C
jobs=${EOW_WINDOWS_JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]]

"$python" -B -c 'import platform, sys; sys.exit(0 if sys.platform == "win32" and sys.version_info[:3] == (3, 12, 10) and platform.machine() == "AMD64" else "Expected native x64 Python 3.12.10")'
if [[ ! -d build/zig ]]; then
    ZIG_TARGET=x86_64-windows bash tools/fetch_zig.sh
fi
EOW_WINDOWS_DEPS_PREFIX="$stage/deps" EOW_WINDOWS_JOBS="$jobs" bash tools/build_deps_windows.sh
make -j"$jobs" RELEASE=1 "B=$stage/runtime" "GEN=$stage/empty-generated" \
    GENARCH=-march=x86-64-v3 ZIG=build/zig/zig.exe "DEPS=$stage/deps" all
test -f "$stage/runtime/eowrecomp.exe"
test -f "$stage/runtime/eowrecomp.dll.a"
test -z "$(find "$stage/runtime" -iname 'eowgame.*' -print -quit)"
test -z "$(find "$stage/empty-generated" -mindepth 1 -print -quit)"
/ucrt64/bin/strip --strip-debug "$stage/runtime/eowrecomp.exe"

pacman -Q > "$stage/packages.txt"
clang --version > "$stage/compiler.txt"
nasm --version >> "$stage/compiler.txt"
make --version >> "$stage/compiler.txt"
git rev-parse HEAD > "$stage/revision.txt"
MSYS2_ARG_CONV_EXCL='*' "$python" -B tools/windows/build_release_payload.py \
    "$(cygpath -am "$output")" "$(cygpath -am /)"
