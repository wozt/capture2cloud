#!/usr/bin/env bash
set -euo pipefail

# Minimal LGPL FFmpeg build for the Old 3DS MPEG-1/2/4 video decoders.
# Only avcodec/avutil/swscale are built; no demuxer, network stack,
# command-line program or unrelated decoder is shipped in the CIA.

script_dir=$(cd "$(dirname "$0")" && pwd)
project_dir=$(cd "$script_dir/.." && pwd)
source_dir="$project_dir/build-tools/ffmpeg-src"
prefix_dir="$project_dir/build-tools/ffmpeg"
commit=dab24a843203b2b191f40e39907fb146f688ec5c
devkitpro_dir=${DEVKITPRO:-/opt/devkitpro}

if [ ! -d "$source_dir/.git" ]; then
    git clone --filter=blob:none --no-checkout \
        https://github.com/Core-2-Extreme/FFmpeg_for_3DS.git "$source_dir"
fi

git -C "$source_dir" fetch --depth=1 origin "$commit"
git -C "$source_dir" checkout --detach "$commit"

cd "$source_dir"
make distclean >/dev/null 2>&1 || true
./configure \
    --enable-cross-compile \
    --cross-prefix="$devkitpro_dir/devkitARM/bin/arm-none-eabi-" \
    --prefix="$prefix_dir" \
    --cpu=armv6k --arch=arm --target-os=linux \
    --extra-cflags="-mfloat-abi=hard -mtune=mpcore -mtp=soft -Wno-error=incompatible-pointer-types -I$devkitpro_dir/libctru/include" \
    --extra-ldflags="-mfloat-abi=hard -L$devkitpro_dir/libctru/lib -specs=3dsx.specs" \
    --extra-libs=-lctru \
    --enable-optimizations --disable-everything --disable-debug --disable-doc \
    --disable-programs --disable-avdevice --disable-avfilter --disable-avformat \
    --disable-swresample --disable-network --disable-autodetect --disable-neon \
    --disable-armv6t2 --disable-pthreads --enable-inline-asm --enable-vfp \
    --enable-armv5te --enable-armv6 --enable-avcodec --enable-avutil \
    --enable-swscale --enable-decoder=mpeg1video,mpeg2video,mpeg4

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
make install
