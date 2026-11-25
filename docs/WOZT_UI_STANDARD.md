# Capture2Cloud — WOZT GTK Standard v1.2

Specification: https://github.com/wozt/wozt-gtk-standard/blob/main/STANDARD.md

## Project-specific navigation rules

1. The eight former sidebar pages are now framed sections on one vertically
   scrolling dashboard: Overview, Clients, Controller output, Local controller
   input, Capture, Local monitor, Reset method, Maintenance.
2. **Clients keeps its own five `GtkNotebook` tabs** (Browser, Switch/Android,
   Wii U GamePad, Wii U Console, Old 3DS). No client settings are merged.
3. At large widths (1810 px and above), Overview and Clients span the full
   width, while the other six functional sections form three pairs. At narrower
   sizes, sections stack vertically and there is no horizontal scroller.

## Window and appearance

- GTK3/X11 borderless main settings window; custom title bar; eight resize
  zones; minimize, maximize/restore, hide-to-tray close action.
- Square frames with compact WUP Stream-like controls. Seven theme JSON files
  are bundled under `data/themes`; nine translations under `data/languages`.
- Theme and language selection persist to `WOZT_UI_THEME` and
  `WOZT_UI_LANGUAGE` in `scripts/.env`; options apply immediately, without
  restarting streams or controllers.
- GTK translation uses the original English text when a key is not present
  in the selected JSON. Runtime status/diagnostic text is intentionally not
  overwritten by changes to the language selector.

## Packaging and CI

- `tools/check-wozt-assets.py` validates JSONs, IDs and required colors.
- `tools/build-appimage.sh` rebuilds the native binary and PCBLE helpers,
  packages resource files, and checks the extracted AppImage. The build must
  also pass the GTK icon runtime smoke test.
- `.github/workflows/ci-appimage.yml` gates a versioned release on all build,
  regression, icon and resource checks. Only successful pushes on `main`
  publish the next four-part GitHub release tag; rerunning a commit does not
  duplicate a release.
- In the AppImage, settings live in
  `${XDG_CONFIG_HOME:-$HOME/.config}/capture2cloud/.env`, never inside the
  compressed image. The source-tree launcher still uses `scripts/.env`.
- Privileged Bluetooth and polkit installation is intentionally separate from
  the AppImage. The AppImage must not silently request root permissions.

## Verification on the actual machine

1. Open GTK dashboard, verify eight frames and all five client tabs.
2. Drag the title, double-click maximize; resize all edges/corners.
3. Test at 820 px, 1100 px and 1920 px width; no horizontal scrolling.
4. Switch all seven themes; cycle FR → JA → DE → EN → FR.
5. While streaming to the Wii U GamePad and Switch, change theme, language
   and size without interrupting capture or controller input.
6. Verify window/taskbar/Alt+Tab icon independently of the tray; e.g.
   `xprop WM_CLASS _NET_WM_ICON` on X11.
7. Test the release AppImage from a different directory and inspect its
   extracted GTK icon runtime and JSON resource files.

The standard is a reference specification, not a linked GTK library. The UI
module is application-local and uses json-glib, which the PCBLE backend already
requires through the existing dependency install script.
