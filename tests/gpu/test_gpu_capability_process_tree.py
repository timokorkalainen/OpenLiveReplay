from __future__ import annotations

import dataclasses
import json
import sys
import os
import subprocess
import socket
import struct
import shutil
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock
import threading


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError, CompilerExecPermit, CompilerLaunchPurpose,
    CompilerPgidReported, FileIdentity, MacOSInspectionPgidReported,
)
from gpu_capability_process_tree import (  # noqa: E402
    MacOSRegisteredPgidAccountant,
    MacOSExecPermitAuthority,
    MacOSLibprocProvider,
    LinuxRendezvousSession,
    LinuxRendezvousClient,
    LinuxSeqpacketRendezvousServer,
    OwnedProcessIdentity,
    OwnedProcessTree,
    WindowsGenerationPeak,
    WindowsInvocationPeak,
    WindowsJobAccountingSnapshot,
    WindowsNativeJob,
    WindowsNativeRunAccountant,
    native_process_resident_bytes,
    verify_windows_nested_job_support,
    LinuxDelegatedCgroupSupervisor,
    WindowsSlotPeak,
    decode_linux_rendezvous_frame,
    decode_linux_rendezvous_reply,
    encode_linux_rendezvous_frame,
    encode_linux_rendezvous_reply,
    linux_systemd_run_prefix,
    _linux_pid_descends_from,
    encode_macos_pgid_message,
    decode_macos_pgid_message,
)


class WindowsProcessTreeAccountingTests(unittest.TestCase):
    def test_unrelated_aggregate_total_increase_is_not_assignment_evidence(self):
        class AggregateJob:
            def __init__(_self):
                _self.calls = 0
                _self.total = 0

            def accounting_totals(_self):
                return _self.total, _self.total, 0

            def process_ids(_self):
                return (99,) if _self.total else ()

            def contains_process_handle(_self, _handle):
                return False

            def assign_process_handle(_self, _handle):
                _self.calls += 1
                _self.total += 1
                raise RuntimeError("unrelated aggregate increase")

        class GenerationJob:
            def __init__(_self): _self.calls = 0
            def accounting_totals(_self): return 0, 0, 0
            def process_ids(_self): return ()
            def contains_process_handle(_self, _handle): return False
            def assign_process_handle(_self, _handle): _self.calls += 1

        aggregate = AggregateJob()
        generation_job = GenerationJob()
        accountant = object.__new__(WindowsNativeRunAccountant)
        accountant.job = aggregate
        accountant._generation_jobs = {(2, 5): generation_job}
        accountant._archived_generation_jobs = {}
        accountant._aggregate_only_generation_jobs = set()
        accountant._discarded_generation_jobs = set()
        accountant._aggregate_assignment_intents = {}
        accountant._generation_assignment_intents = {}
        accountant._aggregate_assignment_progress = set()
        accountant._generation_assignment_progress = set()
        accountant._assigned_generation_jobs = set()
        accountant._retain = mock.Mock()

        for _attempt in range(2):
            with self.assertRaisesRegex(
                RuntimeError, "unrelated aggregate increase"
            ):
                accountant.assign_generation_process(
                    2, 5, 41, 41, "worker:2:5"
                )
        self.assertEqual(aggregate.calls, 2)
        self.assertEqual(generation_job.calls, 0)
        self.assertEqual(
            accountant.generation_job_assignment_state(2, 5), "unassigned"
        )

    def test_generation_job_terminal_transitions_are_retryable_at_every_boundary(self):
        class Job:
            def __init__(_self, total):
                _self.close_calls = 0
                _self.total = total

            def drain_notifications(_self): return ()
            def accounting_totals(_self): return _self.total, 0, _self.total
            def process_ids(_self): return ()
            def peak_commit_charge_bytes(_self): return 17
            def close(_self): _self.close_calls += 1

        for kind in ("aggregate-only", "discard", "archive"):
            for stage in ("close", "terminal", "remove"):
                for position in ("before", "after"):
                    with self.subTest(
                        kind=kind, stage=stage, position=position
                    ):
                        key = (2, 5)
                        job = Job(1 if kind == "archive" else 0)
                        accountant = object.__new__(WindowsNativeRunAccountant)
                        accountant._generation_jobs = {key: job}
                        accountant._archived_generation_jobs = {}
                        accountant._discarded_generation_jobs = set()
                        accountant._aggregate_only_generation_jobs = set()
                        accountant._aggregate_assignment_progress = (
                            {key} if kind != "discard" else set()
                        )
                        accountant._generation_assignment_progress = (
                            {key} if kind == "archive" else set()
                        )
                        accountant._assigned_generation_jobs = (
                            {key} if kind == "archive" else set()
                        )
                        accountant._aggregate_only_job_close_progress = set()
                        accountant._generation_terminal_close_progress = set()
                        accountant._pending_generation_archives = {}
                        faulted = False

                        def inject(actual_kind, actual_stage, actual_position,
                                   worker, generation):
                            nonlocal faulted
                            if (
                                (actual_kind, actual_stage, actual_position)
                                == (kind, stage, position)
                                and (worker, generation) == key
                                and not faulted
                            ):
                                faulted = True
                                raise RuntimeError(
                                    f"{kind} {stage} {position} fault"
                                )

                        def finalize():
                            if kind == "aggregate-only":
                                return accountant.finalize_aggregate_only_generation_job(
                                    *key
                                )
                            if kind == "discard":
                                return accountant.discard_generation_job(*key)
                            return accountant.archive_generation_job(
                                *key, time.monotonic() + 1.0
                            )

                        with mock.patch(
                            "gpu_capability_process_tree._windows_generation_terminal_transition",
                            side_effect=inject,
                            create=True,
                        ), self.assertRaisesRegex(
                            RuntimeError, f"{kind} {stage} {position} fault"
                        ):
                            finalize()

                        published = (
                            key in accountant._aggregate_only_generation_jobs
                            if kind == "aggregate-only"
                            else key in accountant._discarded_generation_jobs
                            if kind == "discard"
                            else key in accountant._archived_generation_jobs
                        )
                        if stage == "close":
                            self.assertFalse(published)
                        elif stage == "terminal" and position == "before":
                            self.assertFalse(published)
                        else:
                            self.assertTrue(published)
                        if stage == "remove" and position == "after":
                            self.assertNotIn(key, accountant._generation_jobs)
                        else:
                            self.assertIn(key, accountant._generation_jobs)
                        if published and key in accountant._generation_jobs:
                            self.assertEqual(
                                accountant.generation_job_assignment_state(*key),
                                "generation-assigned"
                                if kind == "archive"
                                else "aggregate-only"
                                if kind == "aggregate-only"
                                else "unassigned",
                            )

                        with mock.patch(
                            "gpu_capability_process_tree._windows_generation_terminal_transition",
                            side_effect=inject,
                            create=True,
                        ):
                            result = finalize()
                        self.assertEqual(job.close_calls, 1)
                        self.assertNotIn(key, accountant._generation_jobs)
                        if kind == "archive":
                            self.assertEqual(result, (1, 17))

    def test_aggregate_only_assignment_finalizes_without_losing_identity(self):
        class Job:
            def __init__(_self, fail_assignment=False):
                _self.fail_assignment = fail_assignment
                _self.calls = 0
                _self.close_calls = 0
                _self.total = 0
                _self.members = set()

            def assign_process_handle(_self, handle):
                _self.calls += 1
                if _self.fail_assignment:
                    raise RuntimeError("generation assignment failed")
                _self.total = 1
                _self.members.add(handle)

            def process_ids(_self):
                return tuple(_self.members)

            def contains_process_handle(_self, handle):
                return handle in _self.members

            def accounting_totals(_self):
                return _self.total, len(_self.members), 0

            def drain_notifications(_self):
                return ()

            def peak_commit_charge_bytes(_self):
                return 19

            def close(_self):
                _self.close_calls += 1

        for position in ("before", "after"):
            with self.subTest(position=position):
                aggregate = Job()
                generation_job = Job(fail_assignment=True)
                accountant = object.__new__(WindowsNativeRunAccountant)
                accountant.job = aggregate
                accountant._generation_jobs = {(2, 5): generation_job}
                accountant._archived_generation_jobs = {}
                accountant._assigned_generation_jobs = set()
                accountant._discarded_generation_jobs = set()
                accountant._aggregate_only_generation_jobs = set()
                accountant._aggregate_only_job_close_progress = set()
                accountant._aggregate_assignment_intents = {}
                accountant._generation_assignment_intents = {}
                accountant._aggregate_assignment_progress = set()
                accountant._generation_assignment_progress = set()
                accountant._retain = mock.Mock()

                with self.assertRaisesRegex(
                    RuntimeError, "generation assignment failed"
                ):
                    accountant.assign_generation_process(
                        2, 5, 41, 41, "worker:2:5"
                    )
                self.assertEqual(
                    accountant.generation_job_assignment_state(2, 5),
                    "aggregate-only",
                )
                faulted = False

                def fail(worker, generation):
                    nonlocal faulted
                    if (worker, generation) == (2, 5) and not faulted:
                        faulted = True
                        raise RuntimeError(f"{position} partial finalize")

                before = fail if position == "before" else mock.Mock()
                after = fail if position == "after" else mock.Mock()
                with mock.patch(
                    "gpu_capability_process_tree._aggregate_only_before_finalize",
                    side_effect=before,
                    create=True,
                ), mock.patch(
                    "gpu_capability_process_tree._aggregate_only_finalize",
                    side_effect=after,
                    create=True,
                ):
                    with self.assertRaisesRegex(
                        RuntimeError, f"{position} partial finalize"
                    ):
                        accountant.finalize_aggregate_only_generation_job(2, 5)
                    self.assertIn((2, 5), accountant._generation_jobs)
                    self.assertNotIn(
                        (2, 5), accountant._aggregate_only_generation_jobs
                    )
                    accountant.finalize_aggregate_only_generation_job(2, 5)

                self.assertEqual(generation_job.close_calls, 1)
                self.assertNotIn((2, 5), accountant._generation_jobs)
                self.assertIn(
                    (2, 5), accountant._aggregate_only_generation_jobs
                )

                aggregate.total = 3
                aggregate.members.clear()
                accountant._handles = {41: 1, 42: 2, 43: 3}
                accountant._purposes = {
                    41: "worker:2:5",
                    42: "compiler:2:5:0",
                    43: "compiler:2:5:1",
                }
                accountant._peaks = {41: 10, 42: 20, 43: 30}
                accountant._parent_peak = 0
                accountant._simultaneous_peak = 0
                accountant._complete = True
                raw = accountant.snapshot()
                self.assertEqual(
                    raw.archived_generation_identities, ((2, 5),)
                )
                self.assertEqual(raw.job_total_process_count, 3)
                generation = raw.slots[0].generations[0]
                self.assertEqual(
                    generation.worker_root_peak_working_set_bytes, 10
                )
                self.assertEqual(
                    tuple(
                        item.compiler_descendant_peak_tree_bytes
                        for item in generation.invocations
                    ),
                    (20, 30),
                )

    def test_generation_assignment_lost_replies_are_key_idempotent(self):
        class Job:
            def __init__(_self, fault):
                _self.fault = fault
                _self.calls = 0
                _self.total = 0
                _self.members = set()
                _self.closed = False

            def assign_process_handle(_self, handle):
                _self.calls += 1
                _self.total = 1
                _self.members.add(handle)
                if _self.fault and _self.calls == 1:
                    raise RuntimeError(f"{_self.fault} reply lost")

            def process_ids(_self):
                return tuple(_self.members)

            def contains_process_handle(_self, handle):
                return handle in _self.members

            def accounting_totals(_self):
                return _self.total, len(_self.members), _self.total - len(_self.members)

            def open_member_handle(_self, pid):
                self.assertIn(pid, _self.members)
                return 1000 + pid

            def process_identity(_self, _handle, pid):
                return OwnedProcessIdentity("windows", pid, f"start-{pid}")

            def process_parent_pid(_self, _handle):
                return os.getpid()

            def close_process_handle(_self, _handle):
                return None

            def drain_notifications(_self):
                return ()

            def peak_commit_charge_bytes(_self):
                return 17

            def close(_self):
                _self.closed = True

        for target in ("aggregate", "generation"):
            with self.subTest(target=target):
                aggregate = Job("aggregate" if target == "aggregate" else None)
                generation_job = Job(
                    "generation" if target == "generation" else None
                )
                accountant = object.__new__(WindowsNativeRunAccountant)
                accountant.job = aggregate
                accountant._generation_jobs = {(2, 5): generation_job}
                accountant._archived_generation_jobs = {}
                accountant._assigned_generation_jobs = set()
                accountant._discarded_generation_jobs = set()
                accountant._aggregate_only_generation_jobs = set()
                accountant._aggregate_only_job_close_progress = set()
                accountant._aggregate_assignment_intents = {}
                accountant._generation_assignment_intents = {}
                accountant._aggregate_assignment_progress = set()
                accountant._generation_assignment_progress = set()
                accountant._generation_terminal_close_progress = set()
                accountant._pending_generation_archives = {}
                accountant._handles = {}
                accountant._identities = {}
                accountant._purposes = {}
                accountant._parent_pids = {}
                accountant._peaks = {}
                accountant._parent_peak = 0
                accountant._simultaneous_peak = 0
                accountant._complete = True

                accountant.assign_generation_process(
                    2, 5, 41, 41, "worker:2:5"
                )

                self.assertEqual(aggregate.calls, 1)
                self.assertEqual(generation_job.calls, 1)
                self.assertTrue(
                    accountant.generation_job_assignment_completed(2, 5)
                )
                self.assertEqual(
                    accountant._identities[41],
                    OwnedProcessIdentity("windows", 41, "start-41"),
                )
                self.assertEqual(accountant._purposes[41], "worker:2:5")
                aggregate.members.clear()
                generation_job.members.clear()
                archived = accountant.archive_generation_job(
                    2, 5, time.monotonic() + 1.0
                )
                self.assertEqual(archived, (1, 17))
                self.assertTrue(generation_job.closed)
                memory = accountant.snapshot().memory_measurements()
                self.assertEqual(memory.job_total_process_count, 1)
                self.assertEqual(memory.retained_process_identity_count, 1)
                self.assertEqual(memory.surviving_job_process_count, 0)
                self.assertTrue(memory.accounting_complete)

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_native_job_retains_membership_and_kills_on_close(self) -> None:
        process = subprocess.Popen((sys.executable, "-c", "import time; time.sleep(30)"))
        job = WindowsNativeJob()
        try:
            job.assign_process_handle(int(process._handle))
            deadline = time.monotonic() + 2
            while process.pid not in job.process_ids() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertIn(process.pid, job.process_ids())
        finally:
            job.close()
            process.wait(timeout=5)
        self.assertIsNotNone(process.returncode)

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_native_nested_job_support_is_proven_without_breakaway(self) -> None:
        verify_windows_nested_job_support()

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_generation_job_is_created_empty_nested_and_archived_after_exit(self) -> None:
        accountant = WindowsNativeRunAccountant(os.getpid())
        process = subprocess.Popen((
            sys.executable, "-c", "import time; time.sleep(.05)"))
        try:
            accountant.create_generation_job(0, 0)
            accountant.begin_process_scope("worker:0:0")
            accountant.assign_generation_process(
                0, 0, int(process._handle), process.pid, "worker:0:0")
            process.wait(timeout=5)
            accountant.end_process_scope()
            archived = accountant.archive_generation_job(
                0, 0, time.monotonic() + 2)
            self.assertEqual(archived[0], 1)
            self.assertIn((0, 0), accountant._archived_generation_jobs)
        finally:
            accountant.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_unassigned_generation_job_discards_without_archive_identity(self):
        accountant = WindowsNativeRunAccountant(os.getpid())
        try:
            accountant.create_generation_job(3, 4)
            accountant.discard_generation_job(3, 4)
            accountant.discard_generation_job(3, 4)
            self.assertEqual(
                accountant.snapshot().archived_generation_identities, ()
            )
            with self.assertRaisesRegex(
                AuditInfrastructureError, "discarded"
            ):
                accountant.create_generation_job(3, 4)
        finally:
            accountant.close()

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_snapshot_preserves_sticky_incomplete_accounting_state(self):
        accountant = WindowsNativeRunAccountant(os.getpid())
        try:
            accountant._complete = False
            raw = accountant.snapshot()
            self.assertFalse(raw.accounting_complete)
            self.assertFalse(raw.memory_measurements().accounting_complete)
        finally:
            accountant.close()

    @unittest.skipUnless(os.name == "nt", "requires native Windows Job objects")
    def test_native_accountant_retains_short_lived_inspection_peak_and_seals(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            ready = Path(temporary) / "ready"
            release = Path(temporary) / "release"
            script = (
                "import pathlib,time;"
                "x=bytearray(8<<20);"
                "[x.__setitem__(i,1) for i in range(0,len(x),4096)];"
                f"pathlib.Path({str(ready)!r}).write_text('ready');"
                f"r=pathlib.Path({str(release)!r});"
                "d=time.monotonic()+10;"
                "\nwhile not r.exists() and time.monotonic()<d: time.sleep(.01)"
            )
            process = subprocess.Popen((sys.executable, "-c", script))
            accountant = WindowsNativeRunAccountant(os.getpid())
            try:
                accountant.begin_process_scope("inspection:0")
                accountant.assign_process_handle(
                    int(process._handle), process.pid, "inspection:0")
                deadline = time.monotonic() + 5
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists())
                accountant.observe(native_process_resident_bytes(os.getpid()))
                release.write_text("observed", encoding="ascii")
                process.wait(timeout=5)
                accountant.end_process_scope()
                accountant.seal_phase(time.monotonic() + 2)
                memory = accountant.snapshot().memory_measurements()
                self.assertGreaterEqual(memory.inspection_peak_tree_bytes, 8 << 20)
                self.assertEqual(memory.job_total_process_count,
                                 memory.retained_process_identity_count)
                self.assertTrue(memory.accounting_complete)
            finally:
                accountant.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    def test_job_accounts_every_identity_and_conservative_rss_formula(self) -> None:
        slots = (
            WindowsSlotPeak((
                WindowsGenerationPeak(20, (WindowsInvocationPeak(30), WindowsInvocationPeak(70))),
                WindowsGenerationPeak(50, (WindowsInvocationPeak(10),)),
            )),
            WindowsSlotPeak((
                WindowsGenerationPeak(40, (WindowsInvocationPeak(50),)),
            )),
        )
        snapshot = WindowsJobAccountingSnapshot(
            parent_peak_rss_bytes=100,
            maximum_simultaneous_working_set_bytes=205,
            inspection_tree_peak_bytes=(25, 80),
            slots=slots,
            job_peak_commit_charge_bytes=999,
            job_total_process_count=7,
            retained_process_identity_count=7,
            retained_inspection_process_identity_count=2,
            surviving_job_process_count=0,
            archived_generation_identities=((0, 0), (0, 1), (1, 0)),
        )
        memory = snapshot.memory_measurements()
        self.assertEqual(memory.inspection_peak_tree_bytes, 80)
        self.assertEqual(memory.worker_peak_rss_bytes, 180)
        self.assertEqual(memory.aggregate_peak_rss_upper_bound_bytes, 280)
        self.assertEqual(memory.job_total_process_count, 7)
        self.assertEqual(memory.retained_process_identity_count, 7)
        self.assertNotEqual(memory.aggregate_peak_rss_upper_bound_bytes,
                            memory.job_peak_commit_charge_bytes)
        self.assertTrue(memory.accounting_complete)

    def test_sequential_compilers_take_max_and_concurrent_slots_sum(self) -> None:
        first = WindowsGenerationPeak(
            10, (WindowsInvocationPeak(12), WindowsInvocationPeak(40),
                 WindowsInvocationPeak(18)))
        second = WindowsGenerationPeak(20, (WindowsInvocationPeak(50),))
        self.assertEqual(first.compiler_upper_bound_bytes, 40)
        self.assertEqual(first.upper_bound_bytes, 50)
        slots = (WindowsSlotPeak((first,)), WindowsSlotPeak((second,)))
        self.assertEqual(sum(slot.maximum_generation_upper_bound_bytes for slot in slots), 120)

    def test_missed_short_lived_member_fails_closed(self) -> None:
        snapshot = WindowsJobAccountingSnapshot(
            parent_peak_rss_bytes=1,
            maximum_simultaneous_working_set_bytes=1,
            inspection_tree_peak_bytes=(), slots=(),
            job_peak_commit_charge_bytes=1, job_total_process_count=2,
            retained_process_identity_count=1,
            retained_inspection_process_identity_count=0,
            surviving_job_process_count=0,
        )
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "incomplete Job process accounting"):
            snapshot.memory_measurements()

    def test_retained_post_exit_peak_query_failure_fails_accounting(self) -> None:
        class FailingJob:
            @staticmethod
            def drain_notifications():
                return ()

            @staticmethod
            def process_ids():
                return ()

            @staticmethod
            def accounting_totals():
                return (1, 0, 1)

            @staticmethod
            def process_memory(_handle):
                raise AuditInfrastructureError("unavailable")

        accountant = WindowsNativeRunAccountant.__new__(WindowsNativeRunAccountant)
        accountant.job = FailingJob()
        accountant._handles = {9: 1}
        accountant._identities = {
            9: OwnedProcessIdentity("windows", 9, "created")}
        accountant._purposes = {9: "inspection:0"}
        accountant._peaks = {9: 0}
        accountant._exited = set()
        accountant._finalized_peaks = set()
        accountant._active_purpose = None
        accountant._parent_peak = 0
        accountant._simultaneous_peak = 0
        accountant._complete = True
        with self.assertRaisesRegex(AuditInfrastructureError, "peak query"):
            accountant.observe(0)
        self.assertFalse(accountant._complete)

    def test_retained_post_exit_peak_is_queried_after_an_earlier_live_peak(self) -> None:
        class PeakJob:
            @staticmethod
            def drain_notifications():
                return ()

            @staticmethod
            def process_ids():
                return ()

            @staticmethod
            def accounting_totals():
                return (1, 0, 1)

            @staticmethod
            def process_memory(_handle):
                return (0, 100)

        accountant = WindowsNativeRunAccountant.__new__(WindowsNativeRunAccountant)
        accountant.job = PeakJob()
        accountant._handles = {9: 1}
        accountant._identities = {
            9: OwnedProcessIdentity("windows", 9, "created")}
        accountant._purposes = {9: "inspection:0"}
        accountant._peaks = {9: 10}
        accountant._exited = {9}
        accountant._finalized_peaks = set()
        accountant._active_purpose = None
        accountant._parent_peak = 0
        accountant._simultaneous_peak = 0
        accountant._complete = True
        accountant.observe(0)
        self.assertEqual(accountant._peaks[9], 100)

    def test_snapshot_derives_worker_slots_from_retained_native_purposes(self) -> None:
        class SnapshotJob:
            @staticmethod
            def accounting_totals():
                return (3, 0, 3)

            @staticmethod
            def peak_commit_charge_bytes():
                return 999

        accountant = WindowsNativeRunAccountant.__new__(WindowsNativeRunAccountant)
        accountant.job = SnapshotJob()
        accountant._handles = {1: 1, 2: 2, 3: 3}
        accountant._purposes = {
            1: "worker:0:0",
            2: "compiler:0:0:7",
            3: "compiler:0:0:7",
        }
        accountant._peaks = {1: 20, 2: 30, 3: 40}
        accountant._parent_peak = 10
        accountant._simultaneous_peak = 25
        accountant._complete = True
        accountant._generation_jobs = {}
        accountant._archived_generation_jobs = {(0, 0): (3, 999)}
        memory = accountant.snapshot().memory_measurements()
        self.assertEqual(memory.worker_peak_rss_bytes, 90)
        self.assertEqual(memory.aggregate_peak_rss_upper_bound_bytes, 100)

    def test_delayed_descendant_uses_native_ancestry_not_concurrent_scope(self) -> None:
        class AncestryJob:
            parents = {11: 10}

            @staticmethod
            def open_member_handle(pid):
                return pid + 100

            @staticmethod
            def process_identity(_handle, pid):
                return OwnedProcessIdentity("windows", pid, f"created-{pid}")

            def process_parent_pid(self, handle):
                return self.parents[handle - 100]

            @staticmethod
            def close_process_handle(_handle):
                return None

        accountant = WindowsNativeRunAccountant.__new__(WindowsNativeRunAccountant)
        accountant.job = AncestryJob()
        accountant._handles = {10: 110, 20: 120}
        accountant._identities = {
            pid: OwnedProcessIdentity("windows", pid, f"created-{pid}")
            for pid in (10, 20)}
        accountant._purposes = {10: "worker:0:0", 20: "worker:1:0"}
        accountant._parent_pids = {10: 1, 20: 1}
        accountant._peaks = {10: 1, 20: 1}
        accountant._active_purpose = "worker:1:0"
        accountant._complete = True
        accountant._retain(11)
        self.assertEqual(accountant._purposes[11], "worker:0:0")

    def test_tree_uses_pid_and_start_identity_and_reaps_exactly(self) -> None:
        tree = OwnedProcessTree("windows", "run-1")
        parent = OwnedProcessIdentity("windows", 100, "created-1")
        child = OwnedProcessIdentity("windows", 101, "created-2")
        tree.register(parent, None, "coordinator")
        tree.register(child, parent, "inspection")
        tree.observe_resident_bytes(child, current_bytes=3, peak_bytes=8)
        with self.assertRaisesRegex(AuditInfrastructureError, "start identity"):
            tree.register(OwnedProcessIdentity("windows", 101, "created-other"),
                          parent, "inspection")
        tree.mark_exited(child)
        tree.mark_exited(parent)
        tree.reap(child)
        tree.reap(parent)
        self.assertEqual(tree.surviving_owned_process_count, 0)
        self.assertEqual(tree.retained_identity_count, 2)
        self.assertFalse(tree.accounting_complete)


class LinuxSupervisorContractTests(unittest.TestCase):
    def test_missing_memory_controller_fails_without_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "cgroup.controllers").write_text("cpu io", encoding="ascii")
            supervisor = LinuxDelegatedCgroupSupervisor(root)
            with self.assertRaisesRegex(AuditInfrastructureError,
                                        "memory controller"):
                supervisor.prepare()
            self.assertFalse((root / "run").exists())
    def test_workflow_prefix_is_exact_and_external(self) -> None:
        prefix = linux_systemd_run_prefix(
            unit="olr-gpu-audit-7", uid=1000, gid=1001,
            working_directory=Path("/workspace"))
        self.assertEqual(prefix, (
            "sudo", "systemd-run", "--unit=olr-gpu-audit-7", "--wait", "--collect",
            "--pipe",
            "--property=Type=exec", "--property=Delegate=yes",
            "--property=MemoryAccounting=yes", "--property=MemoryMax=536870912",
            "--property=MemoryHigh=469762048", "--uid=1000", "--gid=1001",
            "--working-directory=/workspace", "python3",
            "tests/gpu/gpu_capability_process_tree.py", "prepare-linux-cgroup"))

    def test_seqpacket_frames_are_canonical_authenticated_and_replay_bound(self) -> None:
        token = bytes(range(32))
        fields = {"kind": "create-leaf", "run_id": "run-1", "worker_index": 2,
                  "generation": 3, "worker_pid": 0,
                  "nonce": "n-1", "sequence": 4}
        payload = encode_linux_rendezvous_frame(fields, token)
        self.assertEqual(decode_linux_rendezvous_frame(payload, token), fields)
        with self.assertRaisesRegex(AuditInfrastructureError, "rendezvous"):
            decode_linux_rendezvous_frame(payload + b" ", token)
        with self.assertRaisesRegex(AuditInfrastructureError, "rendezvous"):
            decode_linux_rendezvous_frame(payload, b"x" * 32)

    def test_session_rejects_invalid_fields_credentials_membership_and_replay(self) -> None:
        token = bytes(range(32))
        session = LinuxRendezvousSession(
            token=token, run_id="run-1", coordinator_pid=991,
            coordinator_uid=1000, coordinator_leaf="/cg/run/coordinator")
        fields = {"kind": "create-leaf", "run_id": "run-1", "worker_index": 2,
                  "generation": 3, "worker_pid": 0,
                  "nonce": "n-1", "sequence": 1}
        payload = encode_linux_rendezvous_frame(fields, token)
        self.assertEqual(session.accept(
            payload, peer_pid=991, peer_uid=1000,
            peer_cgroup="/cg/run/coordinator"), fields)
        with self.assertRaisesRegex(AuditInfrastructureError, "replay"):
            session.accept(payload, peer_pid=991, peer_uid=1000,
                           peer_cgroup="/cg/run/coordinator")
        with self.assertRaisesRegex(AuditInfrastructureError, "credential"):
            LinuxRendezvousSession(
                token=token, run_id="run-1", coordinator_pid=991,
                coordinator_uid=1000, coordinator_leaf="/cg/run/coordinator").accept(
                    payload, peer_pid=992, peer_uid=1001,
                    peer_cgroup="/cg/run/coordinator")
        bad = dict(fields, sequence=-1)
        with self.assertRaisesRegex(AuditInfrastructureError, "fields"):
            encode_linux_rendezvous_frame(bad, token)

    def test_reply_is_exact_authenticated_and_request_bound(self) -> None:
        token = bytes(range(32))
        fields = {
            "kind": "leaf-created", "run_id": "run-1", "worker_index": 2,
            "generation": 3, "worker_pid": 0, "nonce": "n-1", "sequence": 4,
            "leaf_path": "/cg/run/worker-2-g3", "leaf_token": "a" * 64,
        }
        payload = encode_linux_rendezvous_reply(fields, token)
        self.assertEqual(decode_linux_rendezvous_reply(payload, token), fields)
        with self.assertRaises(AuditInfrastructureError):
            decode_linux_rendezvous_reply(payload, b"x" * 32)
        malformed = json.dumps(
            {"body": 1, "hmac": "0" * 64,
             "schema": "olr-gpu-linux-rendezvous-v1"},
            sort_keys=True, separators=(",", ":")).encode("ascii")
        with self.assertRaises(AuditInfrastructureError):
            decode_linux_rendezvous_frame(malformed, token)
        with self.assertRaises(AuditInfrastructureError):
            decode_linux_rendezvous_reply(malformed, token)

    def test_coordinator_descendant_ancestry_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            proc = Path(temporary)
            for pid, parent in ((300, 200), (200, 100), (100, 1)):
                directory = proc / str(pid)
                directory.mkdir()
                (directory / "stat").write_text(
                    f"{pid} (process {pid}) S {parent} 0 0 0\n",
                    encoding="ascii")
            self.assertTrue(_linux_pid_descends_from(300, 100, proc))
            self.assertFalse(_linux_pid_descends_from(300, 99, proc))

    def test_accepted_seqpacket_receive_is_bounded_by_the_same_deadline(self) -> None:
        class Connection:
            timeout = None

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return None

            def settimeout(self, timeout):
                self.timeout = timeout

            @staticmethod
            def getsockopt(*_args):
                return struct.pack("3i", os.getpid(), 1, 1)

            @staticmethod
            def recv(_maximum):
                raise socket.timeout("stalled peer")

        class Listener:
            def __init__(self, connection):
                self.connection = connection

            @staticmethod
            def settimeout(_timeout):
                return None

            def accept(self):
                return self.connection, None

        connection = Connection()
        server = LinuxSeqpacketRendezvousServer.__new__(
            LinuxSeqpacketRendezvousServer)
        server._socket = Listener(connection)
        server._session = object()
        with mock.patch.object(socket, "SO_PEERCRED", 17, create=True):
            with self.assertRaisesRegex(AuditInfrastructureError,
                                        "receive failed"):
                server.receive_once(time.monotonic() + 1)
        self.assertIsNotNone(connection.timeout)
        self.assertGreater(connection.timeout, 0)

    @unittest.skipUnless(sys.platform.startswith("linux"),
                         "requires Linux SOCK_SEQPACKET/SO_PEERCRED")
    def test_native_client_derives_hidden_run_id_and_completes_leaf_protocol(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            supervisor = LinuxDelegatedCgroupSupervisor(root)
            supervisor.run.mkdir()
            supervisor.coordinator.mkdir()
            supervisor._prepared = True
            token = "a" * 64
            leaf = supervisor.run / "worker-0-g0"
            observed: list[str] = []

            def create(index, generation, nonce):
                leaf.mkdir()
                return leaf, token

            def verify(index, generation, actual, worker_pid):
                self.assertEqual(actual, token)
                self.assertEqual(worker_pid, os.getpid())
                observed.append("ack")

            def release(index, generation):
                leaf.rmdir()
                observed.append("release")

            supervisor.create_worker_leaf = create
            supervisor.verify_worker_membership = verify
            supervisor.release_worker_leaf = release
            server = LinuxSeqpacketRendezvousServer(
                supervisor, os.getpid(), os.getuid())
            errors: list[BaseException] = []

            def serve():
                try:
                    for _ in range(4):
                        server.receive_once(time.monotonic() + 5)
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=serve)
            with mock.patch(
                    "gpu_capability_process_tree._linux_pid_cgroup_path",
                    return_value=supervisor.coordinator.resolve()):
                thread.start()
                client = LinuxRendezvousClient(server.environment)
                client.hello(time.monotonic() + 5)
                carrier = client.create_leaf(0, 0, time.monotonic() + 5)
                (leaf / "cgroup.procs").write_text(str(os.getpid()), encoding="ascii")
                client.acknowledge_leaf(
                    0, 0, os.getpid(), carrier, time.monotonic() + 5)
                (leaf / "cgroup.procs").unlink()
                client.release_leaf(0, 0, time.monotonic() + 5)
                thread.join(timeout=5)
            server.close()
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(observed, ["ack", "release"])

    @unittest.skipUnless(
        sys.platform.startswith("linux")
        and shutil.which("sudo") is not None
        and shutil.which("systemd-run") is not None
        and Path("/run/systemd/system").exists(),
        "requires a systemd delegated Linux service host",
    )
    def test_systemd_delegated_service_runs_twice_without_stale_collision(self) -> None:
        repository = Path(__file__).resolve().parents[2]
        unit = f"olr-gpu-audit-test-{os.getpid()}"
        coordinator = """
import os, subprocess, sys, time
sys.path.insert(0, 'tests/gpu')
from gpu_capability_process_tree import (LinuxRendezvousClient,
    acknowledge_linux_worker_containment)
client = LinuxRendezvousClient(os.environ)
deadline = time.monotonic() + 20
client.hello(deadline)
carrier = client.create_leaf(0, 0, deadline)
worker = subprocess.Popen((sys.executable, '-c', 'import time; time.sleep(20)'))
try:
    acknowledge_linux_worker_containment(carrier, worker.pid)
    client.acknowledge_leaf(0, 0, worker.pid, carrier, deadline)
finally:
    worker.terminate()
    worker.wait(timeout=10)
while open(carrier.cgroup_leaf_path + '/cgroup.procs', encoding='ascii').read().strip():
    if time.monotonic() >= deadline:
        raise RuntimeError('worker cgroup did not empty')
    time.sleep(.01)
client.release_leaf(0, 0, deadline)
"""
        command = (*linux_systemd_run_prefix(
            unit=unit, uid=os.getuid(), gid=os.getgid(),
            working_directory=repository), "--", sys.executable, "-c",
                   coordinator)
        for invocation in range(2):
            completed = subprocess.run(
                command, cwd=repository, capture_output=True, text=True,
                timeout=60)
            self.assertEqual(
                completed.returncode, 0,
                f"invocation {invocation}: {completed.stdout}\n{completed.stderr}")


class MacOSRegisteredAccountingTests(unittest.TestCase):
    def test_successful_group_reconciliation_is_idempotently_receipted(self):
        leader = OwnedProcessIdentity("macos", 701, "1:2")
        accountant = MacOSRegisteredPgidAccountant()
        accountant.register_group(701, leader, "worker:1:2")
        accountant.register_group(701, leader, "worker:1:2")

        provider = object.__new__(MacOSLibprocProvider)
        provider.reconcile_survivors = mock.Mock(return_value=())
        self.assertTrue(accountant.reconcile_group(701, provider))
        self.assertTrue(accountant.reconcile_group(701, provider))
        self.assertEqual(provider.reconcile_survivors.call_count, 1)
        with self.assertRaisesRegex(
            AuditInfrastructureError, "intent differs"
        ):
            accountant.register_group(701, leader, "worker:1:3")

    def test_registration_lost_reply_is_completed_from_exact_native_effect(self):
        class LostReplyMembers(dict):
            faulted = False

            def __setitem__(_self, key, value):
                super().__setitem__(key, value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("registration reply lost")

        leader = OwnedProcessIdentity("macos", 711, "3:4")
        accountant = MacOSRegisteredPgidAccountant()
        accountant._members = LostReplyMembers()
        with self.assertRaisesRegex(RuntimeError, "registration reply lost"):
            accountant.register_group(711, leader, "worker:3:4")
        accountant.register_group(711, leader, "worker:3:4")
        self.assertEqual(accountant._leaders[711], leader)
        self.assertEqual(accountant._members[711], (leader,))
        with self.assertRaisesRegex(AuditInfrastructureError, "intent differs"):
            accountant.register_group(711, leader, "worker:3:5")

    def test_exec_permit_lost_replies_are_exactly_idempotent(self):
        class LostReplyMembers(dict):
            faulted = False

            def __setitem__(_self, key, value):
                super().__setitem__(key, value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("registration reply lost")

        class LostReplyIssued(set):
            faulted = False

            def add(_self, value):
                super().add(value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("permit reply lost")

        identity = FileIdentity(Path("/toolchain/clang"), None, 1, 2, 0, False)
        cases = (
            (
                "compiler",
                CompilerPgidReported(
                    0, 1, 2, CompilerLaunchPurpose.AUDIT_ACCEPTED,
                    721, 721, "5:6", identity, "a" * 64, "b" * 64,
                ),
                lambda authority, report: authority.permit_compiler(report),
            ),
            (
                "inspection",
                MacOSInspectionPgidReported(
                    "inspection-721", 721, 721, "5:6", identity, "a" * 64,
                ),
                lambda authority, report: authority.permit_inspection(report),
            ),
        )
        for kind, report, issue in cases:
            for boundary in ("registration", "permit"):
                with self.subTest(kind=kind, boundary=boundary):
                    accountant = MacOSRegisteredPgidAccountant()
                    if boundary == "registration":
                        accountant._members = LostReplyMembers()
                    authority = MacOSExecPermitAuthority(
                        accountant,
                        ("b" * 64,),
                        identity_verifier=lambda pid, pgid: OwnedProcessIdentity(
                            "macos", pid, "5:6"
                        ),
                        executable_verifier=lambda _identity, _digest: "b" * 64,
                    )
                    if boundary == "permit":
                        authority._issued = LostReplyIssued()
                    with self.assertRaisesRegex(RuntimeError, "reply lost"):
                        issue(authority, report)
                    permit = issue(authority, report)
                    self.assertEqual(permit.pgid, 721)
                    self.assertEqual(accountant._leaders[721].pid, 721)

    def test_inspection_adoption_lost_reply_reuses_exact_permit(self):
        from gpu_capability_process_tree import MacOSCompilerAuditAttemptAccountant

        class LostReplyPgids(set):
            faulted = False

            def add(_self, value):
                super().add(value)
                if not _self.faulted:
                    _self.faulted = True
                    raise RuntimeError("inspection adoption reply lost")

        identity = FileIdentity(Path("/toolchain/clang"), None, 1, 2, 0, False)
        capability = mock.Mock(
            executable_identity=identity,
            executable_sha256="a" * 64,
            capability_digest="b" * 64,
        )
        accountant = object.__new__(MacOSCompilerAuditAttemptAccountant)
        MacOSRegisteredPgidAccountant.__init__(accountant)
        authority = MacOSExecPermitAuthority(
            accountant,
            ("b" * 64,),
            identity_verifier=lambda pid, _pgid: OwnedProcessIdentity(
                "macos", pid, "7:8"
            ),
            executable_verifier=lambda _identity, _digest: "b" * 64,
        )
        provider = object.__new__(MacOSLibprocProvider)
        provider.observe = mock.Mock()
        provider.reconcile_survivors = lambda _accountant, _pgid: ()
        accountant._provider = provider
        accountant._pending_inspection = (
            "inspection-731", capability, authority, time.monotonic() + 10.0
        )
        accountant._inspection_pgids = LostReplyPgids()
        process_start = mock.Mock(
            platform_kind="macos", pid=731, native_start_token="7:8"
        )
        with self.assertRaisesRegex(RuntimeError, "adoption reply lost"):
            accountant.authorize_macos_compiler_exec(process_start)
        permit = accountant.authorize_macos_compiler_exec(process_start)
        self.assertEqual(permit.pgid, 731)
        self.assertEqual(accountant._inspection_pgids, {731})
        self.assertTrue(accountant.reconcile_group(731, provider))
        accountant._inspection_pgids.remove(731)
        self.assertTrue(accountant.memory_measurements().accounting_complete)

    @staticmethod
    def reconciliation_provider(
            survivors: tuple[OwnedProcessIdentity, ...]) -> MacOSLibprocProvider:
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)
        provider.reconcile_survivors = lambda _accountant, _pgid: survivors
        return provider

    def test_audit_and_inspection_handshakes_have_disjoint_exact_codecs(self) -> None:
        identity = FileIdentity(
            Path("/toolchain/clang"), None, 1, 2, 0, False)
        audit = CompilerPgidReported(
            0, 1, 2, CompilerLaunchPurpose.AUDIT_DISCOVERY, 501, 501,
            "10:20", identity, "a" * 64, "b" * 64)
        inspection = MacOSInspectionPgidReported(
            "inspection-1", 502, 502, "11:21", identity, "a" * 64)
        self.assertEqual(decode_macos_pgid_message(
            encode_macos_pgid_message(audit)), audit)
        self.assertEqual(decode_macos_pgid_message(
            encode_macos_pgid_message(inspection)), inspection)
        permit = CompilerExecPermit(0, 1, 2, 501)
        self.assertEqual(decode_macos_pgid_message(
            encode_macos_pgid_message(permit)), permit)
        mutated = encode_macos_pgid_message(inspection).replace(
            b'"inspection_id":"inspection-1"',
            b'"worker_index":0')
        with self.assertRaises(AuditInfrastructureError):
            decode_macos_pgid_message(mutated)

    def test_exec_permit_is_issued_only_after_identity_and_driver_registration(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        expected = OwnedProcessIdentity("macos", 601, "20:30")
        authority = MacOSExecPermitAuthority(
            accountant, ("b" * 64,),
            identity_verifier=lambda pid, pgid: expected,
            executable_verifier=lambda identity, digest: "b" * 64)
        identity = FileIdentity(Path("/toolchain/clang"), None, 1, 2, 0, False)
        report = CompilerPgidReported(
            0, 0, 1, CompilerLaunchPurpose.AUDIT_ACCEPTED,
            601, 601, "20:30", identity, "a" * 64, "b" * 64)
        self.assertEqual(authority.permit_compiler(report),
                         CompilerExecPermit(0, 0, 1, 601))
        self.assertEqual(authority.permit_compiler(report),
                         CompilerExecPermit(0, 0, 1, 601))
        untrusted = dataclasses.replace(report, task_id=2,
                                        driver_fingerprint="c" * 64)
        with self.assertRaisesRegex(AuditInfrastructureError, "untrusted"):
            authority.permit_compiler(untrusted)
        forged_hash = dataclasses.replace(report, task_id=3,
                                          executable_sha256="d" * 64)
        rejecting = MacOSExecPermitAuthority(
            MacOSRegisteredPgidAccountant(), ("b" * 64,),
            identity_verifier=lambda pid, pgid: expected,
            executable_verifier=lambda identity, digest: (
                "b" * 64 if digest == "a" * 64 else "c" * 64))
        with self.assertRaisesRegex(AuditInfrastructureError, "untrusted"):
            rejecting.permit_compiler(forged_hash)

    def test_registered_pgid_accounting_makes_only_observed_scope_claim(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        identity = OwnedProcessIdentity("macos", 201, "bsd-start-1")
        accountant.register_group(201, identity, "worker")
        accountant.observe_snapshot(1024, {201: {identity: 4096}})
        accountant.reconcile_group(201, self.reconciliation_provider(()))
        memory = accountant.memory_measurements()
        self.assertEqual(memory.maximum_observed_owned_group_resident_bytes, 4096)
        self.assertEqual(memory.maximum_observed_aggregate_resident_bytes, 5120)
        self.assertEqual(memory.surviving_registered_process_count, 0)
        self.assertEqual(memory.known_unreconciled_descendant_count, 0)
        self.assertTrue(memory.accounting_complete)

    def test_reconciliation_survivors_are_current_and_retryable(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 231, "start-leader")
        first_child = OwnedProcessIdentity("macos", 232, "start-child-1")
        second_child = OwnedProcessIdentity("macos", 233, "start-child-2")
        accountant.register_group(231, leader, "worker")
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)
        provider.reconcile_survivors = mock.Mock(
            side_effect=(
                (first_child, second_child),
                (second_child,),
                (),
            )
        )

        with self.assertRaisesRegex(
            AuditInfrastructureError, "survivors"
        ):
            accountant.reconcile_group(231, provider)
        self.assertIn(231, accountant._leaders)
        self.assertIn(231, accountant._members)
        blocked = accountant.memory_measurements()
        self.assertEqual(blocked.known_unreconciled_descendant_count, 2)
        self.assertEqual(
            accountant._members[231], (first_child, second_child)
        )
        self.assertFalse(blocked.accounting_complete)

        with self.assertRaisesRegex(
            AuditInfrastructureError, "survivors"
        ):
            accountant.reconcile_group(231, provider)
        narrowed = accountant.memory_measurements()
        self.assertEqual(narrowed.known_unreconciled_descendant_count, 1)
        self.assertEqual(accountant._members[231], (second_child,))

        self.assertIs(accountant.reconcile_group(231, provider), True)
        self.assertNotIn(231, accountant._leaders)
        self.assertNotIn(231, accountant._members)
        recovered = accountant.memory_measurements()
        self.assertEqual(recovered.known_unreconciled_descendant_count, 0)
        self.assertEqual(recovered.surviving_registered_process_count, 0)
        self.assertTrue(recovered.accounting_complete)

    def test_start_identity_change_fails_closed(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        identity = OwnedProcessIdentity("macos", 202, "bsd-start-1")
        accountant.register_group(202, identity, "compiler")
        with self.assertRaisesRegex(AuditInfrastructureError, "start identity"):
            accountant.observe_group(
                202, {OwnedProcessIdentity("macos", 202, "bsd-start-2"): 1})

    def test_empty_observation_cannot_clear_a_registered_live_group(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 211, "bsd-start-1")
        accountant.register_group(211, leader, "compiler")
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "registered leader"):
            accountant.observe_group(211, {})
        memory = accountant.memory_measurements()
        self.assertEqual(memory.surviving_registered_process_count, 1)
        self.assertFalse(memory.accounting_complete)

    def test_simultaneous_groups_are_summed_without_mixing_historical_maxima(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        first = OwnedProcessIdentity("macos", 301, "start-1")
        second = OwnedProcessIdentity("macos", 302, "start-2")
        accountant.register_group(301, first, "worker")
        accountant.register_group(302, second, "compiler")
        accountant.observe_snapshot(100, {301: {first: 400}, 302: {second: 500}})
        accountant.observe_snapshot(700, {301: {first: 10}, 302: {second: 20}})
        accountant.reconcile_group(301, self.reconciliation_provider(()))
        accountant.reconcile_group(302, self.reconciliation_provider(()))
        memory = accountant.memory_measurements()
        self.assertEqual(memory.maximum_observed_owned_group_resident_bytes, 900)
        self.assertEqual(memory.maximum_observed_parent_resident_bytes, 700)
        self.assertEqual(memory.maximum_observed_aggregate_resident_bytes, 1000)

    def test_reconciliation_rejects_caller_supplied_empty_survivor_claim(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 801, "start-leader")
        accountant.register_group(801, leader, "worker")
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "native provider"):
            accountant.reconcile_group(801, ())
        self.assertEqual(accountant.memory_measurements().surviving_registered_process_count,
                         1)

    def test_provider_fails_when_a_previously_observed_live_child_changes_pgid(self) -> None:
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 701, "start-leader")
        child = OwnedProcessIdentity("macos", 702, "start-child")
        accountant.register_group(701, leader, "worker")
        accountant.observe_snapshot(1, {701: {leader: 2, child: 3}})
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)

        def identity_and_residency(pid: int, expected_pgid: int):
            self.assertEqual(expected_pgid, 701)
            if pid == child.pid:
                raise AuditInfrastructureError(
                    "foreign process in macOS registered group")
            return leader, 2, 1

        provider._identity_and_residency = identity_and_residency
        provider._list_pids = lambda _pgid: (leader.pid,)
        with mock.patch("gpu_capability_process_tree.os.kill", return_value=None):
            with self.assertRaisesRegex(AuditInfrastructureError,
                                        "foreign process"):
                provider.observe(accountant, 1)

    def test_provider_requires_repeated_stable_pgid_enumeration(self) -> None:
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)
        observations = iter(((), (901,), (901,), (901,)))
        provider._list_pids = lambda _pgid: next(observations)
        self.assertEqual(provider._stable_list_pids(901), (901,))

    def test_reconciliation_rejects_stable_enumeration_missing_live_member(self):
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 911, "start-leader")
        accountant.register_group(911, leader, "worker")
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)
        provider._identity_and_residency = (
            lambda _pid, _pgid: (leader, 1, 1))
        provider._list_pids = lambda _pgid: ()
        with mock.patch("gpu_capability_process_tree.os.kill", return_value=None):
            with self.assertRaisesRegex(AuditInfrastructureError,
                                        "omitted a live member"):
                accountant.reconcile_group(911, provider)

    def test_observation_rejects_stable_enumeration_missing_live_prior_member(self):
        accountant = MacOSRegisteredPgidAccountant()
        leader = OwnedProcessIdentity("macos", 921, "start-leader")
        child = OwnedProcessIdentity("macos", 922, "start-child")
        accountant.register_group(921, leader, "worker")
        accountant.observe_snapshot(1, {921: {leader: 2, child: 3}})
        provider = MacOSLibprocProvider.__new__(MacOSLibprocProvider)
        provider._identity_and_residency = lambda pid, _pgid: (
            leader if pid == leader.pid else child, 1, 1)
        provider._stable_list_pids = lambda _pgid: (leader.pid,)
        with mock.patch("gpu_capability_process_tree.os.kill", return_value=None):
            with self.assertRaisesRegex(AuditInfrastructureError,
                                        "omitted a live member"):
                provider.observe(accountant, 1)
        self.assertEqual(
            tuple(identity.pid for identity in accountant._members[921]),
            (leader.pid, child.pid))


if __name__ == "__main__":
    unittest.main()
