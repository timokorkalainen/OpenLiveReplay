from __future__ import annotations

import dataclasses
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_calibration import (  # noqa: E402
    CalibrationAttemptFailure,
    MACOS_RSS_RECYCLE_DISABLED,
    WORKER_DECISION_SCHEMA,
    CALIBRATION_INCLUDED_STAGES,
    _calibrate_platform_worker_count_with_runner,
    _native_measured_sample_passes,
    _linux_cpu_quota_capacity_from_paths,
    allowed_worker_counts,
    derive_pilot_runtime_parameters,
    load_platform_worker_decision,
    prevalidate_platform_worker_decision,
    finalize_platform_worker_decision,
    load_worker_decision,
    reference_calibration_envelope,
    platform_worker_decision_key,
    platform_tag,
    worker_decision_key,
    write_worker_decision_atomic,
    write_platform_worker_decision_atomic,
)
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    MacOSPostReturnSample,
    LinuxCalibrationEnvelope,
    LinuxPhaseSnapshot,
    LinuxPostReturnSample,
    LinuxRunMemoryMeasurements,
    LinuxWorkerCalibrationSample,
    LinuxWorkerCountDecision,
    MacOSCalibrationEnvelope,
    MacOSPhaseSnapshot,
    MacOSRunMemoryMeasurements,
    MacOSWorkerCalibrationSample,
    MacOSWorkerCountDecision,
    WorkerCalibrationSample,
    WorkerCountDecision,
    WorkerStageTimings,
    WindowsPhaseSnapshot,
    WindowsPostReturnSample,
    WindowsRunMemoryMeasurements,
)


def _windows_memory(peak: int = 50) -> WindowsRunMemoryMeasurements:
    return WindowsRunMemoryMeasurements(
        parent_peak_rss_bytes=10, worker_peak_rss_bytes=20,
        maximum_simultaneous_working_set_bytes=50,
        aggregate_peak_rss_upper_bound_bytes=peak,
        job_peak_commit_charge_bytes=200, job_total_process_count=4,
        retained_process_identity_count=4, inspection_peak_tree_bytes=8,
        retained_inspection_process_identity_count=1,
        surviving_job_process_count=0, accounting_complete=True)


class PilotFormulaTests(unittest.TestCase):
    def test_linux_capacity_uses_tightest_inherited_cgroup_quota(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            leaf = root / "parent" / "leaf"
            leaf.mkdir(parents=True)
            membership = root / "self.cgroup"
            membership.write_text("0::/parent/leaf\n", encoding="ascii")
            (leaf / "cpu.max").write_text("max 100000\n", encoding="ascii")
            (leaf.parent / "cpu.max").write_text(
                "200000 100000\n", encoding="ascii")
            (root / "cpu.max").write_text("100000 100000\n", encoding="ascii")
            self.assertEqual(_linux_cpu_quota_capacity_from_paths(
                membership, root), 1)

    def test_allowed_worker_order_is_frozen(self) -> None:
        self.assertEqual(allowed_worker_counts(1), (1,))
        self.assertEqual(allowed_worker_counts(4), (2, 1, 3, 4))

    def test_windows_prefix_and_headroom_formula_are_exact(self) -> None:
        workers = 4
        ceiling = 448 << 20
        samples = tuple(
            WindowsPostReturnSample(
                platform_kind="windows", worker_slot=slot, worker_generation=0,
                task_ordinal_in_generation=ordinal, worker_current_rss_bytes=80 << 20,
                aggregate_peak_rss_upper_bound_bytes=ceiling - 1 - workers,
                accounting_complete=True)
            for slot, count in enumerate((63, 62, 63, 63))
            for ordinal in range(1, count + 1))
        result = derive_pilot_runtime_parameters(samples, workers, 251)
        self.assertEqual(result.maximum_tasks_per_worker, 62)
        self.assertEqual(result.recycle_rss_bytes, (80 << 20) + 1)
        self.assertTrue(result.pilot_eligible)

    def test_nonconsecutive_sample_uses_safe_prefix_and_boundary_is_diagnostic(self) -> None:
        ceiling = 448 << 20
        nonconsecutive = (
            WindowsPostReturnSample("windows", 0, 0, 1, 1, 1, True),
            WindowsPostReturnSample("windows", 0, 0, 3, 1, 1, True),
        )
        nonconsecutive_result = derive_pilot_runtime_parameters(nonconsecutive, 1, 2)
        self.assertTrue(nonconsecutive_result.pilot_eligible)
        self.assertEqual(nonconsecutive_result.maximum_tasks_per_worker, 1)
        boundary = (WindowsPostReturnSample(
            "windows", 0, 0, 1, 1, ceiling, True),)
        self.assertFalse(
            derive_pilot_runtime_parameters(boundary, 1, 1).pilot_eligible)

    def test_safe_prefix_remains_eligible_when_later_sample_crosses_gate(self) -> None:
        ceiling = 448 << 20
        samples = (
            WindowsPostReturnSample("windows", 0, 0, 1, 11, 100, True),
            WindowsPostReturnSample("windows", 0, 0, 2, 12, ceiling, True),
        )
        result = derive_pilot_runtime_parameters(samples, 1, 2)
        self.assertTrue(result.pilot_eligible)
        self.assertEqual(result.maximum_tasks_per_worker, 1)

    def test_macos_uses_task_count_and_disabled_rss_sentinel(self) -> None:
        samples = tuple(MacOSPostReturnSample(
            "macos", 0, 0, ordinal, 99, 100, 0, 0, True)
            for ordinal in (1, 2, 3))
        result = derive_pilot_runtime_parameters(samples, 1, 3)
        self.assertEqual(result.maximum_tasks_per_worker, 3)
        self.assertEqual(result.recycle_rss_bytes, MACOS_RSS_RECYCLE_DISABLED)
        self.assertTrue(result.pilot_eligible)


class WorkerDecisionCodecTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "decision.json"
        self.key = worker_decision_key(
            platform_tag(), "a" * 64, "b" * 64, "c" * 64,
            "d" * 64, 1)
        timings = WorkerStageTimings(1.0, 1.0, 1.0, 1.0)
        phase = WindowsPhaseSnapshot("windows", "inspection", _windows_memory(), 0)
        task_phase = WindowsPhaseSnapshot("windows", "tasks", _windows_memory(), 1)
        sample = WorkerCalibrationSample(
            platform_kind="windows", worker_count=1, configuration_count=251,
            elapsed_seconds=100.0, memory=_windows_memory(),
            inspection_probe_invocations=3, audit_compiler_invocations=502,
            stdout_bytes=1, cache_bytes=2, cache_entries=251,
            maximum_tasks_per_worker=251, recycle_rss_bytes=1024,
            pilot_eligible=True, surviving_owned_process_count=0,
            included_stages=("enumerate-production", "collect-configurations",
                             "snapshot-production", "load-cache", "compile",
                             "compact-audit-publish", "aggregate-authoritative",
                             "validate-policy-boundary", "raw-audit", "source-only",
                             "shutdown-reap", "summary"),
            post_return_samples=(WindowsPostReturnSample(
                "windows", 0, 0, 1, 1, 1, True),),
            stage_p50_seconds=timings, stage_p95_seconds=timings,
            inspection_phase_snapshot=phase, task_phase_snapshot=task_phase)
        self.decision = WorkerCountDecision(
            platform_kind="windows", artifact_schema=WORKER_DECISION_SCHEMA,
            key=self.key, selected_workers=1, maximum_tasks_per_worker=251,
            recycle_rss_bytes=1024, attempted_worker_counts=(1,), rejections=(),
            samples=(sample,), evidence_record_sha256="e" * 64)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_linux_and_macos_artifacts_are_strictly_discriminated(self) -> None:
        active_platform = ["linux"]
        platform_tag_patch = mock.patch(
            "gpu_capability_calibration.platform_tag",
            side_effect=lambda: (
                f"{active_platform[0]}-x86_64-ptr64-py3.11.9"))
        platform_tag_patch.start()
        self.addCleanup(platform_tag_patch.stop)
        timings = WorkerStageTimings(1, 1, 1, 1)
        linux_memory = LinuxRunMemoryMeasurements(
            1, 2, 512 << 20, 448 << 20, 0, 0, 0, 0, 0, 0, 0, True)
        linux_envelope = LinuxCalibrationEnvelope(
            "ubuntu", "6.8", "3.11.9", "18", "255", True, 1)
        linux_key = platform_worker_decision_key(
            "linux", "a" * 64, "b" * 64, "c" * 64, "d" * 64, 1,
            linux_envelope)
        linux_sample = LinuxWorkerCalibrationSample(
            "linux", 1, 251, 100, 251, 1000, linux_memory, 1, 502,
            1, 1, 251, True, 0, CALIBRATION_INCLUDED_STAGES, (), timings,
            timings, LinuxPhaseSnapshot("linux", "inspection", linux_memory, 0),
            LinuxPhaseSnapshot("linux", "tasks", linux_memory, 1))
        linux = LinuxWorkerCountDecision(
            "linux", WORKER_DECISION_SCHEMA, linux_key, linux_envelope, 1, 251,
            1000, (1,), (), (linux_sample,), "e" * 64)
        linux_path = self.path.with_name("linux.json")
        write_platform_worker_decision_atomic(linux_path, linux)

        active_platform[0] = "macos"
        mac_memory = MacOSRunMemoryMeasurements(3, 1, 2, 0, 0, True)
        mac_envelope = MacOSCalibrationEnvelope(
            "macos", "15", "3.11.9", "16", 1)
        mac_key = platform_worker_decision_key(
            "macos", "a" * 64, "b" * 64, "c" * 64, "d" * 64, 1,
            mac_envelope)
        mac_sample = MacOSWorkerCalibrationSample(
            "macos", 1, 251, 100, 251, MACOS_RSS_RECYCLE_DISABLED,
            mac_memory, 1, 502, 1, 1, 251, True, 0,
            CALIBRATION_INCLUDED_STAGES, (), timings, timings,
            MacOSPhaseSnapshot("macos", "inspection", mac_memory, 0),
            MacOSPhaseSnapshot("macos", "tasks", mac_memory, 1))
        mac = MacOSWorkerCountDecision(
            "macos", WORKER_DECISION_SCHEMA, mac_key, mac_envelope, 1, 251,
            MACOS_RSS_RECYCLE_DISABLED, (1,), (), (mac_sample,), "e" * 64)
        mac_path = self.path.with_name("macos.json")
        write_platform_worker_decision_atomic(mac_path, mac)
        with mock.patch("gpu_capability_calibration.effective_worker_capacity",
                        return_value=1), mock.patch(
                            "gpu_capability_calibration.linux_calibration_envelope",
                            return_value=linux_envelope), mock.patch(
                            "gpu_capability_calibration.macos_calibration_envelope",
                            return_value=mac_envelope):
            active_platform[0] = "linux"
            self.assertEqual(load_platform_worker_decision(
                linux_path, "linux", linux_key), linux)
            import gpu_capability_calibration as calibration_module
            with mock.patch(
                    "gpu_capability_calibration._read_bounded_decision_payload",
                    wraps=calibration_module._read_bounded_decision_payload) as reads:
                prevalidated = prevalidate_platform_worker_decision(
                    linux_path, "linux", linux_key.decision_engine_fingerprint,
                    linux_key.uninspected_configuration_digest,
                    time.monotonic() + 10)
                self.assertEqual(finalize_platform_worker_decision(
                    prevalidated, linux_key, time.monotonic() + 10), linux)
            self.assertEqual(reads.call_count, 2)
            active_platform[0] = "macos"
            self.assertEqual(load_platform_worker_decision(
                mac_path, "macos", mac_key), mac)
            with self.assertRaises(AuditInfrastructureError):
                load_platform_worker_decision(linux_path, "macos", mac_key)

    def test_linux_sample_rejects_current_memory_at_or_above_memory_max(self) -> None:
        timings = WorkerStageTimings(1, 1, 1, 1)
        memory = LinuxRunMemoryMeasurements(
            600, 100, 512, 700, 0, 0, 0, 0, 0, 0, 0, True)
        sample = LinuxWorkerCalibrationSample(
            "linux", 1, 251, 100, 251, 1000, memory, 1, 502,
            1, 1, 251, True, 0, CALIBRATION_INCLUDED_STAGES, (), timings,
            timings, LinuxPhaseSnapshot("linux", "inspection", memory, 0),
            LinuxPhaseSnapshot("linux", "tasks", memory, 1))
        self.assertFalse(_native_measured_sample_passes(sample, "linux"))

    def test_native_sample_uses_actual_database_cardinality(self) -> None:
        timings = WorkerStageTimings(1, 1, 1, 1)
        memory = LinuxRunMemoryMeasurements(
            100, 200, 1000, 900, 0, 0, 0, 0, 0, 0, 0, True)
        sample = LinuxWorkerCalibrationSample(
            "linux", 1, 7, 100, 7, 1000, memory, 1, 14,
            1, 1, 7, True, 0, CALIBRATION_INCLUDED_STAGES, (), timings,
            timings, LinuxPhaseSnapshot("linux", "inspection", memory, 0),
            LinuxPhaseSnapshot("linux", "tasks", memory, 1))
        self.assertTrue(_native_measured_sample_passes(sample, "linux"))
        self.assertFalse(_native_measured_sample_passes(
            dataclasses.replace(sample, audit_compiler_invocations=502), "linux"))

    def test_atomic_round_trip_and_reference_envelope(self) -> None:
        write_worker_decision_atomic(self.path, self.decision)
        with mock.patch("gpu_capability_calibration.effective_worker_capacity",
                        return_value=1):
            self.assertEqual(load_worker_decision(self.path, self.key), self.decision)
        self.assertEqual(reference_calibration_envelope().affinity_cpu_indices,
                         (0, 1, 2, 3))

    def test_unknown_key_and_noncanonical_bytes_fail_closed(self) -> None:
        write_worker_decision_atomic(self.path, self.decision)
        artifact = json.loads(self.path.read_text(encoding="utf-8"))
        artifact["unexpected"] = 1
        self.path.write_text(json.dumps(artifact), encoding="utf-8")
        with self.assertRaisesRegex(AuditInfrastructureError, "worker decision"):
            load_worker_decision(self.path, self.key)

    def test_selected_runtime_parameters_must_match_measured_sample(self) -> None:
        invalid = dataclasses.replace(self.decision, recycle_rss_bytes=1025)
        with self.assertRaisesRegex(AuditInfrastructureError, "worker decision"):
            write_worker_decision_atomic(self.path, invalid)

    def test_windows_sample_aggregate_must_equal_recomputed_component_bound(self):
        sample = self.decision.samples[0]
        forged_memory = dataclasses.replace(
            sample.memory, worker_peak_rss_bytes=1000,
            aggregate_peak_rss_upper_bound_bytes=50)
        forged_sample = dataclasses.replace(sample, memory=forged_memory)
        forged = dataclasses.replace(self.decision, samples=(forged_sample,))
        with self.assertRaisesRegex(AuditInfrastructureError, "worker decision"):
            write_worker_decision_atomic(self.path, forged)
        sentinel_sample = dataclasses.replace(
            sample, recycle_rss_bytes=MACOS_RSS_RECYCLE_DISABLED)
        sentinel = dataclasses.replace(
            self.decision, recycle_rss_bytes=MACOS_RSS_RECYCLE_DISABLED,
            samples=(sentinel_sample,))
        with self.assertRaisesRegex(AuditInfrastructureError, "worker decision"):
            write_worker_decision_atomic(self.path, sentinel)

    def test_windows_load_rejects_foreign_platform_tag_even_with_matching_key(self):
        foreign_key = worker_decision_key(
            "foreign-amd64-ptr64-py3.11.9", self.key.compiler_digest,
            self.key.configuration_set_digest,
            self.key.uninspected_configuration_digest,
            self.key.decision_engine_fingerprint,
            self.key.effective_worker_capacity)
        foreign = dataclasses.replace(self.decision, key=foreign_key)
        write_worker_decision_atomic(self.path, foreign)
        with mock.patch("gpu_capability_calibration.effective_worker_capacity",
                        return_value=1), self.assertRaisesRegex(
                            AuditInfrastructureError, "platform"):
            load_worker_decision(self.path, foreign_key)

    def test_fake_runner_uses_pilot_then_fresh_measured_cache_for_each_count(self):
        key = worker_decision_key(
            "windows-amd64-py3.11.9", "a" * 64, "b" * 64, "c" * 64,
            "d" * 64, 4)
        base = self.decision.samples[0]

        class FakeRunner:
            def __init__(runner_self):
                runner_self.calls = []
                runner_self.caches = []

            def prepare(runner_self, *_args):
                return key, 251, "e" * 64

            def run(runner_self, *, kind, worker_count, runtime_parameters,
                    cache_root, included_stages):
                runner_self.calls.append((worker_count, kind))
                runner_self.caches.append(cache_root)
                self.assertEqual(included_stages, CALIBRATION_INCLUDED_STAGES)
                if kind == "pilot":
                    return tuple(WindowsPostReturnSample(
                        "windows", slot, 0, 1, 1,
                        (448 << 20) - 1 - worker_count, True)
                        for slot in range(worker_count))
                elapsed = {2: 80.0, 1: 120.0, 3: 70.0, 4: 70.0}[worker_count]
                return dataclasses.replace(
                    base, worker_count=worker_count, elapsed_seconds=elapsed,
                    maximum_tasks_per_worker=runtime_parameters.maximum_tasks_per_worker,
                    recycle_rss_bytes=runtime_parameters.recycle_rss_bytes,
                    pilot_eligible=runtime_parameters.pilot_eligible,
                    post_return_samples=tuple(WindowsPostReturnSample(
                        "windows", slot, 0, 1, 1, 1, True)
                        for slot in range(worker_count)))

        runner = FakeRunner()
        decision = _calibrate_platform_worker_count_with_runner(
            "windows", runner, Path(self.temporary.name),
            (Path(self.temporary.name) / "compile_commands.json",),
            Path(self.temporary.name), dataclasses.replace(AuditLimits(), workers=4))
        self.assertEqual(tuple(runner.calls), tuple(
            (count, kind) for count in (2, 1, 3, 4)
            for kind in ("pilot", "measured")))
        self.assertEqual(len(set(runner.caches)), 8)
        self.assertEqual(decision.selected_workers, 3)
        self.assertEqual(decision.attempted_worker_counts, (2, 1, 3, 4))

    def test_linux_and_macos_fake_runners_cover_stages_and_clean_rejection(self):
        timings = WorkerStageTimings(1, 1, 1, 1)
        envelopes = {
            "linux": LinuxCalibrationEnvelope(
                "ubuntu", "6.8", "3.11.9", "18", "255", True, 2),
            "macos": MacOSCalibrationEnvelope(
                "macos", "15", "3.11.9", "16", 2),
        }

        for kind in ("linux", "macos"):
            with self.subTest(platform=kind), mock.patch(
                    "gpu_capability_calibration.platform_tag",
                    return_value=f"{kind}-x86_64-ptr64-py3.11.9"):
                envelope = envelopes[kind]
                key = platform_worker_decision_key(
                    kind, "a" * 64, "b" * 64, "c" * 64, "d" * 64, 2,
                    envelope)

                class FakeRunner:
                    def __init__(runner_self):
                        runner_self.calls = []
                        runner_self.executed_stages = set()

                    def prepare(runner_self, *_args):
                        return key, 251, "e" * 64, envelope

                    def run(runner_self, *, kind: str, worker_count: int,
                            runtime_parameters, cache_root, included_stages):
                        runner_self.calls.append((worker_count, kind, cache_root))
                        runner_self.executed_stages.update(included_stages)
                        if kind == "pilot":
                            if envelope is envelopes["linux"]:
                                return tuple(LinuxPostReturnSample(
                                    "linux", slot, 0, 1, 10, 100, 200, 900,
                                    1000, True, True)
                                    for slot in range(worker_count))
                            return tuple(MacOSPostReturnSample(
                                "macos", slot, 0, 1, 10, 100, 0, 0, True)
                                for slot in range(worker_count))
                        if worker_count == 2:
                            raise CalibrationAttemptFailure(
                                "compile", "deadline", cleanup_complete=True,
                                surviving_owned_process_count=0)
                        if envelope is envelopes["linux"]:
                            memory = LinuxRunMemoryMeasurements(
                                100, 200, 1000, 900, 0, 0, 0, 0, 0, 0, 0,
                                True)
                            return LinuxWorkerCalibrationSample(
                                "linux", 1, 251, 100,
                                runtime_parameters.maximum_tasks_per_worker,
                                runtime_parameters.recycle_rss_bytes, memory, 1,
                                502, 1, 1, 251,
                                runtime_parameters.pilot_eligible, 0,
                                CALIBRATION_INCLUDED_STAGES, (), timings, timings,
                                LinuxPhaseSnapshot(
                                    "linux", "inspection", memory, 0),
                                LinuxPhaseSnapshot("linux", "tasks", memory, 1))
                        memory = MacOSRunMemoryMeasurements(300, 100, 200, 0, 0,
                                                           True)
                        return MacOSWorkerCalibrationSample(
                            "macos", 1, 251, 100,
                            runtime_parameters.maximum_tasks_per_worker,
                            runtime_parameters.recycle_rss_bytes, memory, 1, 502,
                            1, 1, 251, runtime_parameters.pilot_eligible, 0,
                            CALIBRATION_INCLUDED_STAGES, (), timings, timings,
                            MacOSPhaseSnapshot("macos", "inspection", memory, 0),
                            MacOSPhaseSnapshot("macos", "tasks", memory, 1))

                runner = FakeRunner()
                decision = _calibrate_platform_worker_count_with_runner(
                    kind, runner, Path(self.temporary.name),
                    (Path(self.temporary.name) / "compile_commands.json",),
                    Path(self.temporary.name),
                    dataclasses.replace(AuditLimits(), workers=2))
                self.assertEqual(decision.selected_workers, 1)
                self.assertEqual(tuple(item.worker_count
                                       for item in decision.rejections), (2,))
                self.assertEqual(runner.executed_stages,
                                 set(CALIBRATION_INCLUDED_STAGES))
                self.assertEqual(len({path for _count, _kind, path in runner.calls}),
                                 4)
                invalid_rejection = dataclasses.replace(
                    decision.rejections[0], failure_stage="unknown-stage")
                invalid_decision = dataclasses.replace(
                    decision, rejections=(invalid_rejection,))
                with self.assertRaises(AuditInfrastructureError):
                    write_platform_worker_decision_atomic(
                        self.path.with_name(f"{kind}-invalid-rejection.json"),
                        invalid_decision)
                if kind == "linux":
                    sentinel_sample = dataclasses.replace(
                        decision.samples[0],
                        recycle_rss_bytes=MACOS_RSS_RECYCLE_DISABLED)
                    sentinel_decision = dataclasses.replace(
                        decision, recycle_rss_bytes=MACOS_RSS_RECYCLE_DISABLED,
                        samples=(sentinel_sample,))
                    with self.assertRaises(AuditInfrastructureError):
                        write_platform_worker_decision_atomic(
                            self.path.with_name("linux-sentinel.json"),
                            sentinel_decision)


if __name__ == "__main__":
    unittest.main()
