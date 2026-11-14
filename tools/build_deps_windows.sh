#!/bin/bash
# make deps-windows (MSYS2 UCRT64 shell): what the shipped Windows build links against instead of MSYS2's packages,
# built from pinned sources into build/deps-win. MSYS2's FFmpeg is a GPLv3 build (x264, x265, xvid, ...); the
# release uses an LGPL-only FFmpeg with just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
# VP8 / Opus / H.264 / AAC in WebM or MP4 (the same configuration as tools/build_deps_macos.sh) - and swscale.
# SDL3 (zlib licence) and the compiler runtime DLLs still come from MSYS2.
# Needs: the packages from tools/setup_windows.ps1 (clang, make, nasm). Skipped when its output exists
# (delete build/deps-win to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
P=${EOW_WINDOWS_DEPS_PREFIX:-$PWD/build/deps-win}
S=$P/src
mkdir -p "$S"
JOBS=${EOW_WINDOWS_JOBS:-$(nproc)}

fetch() {  # url sha256 file
    if [ ! -f "$S/$3" ]; then
        curl -fsSL --proto '=https' --max-time 120 -o "$S/$3.part" "$1"
        mv "$S/$3.part" "$S/$3"
    fi
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

if [ ! -f "$P/lib/libavcodec.dll.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --cc=clang --cxx=clang++ --arch=x86_64 --target-os=mingw32 \
        --enable-shared --disable-static \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --enable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && grep -qx '#define CONFIG_GPL 0' config.h \
        && grep -qx '#define CONFIG_NONFREE 0' config.h \
        && grep -qx '#define CONFIG_VERSION3 0' config.h \
        && grep -qx '#define CONFIG_OPUS_DECODER 1' config_components.h \
        && make -j"$JOBS" && make install)
fi
for dll in avcodec-63.dll avformat-63.dll avutil-61.dll swresample-7.dll swscale-10.dll; do
    test -s "$P/bin/$dll"
done
echo "deps-win: $(ls "$P/bin"/*.dll | xargs -n1 basename | tr '\n' ' ')"
