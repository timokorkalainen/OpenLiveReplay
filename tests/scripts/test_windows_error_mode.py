#!/usr/bin/env python3
"""Tests for inherited Windows child-process error-mode containment."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[2] / "tools" / "windows_error_mode.py"


def load_module():
    if not MODULE_PATH.is_file():
        raise AssertionError(f"missing Windows error-mode helper: {MODULE_PATH}")
    spec = importlib.util.spec_from_file_location("windows_error_mode", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load Windows error-mode helper")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsErrorModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.module = load_module()

    def test_sets_exact_mode_and_restores_prior_mode(self) -> None:
        calls: list[int] = []

        def set_error_mode(value: int) -> int:
            calls.append(value)
            return 0x1234

        with mock.patch.object(self.module, "_set_error_mode", side_effect=set_error_mode):
            with self.module.suppress_windows_error_dialogs(True):
                self.assertEqual(calls, [0x8003])
        self.assertEqual(calls, [0x8003, 0x1234])

    def test_restores_prior_mode_when_child_creation_raises(self) -> None:
        with mock.patch.object(
            self.module, "_set_error_mode", side_effect=[0x2468, 0]
        ) as set_mode:
            with self.assertRaisesRegex(RuntimeError, "spawn failed"):
                with self.module.suppress_windows_error_dialogs(True):
                    raise RuntimeError("spawn failed")
        self.assertEqual([call.args[0] for call in set_mode.call_args_list], [0x8003, 0x2468])

    def test_non_windows_path_is_a_no_op(self) -> None:
        with mock.patch.object(self.module, "_set_error_mode") as set_mode:
            with self.module.suppress_windows_error_dialogs(False):
                pass
        set_mode.assert_not_called()


if __name__ == "__main__":
    unittest.main()
