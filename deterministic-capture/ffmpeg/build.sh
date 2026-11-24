#!/usr/bin/env bash
# Builds the static libav* libraries that dcap links, from the ffmpeg source
# tree of a gclient-synced Chromium checkout (third_party/ffmpeg), configured
# for exactly the codecs, muxers and demuxers the pipeline uses (LGPL only;
# NVENC through the vendored nv-codec-headers, loaded at run time).
#
#   CHROMIUM_SRC=~/DeterministicChromium/src ffmpeg/build.sh [out-dir]
#
# Output: <out-dir>/prefix with include/ and lib/ (default: ffmpeg/out).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CHROMIUM_SRC="${CHROMIUM_SRC:-$HOME/DeterministicChromium/src}"
FFMPEG_SRC="${FFMPEG_SRC:-$CHROMIUM_SRC/third_party/ffmpeg}"
OUT="${1:-$HERE/out}"
NASM="${NASM:-$CHROMIUM_SRC/out/dev/nasm}"
[ -x "$NASM" ] || NASM="$(command -v nasm)"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$OUT/build" "$OUT/nvh/lib/pkgconfig" "$OUT/nvh/include"
cp -r "$HERE/../third_party/nv-codec-headers/include/ffnvcodec" "$OUT/nvh/include/"
sed "s|@@PREFIX@@|$OUT/nvh|" "$HERE/../third_party/nv-codec-headers/ffnvcodec.pc.in" > "$OUT/nvh/lib/pkgconfig/ffnvcodec.pc"

cd "$OUT/build"
PKG_CONFIG_PATH="$OUT/nvh/lib/pkgconfig" "$FFMPEG_SRC/configure" \
  --prefix="$OUT/prefix" \
  --disable-everything --disable-all \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale \
  --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-network --disable-autodetect \
  --enable-protocol=file \
  --enable-encoder=ffv1,utvideo,magicyuv,ffvhuff,rawvideo,pcm_s16le,pcm_f32le,flac,h264_nvenc,hevc_nvenc \
  --enable-decoder=ffv1,utvideo,magicyuv,ffvhuff,rawvideo,pcm_s16le,pcm_f32le,flac \
  --enable-muxer=mov,mp4,matroska,wav,rawvideo \
  --enable-demuxer=mov,matroska,wav \
  --enable-parser=h264,hevc \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
  --enable-ffnvcodec --enable-nvenc --disable-cuda-llvm \
  --cc="${CC:-gcc}" --x86asmexe="$NASM" --extra-cflags="-O2" > configure.log
make -j"$JOBS" > make.log 2>&1
make install > install.log 2>&1
echo "ffmpeg static libraries installed in $OUT/prefix"
