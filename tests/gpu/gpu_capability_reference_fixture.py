"""Retain-all preprocessing reference used only by unit and live-compiler tests."""

from __future__ import annotations

import concurrent.futures
import threading
import time
from collections.abc import Mapping
from pathlib import PurePosixPath

from gpu_capability_cache import PreprocessCache
from gpu_capability_model import (
    AuditInfrastructureError,
    AuditLimits,
    CoverageReport,
    DependencyRootAuthority,
    FileIdentity,
    PreprocessedTranslationUnitView,
    PreprocessConfiguration,
    requires_compile_entry,
    validate_dependency_root_authority,
)
import gpu_capability_runner as production_runner
from gpu_capability_runner import (
    _configuration_is_objcpp,
    _is_windows_backend_path,
    _validated_orchestration_inputs,
    _view_production_provenance,
)


def reference_preprocess_all(
    configurations: tuple[PreprocessConfiguration, ...],
    dependency_roots: DependencyRootAuthority,
    production: Mapping[PurePosixPath, FileIdentity],
    cache: PreprocessCache,
    limits: AuditLimits,
    pipeline_deadline: float,
) -> tuple[tuple[PreprocessedTranslationUnitView, ...], CoverageReport]:
    """Retain every view as an equivalence oracle outside production code."""

    authority = validate_dependency_root_authority(dependency_roots)
    if (
        not isinstance(pipeline_deadline, (int, float))
        or isinstance(pipeline_deadline, bool)
        or time.monotonic() >= pipeline_deadline
    ):
        raise AuditInfrastructureError("preprocess pipeline deadline exceeded")
    ordered = _validated_orchestration_inputs(configurations, production, cache, limits)
    if any(
        item.dependency_root_authority_digest != authority.portable_authority_digest
        for item in ordered
    ):
        raise AuditInfrastructureError("configuration dependency authority differs")
    configured_families = frozenset(item.family for item in ordered)
    has_objcpp = any(_configuration_is_objcpp(item) for item in ordered)
    has_windows_backend = any(
        item.source.relative is not None
        and _is_windows_backend_path(item.source.relative)
        for item in ordered
    )
    active = frozenset(
        path
        for path in production
        if requires_compile_entry(
            path,
            configured_families,
            has_objcpp,
            has_windows_backend,
        )
    )
    configured_sources = frozenset(
        item.source.relative for item in ordered if item.source.relative is not None
    )
    missing_commands = sorted(active - configured_sources, key=PurePosixPath.as_posix)
    if missing_commands:
        raise AuditInfrastructureError(
            "active source has no compile command: "
            + ", ".join(path.as_posix() for path in missing_commands)
        )

    cancellation = threading.Event()
    views: dict[str, PreprocessedTranslationUnitView] = {}
    failures: list[tuple[str, str]] = []
    iterator = iter(ordered)

    def execute(configuration: PreprocessConfiguration) -> PreprocessedTranslationUnitView:
        return production_runner.load_or_preprocess(
            configuration,
            authority,
            production,
            cache,
            limits,
            pipeline_deadline,
            cancellation,
        )

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=min(limits.workers, len(ordered)),
        thread_name_prefix="gpu-capability-preprocess",
    ) as executor:
        pending: dict[
            concurrent.futures.Future[PreprocessedTranslationUnitView],
            PreprocessConfiguration,
        ] = {}
        for _ in range(min(limits.workers, len(ordered))):
            configuration = next(iterator, None)
            if configuration is not None:
                pending[executor.submit(execute, configuration)] = configuration

        while pending:
            completed, _ = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in sorted(completed, key=lambda item: pending[item].digest):
                configuration = pending.pop(future)
                try:
                    view = future.result()
                    if view.configuration != configuration:
                        raise AuditInfrastructureError(
                            "preprocessor returned a mismatched orchestration configuration"
                        )
                    views[configuration.digest] = view
                except Exception as error:
                    failures.append((configuration.digest, str(error)))
            if failures:
                cancellation.set()
                continue
            while len(pending) < limits.workers:
                configuration = next(iterator, None)
                if configuration is None:
                    break
                pending[executor.submit(execute, configuration)] = configuration

    if failures:
        details = "; ".join(
            f"digest={digest}: {message}"
            for digest, message in sorted(failures, key=lambda item: (item[0], item[1]))
        )
        raise AuditInfrastructureError(
            f"GPU capability preprocessing configurations failed: {details}"
        )

    ordered_views = tuple(views[item.digest] for item in ordered)
    authoritative: set[PurePosixPath] = set()
    missing_view_sources: list[tuple[str, PurePosixPath]] = []
    for view in ordered_views:
        reached = _view_production_provenance(view, production)
        authoritative.update(reached)
        source = view.configuration.source
        if source.relative is None:
            raise AuditInfrastructureError("production source has no relative path")
        if source.relative in reached:
            continue
        if source.line_count == 0 and source in view.dependencies:
            authoritative.add(source.relative)
            continue
        missing_view_sources.append((view.configuration.digest, source.relative))
    if missing_view_sources:
        details = ", ".join(
            f"digest={digest} source={path.as_posix()}"
            for digest, path in sorted(
                missing_view_sources,
                key=lambda item: (item[0], item[1].as_posix()),
            )
        )
        raise AuditInfrastructureError(
            f"configuration view lacks main-source provenance: {details}"
        )
    missing_authoritative = sorted(active - authoritative, key=PurePosixPath.as_posix)
    if missing_authoritative:
        raise AuditInfrastructureError(
            "active source lacks authoritative compiler coverage: "
            + ", ".join(path.as_posix() for path in missing_authoritative)
        )
    return ordered_views, CoverageReport(
        authoritative=frozenset(authoritative),
        source_only=frozenset(set(production) - authoritative),
        configurations=tuple(item.digest for item in ordered),
    )
