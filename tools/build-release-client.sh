#!/usr/bin/env bash
# Build each Capture2Cloud client from this checkout and package only fresh outputs.
# Usage: bash tools/build-release-client.sh android|switch|wiiu-console|old3ds|wiiu-gamepad
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
platform="${1:-}"
OUT="$ROOT/dist/client-artifacts"
mkdir -p "$OUT"

needs() { command -v "$1" >/dev/null || { echo "Missing required tool: $1" >&2; exit 1; }; }
archive() {
  local output="$1"; shift
  local entry
  for entry in "$@"; do test -s "$entry" || { echo "Missing/empty output: $entry" >&2; exit 1; }; done
  rm -f "$OUT/$output"
  zip -q -j "$OUT/$output" "$@"
  test -s "$OUT/$output"
}

case "$platform" in
  android)
    needs java
    test -x android/gradlew || { echo 'Missing executable android/gradlew' >&2; exit 1; }
    if [[ -f "$HOME/.android/capture2cloud-signing/release.jks" && -f "$HOME/.android/capture2cloud-signing/password" ]]; then
      (cd android && ./gradlew --no-daemon :app:assembleRelease)
      apk="$ROOT/android/app/build/outputs/apk/release/app-release.apk"
      test -s "$apk"
      version="$(tr -d '\r\n' < VERSION)"
      install -m 644 "$apk" "$OUT/capture2cloud-$version.apk"
    else
      # Never present a debug-signed APK as a production-signed release.
      (cd android && ./gradlew --no-daemon :app:assembleDebug)
      apk="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
      test -s "$apk"
      install -m 644 "$apk" "$OUT/capture2cloud-android-debug.apk"
      echo 'INFO: Android APK is debug-signed. Configure signing secrets for upgrade-compatible releases.'
    fi
    ;;
  switch)
    needs make; needs zip
    test -n "${DEVKITPRO:-}" || { echo 'DEVKITPRO must be set for Switch' >&2; exit 1; }
    make -C switch_homebrew -j2
    archive capture2cloud-switch.zip switch_homebrew/capture2switch.nro switch_homebrew/capture2switch.cfg.example
    ;;
  wiiu-console)
    needs make; needs zip
    test -n "${DEVKITPRO:-}" || { echo 'DEVKITPRO must be set for Wii U' >&2; exit 1; }
    make -C wiiu_console -j2
    archive capture2cloud-wiiu-console.zip wiiu_console/capture2wiiu.wuhb wiiu_console/capture2wiiu.rpx wiiu_console/README.md
    ;;
  old3ds)
    needs make; needs zip
    test -n "${DEVKITPRO:-}" || { echo 'DEVKITPRO must be set for Old 3DS' >&2; exit 1; }
    make -C 3ds -j2
    archive capture2cloud-old3ds.zip 3ds/capture2cloud-old3ds.cia 3ds/capture2cloud-old3ds.3dsx 3ds/README.md
    ;;
  wiiu-gamepad)
    needs make; needs zip; needs git
    # libdrc and the x264 ABI 140 are deliberately not checked into this
    # repository. Fetch their published, committed copy from the owner's
    # bottom_screen repository at a fixed Git revision instead of borrowing
    # an obsolete Wii U GamePad client from an old Capture2Cloud release.
    VENDOR_REV=06922d55645a13b2558990dd3c7bde3212c9443c
    if [[ ! -s wiiu_gamepad/vendor/libdrc/libdrc.so || ! -s wiiu_gamepad/vendor/x264/lib/libx264.so.140 ]]; then
      repo="$ROOT/dist/bottom-screen-vendor-source"
      if [[ ! -d "$repo/.git" ]]; then
        git clone --filter=blob:none https://github.com/wozt/bottom_screen.git "$repo"
      fi
      git -C "$repo" fetch --depth=1 origin "$VENDOR_REV"
      git -C "$repo" checkout --detach "$VENDOR_REV"
      [[ "$(git -C "$repo" rev-parse HEAD)" == "$VENDOR_REV" ]] || exit 1
      mkdir -p wiiu_gamepad/vendor
      cp -a "$repo/gamepad/vendor/." wiiu_gamepad/vendor/
    fi
    make -C wiiu_gamepad -j2
    test -s wiiu_gamepad/wiiu_pad
    test -s wiiu_gamepad/vendor/libdrc/libdrc.so
    test -s wiiu_gamepad/vendor/x264/lib/libx264.so.140
    package="$ROOT/dist/wiiu-gamepad-package"
    rm -rf "$package"
    mkdir -p "$package/vendor"
    install -m 755 wiiu_gamepad/wiiu_pad "$package/wiiu_pad"
    cp -a wiiu_gamepad/vendor/libdrc "$package/vendor/"
    cp -a wiiu_gamepad/vendor/x264 "$package/vendor/"
    cp -a wiiu_gamepad/conf "$package/conf"
    install -m 644 wiiu_gamepad/README.md "$package/README.md"
    rm -f "$OUT/capture2cloud-wiiu-gamepad.zip"
    (cd "$package" && zip -q -r -y "$OUT/capture2cloud-wiiu-gamepad.zip" .)
    test -s "$OUT/capture2cloud-wiiu-gamepad.zip"
    ;;
  *)
    echo "Usage: $0 android|switch|wiiu-console|old3ds|wiiu-gamepad" >&2
    exit 2
    ;;
esac
printf 'Fresh client artifact(s):\n'
find "$OUT" -maxdepth 1 -type f -printf '  %f (%s bytes)\n'
