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
# Override the upstream GTK hook's non-relocatable pixbuf cache.
export LD_LIBRARY_PATH="$APPROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$APPROOT/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GTK_DATA_PREFIX="$APPROOT/usr"
export GDK_PIXBUF_MODULEDIR="$APPROOT/usr/lib"
cache="$(mktemp "${TMPDIR:-/tmp}/c2c-pixbuf.XXXXXXXX")"
trap 'rm -f "$cache"' EXIT
escaped="$(printf '%s' "$APPROOT" | sed 's/[\\&|]/\\&/g')"
sed "s|@APPDIR@|$escaped|g" "$APPROOT/usr/share/capture2cloud/pixbuf-loaders.cache.in" > "$cache"
export GDK_PIXBUF_MODULE_FILE="$cache"
if [ "${1:-}" = --self-test-icons ]; then
    "$APPROOT/usr/bin/check-gtk-icons"
    exit $?
fi
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/capture2cloud"
mkdir -p "$CONF_DIR"
if [ ! -f "$CONF_DIR/.env" ]; then
    cp "$APPROOT/usr/bin/scripts/.env.example" "$CONF_DIR/.env"
    chmod 600 "$CONF_DIR/.env"
fi
export C2C_CONFIG_PATH="$CONF_DIR/.env"
"$APPROOT/usr/bin/capture2cloud" "$@" &
child=$!
trap 'kill -TERM "$child" 2>/dev/null || true' TERM
trap 'kill -INT "$child" 2>/dev/null || true' INT
wait "$child"
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
# Explicitly deploy the full pixbuf loader set and generate a relocatable
# cache, following the working pcble2gamepad AppImage implementation.
find "$APPDIR/usr/share/icons" -name icon-theme.cache -delete
PIXBUF_LOADER_DIR="$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0)"
PIXBUF_QUERY="$(pkg-config --variable=gdk_pixbuf_query_loaders gdk-pixbuf-2.0)"
[[ -x "$PIXBUF_QUERY" ]] || PIXBUF_QUERY="$(dirname "$PIXBUF_LOADER_DIR")/gdk-pixbuf-query-loaders"
[[ -x "$PIXBUF_QUERY" && -r "$PIXBUF_LOADER_DIR/libpixbufloader-svg.so" ]] || { echo 'Missing SVG loader runtime' >&2; exit 1; }
shopt -s nullglob
loaders=("$PIXBUF_LOADER_DIR"/*.so)
(( ${#loaders[@]} > 0 ))
loader_args=()
for loader in "${loaders[@]}"; do loader_args+=(--library "$loader"); done
"$PIXBUF_QUERY" "${loaders[@]}" | sed "s|$PIXBUF_LOADER_DIR/|@APPDIR@/usr/lib/|g" > "$APPDIR/usr/share/capture2cloud/pixbuf-loaders.cache.in"
grep -Fq '@APPDIR@/usr/lib/libpixbufloader-svg.so' "$APPDIR/usr/share/capture2cloud/pixbuf-loaders.cache.in"
# GdkPixbuf/GTK may use MIME metadata when selecting a decoder.
[[ -r /usr/share/mime/mime.cache ]] || { echo 'Missing shared MIME database' >&2; exit 1; }
mkdir -p "$APPDIR/usr/share/mime"
cp -a /usr/share/mime/. "$APPDIR/usr/share/mime/"
gcc -O2 -Wall -Wextra -o "$APPDIR/usr/bin/check-gtk-icons" \
    tools/check-gtk-icons.c $(pkg-config --cflags --libs gtk+-3.0)
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
        --executable "$APPDIR/usr/bin/check-gtk-icons" \
        "${loader_args[@]}" \
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
[[ -x "$EXTRACT/usr/bin/check-gtk-icons" ]] || exit 1
for loader in "${loaders[@]}"; do
    [[ -r "$EXTRACT/usr/lib/${loader##*/}" ]] || { echo "Missing bundled loader: $loader" >&2; exit 1; }
done
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
