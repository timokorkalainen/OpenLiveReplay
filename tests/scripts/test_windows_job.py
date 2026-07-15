#!/usr/bin/env python3
"""Unit tests for Windows kill-on-close process-tree ownership."""

from __future__ import annotations

import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from tools import windows_job


class WindowsJobTests(unittest.TestCase):
    def test_environment_block_rejects_case_insensitive_duplicates(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate.*PATH"):
            windows_job._environment_block({"PATH": "controlled", "Path": "poison"})

    def test_non_windows_path_preserves_subprocess_behavior(self) -> None:
        completed = subprocess.CompletedProcess(["child"], 23)
        with mock.patch.object(
            windows_job.subprocess, "run", return_value=completed
        ) as run:
            result = windows_job.run_process_tree(["child"], enabled=False, cwd="here")
        self.assertEqual(result, 23)
        run.assert_called_once_with(["child"], cwd="here", check=False)

    def test_windows_child_is_assigned_before_its_primary_thread_resumes(self) -> None:
        events: list[str] = []
        process = mock.Mock(pid=2468)
        process.wait.return_value = 0
        job = mock.MagicMock()
        job.__enter__.return_value = job
        job.assign.side_effect = lambda _pid: events.append("assign")

        def popen(*_args: object, **_kwargs: object) -> mock.Mock:
            events.append("create-suspended")
            return process

        with (
            mock.patch.object(windows_job, "KillOnCloseJob", return_value=job),
            mock.patch.object(windows_job.subprocess, "Popen", side_effect=popen) as create,
            mock.patch.object(
                windows_job,
                "_resume_process_threads",
                create=True,
                side_effect=lambda _pid: events.append("resume"),
            ),
        ):
            self.assertEqual(
                windows_job.run_process_tree(
                    ["child"],
                    enabled=True,
                    creationflags=0x08000000,
                ),
                0,
            )

        self.assertEqual(events, ["create-suspended", "assign", "resume"])
        self.assertEqual(
            create.call_args.kwargs["creationflags"]
            & windows_job._CREATE_SUSPENDED,
            windows_job._CREATE_SUSPENDED,
        )
        self.assertEqual(
            create.call_args.kwargs["creationflags"] & 0x08000000,
            0x08000000,
        )

    def test_windows_job_closes_on_wait_exception(self) -> None:
        process = mock.Mock(pid=1234)
        process.wait.side_effect = KeyboardInterrupt
        job = mock.MagicMock()
        job.__enter__.return_value = job
        with (
            mock.patch.object(windows_job, "KillOnCloseJob", return_value=job),
            mock.patch.object(windows_job.subprocess, "Popen", return_value=process),
            mock.patch.object(windows_job, "_resume_process_threads"),
            self.assertRaises(KeyboardInterrupt),
        ):
            windows_job.run_process_tree(["child"], enabled=True)
        job.assign.assert_called_once_with(1234)
        job.__exit__.assert_called_once()

    def test_assignment_failure_terminates_direct_child_before_raising(self) -> None:
        process = mock.Mock(pid=4321)
        job = mock.MagicMock()
        job.__enter__.return_value = job
        job.assign.side_effect = OSError("assign failed")
        job.close.side_effect = OSError("job close failed")
        job.__exit__.side_effect = lambda *args: windows_job.KillOnCloseJob.__exit__(
            job, *args
        )
        with (
            mock.patch.object(windows_job, "KillOnCloseJob", return_value=job),
            mock.patch.object(windows_job.subprocess, "Popen", return_value=process),
            self.assertRaisesRegex(OSError, "assign failed"),
        ):
            windows_job.run_process_tree(["child"], enabled=True)
        process.kill.assert_called_once()
        process.wait.assert_called_once()

    def test_assignment_failure_preserves_primary_error_when_cleanup_also_fails(self) -> None:
        process = mock.Mock(pid=4322)
        process.kill.side_effect = OSError("kill failed")
        process.wait.side_effect = OSError("wait failed")
        job = mock.MagicMock()
        job.__enter__.return_value = job
        job.assign.side_effect = OSError("assign failed")

        with (
            mock.patch.object(windows_job, "KillOnCloseJob", return_value=job),
            mock.patch.object(windows_job.subprocess, "Popen", return_value=process),
            self.assertRaisesRegex(OSError, "assign failed"),
        ):
            windows_job.run_process_tree(["child"], enabled=True)

        process.kill.assert_called_once()
        process.wait.assert_called_once()
        job.__exit__.assert_called_once()

    def test_created_process_is_terminated_if_native_wrapper_construction_fails(self) -> None:
        kernel32 = mock.Mock()
        information = windows_job._ProcessInformation()
        information.process = 101
        information.thread = 202
        with (
            mock.patch.object(
                windows_job, "_NativeProcess", side_effect=RuntimeError("wrap failed")
            ),
            self.assertRaisesRegex(RuntimeError, "wrap failed"),
        ):
            windows_job._adopt_created_process(kernel32, information)

        kernel32.TerminateProcess.assert_called_once_with(101, 1)
        self.assertEqual(
            kernel32.CloseHandle.call_args_list,
            [mock.call(202), mock.call(101)],
        )

    def test_private_desktop_is_bound_to_startup_and_closed_after_job(self) -> None:
        events: list[str] = []
        process = mock.Mock(pid=9876)
        process.wait.return_value = 0
        job = mock.MagicMock()
        job.__enter__.return_value = job
        job.__exit__.side_effect = lambda *_args: events.append("job-close")
        desktop = mock.MagicMock()
        desktop.__enter__.return_value = desktop
        desktop.__exit__.side_effect = lambda *_args: events.append("desktop-close")
        desktop.path = "winsta0\\private-test"

        with (
            mock.patch.object(windows_job, "KillOnCloseJob", return_value=job),
            mock.patch.object(
                windows_job, "PrivateDesktop", return_value=desktop, create=True
            ),
            mock.patch.object(
                windows_job,
                "_popen_private_desktop",
                return_value=process,
                create=True,
            ) as create,
            mock.patch.object(windows_job, "_resume_process_threads"),
        ):
            self.assertEqual(
                windows_job.run_process_tree(
                    ["child"], enabled=True, private_desktop=True
                ),
                0,
            )

        self.assertEqual(create.call_args.kwargs["desktop_path"], desktop.path)
        self.assertEqual(events, ["job-close", "desktop-close"])

    @unittest.skipUnless(os.name == "nt", "requires Windows desktop APIs")
    def test_real_private_desktop_launch_preserves_child_exit(self) -> None:
        self.assertEqual(
            windows_job.run_process_tree(
                [sys.executable, "-c", "raise SystemExit(19)"],
                enabled=True,
                private_desktop=True,
            ),
            19,
        )

    @unittest.skipUnless(os.name == "nt", "requires Windows desktop APIs")
    def test_private_desktop_preserves_redirected_standard_handles(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            stdout_path = Path(temporary) / "stdout.txt"
            stderr_path = Path(temporary) / "stderr.txt"
            with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                result = windows_job.run_process_tree(
                    [
                        sys.executable,
                        "-c",
                        'import sys; print("OUT"); print("ERR", file=sys.stderr); '
                        "raise SystemExit(7)",
                    ],
                    enabled=True,
                    private_desktop=True,
                    stdout=stdout,
                    stderr=stderr,
                )

            self.assertEqual(result, 7)
            self.assertEqual(stdout_path.read_text(encoding="utf-8").strip(), "OUT")
            self.assertEqual(stderr_path.read_text(encoding="utf-8").strip(), "ERR")

    @unittest.skipUnless(
        os.name == "nt" and os.environ.get("OLR_RUN_WINDOWS_JOB_INTEGRATION") == "1",
        "set OLR_RUN_WINDOWS_JOB_INTEGRATION=1 for the real Windows process-tree probe",
    )
    def test_abrupt_parent_exit_terminates_child_and_grandchild(self) -> None:
        helper = Path(__file__).with_name("windows_job_probe.py")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.TerminateProcess.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)

        with tempfile.TemporaryDirectory() as temporary:
            state_file = Path(temporary) / "processes.json"
            parent = subprocess.Popen(
                [sys.executable, str(helper), "parent", str(state_file)]
            )
            handles: list[int] = []
            try:
                deadline = time.monotonic() + 10
                while not state_file.is_file() and time.monotonic() < deadline:
                    if parent.poll() is not None:
                        self.fail(f"probe parent exited early with {parent.returncode}")
                    time.sleep(0.02)
                self.assertTrue(state_file.is_file(), "probe child did not report its PIDs")
                process_ids = json.loads(state_file.read_text(encoding="utf-8"))
                for name in ("child", "grandchild"):
                    handle = kernel32.OpenProcess(0x00100001, False, process_ids[name])
                    self.assertTrue(handle, f"could not open live {name} process")
                    handles.append(handle)

                parent.kill()
                parent.wait(timeout=5)
                for handle in handles:
                    self.assertEqual(
                        kernel32.WaitForSingleObject(handle, 5000),
                        0,
                        "Job Object member survived abrupt owner termination",
                    )
                print(
                    json.dumps(
                        {
                            "parent": parent.pid,
                            **process_ids,
                            "descendants_terminated": True,
                        },
                        sort_keys=True,
                    )
                )
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait(timeout=5)
                for handle in handles:
                    if kernel32.WaitForSingleObject(handle, 0) == 0x102:
                        kernel32.TerminateProcess(handle, 1)
                    kernel32.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
