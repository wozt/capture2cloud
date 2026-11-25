#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT="$PROJECT_DIR/build-tools/makerom"
REVISION="e8f5f529c54ff9b22a2491a480ffa69206bf7b19"

if [ -x "$OUTPUT" ]; then
    exit 0
fi

TEMP_DIR="$(mktemp -d /tmp/capture2cloud-makerom.XXXXXX)"
cleanup() {
    rm -rf -- "$TEMP_DIR"
}
trap cleanup EXIT

git clone --filter=blob:none https://github.com/3DSGuy/Project_CTR.git "$TEMP_DIR/Project_CTR"
git -C "$TEMP_DIR/Project_CTR" checkout --detach "$REVISION"
# The recursive 3DS make exports cross-compilers. Build makerom and its
# dependencies with the native host compiler, never devkitARM.
host_make() {
    env -u MAKEFLAGS -u MFLAGS -u MAKEOVERRIDES -u CC -u CXX -u LD \
        -u AR -u CFLAGS -u CXXFLAGS -u LDFLAGS -u CPPFLAGS \
        make -C "$TEMP_DIR/Project_CTR/makerom" \
        CC=/usr/bin/gcc CXX=/usr/bin/g++ "$@"
}
host_make deps
host_make program
file "$TEMP_DIR/Project_CTR/makerom/bin/makerom"
if [[ "$(uname -m)" == x86_64 ]]; then
    file "$TEMP_DIR/Project_CTR/makerom/bin/makerom" | grep -q 'x86-64'
fi
mkdir -p "$PROJECT_DIR/build-tools"
install -m 0755 "$TEMP_DIR/Project_CTR/makerom/bin/makerom" "$OUTPUT"

echo "Installed open-source makerom at $OUTPUT"
