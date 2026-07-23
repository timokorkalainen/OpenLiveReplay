"""Versioned worker calibration formulas and strict decision artifacts."""

from __future__ import annotations

import dataclasses
import hashlib
import hmac
import json
import math
import os
import platform
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Mapping

from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CalibrationRejection,
    CompilerAuditRun,
    DecisionConfigurationRecord,
    DependencyRootAuthority,
    LinuxPostReturnSample,
    LinuxCalibrationEnvelope,
    LinuxPhaseSnapshot,
    LinuxRunMemoryMeasurements,
    LinuxWorkerCalibrationSample,
    LinuxWorkerCountDecision,
    MacOSPostReturnSample,
    MacOSCalibrationEnvelope,
    MacOSPhaseSnapshot,
    MacOSRunMemoryMeasurements,
    MacOSWorkerCalibrationSample,
    MacOSWorkerCountDecision,
    PilotRuntimeParameters,
    PlatformWorkerDecision,
    PreprocessConfiguration,
    PrevalidatedWorkerDecision,
    ReferenceCalibrationEnvelope,
    WorkerCalibrationSample,
    WorkerCountDecision,
    WorkerDecisionKey,
    WorkerStageTimings,
    WorkerRuntimeContract,
    WindowsPhaseSnapshot,
    WindowsPostReturnSample,
    WindowsRunMemoryMeasurements,
)


WORKER_DECISION_SCHEMA = 1
WORKER_DECISION_SCHEMA_BYTES = b"olr-gpu-worker-decision-v1"
WORKER_SELECTION_SEMANTICS_BYTES = b"olr-gpu-worker-selection-v1"
_WORKER_SELECTION_ORDER = (2, 1)
WORKER_DECISION_MAX_BYTES = 16 * 1024 * 1024
REFERENCE_ENVELOPE_SCHEMA_BYTES = b"olr-gpu-reference-envelope-v1"
MACOS_RSS_RECYCLE_DISABLED = (1 << 64) - 1
WINDOWS_COLD_RSS_CEILING_BYTES = 448 << 20
CALIBRATION_SMOKE_SECONDS = 170.0
CALIBRATION_SMOKE_CLEANUP_RESERVE_SECONDS = 30.0

CALIBRATION_INCLUDED_STAGES = (
    "enumerate-production", "collect-configurations", "snapshot-production",
    "load-cache", "compile", "compact-audit-publish",
    "aggregate-authoritative", "validate-policy-boundary", "raw-audit",
    "source-only", "shutdown-reap", "summary",
)
CALIBRATION_FAILURE_STAGES = frozenset(CALIBRATION_INCLUDED_STAGES)
CALIBRATION_FAILURE_CODES = frozenset({
    "deadline", "native-memory-limit", "native-accounting-incomplete",
    "protocol", "pipeline-failure", "cleanup-failure",
})
CALIBRATION_SMOKE_PIPELINE_STAGES = (
    "enumerate-production", "collect", "snapshot", "cache", "schedule",
    "finalize", "decode", "coverage", "raw", "source-only", "shutdown-reap",
)


class CalibrationAttemptFailure(AuditInfrastructureError):
    def __init__(self, failure_stage: str, failure_code: str, *,
                 cleanup_complete: bool,
                 surviving_owned_process_count: int) -> None:
        _require_contract(failure_stage in CALIBRATION_FAILURE_STAGES
                          and failure_code in CALIBRATION_FAILURE_CODES
                          and isinstance(cleanup_complete, bool)
                          and isinstance(surviving_owned_process_count, int)
                          and not isinstance(surviving_owned_process_count, bool)
                          and surviving_owned_process_count >= 0,
                          "calibration rejection is invalid")
        super().__init__(f"calibration rejected at {failure_stage}: {failure_code}")
        self.failure_stage = failure_stage
        self.failure_code = failure_code
        self.cleanup_complete = cleanup_complete
        self.surviving_owned_process_count = surviving_owned_process_count


def _require_contract(condition: bool, message: str = "worker decision is invalid") -> None:
    if not condition:
        raise AuditInfrastructureError(message)


def _update_framed(digest: object, payload: bytes) -> None:
    _require_contract(isinstance(payload, bytes), "decision digest payload is invalid")
    digest.update(struct.pack(">Q", len(payload)))
    digest.update(payload)


def _canonical_json(value: object) -> bytes:
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise AuditInfrastructureError("worker decision is invalid") from error


def reference_calibration_envelope() -> ReferenceCalibrationEnvelope:
    return ReferenceCalibrationEnvelope(
        runner_image="windows-2022",
        python_version="3.11.9",
        qt_package="qt-6.10.2-win64_mingw",
        mingw_package="tools_mingw1310-13.1.0",
        worker_capacity=4,
        affinity_cpu_indices=(0, 1, 2, 3),
    )


def windows_calibration_envelope() -> ReferenceCalibrationEnvelope:
    return reference_calibration_envelope()


def _bounded_version_command(arguments: tuple[str, ...]) -> str:
    try:
        completed = subprocess.run(
            arguments, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
            errors="strict", timeout=10.0)
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        raise AuditInfrastructureError("calibration envelope probe failed") from error
    value = completed.stdout.strip()
    _require_contract(completed.returncode == 0 and 0 < len(value) <= 16 * 1024,
                      "calibration envelope probe failed")
    return value


def linux_calibration_envelope() -> LinuxCalibrationEnvelope:
    _require_contract(sys.platform.startswith("linux"),
                      "Linux calibration envelope requires Linux")
    runner_image = os.environ.get("ImageOS", "ubuntu-24.04")
    compiler = os.environ.get("OLR_GPU_CALIBRATION_CLANG")
    _require_contract(isinstance(compiler, str) and bool(compiler),
                      "pinned Linux calibration compiler is unavailable")
    controllers = Path("/sys/fs/cgroup/cgroup.controllers")
    _require_contract(controllers.is_file()
                      and "memory" in controllers.read_text(
                          encoding="ascii").split(),
                      "Linux calibration requires cgroup v2 memory")
    return LinuxCalibrationEnvelope(
        runner_image=runner_image, kernel_release=platform.release(),
        python_version=platform.python_version(),
        clang_version=_bounded_version_command((compiler, "--version")),
        systemd_version=_bounded_version_command(("systemd", "--version")),
        cgroup_v2=True,
        effective_worker_capacity=effective_worker_capacity(AuditLimits()))


def macos_calibration_envelope() -> MacOSCalibrationEnvelope:
    _require_contract(sys.platform == "darwin",
                      "macOS calibration envelope requires macOS")
    compiler = os.environ.get("OLR_GPU_CALIBRATION_APPLECLANG")
    _require_contract(isinstance(compiler, str) and bool(compiler),
                      "pinned macOS calibration compiler is unavailable")
    return MacOSCalibrationEnvelope(
        runner_image=os.environ.get("ImageOS", "macos-15"),
        macos_version=platform.mac_ver()[0],
        python_version=platform.python_version(),
        appleclang_version=_bounded_version_command((compiler, "--version")),
        effective_worker_capacity=effective_worker_capacity(AuditLimits()))


def _reference_envelope_digest(envelope: ReferenceCalibrationEnvelope) -> str:
    digest = hashlib.sha256()
    _update_framed(digest, REFERENCE_ENVELOPE_SCHEMA_BYTES)
    for value in (
        envelope.runner_image, envelope.python_version, envelope.qt_package,
        envelope.mingw_package, str(envelope.worker_capacity),
        ",".join(str(index) for index in envelope.affinity_cpu_indices),
    ):
        _update_framed(digest, value.encode("ascii"))
    return digest.hexdigest()


def worker_decision_key(platform_tag: str, compiler_digest: str,
                        configuration_set_digest: str,
                        uninspected_configuration_digest: str,
                        decision_engine_fingerprint: str,
                        effective_worker_capacity: int) -> WorkerDecisionKey:
    envelope_digest = _reference_envelope_digest(reference_calibration_envelope())
    values = (
        platform_tag, compiler_digest, configuration_set_digest,
        uninspected_configuration_digest, decision_engine_fingerprint,
        str(effective_worker_capacity), envelope_digest,
    )
    _require_contract(isinstance(platform_tag, str) and bool(platform_tag),
                      "worker decision platform tag is invalid")
    for value in values[1:5]:
        _require_contract(isinstance(value, str) and len(value) == 64
                          and all(character in "0123456789abcdef" for character in value),
                          "worker decision digest is invalid")
    _require_contract(isinstance(effective_worker_capacity, int)
                      and not isinstance(effective_worker_capacity, bool)
                      and effective_worker_capacity > 0,
                      "effective worker capacity is invalid")
    digest = hashlib.sha256()
    _update_framed(digest, WORKER_DECISION_SCHEMA_BYTES)
    for value in values:
        _update_framed(digest, value.encode("ascii"))
    return WorkerDecisionKey(
        platform_tag=platform_tag,
        compiler_digest=compiler_digest,
        configuration_set_digest=configuration_set_digest,
        uninspected_configuration_digest=uninspected_configuration_digest,
        decision_engine_fingerprint=decision_engine_fingerprint,
        effective_worker_capacity=effective_worker_capacity,
        reference_envelope_digest=envelope_digest,
        key_digest=digest.hexdigest(),
    )


def _platform_envelope_digest(
    envelope: ReferenceCalibrationEnvelope | LinuxCalibrationEnvelope
    | MacOSCalibrationEnvelope,
) -> str:
    _require_contract(isinstance(envelope, (
        ReferenceCalibrationEnvelope, LinuxCalibrationEnvelope,
        MacOSCalibrationEnvelope)), "platform calibration envelope is invalid")
    if isinstance(envelope, ReferenceCalibrationEnvelope):
        return _reference_envelope_digest(envelope)
    digest = hashlib.sha256()
    _update_framed(digest, REFERENCE_ENVELOPE_SCHEMA_BYTES)
    _update_framed(digest, type(envelope).__name__.encode("ascii"))
    _update_framed(digest, _canonical_json(dataclasses.asdict(envelope)))
    return digest.hexdigest()


def platform_worker_decision_key(
    platform_kind: str, compiler_digest: str, configuration_set_digest: str,
    uninspected_configuration_digest: str, decision_engine_fingerprint: str,
    effective_worker_capacity: int,
    envelope: ReferenceCalibrationEnvelope | LinuxCalibrationEnvelope
    | MacOSCalibrationEnvelope,
) -> WorkerDecisionKey:
    expected_type = {
        "windows": ReferenceCalibrationEnvelope,
        "linux": LinuxCalibrationEnvelope,
        "macos": MacOSCalibrationEnvelope,
    }.get(platform_kind)
    _require_contract(expected_type is not None and isinstance(envelope, expected_type),
                      "platform calibration envelope differs")
    native_platform_tag = platform_tag()
    _require_contract(native_platform_tag.startswith(f"{platform_kind}-"),
                      "platform calibration is not native")
    envelope_digest = _platform_envelope_digest(envelope)
    values = (native_platform_tag, compiler_digest, configuration_set_digest,
              uninspected_configuration_digest, decision_engine_fingerprint,
              str(effective_worker_capacity), envelope_digest)
    for value in values[1:5]:
        _digest(value)
    _require_contract(isinstance(effective_worker_capacity, int)
                      and not isinstance(effective_worker_capacity, bool)
                      and effective_worker_capacity > 0)
    digest = hashlib.sha256()
    _update_framed(digest, WORKER_DECISION_SCHEMA_BYTES)
    for value in values:
        _update_framed(digest, value.encode("ascii"))
    return WorkerDecisionKey(
        platform_tag=native_platform_tag, compiler_digest=compiler_digest,
        configuration_set_digest=configuration_set_digest,
        uninspected_configuration_digest=uninspected_configuration_digest,
        decision_engine_fingerprint=decision_engine_fingerprint,
        effective_worker_capacity=effective_worker_capacity,
        reference_envelope_digest=envelope_digest, key_digest=digest.hexdigest())


def canonical_platform_kind(sys_platform: str) -> str:
    mapping = {"win32": "windows", "linux": "linux", "darwin": "macos"}
    try:
        return mapping[sys_platform]
    except (KeyError, TypeError) as error:
        raise AuditInfrastructureError("unsupported calibration platform") from error


def platform_tag() -> str:
    kind = canonical_platform_kind(sys.platform)
    architecture = platform.machine().casefold() or "unknown"
    return (f"{kind}-{architecture}-ptr{struct.calcsize('P') * 8}-"
            f"py{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")


def _affinity_logical_capacity() -> int:
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        process_mask = ctypes.c_size_t()
        system_mask = ctypes.c_size_t()
        function = kernel32.GetProcessAffinityMask
        function.argtypes = (wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t),
                             ctypes.POINTER(ctypes.c_size_t))
        function.restype = wintypes.BOOL
        if not function(kernel32.GetCurrentProcess(), ctypes.byref(process_mask),
                        ctypes.byref(system_mask)):
            raise AuditInfrastructureError("effective worker capacity affinity query failed")
        count = int(process_mask.value).bit_count()
        _require_contract(count > 0, "effective worker capacity affinity is empty")
        return count
    get_affinity = getattr(os, "sched_getaffinity", None)
    if get_affinity is not None:
        try:
            count = len(get_affinity(0))
        except OSError as error:
            raise AuditInfrastructureError(
                "effective worker capacity affinity query failed") from error
        _require_contract(count > 0, "effective worker capacity affinity is empty")
        return count
    return max(1, os.cpu_count() or 1)


def _linux_cpu_quota_capacity_from_paths(
    membership_path: Path, mount_root: Path,
) -> int | None:
    _require_contract(isinstance(membership_path, Path)
                      and isinstance(mount_root, Path),
                      "effective worker capacity cgroup paths are invalid")
    try:
        relative = None
        for line in membership_path.read_text(encoding="ascii").splitlines():
            fields = line.split(":", 2)
            if len(fields) == 3 and fields[0] == "0":
                relative = fields[2].lstrip("/")
                break
        if relative is None:
            return None
        relative_path = Path(relative)
        _require_contract(not relative_path.is_absolute()
                          and ".." not in relative_path.parts,
                          "effective worker capacity cgroup membership is invalid")
        root = mount_root.resolve()
        current = (root / relative_path).resolve()
        _require_contract(current == root or root in current.parents,
                          "effective worker capacity cgroup membership is invalid")
        capacities: list[int] = []
        while True:
            payload = (current / "cpu.max").read_text(
                encoding="ascii").strip().split()
            _require_contract(len(payload) == 2,
                              "effective worker capacity cgroup quota is invalid")
            if payload[0] != "max":
                quota, period = int(payload[0]), int(payload[1])
                _require_contract(quota > 0 and period > 0,
                                  "effective worker capacity cgroup quota is invalid")
                capacities.append(max(1, math.ceil(quota / period)))
            if current == root:
                break
            current = current.parent
        return min(capacities) if capacities else None
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as error:
        raise AuditInfrastructureError(
            "effective worker capacity cgroup quota query failed") from error


def _linux_cpu_quota_capacity() -> int | None:
    if not sys.platform.startswith("linux"):
        return None
    return _linux_cpu_quota_capacity_from_paths(
        Path("/proc/self/cgroup"), Path("/sys/fs/cgroup"))


def _windows_job_cpu_quota_capacity(logical_capacity: int) -> int | None:
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.IsProcessInJob.argtypes = (
        wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL))
    kernel32.IsProcessInJob.restype = wintypes.BOOL
    kernel32.QueryInformationJobObject.argtypes = (
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD))
    kernel32.QueryInformationJobObject.restype = wintypes.BOOL
    in_job = wintypes.BOOL()
    if not kernel32.IsProcessInJob(
            kernel32.GetCurrentProcess(), None, ctypes.byref(in_job)):
        raise AuditInfrastructureError("effective worker capacity Job query failed")
    if not in_job.value:
        return None

    class CpuRateControl(ctypes.Structure):
        _fields_ = [("ControlFlags", wintypes.DWORD),
                    ("CpuRate", wintypes.DWORD)]

    control = CpuRateControl()
    if not kernel32.QueryInformationJobObject(
            None, 15, ctypes.byref(control), ctypes.sizeof(control), None):
        raise AuditInfrastructureError("effective worker capacity Job quota query failed")
    enabled = 0x1
    hard_cap = 0x4
    minimum_maximum = 0x10
    if not control.ControlFlags & enabled:
        return None
    if control.ControlFlags & hard_cap:
        _require_contract(1 <= control.CpuRate <= 10000,
                          "effective worker capacity Job quota is invalid")
        return max(1, math.ceil(logical_capacity * control.CpuRate / 10000))
    if control.ControlFlags & minimum_maximum:
        maximum_rate = (control.CpuRate >> 16) & 0xFFFF
        _require_contract(1 <= maximum_rate <= 10000,
                          "effective worker capacity Job quota is invalid")
        return max(1, math.ceil(logical_capacity * maximum_rate / 10000))
    return None


def effective_worker_capacity(limits: AuditLimits) -> int:
    _require_contract(isinstance(limits, AuditLimits),
                      "effective worker capacity limits are invalid")
    logical = _affinity_logical_capacity()
    candidates = [max(1, limits.workers), logical]
    job_quota = _windows_job_cpu_quota_capacity(logical)
    if job_quota is not None:
        candidates.append(job_quota)
    quota = _linux_cpu_quota_capacity()
    if quota is not None:
        candidates.append(quota)
    return max(1, min(candidates))


def constrain_reference_affinity(envelope: ReferenceCalibrationEnvelope) -> None:
    _require_contract(isinstance(envelope, ReferenceCalibrationEnvelope),
                      "reference calibration envelope is invalid")
    _require_contract(_affinity_logical_capacity() >= envelope.worker_capacity,
                      "reference worker capacity is unavailable")
    indices = set(envelope.affinity_cpu_indices)
    if os.name == "nt":
        import ctypes

        mask = sum(1 << index for index in indices)
        if not ctypes.WinDLL("kernel32", use_last_error=True).SetProcessAffinityMask(
                ctypes.WinDLL("kernel32", use_last_error=True).GetCurrentProcess(), mask):
            raise AuditInfrastructureError("reference worker affinity setup failed")
        return
    setter = getattr(os, "sched_setaffinity", None)
    _require_contract(setter is not None,
                      "reference worker affinity setup is unsupported")
    try:
        setter(0, indices)
    except OSError as error:
        raise AuditInfrastructureError("reference worker affinity setup failed") from error


def allowed_worker_counts(effective_worker_capacity: int) -> tuple[int, ...]:
    _require_contract(isinstance(effective_worker_capacity, int)
                      and not isinstance(effective_worker_capacity, bool)
                      and effective_worker_capacity > 0,
                      "effective worker capacity is invalid")
    if effective_worker_capacity == 1:
        return (1,)
    return (*_WORKER_SELECTION_ORDER,
            *range(3, effective_worker_capacity + 1))


def configuration_set_digest(
    configurations: tuple[PreprocessConfiguration, ...],
    decision_records: Mapping[str, DecisionConfigurationRecord],
    dependency_roots: DependencyRootAuthority,
) -> str:
    if (not isinstance(configurations, tuple) or not configurations
            or any(not isinstance(item, PreprocessConfiguration)
                   for item in configurations)
            or not isinstance(decision_records, Mapping)
            or not isinstance(dependency_roots, DependencyRootAuthority)):
        raise AuditInfrastructureError("decision configuration set is invalid")
    digests = tuple(item.digest for item in configurations)
    if len(set(digests)) != len(digests) or set(decision_records) != set(digests):
        raise AuditInfrastructureError("decision configuration set is invalid")
    projections: list[bytes] = []
    common_compiler: str | None = None
    for configuration in configurations:
        record = decision_records.get(configuration.digest)
        if (not isinstance(record, DecisionConfigurationRecord)
                or record.configuration_digest != configuration.digest
                or record.compiler_family is not configuration.family
                or record.compiler_capability_digest
                != configuration.compiler_capability_digest):
            raise AuditInfrastructureError("decision configuration record differs")
        if common_compiler is None:
            common_compiler = record.compiler_digest
        elif record.compiler_digest != common_compiler:
            raise AuditInfrastructureError("decision configuration compilers differ")
        projections.append(_canonical_json({
            "compiler_family": record.compiler_family.value,
            "compiler_digest": record.compiler_digest,
            "production_source": record.production_source.as_posix(),
            "arguments": record.normalized_decision_arguments,
            "environment": record.decision_environment_digest,
            "working_directory_role": record.working_directory_role,
            "compiler_capability": record.compiler_capability_digest,
        }))
    digest = hashlib.sha256()
    _update_framed(digest, b"olr-gpu-decision-configuration-set-v1")
    _update_framed(digest, dependency_roots.portable_authority_digest.encode("ascii"))
    for projection in sorted(projections):
        _update_framed(digest, projection)
    return digest.hexdigest()


def _slot_safe_prefixes(samples: tuple[object, ...], worker_count: int,
                        platform_kind: str) -> tuple[tuple[object, ...], ...] | None:
    by_slot: dict[int, list[object]] = {index: [] for index in range(worker_count)}
    for sample in samples:
        if (getattr(sample, "platform_kind", None) != platform_kind
                or getattr(sample, "worker_slot", None) not in by_slot):
            return None
        by_slot[getattr(sample, "worker_slot")].append(sample)
    prefixes: list[tuple[object, ...]] = []
    for slot in range(worker_count):
        current = by_slot[slot]
        if not current:
            return None
        generation = getattr(current[0], "worker_generation")
        safe: list[object] = []
        expected_ordinal = 1
        for item in current:
            if (getattr(item, "worker_generation") != generation
                    or getattr(item, "task_ordinal_in_generation") != expected_ordinal
                    or not _sample_is_eligible(item, platform_kind)):
                break
            safe.append(item)
            expected_ordinal += 1
        prefixes.append(tuple(safe))
    return tuple(prefixes)


def _sample_is_eligible(sample: object, platform_kind: str) -> bool:
    if not getattr(sample, "accounting_complete", False):
        return False
    if platform_kind == "windows":
        return getattr(sample, "aggregate_peak_rss_upper_bound_bytes") < (
            WINDOWS_COLD_RSS_CEILING_BYTES)
    if platform_kind == "linux":
        return (
            getattr(sample, "service_and_run_event_deltas_zero")
            and getattr(sample, "cgroup_current_accounted_memory_bytes")
            < getattr(sample, "cgroup_memory_high_bytes")
            and getattr(sample, "cgroup_peak_accounted_memory_bytes")
            < getattr(sample, "cgroup_memory_high_bytes")
            and getattr(sample, "cgroup_current_accounted_memory_bytes")
            < getattr(sample, "cgroup_memory_max_bytes")
            and getattr(sample, "cgroup_peak_accounted_memory_bytes")
            < getattr(sample, "cgroup_memory_max_bytes")
        )
    return (getattr(sample, "registered_survivors") == 0
            and getattr(sample, "known_unreconciled_descendants") == 0)


def derive_pilot_runtime_parameters(samples: tuple[object, ...], worker_count: int,
                                    configuration_count: int) -> PilotRuntimeParameters:
    _require_contract(isinstance(samples, tuple), "pilot samples are invalid")
    _require_contract(isinstance(worker_count, int) and not isinstance(worker_count, bool)
                      and worker_count > 0, "pilot worker count is invalid")
    _require_contract(isinstance(configuration_count, int)
                      and not isinstance(configuration_count, bool)
                      and configuration_count > 0,
                      "pilot configuration count is invalid")
    kinds = {getattr(sample, "platform_kind", None) for sample in samples}
    if len(kinds) != 1 or next(iter(kinds), None) not in {
            "windows", "linux", "macos"}:
        return PilotRuntimeParameters(1, 1, False)
    kind = next(iter(kinds))
    expected_type = {
        "windows": WindowsPostReturnSample,
        "linux": LinuxPostReturnSample,
        "macos": MacOSPostReturnSample,
    }[kind]
    if any(not isinstance(sample, expected_type) for sample in samples):
        return PilotRuntimeParameters(1, 1, False)
    prefixes = _slot_safe_prefixes(samples, worker_count, kind)
    if prefixes is None or any(not prefix for prefix in prefixes):
        return PilotRuntimeParameters(1, 1, False)
    maximum_tasks = min(math.ceil(configuration_count / worker_count),
                        min(len(prefix) for prefix in prefixes))
    if maximum_tasks <= 0:
        return PilotRuntimeParameters(1, 1, False)
    if kind == "macos":
        return PilotRuntimeParameters(maximum_tasks, MACOS_RSS_RECYCLE_DISABLED, True)
    safe_samples = tuple(sample for prefix in prefixes for sample in prefix)
    worker_current = max(getattr(sample, "worker_current_rss_bytes")
                         for sample in safe_samples)
    if kind == "windows":
        upper = max(getattr(sample, "aggregate_peak_rss_upper_bound_bytes")
                    for sample in safe_samples)
        headroom = WINDOWS_COLD_RSS_CEILING_BYTES - 1 - upper
    else:
        upper = max(getattr(sample, "cgroup_peak_accounted_memory_bytes")
                    for sample in safe_samples)
        high_values = {getattr(sample, "cgroup_memory_high_bytes")
                       for sample in safe_samples}
        if len(high_values) != 1:
            return PilotRuntimeParameters(1, 1, False)
        headroom = next(iter(high_values)) - 1 - upper
    if headroom < worker_count:
        return PilotRuntimeParameters(1, 1, False)
    recycle = worker_current + headroom // worker_count
    if recycle < 0 or recycle > MACOS_RSS_RECYCLE_DISABLED:
        return PilotRuntimeParameters(1, 1, False)
    return PilotRuntimeParameters(maximum_tasks, recycle, True)


def _decision_to_object(decision: WorkerCountDecision) -> dict[str, object]:
    _validate_windows_decision(decision)
    return dataclasses.asdict(decision)


def _expect_keys(value: object, keys: set[str]) -> dict[str, object]:
    _require_contract(isinstance(value, dict) and set(value) == keys)
    return value


def _integer(value: object, *, minimum: int = 0) -> int:
    _require_contract(isinstance(value, int) and not isinstance(value, bool)
                      and value >= minimum)
    return value


def _number(value: object) -> float:
    _require_contract(isinstance(value, (int, float)) and not isinstance(value, bool)
                      and math.isfinite(value) and value >= 0)
    return float(value)


def _digest(value: object) -> str:
    _require_contract(isinstance(value, str) and len(value) == 64
                      and all(character in "0123456789abcdef" for character in value))
    return value


def _decode_memory(value: object) -> WindowsRunMemoryMeasurements:
    keys = {field.name for field in dataclasses.fields(WindowsRunMemoryMeasurements)}
    item = _expect_keys(value, keys)
    arguments = {name: (item[name] if name == "accounting_complete"
                        else _integer(item[name])) for name in keys}
    _require_contract(isinstance(arguments["accounting_complete"], bool))
    return WindowsRunMemoryMeasurements(**arguments)


def _decode_timings(value: object) -> WorkerStageTimings:
    keys = {field.name for field in dataclasses.fields(WorkerStageTimings)}
    item = _expect_keys(value, keys)
    return WorkerStageTimings(**{name: _number(item[name]) for name in keys})


def _decode_phase(value: object) -> WindowsPhaseSnapshot:
    item = _expect_keys(value, {"platform_kind", "phase", "memory",
                                "archived_generation_identities"})
    _require_contract(item["platform_kind"] == "windows"
                      and item["phase"] in {"inspection", "tasks"})
    identities = item["archived_generation_identities"]
    _require_contract(
        isinstance(identities, list)
        and all(
            isinstance(identity, list)
            and len(identity) == 2
            for identity in identities
        )
    )
    return WindowsPhaseSnapshot("windows", item["phase"],
                                _decode_memory(item["memory"]),
                                tuple(
                                    (_integer(identity[0]),
                                     _integer(identity[1]))
                                    for identity in identities
                                ))


def _decode_post_sample(value: object) -> WindowsPostReturnSample:
    keys = {field.name for field in dataclasses.fields(WindowsPostReturnSample)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == "windows"
                      and isinstance(item["accounting_complete"], bool))
    return WindowsPostReturnSample(
        platform_kind="windows", worker_slot=_integer(item["worker_slot"]),
        worker_generation=_integer(item["worker_generation"]),
        task_ordinal_in_generation=_integer(item["task_ordinal_in_generation"], minimum=1),
        worker_current_rss_bytes=_integer(item["worker_current_rss_bytes"]),
        aggregate_peak_rss_upper_bound_bytes=_integer(
            item["aggregate_peak_rss_upper_bound_bytes"]),
        accounting_complete=item["accounting_complete"],
    )


def _decode_sample(value: object) -> WorkerCalibrationSample:
    keys = {field.name for field in dataclasses.fields(WorkerCalibrationSample)}
    item = _expect_keys(value, keys)
    integer_names = {
        "worker_count", "configuration_count", "inspection_probe_invocations",
        "audit_compiler_invocations", "stdout_bytes", "cache_bytes", "cache_entries",
        "maximum_tasks_per_worker", "recycle_rss_bytes",
        "surviving_owned_process_count",
    }
    _require_contract(item["platform_kind"] == "windows"
                      and isinstance(item["pilot_eligible"], bool)
                      and isinstance(item["included_stages"], list)
                      and all(isinstance(stage, str) for stage in item["included_stages"])
                      and isinstance(item["post_return_samples"], list))
    values = {name: _integer(item[name], minimum=(1 if name in {
        "worker_count", "configuration_count", "maximum_tasks_per_worker"
    } else 0)) for name in integer_names}
    return WorkerCalibrationSample(
        platform_kind="windows", elapsed_seconds=_number(item["elapsed_seconds"]),
        memory=_decode_memory(item["memory"]),
        included_stages=tuple(item["included_stages"]),
        post_return_samples=tuple(_decode_post_sample(entry)
                                  for entry in item["post_return_samples"]),
        stage_p50_seconds=_decode_timings(item["stage_p50_seconds"]),
        stage_p95_seconds=_decode_timings(item["stage_p95_seconds"]),
        inspection_phase_snapshot=_decode_phase(item["inspection_phase_snapshot"]),
        task_phase_snapshot=_decode_phase(item["task_phase_snapshot"]),
        pilot_eligible=item["pilot_eligible"], **values)


def _decode_key(value: object) -> WorkerDecisionKey:
    keys = {field.name for field in dataclasses.fields(WorkerDecisionKey)}
    item = _expect_keys(value, keys)
    return WorkerDecisionKey(
        platform_tag=item["platform_tag"], compiler_digest=_digest(item["compiler_digest"]),
        configuration_set_digest=_digest(item["configuration_set_digest"]),
        uninspected_configuration_digest=_digest(item["uninspected_configuration_digest"]),
        decision_engine_fingerprint=_digest(item["decision_engine_fingerprint"]),
        effective_worker_capacity=_integer(item["effective_worker_capacity"], minimum=1),
        reference_envelope_digest=_digest(item["reference_envelope_digest"]),
        key_digest=_digest(item["key_digest"]),
    )


def _decode_rejection(value: object) -> CalibrationRejection:
    keys = {field.name for field in dataclasses.fields(CalibrationRejection)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == "windows"
                      and item["failure_stage"] in CALIBRATION_FAILURE_STAGES
                      and item["failure_code"] in CALIBRATION_FAILURE_CODES
                      and isinstance(item["cleanup_complete"], bool))
    return CalibrationRejection(
        "windows", _integer(item["worker_count"], minimum=1),
        item["failure_stage"], item["failure_code"], item["cleanup_complete"],
        _integer(item["surviving_owned_process_count"]))


def _decode_decision(value: object) -> WorkerCountDecision:
    keys = {field.name for field in dataclasses.fields(WorkerCountDecision)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == "windows"
                      and item["artifact_schema"] == WORKER_DECISION_SCHEMA
                      and isinstance(item["attempted_worker_counts"], list)
                      and isinstance(item["rejections"], list)
                      and isinstance(item["samples"], list))
    return WorkerCountDecision(
        platform_kind="windows", artifact_schema=WORKER_DECISION_SCHEMA,
        key=_decode_key(item["key"]),
        selected_workers=_integer(item["selected_workers"], minimum=1),
        maximum_tasks_per_worker=_integer(item["maximum_tasks_per_worker"], minimum=1),
        recycle_rss_bytes=_integer(item["recycle_rss_bytes"]),
        attempted_worker_counts=tuple(_integer(count, minimum=1)
                                      for count in item["attempted_worker_counts"]),
        rejections=tuple(_decode_rejection(entry) for entry in item["rejections"]),
        samples=tuple(_decode_sample(entry) for entry in item["samples"]),
        evidence_record_sha256=_digest(item["evidence_record_sha256"]),
    )


def _decode_platform_memory(value: object, platform_kind: str):
    memory_type = {
        "linux": LinuxRunMemoryMeasurements,
        "macos": MacOSRunMemoryMeasurements,
    }.get(platform_kind)
    _require_contract(memory_type is not None)
    keys = {field.name for field in dataclasses.fields(memory_type)}
    item = _expect_keys(value, keys)
    values: dict[str, object] = {}
    for name in keys:
        if name == "accounting_complete":
            _require_contract(isinstance(item[name], bool))
            values[name] = item[name]
        else:
            values[name] = _integer(item[name])
    return memory_type(**values)


def _decode_platform_post_sample(value: object, platform_kind: str):
    sample_type = {
        "linux": LinuxPostReturnSample,
        "macos": MacOSPostReturnSample,
    }.get(platform_kind)
    _require_contract(sample_type is not None)
    keys = {field.name for field in dataclasses.fields(sample_type)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == platform_kind
                      and isinstance(item["accounting_complete"], bool))
    values: dict[str, object] = {"platform_kind": platform_kind,
                                "accounting_complete": item["accounting_complete"]}
    for name in keys - {"platform_kind", "accounting_complete"}:
        if name == "service_and_run_event_deltas_zero":
            _require_contract(isinstance(item[name], bool))
            values[name] = item[name]
        else:
            values[name] = _integer(
                item[name], minimum=(1 if name == "task_ordinal_in_generation" else 0))
    return sample_type(**values)


def _decode_platform_phase(value: object, platform_kind: str):
    phase_type = {"linux": LinuxPhaseSnapshot,
                  "macos": MacOSPhaseSnapshot}.get(platform_kind)
    item = _expect_keys(value, {"platform_kind", "phase", "memory",
                                "archived_generation_count"})
    _require_contract(phase_type is not None and item["platform_kind"] == platform_kind
                      and item["phase"] in {"inspection", "tasks"})
    return phase_type(
        platform_kind, item["phase"],
        _decode_platform_memory(item["memory"], platform_kind),
        _integer(item["archived_generation_count"]))


def _decode_platform_sample(value: object, platform_kind: str):
    sample_type = {"linux": LinuxWorkerCalibrationSample,
                   "macos": MacOSWorkerCalibrationSample}.get(platform_kind)
    _require_contract(sample_type is not None)
    keys = {field.name for field in dataclasses.fields(sample_type)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == platform_kind
                      and isinstance(item["pilot_eligible"], bool)
                      and isinstance(item["included_stages"], list)
                      and all(isinstance(stage, str) for stage in item["included_stages"])
                      and isinstance(item["post_return_samples"], list))
    integer_names = {
        "worker_count", "configuration_count", "inspection_probe_invocations",
        "audit_compiler_invocations", "stdout_bytes", "cache_bytes", "cache_entries",
        "maximum_tasks_per_worker", "recycle_rss_bytes",
        "surviving_owned_process_count",
    }
    values = {name: _integer(item[name], minimum=(1 if name in {
        "worker_count", "configuration_count", "maximum_tasks_per_worker"
    } else 0)) for name in integer_names}
    return sample_type(
        platform_kind=platform_kind, elapsed_seconds=_number(item["elapsed_seconds"]),
        memory=_decode_platform_memory(item["memory"], platform_kind),
        included_stages=tuple(item["included_stages"]),
        post_return_samples=tuple(_decode_platform_post_sample(entry, platform_kind)
                                  for entry in item["post_return_samples"]),
        stage_p50_seconds=_decode_timings(item["stage_p50_seconds"]),
        stage_p95_seconds=_decode_timings(item["stage_p95_seconds"]),
        inspection_phase_snapshot=_decode_platform_phase(
            item["inspection_phase_snapshot"], platform_kind),
        task_phase_snapshot=_decode_platform_phase(
            item["task_phase_snapshot"], platform_kind),
        pilot_eligible=item["pilot_eligible"], **values)


def _decode_platform_envelope(value: object, platform_kind: str):
    envelope_type = {"linux": LinuxCalibrationEnvelope,
                     "macos": MacOSCalibrationEnvelope}.get(platform_kind)
    _require_contract(envelope_type is not None)
    keys = {field.name for field in dataclasses.fields(envelope_type)}
    item = _expect_keys(value, keys)
    values: dict[str, object] = {}
    for field in dataclasses.fields(envelope_type):
        current = item[field.name]
        if field.name == "cgroup_v2":
            _require_contract(isinstance(current, bool))
        elif field.name == "effective_worker_capacity":
            current = _integer(current, minimum=1)
        else:
            _require_contract(isinstance(current, str) and bool(current))
        values[field.name] = current
    return envelope_type(**values)


def _decode_platform_rejection(value: object, platform_kind: str) -> CalibrationRejection:
    keys = {field.name for field in dataclasses.fields(CalibrationRejection)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == platform_kind
                      and item["failure_stage"] in CALIBRATION_FAILURE_STAGES
                      and item["failure_code"] in CALIBRATION_FAILURE_CODES
                      and isinstance(item["cleanup_complete"], bool))
    return CalibrationRejection(
        platform_kind, _integer(item["worker_count"], minimum=1),
        item["failure_stage"], item["failure_code"], item["cleanup_complete"],
        _integer(item["surviving_owned_process_count"]))


def _decode_native_decision(value: object, platform_kind: str) -> PlatformWorkerDecision:
    decision_type = {"linux": LinuxWorkerCountDecision,
                     "macos": MacOSWorkerCountDecision}.get(platform_kind)
    _require_contract(decision_type is not None)
    keys = {field.name for field in dataclasses.fields(decision_type)}
    item = _expect_keys(value, keys)
    _require_contract(item["platform_kind"] == platform_kind
                      and item["artifact_schema"] == WORKER_DECISION_SCHEMA
                      and isinstance(item["attempted_worker_counts"], list)
                      and isinstance(item["rejections"], list)
                      and isinstance(item["samples"], list))
    return decision_type(
        platform_kind=platform_kind, artifact_schema=WORKER_DECISION_SCHEMA,
        key=_decode_key(item["key"]),
        envelope=_decode_platform_envelope(item["envelope"], platform_kind),
        selected_workers=_integer(item["selected_workers"], minimum=1),
        maximum_tasks_per_worker=_integer(item["maximum_tasks_per_worker"], minimum=1),
        recycle_rss_bytes=_integer(item["recycle_rss_bytes"]),
        attempted_worker_counts=tuple(_integer(count, minimum=1)
                                      for count in item["attempted_worker_counts"]),
        rejections=tuple(_decode_platform_rejection(entry, platform_kind)
                         for entry in item["rejections"]),
        samples=tuple(_decode_platform_sample(entry, platform_kind)
                      for entry in item["samples"]),
        evidence_record_sha256=_digest(item["evidence_record_sha256"]))


def _validate_windows_decision(decision: WorkerCountDecision,
                               expected: WorkerDecisionKey | None = None) -> None:
    _require_contract(isinstance(decision, WorkerCountDecision)
                      and decision.platform_kind == "windows"
                      and decision.artifact_schema == WORKER_DECISION_SCHEMA)
    recomputed = worker_decision_key(
        decision.key.platform_tag, decision.key.compiler_digest,
        decision.key.configuration_set_digest,
        decision.key.uninspected_configuration_digest,
        decision.key.decision_engine_fingerprint,
        decision.key.effective_worker_capacity)
    _require_contract(recomputed == decision.key)
    if expected is not None:
        _require_contract(decision.key == expected)
    counts = allowed_worker_counts(decision.key.effective_worker_capacity)
    _require_contract(decision.attempted_worker_counts == counts)
    _require_contract(decision.recycle_rss_bytes != MACOS_RSS_RECYCLE_DISABLED
                      and all(isinstance(sample, WorkerCalibrationSample)
                              and sample.recycle_rss_bytes
                              != MACOS_RSS_RECYCLE_DISABLED
                              for sample in decision.samples))
    sample_counts = tuple(sample.worker_count for sample in decision.samples)
    rejection_counts = tuple(item.worker_count for item in decision.rejections)
    _require_contract(len(set(sample_counts)) == len(sample_counts)
                      and len(set(rejection_counts)) == len(rejection_counts)
                      and not set(sample_counts).intersection(rejection_counts)
                      and set(sample_counts).union(rejection_counts) == set(counts))
    for rejection in decision.rejections:
        _require_contract(rejection.platform_kind == "windows"
                          and rejection.failure_stage in CALIBRATION_FAILURE_STAGES
                          and rejection.failure_code in CALIBRATION_FAILURE_CODES
                          and rejection.cleanup_complete
                          and rejection.surviving_owned_process_count == 0)
    passing = [sample for sample in decision.samples if _measured_sample_passes(sample)]
    _require_contract(bool(passing))
    selected = min(passing, key=lambda sample: (sample.elapsed_seconds,
                                                sample.worker_count))
    _require_contract(decision.selected_workers == selected.worker_count
                      and decision.maximum_tasks_per_worker
                      == selected.maximum_tasks_per_worker
                      and decision.recycle_rss_bytes == selected.recycle_rss_bytes)
    _digest(decision.evidence_record_sha256)


def _measured_sample_passes(sample: WorkerCalibrationSample) -> bool:
    memory = sample.memory
    recomputed_aggregate = max(
        memory.maximum_simultaneous_working_set_bytes,
        memory.parent_peak_rss_bytes + memory.inspection_peak_tree_bytes,
        memory.parent_peak_rss_bytes + memory.worker_peak_rss_bytes,
    )
    return (
        sample.platform_kind == "windows"
        and sample.configuration_count == 251
        and sample.elapsed_seconds < 180.0
        and sample.pilot_eligible is True
        and sample.audit_compiler_invocations == 502
        and sample.inspection_probe_invocations >= 0
        and sample.included_stages == CALIBRATION_INCLUDED_STAGES
        and sample.surviving_owned_process_count == 0
        and memory.accounting_complete is True
        and memory.job_total_process_count == memory.retained_process_identity_count
        and memory.aggregate_peak_rss_upper_bound_bytes
        < WINDOWS_COLD_RSS_CEILING_BYTES
        and memory.aggregate_peak_rss_upper_bound_bytes == recomputed_aggregate
        and memory.surviving_job_process_count == 0
        and sample.inspection_phase_snapshot.phase == "inspection"
        and sample.task_phase_snapshot.phase == "tasks"
    )


def _native_measured_sample_passes(sample: object, platform_kind: str) -> bool:
    common = (
        getattr(sample, "platform_kind", None) == platform_kind
        and isinstance(getattr(sample, "configuration_count", None), int)
        and not isinstance(getattr(sample, "configuration_count", None), bool)
        and getattr(sample, "configuration_count", None) > 0
        and getattr(sample, "elapsed_seconds", 180.0) < 180.0
        and getattr(sample, "pilot_eligible", False) is True
        and getattr(sample, "audit_compiler_invocations", None)
        == 2 * getattr(sample, "configuration_count", 0)
        and getattr(sample, "included_stages", None) == CALIBRATION_INCLUDED_STAGES
        and getattr(sample, "surviving_owned_process_count", None) == 0
        and getattr(getattr(sample, "memory", None), "accounting_complete", False) is True
        and getattr(sample, "inspection_phase_snapshot", None).phase == "inspection"
        and getattr(sample, "task_phase_snapshot", None).phase == "tasks")
    if not common:
        return False
    memory = sample.memory
    if platform_kind == "linux":
        return (
            sample.recycle_rss_bytes != MACOS_RSS_RECYCLE_DISABLED
            and
            memory.surviving_cgroup_process_count == 0
            and memory.cgroup_oom_count_delta == 0
            and memory.cgroup_oom_kill_count_delta == 0
            and memory.cgroup_max_event_count_delta == 0
            and memory.service_root_oom_count_delta == 0
            and memory.service_root_oom_kill_count_delta == 0
            and memory.service_root_max_event_count_delta == 0
            and memory.cgroup_current_accounted_memory_bytes
            < memory.cgroup_memory_high_bytes
            and memory.cgroup_current_accounted_memory_bytes
            < memory.cgroup_memory_max_bytes
            and memory.cgroup_peak_accounted_memory_bytes
            < memory.cgroup_memory_high_bytes
            and memory.cgroup_peak_accounted_memory_bytes
            < memory.cgroup_memory_max_bytes)
    return (memory.surviving_registered_process_count == 0
            and memory.known_unreconciled_descendant_count == 0
            and sample.recycle_rss_bytes == MACOS_RSS_RECYCLE_DISABLED)


def _validate_native_decision(
    decision: PlatformWorkerDecision, expected: WorkerDecisionKey | None = None,
    *, validate_current_envelope: bool = False,
) -> None:
    _require_contract(isinstance(decision, (LinuxWorkerCountDecision,
                                            MacOSWorkerCountDecision)))
    kind = decision.platform_kind
    _require_contract(kind in {"linux", "macos"}
                      and decision.artifact_schema == WORKER_DECISION_SCHEMA)
    recomputed = platform_worker_decision_key(
        kind, decision.key.compiler_digest, decision.key.configuration_set_digest,
        decision.key.uninspected_configuration_digest,
        decision.key.decision_engine_fingerprint,
        decision.key.effective_worker_capacity, decision.envelope)
    _require_contract(recomputed == decision.key)
    if expected is not None:
        _require_contract(decision.key == expected)
    _require_contract(decision.envelope.effective_worker_capacity
                      == decision.key.effective_worker_capacity)
    if validate_current_envelope:
        current_envelope = (linux_calibration_envelope() if kind == "linux"
                            else macos_calibration_envelope())
        _require_contract(decision.envelope == current_envelope,
                          "worker decision calibration envelope mismatch")
    counts = allowed_worker_counts(decision.key.effective_worker_capacity)
    _require_contract(decision.attempted_worker_counts == counts)
    expected_sample_type = (LinuxWorkerCalibrationSample
                            if kind == "linux"
                            else MacOSWorkerCalibrationSample)
    _require_contract(all(type(sample) is expected_sample_type
                          for sample in decision.samples)
                      and ((kind == "macos"
                            and decision.recycle_rss_bytes
                            == MACOS_RSS_RECYCLE_DISABLED)
                           or (kind == "linux"
                               and decision.recycle_rss_bytes
                               != MACOS_RSS_RECYCLE_DISABLED)))
    sample_counts = tuple(sample.worker_count for sample in decision.samples)
    rejection_counts = tuple(item.worker_count for item in decision.rejections)
    _require_contract(len(set(sample_counts)) == len(sample_counts)
                      and len(set(rejection_counts)) == len(rejection_counts)
                      and not set(sample_counts).intersection(rejection_counts)
                      and set(sample_counts).union(rejection_counts) == set(counts))
    configuration_counts = {
        sample.configuration_count for sample in decision.samples}
    _require_contract(len(configuration_counts) == 1
                      and next(iter(configuration_counts)) > 0)
    for rejection in decision.rejections:
        _require_contract(rejection.platform_kind == kind
                          and rejection.failure_stage
                          in CALIBRATION_FAILURE_STAGES
                          and rejection.failure_code
                          in CALIBRATION_FAILURE_CODES
                          and rejection.cleanup_complete
                          and rejection.surviving_owned_process_count == 0)
    passing = [sample for sample in decision.samples
               if _native_measured_sample_passes(sample, kind)]
    _require_contract(bool(passing))
    selected = min(passing, key=lambda sample: (sample.elapsed_seconds,
                                                sample.worker_count))
    _require_contract(decision.selected_workers == selected.worker_count
                      and decision.maximum_tasks_per_worker
                      == selected.maximum_tasks_per_worker
                      and decision.recycle_rss_bytes == selected.recycle_rss_bytes)
    _digest(decision.evidence_record_sha256)


def write_platform_worker_decision_atomic(
    path: Path, decision: PlatformWorkerDecision,
) -> None:
    if isinstance(decision, WorkerCountDecision):
        _validate_windows_decision(decision)
    else:
        _validate_native_decision(decision)
    _write_decision_payload_atomic(path, _canonical_json(dataclasses.asdict(decision)))


def _write_decision_payload_atomic(path: Path, payload: bytes) -> None:
    _require_contract(isinstance(path, Path) and path.name not in {"", ".", ".."})
    _require_contract(0 < len(payload) <= WORKER_DECISION_MAX_BYTES)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                                            dir=path.parent)
        temporary = Path(name)
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
        if os.name != "nt":
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    except OSError as error:
        raise AuditInfrastructureError("worker decision atomic write failed") from error
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def write_worker_decision_atomic(path: Path, decision: WorkerCountDecision) -> None:
    write_platform_worker_decision_atomic(path, decision)


def _read_bounded_decision_payload(
    path: Path, deadline: float | None = None,
) -> bytes:
    _require_contract(isinstance(path, Path)
                      and (deadline is None
                           or (isinstance(deadline, (int, float))
                               and not isinstance(deadline, bool)
                               and time.monotonic() < deadline)),
                      "worker decision read input is invalid")
    try:
        with path.open("rb") as stream:
            payload = stream.read(WORKER_DECISION_MAX_BYTES + 1)
    except OSError as error:
        raise AuditInfrastructureError("worker decision load failed") from error
    if deadline is not None:
        _require_contract(time.monotonic() < deadline,
                          "worker decision read deadline expired")
    _require_contract(0 < len(payload) <= WORKER_DECISION_MAX_BYTES)
    return payload


def _decode_validated_platform_payload(
    payload: bytes, expected_platform: str, expected_key: WorkerDecisionKey,
) -> PlatformWorkerDecision:
    _require_contract(isinstance(payload, bytes)
                      and 0 < len(payload) <= WORKER_DECISION_MAX_BYTES
                      and expected_platform in {"windows", "linux", "macos"}
                      and isinstance(expected_key, WorkerDecisionKey),
                      "worker decision payload input is invalid")
    try:
        value = json.loads(payload)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise AuditInfrastructureError("worker decision is invalid") from error
    _require_contract(_canonical_json(value) == payload)
    if expected_platform == "windows":
        decision = _decode_decision(value)
        _validate_windows_decision(decision, expected_key)
        _require_contract(decision.key.platform_tag == platform_tag(),
                          "worker decision platform mismatch")
    else:
        decision = _decode_native_decision(value, expected_platform)
        _validate_native_decision(
            decision, expected_key, validate_current_envelope=True)
    _require_contract(effective_worker_capacity(AuditLimits())
                      == expected_key.effective_worker_capacity,
                      "worker decision effective worker capacity mismatch")
    return decision


def load_worker_decision(path: Path, expected: WorkerDecisionKey) -> WorkerCountDecision:
    _require_contract(isinstance(path, Path) and isinstance(expected, WorkerDecisionKey))
    decision = _decode_validated_platform_payload(
        _read_bounded_decision_payload(path), "windows", expected)
    _require_contract(isinstance(decision, WorkerCountDecision))
    return decision


def runtime_contract_from_decision(
    decision: WorkerCountDecision, pipeline_deadline: float,
) -> WorkerRuntimeContract:
    _validate_windows_decision(decision)
    return WorkerRuntimeContract(
        decision.selected_workers, decision.maximum_tasks_per_worker,
        decision.recycle_rss_bytes, pipeline_deadline)


def runtime_contract_from_platform_decision(
    decision: PlatformWorkerDecision, pipeline_deadline: float,
) -> WorkerRuntimeContract:
    if isinstance(decision, WorkerCountDecision):
        return runtime_contract_from_decision(decision, pipeline_deadline)
    if not isinstance(decision, (LinuxWorkerCountDecision, MacOSWorkerCountDecision)):
        raise AuditInfrastructureError("platform worker decision is invalid")
    _validate_native_decision(decision)
    selected = next((sample for sample in decision.samples
                     if sample.worker_count == decision.selected_workers), None)
    _require_contract(selected is not None
                      and selected.maximum_tasks_per_worker
                      == decision.maximum_tasks_per_worker
                      and selected.recycle_rss_bytes == decision.recycle_rss_bytes)
    if isinstance(decision, MacOSWorkerCountDecision):
        _require_contract(decision.recycle_rss_bytes == MACOS_RSS_RECYCLE_DISABLED)
    else:
        _require_contract(decision.recycle_rss_bytes != MACOS_RSS_RECYCLE_DISABLED)
    return WorkerRuntimeContract(
        decision.selected_workers, decision.maximum_tasks_per_worker,
        decision.recycle_rss_bytes, pipeline_deadline)


def schedule_decision_configuration_audits(
    *,
    decision: PlatformWorkerDecision,
    pipeline_deadline: float,
    source_root: Path,
    configurations: tuple[PreprocessConfiguration, ...],
    dependency_roots: DependencyRootAuthority,
    capability_registry: object,
    initial_digest_map: Mapping[object, object],
    prepared_cache: object,
    limits: AuditLimits,
    expected_audit_engine_fingerprint: str,
    inspection_probe_invocations: int,
    run_accountant: object,
    compact_observer: object,
):
    """Decision-run entry that freezes runtime parameters before scheduling."""

    runtime_contract = runtime_contract_from_platform_decision(
        decision, pipeline_deadline
    )
    from gpu_capability_runner import schedule_configuration_audits

    return schedule_configuration_audits(
        source_root,
        configurations,
        dependency_roots,
        capability_registry,
        initial_digest_map,
        prepared_cache,
        limits,
        expected_audit_engine_fingerprint,
        runtime_contract,
        inspection_probe_invocations=inspection_probe_invocations,
        run_accountant=run_accountant,
        compact_observer=compact_observer,
    )


def load_platform_worker_decision(
    path: Path, expected_platform: str, expected_key: WorkerDecisionKey,
) -> PlatformWorkerDecision:
    if expected_platform == "windows":
        return load_worker_decision(path, expected_key)
    if expected_platform not in {"linux", "macos"}:
        raise AuditInfrastructureError("worker decision platform is invalid")
    _require_contract(isinstance(path, Path) and isinstance(expected_key,
                                                         WorkerDecisionKey))
    return _decode_validated_platform_payload(
        _read_bounded_decision_payload(path), expected_platform, expected_key)


def prevalidate_platform_worker_decision(
    path: Path, expected_platform: str,
    expected_audit_engine_fingerprint: str,
    uninspected_configuration_digest: str, deadline: float,
) -> PrevalidatedWorkerDecision:
    _require_contract(expected_platform in {"windows", "linux", "macos"}
                      and isinstance(deadline, (int, float))
                      and not isinstance(deadline, bool)
                      and time.monotonic() < deadline)
    _digest(expected_audit_engine_fingerprint)
    _digest(uninspected_configuration_digest)
    payload = _read_bounded_decision_payload(path, deadline)
    try:
        value = json.loads(payload)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise AuditInfrastructureError("worker decision is invalid") from error
    _require_contract(_canonical_json(value) == payload and isinstance(value, dict)
                      and value.get("platform_kind") == expected_platform
                      and isinstance(value.get("key"), dict)
                      and value["key"].get("decision_engine_fingerprint")
                      == expected_audit_engine_fingerprint
                      and value["key"].get("uninspected_configuration_digest")
                      == uninspected_configuration_digest)
    return PrevalidatedWorkerDecision(
        path=path, platform_kind=expected_platform,
        audit_engine_fingerprint=expected_audit_engine_fingerprint,
        uninspected_configuration_digest=uninspected_configuration_digest,
        bounded_payload_sha256=hashlib.sha256(payload).hexdigest())


def finalize_platform_worker_decision(
    prevalidated: PrevalidatedWorkerDecision, expected_key: WorkerDecisionKey,
    deadline: float,
) -> PlatformWorkerDecision:
    _require_contract(isinstance(prevalidated, PrevalidatedWorkerDecision)
                      and isinstance(expected_key, WorkerDecisionKey)
                      and isinstance(deadline, (int, float))
                      and not isinstance(deadline, bool)
                      and time.monotonic() < deadline)
    payload = _read_bounded_decision_payload(prevalidated.path, deadline)
    _require_contract(hmac.compare_digest(hashlib.sha256(payload).hexdigest(),
                                              prevalidated.bounded_payload_sha256))
    return _decode_validated_platform_payload(
        payload, prevalidated.platform_kind, expected_key)


class RealCalibrationRunner:
    """Decision scheduler seam plus Task 9's native one-count smoke lane."""

    def __init__(self, platform_kind: str, *,
                 full_pipeline_attempt: object | None = None) -> None:
        if platform_kind not in {"windows", "linux", "macos"}:
            raise AuditInfrastructureError("calibration platform is invalid")
        self.platform_kind = platform_kind
        self._full_pipeline_attempt = full_pipeline_attempt
        self._smoke_preparation: CalibrationSmokePreparation | None = None
        self._smoke_cache_roots: set[Path] = set()

    def prepare(
        self,
        *args: object,
        smoke_only: bool = False,
        operation_deadline: float | None = None,
        cleanup_deadline: float | None = None,
    ) -> object:
        if not smoke_only:
            raise AuditInfrastructureError(
                "real calibration runner integration belongs to Task 10")
        if (
            self.platform_kind not in {"linux", "macos"}
            or len(args) != 5
            or args[0] != self.platform_kind
            or not isinstance(args[1], Path)
            or not isinstance(args[2], tuple)
            or len(args[2]) != 1
            or not isinstance(args[2][0], Path)
            or not isinstance(args[3], Path)
            or not isinstance(args[4], AuditLimits)
            or not callable(self._full_pipeline_attempt)
            or not isinstance(operation_deadline, (int, float))
            or isinstance(operation_deadline, bool)
            or not isinstance(cleanup_deadline, (int, float))
            or isinstance(cleanup_deadline, bool)
            or not math.isfinite(float(operation_deadline))
            or not math.isfinite(float(cleanup_deadline))
            or time.monotonic() >= operation_deadline
            or cleanup_deadline - operation_deadline
            != CALIBRATION_SMOKE_CLEANUP_RESERVE_SECONDS
        ):
            message = ("real calibration smoke requires exactly one configuration"
                       if isinstance(args[2] if len(args) > 2 else None, tuple)
                       and len(args[2]) != 1
                       else "real calibration smoke preparation is invalid")
            raise AuditInfrastructureError(message)
        capacity = effective_worker_capacity(args[4])
        if capacity > args[4].workers:
            raise AuditInfrastructureError(
                "calibration runner capacity exceeds limits")
        preparation = CalibrationSmokePreparation(
            platform_kind=self.platform_kind,
            source_root=args[1],
            compile_commands=args[2],
            cache_parent=args[3],
            limits=args[4],
            configuration_count=1,
            candidate_worker_counts=allowed_worker_counts(capacity),
            operation_deadline=float(operation_deadline),
            cleanup_deadline=float(cleanup_deadline),
        )
        self._smoke_preparation = preparation
        self._smoke_cache_roots.clear()
        return preparation

    def run(self, *, kind: str, **kwargs: object) -> object:
        """Execute a decision-bound audit or a non-authoritative smoke run.

        Task 10 owns native decision production and exhaustive calibration.
        The smoke lane accepts only the bounded output of the existing full
        pipeline and never constructs or persists a worker decision.
        """

        decision = kwargs.get("decision")
        if kind == "decision-audit":
            if getattr(decision, "platform_kind", None) != self.platform_kind:
                raise AuditInfrastructureError(
                    "real calibration runner operation is invalid")
            return schedule_decision_configuration_audits(**kwargs)
        if kind not in {"pilot", "measured"}:
            raise AuditInfrastructureError(
                "real calibration runner operation is invalid")
        preparation = kwargs.pop("preparation", None)
        worker_count = kwargs.get("worker_count")
        cache_root = kwargs.get("cache_root")
        included_stages = kwargs.get("included_stages")
        runtime_parameters = kwargs.get("runtime_parameters")
        operation_deadline = kwargs.get("operation_deadline")
        cleanup_deadline = kwargs.get("cleanup_deadline")
        try:
            resolved_cache = cache_root.resolve(strict=True)
            resolved_parent = preparation.cache_parent.resolve(strict=True)
        except (AttributeError, OSError) as error:
            raise AuditInfrastructureError(
                "real calibration smoke cache is invalid") from error
        if (
            preparation is not self._smoke_preparation
            or not isinstance(preparation, CalibrationSmokePreparation)
            or worker_count not in preparation.candidate_worker_counts
            or included_stages != CALIBRATION_INCLUDED_STAGES
            or (kind == "pilot" and runtime_parameters is not None)
            or (kind == "measured"
                and not isinstance(runtime_parameters, PilotRuntimeParameters))
            or resolved_cache.parent != resolved_parent
            or resolved_cache in self._smoke_cache_roots
            or any(resolved_cache.iterdir())
            or operation_deadline != preparation.operation_deadline
            or cleanup_deadline != preparation.cleanup_deadline
            or time.monotonic() >= preparation.operation_deadline
        ):
            raise AuditInfrastructureError(
                "real calibration smoke operation is invalid")
        self._smoke_cache_roots.add(resolved_cache)
        audit_run = self._full_pipeline_attempt(
            kind=kind,
            worker_count=worker_count,
            runtime_parameters=runtime_parameters,
            cache_root=cache_root,
            included_stages=included_stages,
            preparation=preparation,
            operation_deadline=preparation.operation_deadline,
            cleanup_deadline=preparation.cleanup_deadline,
        )
        if not isinstance(audit_run, CompilerAuditRun):
            raise AuditInfrastructureError(
                "real calibration smoke full-pipeline result is invalid")
        measurements = audit_run.measurements
        if (
            measurements.configuration_count != 1
            or measurements.memory_backend != self.platform_kind
            or measurements.cache_root.resolve() != resolved_cache
            or measurements.worker_counts_started != (worker_count,)
            or measurements.runtime_contract.workers != worker_count
            or measurements.audit_compiler_invocations != 2
            or measurements.included_stages != CALIBRATION_SMOKE_PIPELINE_STAGES
            or measurements.inspection_phase_snapshot.phase != "inspection"
            or measurements.task_phase_snapshot.phase != "tasks"
            or len(audit_run.summary.configuration_digests) != 1
        ):
            raise AuditInfrastructureError(
                "real calibration smoke full-pipeline measurements are invalid")
        memory = measurements.memory
        if self.platform_kind == "linux":
            events_zero = all(getattr(memory, name) == 0 for name in (
                "cgroup_oom_count_delta", "cgroup_oom_kill_count_delta",
                "cgroup_max_event_count_delta", "service_root_oom_count_delta",
                "service_root_oom_kill_count_delta",
                "service_root_max_event_count_delta"))
            post_samples = tuple(LinuxPostReturnSample(
                "linux", slot, 0, 1, 0,
                memory.cgroup_current_accounted_memory_bytes,
                memory.cgroup_peak_accounted_memory_bytes,
                memory.cgroup_memory_high_bytes,
                memory.cgroup_memory_max_bytes,
                events_zero,
                memory.accounting_complete,
            ) for slot in range(worker_count))
            surviving_count = memory.surviving_cgroup_process_count
        else:
            post_samples = tuple(MacOSPostReturnSample(
                "macos", slot, 0, 1, 0,
                memory.maximum_observed_owned_group_resident_bytes,
                memory.surviving_registered_process_count,
                memory.known_unreconciled_descendant_count,
                memory.accounting_complete,
            ) for slot in range(worker_count))
            surviving_count = memory.surviving_registered_process_count
        if kind == "pilot":
            if surviving_count != 0:
                raise AuditInfrastructureError(
                    "real calibration smoke containment is invalid")
            return post_samples
        runtime = measurements.runtime_contract
        if (runtime.maximum_tasks_per_worker
                != runtime_parameters.maximum_tasks_per_worker
                or runtime.recycle_rss_bytes
                != runtime_parameters.recycle_rss_bytes):
            raise AuditInfrastructureError(
                "real calibration smoke runtime differs")
        timings = WorkerStageTimings(
            measurements.elapsed_seconds, measurements.elapsed_seconds,
            measurements.elapsed_seconds, measurements.elapsed_seconds)
        common = dict(
            platform_kind=self.platform_kind,
            worker_count=worker_count,
            configuration_count=1,
            elapsed_seconds=measurements.elapsed_seconds,
            maximum_tasks_per_worker=runtime.maximum_tasks_per_worker,
            recycle_rss_bytes=runtime.recycle_rss_bytes,
            memory=memory,
            inspection_probe_invocations=(
                measurements.inspection_probe_invocations),
            audit_compiler_invocations=measurements.audit_compiler_invocations,
            stdout_bytes=measurements.stdout_bytes,
            cache_bytes=measurements.cache_bytes,
            cache_entries=measurements.cache_entries,
            pilot_eligible=runtime_parameters.pilot_eligible,
            surviving_owned_process_count=surviving_count,
            included_stages=CALIBRATION_INCLUDED_STAGES,
            post_return_samples=post_samples,
            stage_p50_seconds=timings,
            stage_p95_seconds=timings,
            inspection_phase_snapshot=measurements.inspection_phase_snapshot,
            task_phase_snapshot=measurements.task_phase_snapshot,
        )
        sample = (LinuxWorkerCalibrationSample(**common)
                  if self.platform_kind == "linux"
                  else MacOSWorkerCalibrationSample(**common))
        if not _native_measured_sample_passes(sample, self.platform_kind):
            raise AuditInfrastructureError(
                "real calibration smoke measured result is invalid")
        return sample


@dataclasses.dataclass(frozen=True)
class CalibrationSmokePreparation:
    platform_kind: str
    source_root: Path
    compile_commands: tuple[Path, ...]
    cache_parent: Path
    limits: AuditLimits
    configuration_count: int
    candidate_worker_counts: tuple[int, ...]
    operation_deadline: float
    cleanup_deadline: float


@dataclasses.dataclass(frozen=True)
class CalibrationSmokeReadiness:
    """Non-authoritative smoke result; deliberately has no decision key."""

    platform_kind: str
    candidate_worker_counts: tuple[int, ...]
    attempted_worker_count: int
    pilot: tuple[object, ...]
    runtime_parameters: PilotRuntimeParameters
    measured: object


def run_real_calibration_smoke(
    runner: RealCalibrationRunner,
    source_root: Path,
    compile_commands: tuple[Path, ...],
    cache_parent: Path,
    limits: AuditLimits,
) -> CalibrationSmokeReadiness:
    """Run one native full-pipeline candidate without producing an artifact."""

    if not isinstance(runner, RealCalibrationRunner):
        raise AuditInfrastructureError("real calibration smoke runner is invalid")
    started = time.monotonic()
    cleanup_deadline = started + CALIBRATION_SMOKE_SECONDS
    operation_deadline = (
        cleanup_deadline - CALIBRATION_SMOKE_CLEANUP_RESERVE_SECONDS
    )
    _require_contract(
        0.0 < operation_deadline - started < cleanup_deadline - started < 180.0,
        "real calibration smoke deadline is invalid",
    )
    preparation = runner.prepare(
        runner.platform_kind, source_root, compile_commands, cache_parent, limits,
        smoke_only=True,
        operation_deadline=operation_deadline,
        cleanup_deadline=cleanup_deadline)
    if not isinstance(preparation, CalibrationSmokePreparation):
        raise AuditInfrastructureError(
            "real calibration smoke preparation is invalid")
    cache_parent.mkdir(parents=True, exist_ok=True)
    # Task 9 proves the real native path with one configuration and therefore
    # one schedulable worker. Exhaustive candidate selection remains Task 10;
    # the full ordering is retained above for codec/readiness validation only.
    worker_count = 1
    _require_contract(worker_count in preparation.candidate_worker_counts)
    pilot_directory = tempfile.TemporaryDirectory(
        prefix=f"gpu-calibration-smoke-{worker_count}-pilot-",
        dir=cache_parent,
    )
    pilot_error = None
    try:
        _require_contract(
            time.monotonic() < operation_deadline,
            "real calibration smoke deadline expired",
        )
        pilot = runner.run(
            kind="pilot", worker_count=worker_count, runtime_parameters=None,
            cache_root=Path(pilot_directory.name),
            included_stages=CALIBRATION_INCLUDED_STAGES,
            preparation=preparation,
            operation_deadline=operation_deadline,
            cleanup_deadline=cleanup_deadline)
    except BaseException as error:
        pilot_error = error
        raise
    finally:
        try:
            pilot_directory.cleanup()
            _require_contract(
                time.monotonic() < cleanup_deadline,
                "real calibration smoke deadline expired during cleanup",
            )
        except BaseException as cleanup_error:
            if pilot_error is not None:
                pilot_error.add_note(
                    f"pilot cache cleanup also failed: {cleanup_error!r}"
                )
            else:
                raise
    parameters = derive_pilot_runtime_parameters(
        pilot, worker_count, preparation.configuration_count)
    measured_directory = tempfile.TemporaryDirectory(
        prefix=f"gpu-calibration-smoke-{worker_count}-measured-",
        dir=cache_parent,
    )
    measured_error = None
    try:
        _require_contract(
            time.monotonic() < operation_deadline,
            "real calibration smoke deadline expired",
        )
        measured = runner.run(
            kind="measured", worker_count=worker_count,
            runtime_parameters=parameters,
            cache_root=Path(measured_directory.name),
            included_stages=CALIBRATION_INCLUDED_STAGES,
            preparation=preparation,
            operation_deadline=operation_deadline,
            cleanup_deadline=cleanup_deadline)
    except BaseException as error:
        measured_error = error
        raise
    finally:
        try:
            measured_directory.cleanup()
            _require_contract(
                time.monotonic() < cleanup_deadline,
                "real calibration smoke deadline expired during cleanup",
            )
        except BaseException as cleanup_error:
            if measured_error is not None:
                measured_error.add_note(
                    f"measured cache cleanup also failed: {cleanup_error!r}"
                )
            else:
                raise
    return CalibrationSmokeReadiness(
        platform_kind=preparation.platform_kind,
        candidate_worker_counts=preparation.candidate_worker_counts,
        attempted_worker_count=worker_count,
        pilot=pilot,
        runtime_parameters=parameters,
        measured=measured,
    )


def _calibrate_platform_worker_count_with_runner(
    platform_kind: str,
    runner: object,
    source_root: Path,
    compile_commands: tuple[Path, ...],
    cache_parent: Path,
    limits: AuditLimits,
) -> PlatformWorkerDecision:
    if (platform_kind not in {"windows", "linux", "macos"}
            or not isinstance(source_root, Path)
            or not isinstance(compile_commands, tuple) or not compile_commands
            or any(not isinstance(path, Path) for path in compile_commands)
            or not isinstance(cache_parent, Path)
            or not isinstance(limits, AuditLimits)):
        raise AuditInfrastructureError("calibration runner input is invalid")
    prepare = getattr(runner, "prepare", None)
    run = getattr(runner, "run", None)
    if not callable(prepare) or not callable(run):
        raise AuditInfrastructureError("calibration runner is invalid")
    prepared = prepare(platform_kind, source_root, compile_commands, cache_parent, limits)
    expected_prepared_length = 3 if platform_kind == "windows" else 4
    if (not isinstance(prepared, tuple) or len(prepared) != expected_prepared_length
            or not isinstance(prepared[0], WorkerDecisionKey)
            or not isinstance(prepared[1], int) or isinstance(prepared[1], bool)
            or prepared[1] <= 0):
        raise AuditInfrastructureError("calibration runner preparation is invalid")
    envelope = None
    if platform_kind == "windows":
        key, configuration_count, evidence_record_sha256 = prepared
    else:
        key, configuration_count, evidence_record_sha256, envelope = prepared
        expected_envelope_type = (LinuxCalibrationEnvelope
                                  if platform_kind == "linux"
                                  else MacOSCalibrationEnvelope)
        if not isinstance(envelope, expected_envelope_type):
            raise AuditInfrastructureError("calibration runner envelope is invalid")
    _digest(evidence_record_sha256)
    counts = allowed_worker_counts(key.effective_worker_capacity)
    if key.effective_worker_capacity > limits.workers:
        raise AuditInfrastructureError("calibration runner capacity exceeds limits")
    cache_parent.mkdir(parents=True, exist_ok=True)
    samples: list[object] = []
    rejections: list[CalibrationRejection] = []
    cache_directories: list[tempfile.TemporaryDirectory[str]] = []
    try:
        for worker_count in counts:
            pilot_cache = tempfile.TemporaryDirectory(
                prefix=f"gpu-calibration-{worker_count}-pilot-", dir=cache_parent)
            cache_directories.append(pilot_cache)
            pilot = run(
                kind="pilot", worker_count=worker_count,
                runtime_parameters=None, cache_root=Path(pilot_cache.name),
                included_stages=CALIBRATION_INCLUDED_STAGES)
            if not isinstance(pilot, tuple):
                raise AuditInfrastructureError("calibration pilot result is invalid")
            parameters = derive_pilot_runtime_parameters(
                pilot, worker_count, configuration_count)
            measured_cache = tempfile.TemporaryDirectory(
                prefix=f"gpu-calibration-{worker_count}-measured-", dir=cache_parent)
            cache_directories.append(measured_cache)
            try:
                measured = run(
                    kind="measured", worker_count=worker_count,
                    runtime_parameters=parameters,
                    cache_root=Path(measured_cache.name),
                    included_stages=CALIBRATION_INCLUDED_STAGES)
            except CalibrationAttemptFailure as failure:
                rejection = CalibrationRejection(
                    platform_kind=platform_kind, worker_count=worker_count,
                    failure_stage=failure.failure_stage,
                    failure_code=failure.failure_code,
                    cleanup_complete=failure.cleanup_complete,
                    surviving_owned_process_count=(
                        failure.surviving_owned_process_count))
                if (not rejection.cleanup_complete
                        or rejection.surviving_owned_process_count != 0):
                    raise AuditInfrastructureError(
                        "calibration rejection cleanup is incomplete") from failure
                rejections.append(rejection)
                continue
            expected_sample_type = {
                "windows": WorkerCalibrationSample,
                "linux": LinuxWorkerCalibrationSample,
                "macos": MacOSWorkerCalibrationSample,
            }[platform_kind]
            if (not isinstance(measured, expected_sample_type)
                    or measured.worker_count != worker_count
                    or measured.configuration_count != configuration_count
                    or measured.maximum_tasks_per_worker
                    != parameters.maximum_tasks_per_worker
                    or measured.recycle_rss_bytes != parameters.recycle_rss_bytes
                    or measured.pilot_eligible is not parameters.pilot_eligible
                    or measured.included_stages != CALIBRATION_INCLUDED_STAGES
                    or measured.surviving_owned_process_count != 0):
                raise AuditInfrastructureError(
                    "calibration measured result is invalid")
            samples.append(measured)
    finally:
        for directory in reversed(cache_directories):
            directory.cleanup()
    passing = [sample for sample in samples if (
        _measured_sample_passes(sample) if platform_kind == "windows"
        else _native_measured_sample_passes(sample, platform_kind))]
    if not passing:
        raise AuditInfrastructureError("calibration produced no passing sample")
    selected = min(passing, key=lambda sample: (sample.elapsed_seconds,
                                                sample.worker_count))
    common = dict(
        platform_kind=platform_kind, artifact_schema=WORKER_DECISION_SCHEMA,
        key=key, selected_workers=selected.worker_count,
        maximum_tasks_per_worker=selected.maximum_tasks_per_worker,
        recycle_rss_bytes=selected.recycle_rss_bytes,
        attempted_worker_counts=counts, rejections=tuple(rejections),
        samples=tuple(samples), evidence_record_sha256=evidence_record_sha256)
    if platform_kind == "windows":
        decision = WorkerCountDecision(**common)
        _validate_windows_decision(decision)
    elif platform_kind == "linux":
        decision = LinuxWorkerCountDecision(envelope=envelope, **common)
        _validate_native_decision(decision)
    else:
        decision = MacOSWorkerCountDecision(envelope=envelope, **common)
        _validate_native_decision(decision)
    return decision


def calibrate_platform_worker_count(
    platform_kind: str, source_root: Path, compile_commands: tuple[Path, ...],
    cache_parent: Path, limits: AuditLimits,
) -> PlatformWorkerDecision:
    runner = RealCalibrationRunner(platform_kind)
    return _calibrate_platform_worker_count_with_runner(
        platform_kind, runner, source_root, compile_commands, cache_parent, limits)
