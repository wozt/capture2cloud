#!/usr/bin/env python3
"""Regression tests for the Git-aware Capture2Cloud test selector."""
from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest import mock

SOURCE = Path(__file__).resolve().parents[1] / "tools" / "smart_tests.py"
spec = importlib.util.spec_from_file_location("smart_tests", SOURCE)
smart = importlib.util.module_from_spec(spec)
# Dataclasses inspect sys.modules during class definition.
import sys
sys.modules[spec.name] = smart
spec.loader.exec_module(smart)


class SmartTestSelection(unittest.TestCase):
    def selected(self, *paths):
        return smart.select_tests(list(paths))

    def test_gtk_only(self):
        result = self.selected("server/gtk_shell.c")
        self.assertFalse(result.full)
        self.assertEqual(result.suites, {"test_gtk_layout", "wozt-tooltips"})

    def test_web_only(self):
        result = self.selected("web/player.js")
        self.assertEqual(result.suites, {"javascript", "test_web_stream_auth"})
        self.assertFalse(result.full)

    def test_localizations(self):
        result = self.selected("data/languages/01-fr.json")
        self.assertEqual(result.suites, {"test_gtk_layout", "wozt-assets", "wozt-tooltips"})

    def test_pcble_only(self):
        result = self.selected("server/output_pcble.c")
        self.assertEqual(result.suites, {"test_output_pcble", "test_pcble_protocol", "test_jc2_stick_routing"})

    def test_native_pcble_manual_hardware(self):
        result = self.selected("pcble_backend/src/main.c")
        self.assertFalse(result.full)
        self.assertTrue(result.manual)

    def test_single_unit_test(self):
        result = self.selected("tests/c/test_gtk_layout.c")
        self.assertEqual(result.suites, {"test_gtk_layout"})

    def test_docs_only(self):
        result = self.selected("README.md", "docs/setup.md")
        self.assertFalse(result.full)
        self.assertFalse(result.suites)

    def test_common_header_fails_safe(self):
        result = self.selected("shared/protocol.h")
        self.assertTrue(result.full)

    def test_server_without_direct_test_fails_safe(self):
        result = self.selected("server/audio_capture.c")
        self.assertTrue(result.full)
        self.assertTrue(result.manual)

    def test_unmapped_fails_safe(self):
        self.assertTrue(self.selected("new_runtime_magic.c").full)

    def test_mixed_changes_union(self):
        result = self.selected("server/gtk_shell.c", "server/drc_encoder.c")
        self.assertEqual(result.suites, {"test_gtk_layout", "wozt-tooltips", "test_drc_encoder"})
        self.assertFalse(result.full)

    def test_runner_change_requires_full(self):
        self.assertTrue(self.selected("tests/run_all.sh").full)

    def test_selftest_paths(self):
        self.assertEqual(self.selected("tools/smart_tests.py").suites, {"smart"})
        self.assertEqual(self.selected("tests/test_smart_tests.py").suites, {"smart"})

    def test_clean_tree_checks_last_commit(self):
        def fake_git(*args):
            if args[0] == "diff": return b""
            if args[0] == "ls-files": return b""
            if args[0] == "rev-parse":
                raise smart.subprocess.CalledProcessError(1, args)
            if args[0] == "diff-tree": return b"server/gtk_shell.c\x00"
            raise AssertionError(args)
        with mock.patch.object(smart, "git", side_effect=fake_git):
            paths, description = smart.changed_files()
        self.assertEqual(paths, ["server/gtk_shell.c"])
        self.assertEqual(description, "last commit HEAD")

    def test_working_changes_precede_last_commit(self):
        def fake_git(*args):
            if args[0] == "diff": return b"server/gtk_shell.c\x00"
            if args[0] == "ls-files": return b"tests/c/test_gtk_layout.c\x00tests/__pycache__/junk.pyc\x00"
            raise AssertionError("must not ask about last commit")
        with mock.patch.object(smart, "git", side_effect=fake_git):
            paths, description = smart.changed_files()
        self.assertEqual(paths, ["server/gtk_shell.c", "tests/c/test_gtk_layout.c"])
        self.assertIn("working tree", description)

    def test_no_last_commit_option(self):
        def fake_git(*args):
            if args[0] in {"diff", "ls-files"}: return b""
            raise AssertionError(args)
        with mock.patch.object(smart, "git", side_effect=fake_git):
            paths, description = smart.changed_files(include_last_commit=False)
        self.assertEqual(paths, [])
        self.assertIn("disabled", description)

    def test_generated_python_cache_is_ignored(self):
        self.assertTrue(smart.generated_cache("tests/__pycache__/test_smart_tests.cpython-313.pyc"))
        self.assertFalse(smart.generated_cache("tests/test_smart_tests.py"))

    def test_last_commit_merge_uses_first_parent(self):
        def fake_git(*args):
            if args[0] == "diff" and "HEAD^1" not in args: return b""
            if args[0] == "ls-files": return b""
            if args[0] == "rev-parse": return b"abcdef\n"
            if args[0] == "diff": return b"server/gtk_shell.c\x00"
            raise AssertionError(args)
        with mock.patch.object(smart, "git", side_effect=fake_git):
            paths, description = smart.changed_files()
        self.assertEqual(paths, ["server/gtk_shell.c"])
        self.assertEqual(description, "last commit HEAD")

    def test_base_includes_dirty_work(self):
        calls = []
        def fake_git(*args):
            calls.append(args)
            return b"server/reset_method.c\x00" if args[0] == "diff" else b""
        with mock.patch.object(smart, "git", side_effect=fake_git):
            paths, _ = smart.changed_files("origin/main")
        self.assertEqual(paths, ["server/reset_method.c"])
        self.assertEqual(calls[0][:4], ("diff", "--name-only", "-z", "origin/main"))

    def test_dry_run_does_not_execute(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), mock.patch.object(smart.subprocess, "run") as run:
            rc = smart.main(["--files", "server/gtk_shell.c", "--dry-run"])
        self.assertEqual(rc, 0)
        self.assertIn("test_gtk_layout", output.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
