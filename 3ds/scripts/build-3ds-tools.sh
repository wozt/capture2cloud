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
make -C "$TEMP_DIR/Project_CTR/makerom" deps
make -C "$TEMP_DIR/Project_CTR/makerom" program
mkdir -p "$PROJECT_DIR/build-tools"
install -m 0755 "$TEMP_DIR/Project_CTR/makerom/bin/makerom" "$OUTPUT"

echo "Installed open-source makerom at $OUTPUT"
