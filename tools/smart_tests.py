#!/usr/bin/env python3
"""Pick Capture2Cloud tests from Git changes without duplicating the test harness.

Default: compare tracked and untracked working-tree changes with HEAD. When
clean, inspect the last commit (works immediately after a commit/push).

Usage:
  ./tests/run_all.sh --changed
  ./tests/run_all.sh --changed --dry-run
  python3 tools/smart_tests.py --base origin/main --dry-run
  python3 tools/smart_tests.py --files server/gtk_shell.c data/languages/01-fr.json
  ./tests/run_all.sh --only test_gtk_layout
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
from dataclasses import dataclass, field

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tests" / "run_all.sh"
ALL_SUITES = (
    "test_gamepad_bridge", "test_controller_shaping", "test_wup_touch",
    "test_output_pcble", "test_pcble_protocol", "test_jc2_stick_routing",
    "test_reset_method", "test_web_stream_auth", "test_change_watch",
    "test_ws_frame", "test_rtp_mtu", "test_h264_encoders",
    "test_gtk_layout", "test_drc_encoder", "wozt-assets", "wozt-tooltips",
    "javascript", "native", "security", "smart",
)


@dataclass
class Selection:
    suites: set[str] = field(default_factory=set)
    reasons: dict[str, list[str]] = field(default_factory=dict)
    full: bool = False
    manual: list[str] = field(default_factory=list)

    def add(self, path: str, *suites: str) -> None:
        for suite in suites:
            if suite not in ALL_SUITES:
                raise ValueError(f"Unrecognized suite: {suite}")
            self.suites.add(suite)
            self.reasons.setdefault(suite, []).append(path)

    def all(self, path: str, detail: str) -> None:
        self.full = True
        self.manual.append(f"{path}: {detail}")


def select_tests(paths: list[str]) -> Selection:
    """Conservative source-to-suite map. Unknown executable files -> ALL.

    No source is declared 'covered' by unrelated tests: when the repo has no
    dedicated test for a subsystem, require the full suite plus manual checks.
    """
    result = Selection()
    for raw in paths:
        path = raw.replace("\\", "/").removeprefix("./")
        if not path or path.startswith("../") or path.startswith("/"):
            result.all(raw, "unsafe or unrecognized path")
            continue
        name = Path(path).name
        if path.startswith("docs/") or path in {"README.md", "LICENSE", ".gitignore"}:
            continue
        if path.startswith("tests/c/test_") and name.endswith(".c"):
            suite = name[:-2]
            if suite in ALL_SUITES:
                result.add(path, suite)
            else:
                result.all(path, "new C test not registered in the harness")
        elif path in {"tests/test_smart_tests.py", "tools/smart_tests.py"}:
            result.add(path, "smart")
        elif path == "tests/run_all.sh":
            result.all(path, "test harness changed")
        elif path in {"tests/run_tests.js", "tests/dom_stub.js"} or path.startswith("tests/browser/"):
            result.add(path, "javascript")
        elif path.startswith("tests/security/"):
            result.add(path, "security")
        elif path.startswith("tests/native/"):
            result.add(path, "native")
        elif path.startswith("tests/"):
            result.all(path, "unmapped test component")
        elif path.startswith("server/"):
            stem = name.rsplit(".", 1)[0]
            mapping = {
                "gtk_shell": ("test_gtk_layout", "wozt-tooltips"),
                "wozt_ui_standard": ("test_gtk_layout", "wozt-assets", "wozt-tooltips"),
                "controller_shaping": ("test_controller_shaping", "test_output_pcble", "test_gamepad_bridge"),
                "controller_state": ("test_controller_shaping", "test_output_pcble", "test_gamepad_bridge"),
                "local_pad": ("test_controller_shaping", "test_gtk_layout"),
                "gamepad_bridge": ("test_gamepad_bridge", "test_output_pcble"),
                "output_pcble": ("test_output_pcble", "test_pcble_protocol", "test_jc2_stick_routing"),
                "output_titan": ("test_gamepad_bridge", "test_output_pcble"),
                "output_backend": ("test_gamepad_bridge", "test_output_pcble"),
                "reset_method": ("test_reset_method", "test_web_stream_auth", "test_gtk_layout"),
                "web_stream": ("test_web_stream_auth", "test_ws_frame", "javascript", "security"),
                "ws_frame": ("test_ws_frame", "test_web_stream_auth"),
                "switch_stream": ("test_ws_frame", "test_web_stream_auth", "native"),
                "gst_webrtc": ("test_rtp_mtu", "test_h264_encoders"),
                "video_capture": ("test_change_watch", "test_h264_encoders"),
                "drc_encoder": ("test_drc_encoder",),
                "wiiu_pad": ("test_drc_encoder", "test_wup_touch"),
                "wup_stream": ("test_wup_touch", "test_ws_frame"),
                "wup_touch": ("test_wup_touch", "test_jc2_stick_routing"),
            }
            if name.endswith((".c", ".h")) and stem in mapping:
                result.add(path, *mapping[stem])
            elif stem in {"app_config", "app_settings", "capture2cloud", "audio_capture", "old3ds_encoder"}:
                result.all(path, "shared runtime or subsystem without direct regression coverage")
            else:
                result.all(path, "unknown server source: fail safe")
        elif path.startswith("shared/"):
            result.all(path, "shared protocol/API affects several clients")
        elif path.startswith("data/languages/"):
            result.add(path, "wozt-assets", "wozt-tooltips", "test_gtk_layout")
        elif path.startswith("data/themes/") or path.startswith("assets/"):
            result.add(path, "wozt-assets", "test_gtk_layout")
        elif path.startswith("data/"):
            result.all(path, "unknown runtime data")
        elif path.startswith("web/"):
            result.add(path, "javascript", "test_web_stream_auth")
        elif path.startswith("pcble_backend/"):
            result.add(path, "test_output_pcble", "test_pcble_protocol", "test_jc2_stick_routing")
            result.manual.append(f"{path}: test real Bluetooth pairing/reconnection on hardware if affected")
        elif path in {"tools/check-wozt-assets.py", "tools/check-gtk-icons.c"}:
            result.add(path, "wozt-assets", "test_gtk_layout")
        elif path == "tools/check-wozt-tooltips.py":
            result.add(path, "wozt-tooltips")
        elif path.startswith("tools/") or path.startswith("scripts/") or path.startswith(".github/"):
            result.all(path, "build/release/tooling needs full regression + relevant build checks")
        elif path.startswith(("android/", "3ds/", "wiiu_console/", "wiiu_gamepad/", "wiiu_wupstream/", "switch_homebrew/")):
            result.all(path, "native client needs platform toolchain build and device smoke test")
        else:
            result.all(path, "unmapped file: fail safe")
    return result


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(ROOT), *args], stderr=subprocess.PIPE)


def parse_null(raw: bytes) -> list[str]:
    return [x.decode("utf-8", errors="surrogateescape") for x in raw.split(b"\0") if x]


def generated_cache(path: str) -> bool:
    """Do not run the full suite due solely to generated Python/build caches."""
    parts = Path(path).parts
    return (any(p in {"__pycache__", ".pytest_cache", ".mypy_cache", ".build"} for p in parts)
            or path.endswith((".pyc", ".pyo", ".swp", "~")))


def changed_files(base: str | None = None, include_last_commit: bool = True) -> tuple[list[str], str]:
    # `git diff HEAD` includes staged + unstaged changes, including removals.
    # Untracked files matter too, especially new tests or source modules.
    work = set(parse_null(git("diff", "--name-only", "-z", base or "HEAD", "--")))
    untracked = set(parse_null(git("ls-files", "--others", "--exclude-standard", "-z")))
    paths = {p for p in (work | untracked) if not generated_cache(p)}
    if paths:
        return sorted(paths), f"working tree + untracked{' since ' + base if base else ''}"
    if base:
        return [], f"no changes since {base}"
    if not include_last_commit:
        return [], "clean working tree (last commit disabled)"
    # After push, compare HEAD to its first parent. This works for ordinary
    # and merge commits (diff-tree -r HEAD omits merges by default).
    try:
        git("rev-parse", "--verify", "HEAD^1")
    except subprocess.CalledProcessError:
        raw = git("diff-tree", "--root", "--no-commit-id", "--name-only", "-r", "-z", "HEAD")
    else:
        raw = git("diff", "--name-only", "-z", "HEAD^1", "HEAD", "--")
    return sorted({p for p in parse_null(raw) if not generated_cache(p)}), "last commit HEAD"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true", help="print selection, do not run")
    ap.add_argument("--base", metavar="REV", help="compare REV to working tree, instead of HEAD")
    ap.add_argument("--no-last-commit", action="store_true", help="do nothing if working tree is clean")
    ap.add_argument("--files", nargs="+", metavar="PATH", help="explicit file paths (for testing/manual selection)")
    ap.add_argument("--all", action="store_true", help="force the entire test suite")
    args = ap.parse_args(argv)
    try:
        if args.files is not None:
            paths, source = sorted(set(args.files)), "explicit paths"
        else:
            paths, source = changed_files(args.base, not args.no_last_commit)
    except (subprocess.CalledProcessError, OSError) as e:
        print(f"ERROR: git change detection failed: {e}", file=sys.stderr)
        return 2
    selection = select_tests(paths)
    if args.all:
        selection.full = True
    print(f"Capture2Cloud smart tests | basis: {source}", flush=True)
    for path in paths:
        print(f"  changed: {path}", flush=True)
    for warning in selection.manual:
        print(f"  attention: {warning}", flush=True)
    if selection.full:
        print("  plan: FULL regression suite (conservative fallback)", flush=True)
        command = ["bash", str(RUNNER)]
    elif selection.suites:
        suites = [x for x in ALL_SUITES if x in selection.suites]
        for suite in suites:
            print(f"  selected: {suite} ({', '.join(selection.reasons[suite])})", flush=True)
        command = ["bash", str(RUNNER), "--only", ",".join(suites)]
    else:
        print("  plan: no executable test needed (docs only / no changes)", flush=True)
        return 0
    print("  command: " + " ".join(command), flush=True)
    if args.dry_run:
        return 0
    return subprocess.run(command, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
