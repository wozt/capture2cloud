#!/usr/bin/env bash
# Existing historical Linux ZIP alongside the self-contained AppImage.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
test -s capture2cloud
command -v zip >/dev/null
DIR="$ROOT/dist/linux-zip-package"
rm -rf "$DIR"
mkdir -p "$DIR/scripts" "$DIR/pcble_backend"
install -m 755 capture2cloud "$DIR/capture2cloud"
for folder in assets web data; do cp -a "$folder" "$DIR/"; done
install -m 644 scripts/.env.example "$DIR/scripts/.env.example"
for helper in capture2cloud-pcble-backend capture2cloud-pcble-joycon2-backend; do
  test -s "pcble_backend/$helper"
  install -m 755 "pcble_backend/$helper" "$DIR/pcble_backend/$helper"
done
install -m 644 README.md "$DIR/README.md"
rm -f "$ROOT/dist/capture2cloud-linux-x86_64.zip"
(cd "$DIR" && zip -q -r -y "$ROOT/dist/capture2cloud-linux-x86_64.zip" .)
test -s "$ROOT/dist/capture2cloud-linux-x86_64.zip"
