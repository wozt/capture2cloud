#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Reuse the project's normal build recipe.
# shellcheck source=lib_toggle.sh
source "$SCRIPT_DIR/lib_toggle.sh"

c2c_build_pcble_if_needed

DEST=/usr/local/libexec/capture2cloud/pcble
POLICY=/usr/share/polkit-1/actions/io.github.wozt.capture2cloud.pcble.policy

echo "Installing root-owned Capture2Cloud pcble runtime..."

sudo install -d -o root -g root -m 0755 "$DEST"

sudo install -o root -g root -m 0755 \
    "$C2C_PCBLE_BIN" \
    "$DEST/capture2cloud-pcble-backend"

sudo install -o root -g root -m 0755 \
    "$C2C_PCBLE_DIR/poc/run-classic.sh" \
    "$DEST/run-classic.sh"

sudo install -o root -g root -m 0644 \
    "$C2C_PCBLE_DIR/poc/pro-controller.xml" \
    "$DEST/pro-controller.xml"

sudo install -o root -g root -m 0644 \
    "$C2C_PCBLE_DIR/io.github.wozt.capture2cloud.pcble.policy" \
    "$POLICY"

echo
echo "Installed:"
echo "  $DEST/capture2cloud-pcble-backend"
echo "  $DEST/run-classic.sh"
echo "  $DEST/pro-controller.xml"
echo "  $POLICY"
