#!/usr/bin/env bash
# Capture2Cloud x86_64 AppImage. Based on WOZT GTK Standard v1.2.
# Privileged Bluetooth/polkit setup remains an explicit host installation.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
[[ "$(uname -m)" == x86_64 ]] || { echo 'AppImage supports x86_64 only' >&2; exit 1; }

# Always compile before copying executables. No stale packaged binaries.
# shellcheck source=scripts/lib_toggle.sh
source scripts/lib_toggle.sh
c2c_build_if_needed

DIST="$ROOT/dist"
APPDIR="$DIST/capture2cloud.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" \
  "$APPDIR/usr/share/icons/hicolor/64x64/apps" \
  "$APPDIR/usr/share/capture2cloud"
install -m 755 "$ROOT/capture2cloud" "$APPDIR/usr/bin/capture2cloud"
# Resource resolution is binary-relative for development and AppImage alike.
for folder in assets web data; do
  mkdir -p "$APPDIR/usr/bin/$folder"
  cp -R "$ROOT/$folder/." "$APPDIR/usr/bin/$folder/"
done
mkdir -p "$APPDIR/usr/bin/scripts" "$APPDIR/usr/bin/pcble_backend" "$APPDIR/usr/bin/wiiu_wupstream"
install -m 644 scripts/.env.example "$APPDIR/usr/bin/scripts/.env.example"
install -m 755 pcble_backend/capture2cloud-pcble-backend "$APPDIR/usr/bin/pcble_backend/"
install -m 755 pcble_backend/capture2cloud-pcble-joycon2-backend "$APPDIR/usr/bin/pcble_backend/"
if [[ -x wiiu_wupstream/wup_stream_helper ]]; then
  install -m 755 wiiu_wupstream/wup_stream_helper "$APPDIR/usr/bin/wiiu_wupstream/"
fi
install -m 644 assets/icon-64.png "$APPDIR/usr/share/icons/hicolor/64x64/apps/capture2cloud.png"
install -m 644 scripts/.env.example "$APPDIR/usr/share/capture2cloud/config.example"

cat > "$APPDIR/usr/share/applications/capture2cloud.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Capture2Cloud
Comment=Capture, stream and bridge controllers
Exec=capture2cloud-appimage-launcher
Icon=capture2cloud
StartupWMClass=capture2cloud
Categories=AudioVideo;Network;
Terminal=false
DESKTOP

cat > "$APPDIR/usr/bin/capture2cloud-appimage-launcher" <<'LAUNCHER'
#!/bin/sh
set -eu
APPROOT="${APPDIR:-$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)}"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/capture2cloud"
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/.env" ]; then
    cp "$APPROOT/usr/bin/scripts/.env.example" "$CONF_DIR/.env"
    chmod 600 "$CONF_DIR/.env"
fi
export C2C_CONFIG_PATH="$CONF_DIR/.env"
exec "$APPROOT/usr/bin/capture2cloud" "$@"
LAUNCHER
chmod 755 "$APPDIR/usr/bin/capture2cloud-appimage-launcher"

# Full GTK icon theme and loaders: shipping the application icon alone does
# not provide the symbolic icons used in labels/buttons, menus, and dialogs.
for folder in /usr/share/icons/Adwaita /usr/share/icons/hicolor; do
    if [[ -d "$folder" ]]; then
        dest="$APPDIR/usr/share/icons/$(basename "$folder")"
        mkdir -p "$dest"
        cp -a "$folder/." "$dest/"
    fi
done
if [[ -d /usr/share/glib-2.0/schemas ]]; then
    mkdir -p "$APPDIR/usr/share/glib-2.0/schemas"
    cp -a /usr/share/glib-2.0/schemas/. "$APPDIR/usr/share/glib-2.0/schemas/"
    if command -v glib-compile-schemas >/dev/null; then
        glib-compile-schemas "$APPDIR/usr/share/glib-2.0/schemas"
    fi
fi
# linuxdeploy's GTK plugin collects GTK and pixbuf loaders, but keep these
# search paths in the AppDir so GtkIconTheme can find the shipped icons.
chmod -R a+rX "$APPDIR"

LINUXDEPLOY="${LINUXDEPLOY:-$ROOT/tools/linuxdeploy-x86_64.AppImage}"
if [[ ! -x "$LINUXDEPLOY" ]]; then
    command -v curl >/dev/null || { echo 'curl or LINUXDEPLOY is required' >&2; exit 1; }
    curl -fL --retry 3 -o "$LINUXDEPLOY" \
      https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod 755 "$LINUXDEPLOY"
fi
if ! command -v linuxdeploy-plugin-gtk.sh >/dev/null 2>&1; then
    mkdir -p "$DIST/tools"
    if [[ ! -x "$DIST/tools/linuxdeploy-plugin-gtk.sh" ]]; then
        curl -fL --retry 3 -o "$DIST/tools/linuxdeploy-plugin-gtk.sh" \
          https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/7a3fbc31a9e5075073ff8790f26effbac5f84453/linuxdeploy-plugin-gtk.sh
        chmod 755 "$DIST/tools/linuxdeploy-plugin-gtk.sh"
    fi
    export PATH="$DIST/tools:$PATH"
fi
export APPIMAGE_EXTRACT_AND_RUN=1
export DEPLOY_GTK_VERSION=3
(
    cd "$DIST"
    "$LINUXDEPLOY" --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/capture2cloud" \
        --desktop-file "$APPDIR/usr/share/applications/capture2cloud.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/64x64/apps/capture2cloud.png" \
        --plugin gtk --output appimage
)
mapfile -t BUILT < <(find "$DIST" -maxdepth 1 -name '*.AppImage' -type f)
if [[ "${#BUILT[@]}" != 1 ]]; then
    printf 'Expected exactly one AppImage, found %s\n' "${#BUILT[@]}" >&2
    exit 1
fi
OUT="$DIST/capture2cloud-x86_64.AppImage"
if [[ "${BUILT[0]}" != "$OUT" ]]; then mv -- "${BUILT[0]}" "$OUT"; fi
chmod 755 "$OUT"
(cd "$DIST" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
# Extracted smoke check: verify that fonts/icons/JSON can be opened by all
# users, and that the output has executable launchers.
EXTRACT="$DIST/squashfs-root"
rm -rf "$EXTRACT"
(cd "$DIST" && "$OUT" --appimage-extract >/dev/null)
[[ -x "$EXTRACT/AppRun" ]] || { echo 'Missing AppRun' >&2; exit 1; }
[[ -r "$EXTRACT/usr/bin/data/themes/07-steam-legacy.json" ]] || exit 1
[[ -r "$EXTRACT/usr/bin/data/languages/01-fr.json" ]] || exit 1
[[ -r "$EXTRACT/usr/share/icons/hicolor/64x64/apps/capture2cloud.png" ]] || exit 1
[[ -d "$EXTRACT/usr/share/icons/Adwaita" ]] || { echo 'Missing GTK icon theme' >&2; exit 1; }
if ! find "$EXTRACT/usr/share/icons" -type f -name '*-symbolic.svg' -print -quit | grep -q .; then
    echo 'Missing symbolic GTK icons' >&2; exit 1
fi
if find "$EXTRACT" -type f ! -perm -0444 | grep -q .; then
    echo 'AppImage has files inaccessible to other users' >&2; exit 1
fi
printf 'Built %s\n' "$OUT"
