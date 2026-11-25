#!/usr/bin/env bash
set -euo pipefail
# Separate executable: patched libwupstream x264 must not link into
# Capture2Cloud, which already loads different x264 ABI versions.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UPSTREAM="$ROOT/wiiu_wupstream/build/upstream"
COMMIT=8609916410a2c7b0ef7f052e00d788538e2ca43e
mkdir -p "$(dirname "$UPSTREAM")"
if [[ ! -d "$UPSTREAM/.git" ]]; then
    git clone https://github.com/wozt/wup-stream.git "$UPSTREAM"
fi
if [[ $(git -C "$UPSTREAM" rev-parse HEAD) != "$COMMIT" ]]; then
    git -C "$UPSTREAM" fetch --depth=1 origin "$COMMIT"
    git -C "$UPSTREAM" checkout --detach "$COMMIT"
fi
[[ $(git -C "$UPSTREAM" rev-parse HEAD) == "$COMMIT" ]] || { echo "Wrong wup-stream revision" >&2; exit 1; }
if [[ ! -s "$UPSTREAM/build/host/libwupstream.a" ]]; then
    (cd "$UPSTREAM" && sh host/build.sh)
fi
cc -O2 -std=c11 -Wall -Wextra -Werror \
    -I"$UPSTREAM/host" \
    -o "$ROOT/wiiu_wupstream/wup_stream_helper" \
    "$ROOT/wiiu_wupstream/helper.c" \
    "$UPSTREAM/build/host/libwupstream.a" -lm -pthread -ldl
printf 'Built %s\n' "$ROOT/wiiu_wupstream/wup_stream_helper"
