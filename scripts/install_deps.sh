#!/bin/bash
# Installs everything Capture2Cloud needs to build and run, and sets up
# USB access for the gamepad adapter.
#
# Safe to re-run: apt skips what's already installed, and the udev rule is
# rewritten identically.
#
# Usage:
#   ./scripts/install_deps.sh              # dependencies + udev rule
#   ./scripts/install_deps.sh --no-udev    # dependencies only
#   ./scripts/install_deps.sh --dev        # also the browser-test tooling

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

WITH_UDEV=1
WITH_DEV=0
for arg in "$@"; do
    case "$arg" in
        --no-udev) WITH_UDEV=0 ;;
        --dev) WITH_DEV=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 1 ;;
    esac
done

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$1" >&2; }

# --- sanity checks -----------------------------------------------------

if [ "$(id -u)" -eq 0 ]; then
    warn "running as root: the udev rule and packages will be fine, but"
    warn "build the project as your normal user afterwards."
fi

if ! command -v apt-get >/dev/null 2>&1; then
    cat >&2 <<'EOF'
This script only knows apt (Debian/Ubuntu/Mint/Pop!_OS...).

On another distribution, install the equivalents of:
  build tools           gcc, pkg-config
  windowing/audio       SDL2, GTK3, X11, PulseAudio, libjpeg
  media pipeline        GStreamer 1.x + base/good/bad plugins, the "nice"
                        plugin (WebRTC ICE), libswscale (ffmpeg)
  usb                   libusb 1.0
then build with the command shown in README.md.
EOF
    exit 1
fi

# --- packages ----------------------------------------------------------

PACKAGES=(
    # build
    build-essential pkg-config
    # local window, audio, capture
    libsdl2-dev libpulse-dev libjpeg-dev libgtk-3-dev libx11-dev
    # gstreamer: pipeline + encoders + webrtc + ICE
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    libgstreamer-plugins-bad1.0-dev
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
    gstreamer1.0-libav gstreamer1.0-nice
    # colour conversion + usb
    libavutil-dev libswscale-dev libusb-1.0-0-dev
    # handy for finding your capture device / debugging
    v4l-utils
)

say "Installing packages (sudo may prompt)"
sudo apt-get update
sudo apt-get install -y "${PACKAGES[@]}"

if [ "$WITH_DEV" -eq 1 ]; then
    say "Installing browser-test tooling"
    if ! command -v node >/dev/null 2>&1; then
        sudo apt-get install -y nodejs npm
    fi
    (cd "$PROJECT_DIR" && npm install --no-fund --no-audit)
    (cd "$PROJECT_DIR" && npx playwright install chromium)
fi

# --- USB access for the adapter ----------------------------------------
#
# Talking to the ConsoleTuner adapter needs write access to its USB
# device, which is root-only by default. A udev rule grants it to the
# plugdev group instead -- narrower than making it world-writable.

if [ "$WITH_UDEV" -eq 1 ]; then
    say "Setting up USB access for the gamepad adapter"

    RULE_FILE=/etc/udev/rules.d/99-capture2cloud-adapter.rules
    # 2508 = ConsoleTuner (Titan One, Cronus, CronusMAX),
    # 2008 = CronusMAX PLUS v3.
    sudo tee "$RULE_FILE" >/dev/null <<'EOF'
# Capture2Cloud: let the plugdev group talk to ConsoleTuner adapters.
SUBSYSTEM=="usb", ATTR{idVendor}=="2508", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="2008", MODE="0660", GROUP="plugdev"
EOF
    sudo udevadm control --reload-rules
    sudo udevadm trigger || true
    echo "installed $RULE_FILE"

    if ! id -nG "$USER" | grep -qw plugdev; then
        sudo usermod -aG plugdev "$USER"
        warn "added $USER to the 'plugdev' group -- log out and back in"
        warn "(or reboot) for it to take effect."
    fi
fi

# --- configuration -----------------------------------------------------

say "Configuration"
ENV_FILE="$SCRIPT_DIR/.env"
if [ -f "$ENV_FILE" ]; then
    echo "$ENV_FILE already exists, left untouched."
else
    cp "$SCRIPT_DIR/.env.example" "$ENV_FILE"
    chmod 600 "$ENV_FILE"
    echo "created $ENV_FILE from the template."
fi

cat <<EOF

Next steps:

  1. Point the config at your capture card:
       v4l2-ctl --list-devices     # -> VIDEO_DEVICE
       pactl list short sources    # -> AUDIO_SOURCE
     then edit $ENV_FILE
     (prefer a /dev/v4l/by-id/... path: /dev/videoN numbers move around)

  2. Set PLAYER_PASSWORD in that same file if you want the console to be
     controllable only after logging in.

  3. Build and run -- see README.md.

EOF
