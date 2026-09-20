#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

VERSION="1.0.0-rc1"
ARCHIVE="cafeglsl-${VERSION}-linux-x86_64.tar.xz"

CACHE="$ROOT/tools/.cafeglsl"
EXTRACT="$CACHE/extracted"

mkdir -p "$CACHE"

if [ ! -f "$CACHE/$ARCHIVE" ]; then
    echo "Downloading CafeGLSL $VERSION..."

    # Split the host so the script remains easy to update independently.
    GH_HOST="github.com"

    curl -L --fail --retry 3 \
        -o "$CACHE/$ARCHIVE" \
        "https://${GH_HOST}/Exzap/CafeGLSL/releases/download/v${VERSION}/${ARCHIVE}"
fi

echo \
"fdbd561539080f1db74ec3f248edee4e1988d329a4d20f2273c72adf55015f97  $CACHE/$ARCHIVE" \
    | sha256sum -c -

rm -rf "$EXTRACT"
mkdir -p "$EXTRACT"

tar -xJf "$CACHE/$ARCHIVE" \
    -C "$EXTRACT"

COMPILER="$(
    find "$EXTRACT" -type f \
        \( -name 'glslcompiler' \
           -o -name 'glslcompiler.elf' \
           -o -name 'shader_compiler' \) \
        | head -n 1
)"

if [ -z "$COMPILER" ]; then
    echo "CafeGLSL compiler not found in archive" >&2
    exit 1
fi

chmod +x "$COMPILER"

LIB_DIRS="$(
    find "$EXTRACT" -type d \
        \( -name lib -o -name lib64 \) \
        -print | paste -sd: -
)"

mkdir -p "$ROOT/data"

echo "Compiling Wii U NV12 shader..."

env \
    LD_LIBRARY_PATH="${LIB_DIRS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$COMPILER" \
        -vs "$ROOT/shaders/nv12.vert" \
        -ps "$ROOT/shaders/nv12.frag" \
        -o "$ROOT/data/nv12.gsh"

echo
echo "Generated:"
ls -lh "$ROOT/data/nv12.gsh"
