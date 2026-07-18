from __future__ import annotations

import os
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))

from gpu_capability_cache import PreprocessCache  # noqa: E402
from gpu_capability_model import (  # noqa: E402
    AuditInfrastructureError,
    AuditLimits,
    CompilerFamily,
    PreprocessedTranslationUnitView,
    enumerate_production_identities,
    build_dependency_root_authority,
)
from gpu_capability_runner import collect_configurations, preprocess_all  # noqa: E402
from gpu_capability_provenance import (  # noqa: E402
    parse_gcc_dependencies,
    validate_dependency_identities,
)
import gpu_capability_source_audit as capability_audit  # noqa: E402
from gpu_capability_source_audit import (  # noqa: E402
    AggregatedFinding,
    Finding,
    TOKEN_PATTERN,
)


INCLUDED_MACROS = """\
#ifndef OLR_GPU_LIVE_MACROS_H
#define OLR_GPU_LIVE_MACROS_H
#define CAT(a,b) a##b
#endif
"""

FORBIDDEN_EXPRESSION = "GpuSurface::nativeHandle()"
FORBIDDEN_REASON = (
    "native handle access must use the lease returned by its specific read "
    "scope or withRead callback"
)
FIXTURE_PATH = PurePosixPath("playback/gpu/live_fixture.cpp")


FORBIDDEN_FIXTURES = {
    "nested-self": """\
#define F(x) x
#define A PASTE
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.F(F(A))(native,Handle)();
""",
    "original-tail": """\
#define F(x) F_I(x)
#define F_I(x) F_##x
#define F_call PASTE
#define F_native native
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.F(call)(F(native),Handle)();
""",
    "object-tail": """\
#define OPEN PASTE(
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.OPEN native,Handle)();
""",
    "va-opt": """\
#define CAT(a,...) a ## __VA_OPT__(__VA_ARGS__)
lease.CAT(native,Handle)();
""",
    "inactive-undef": """\
#include "gpu_live_macros.h"
#if 0
#undef CAT
#endif
lease.CAT(native,Handle)();
""",
    "late-undef": """\
#include "gpu_live_macros.h"
lease.CAT(native,Handle)();
#undef CAT
""",
}

FORBIDDEN_OUTPUT_ORACLES = {
    "nested-self": b"lease.nativeHandle()",
    "original-tail": b"lease.nativeHandle()",
    "object-tail": b"lease.nativeHandle()",
    "va-opt": b"lease.nativeHandle()",
    "inactive-undef": b"lease.nativeHandle()",
    "late-undef": b"lease.nativeHandle()",
}


SAFE_FIXTURES = {
    "raw-paste-suppression": (
        """\
#define CAT(a,b) a##b
#define X nativeHandle
lease.CAT(safe,X)();
""",
        b"lease.safeX()",
    ),
    "inactive-forbidden": (
        """\
#if 0
lease.CAT(native,Handle)();
#endif
lease.safe();
""",
        b"lease.safe()",
    ),
    "stringification": (
        """\
#define STRINGIFY(x) #x
const char* value = STRINGIFY(lease.CAT(native,Handle)());
lease.safe();
""",
        b"lease.safe()",
    ),
    "empty-placemarker": (
        """\
#define CAT(a,...) a##__VA_ARGS__
lease.CAT(safe,)();
""",
        b"lease.safe()",
    ),
    "benign-self-recursion": (
        """\
#define SELF SELF
SELF
lease.safe();
""",
        b"lease.safe()",
    ),
    "benign-mutual-recursion": (
        """\
#define FIRST SECOND
#define SECOND FIRST
FIRST
lease.safe();
""",
        b"lease.safe()",
    ),
    "ordinary-forwarding": (
        """\
#define FORWARD(x) x
FORWARD(lease.safe)();
""",
        b"lease.safe()",
    ),
}

SAFE_OUTPUT_ORACLES = {
    "raw-paste-suppression": ((b"safeX",), (b"nativeHandle",)),
    "inactive-forbidden": ((b"safe",), (b"nativeHandle", b"CAT")),
    "stringification": (
        (b'"lease.CAT(native,Handle)()"', b"safe"),
        (b"nativeHandle",),
    ),
    "empty-placemarker": ((b"safe",), (b"CAT",)),
    "benign-self-recursion": ((b"SELF", b"safe"), ()),
    "benign-mutual-recursion": ((b"FIRST", b"safe"), (b"SECOND",)),
    "ordinary-forwarding": ((b"safe",), (b"FORWARD",)),
}


def _invocation_line(source: str) -> int:
    return next(
        line
        for line, text in enumerate(source.splitlines(), 1)
        if text.startswith("lease.")
    )


def _optional_live_compilers() -> tuple[tuple[CompilerFamily, Path], ...]:
    candidates = {
        CompilerFamily.GCC: ("g++", "g++.exe", "gcc", "gcc.exe"),
        CompilerFamily.CLANG: ("clang++", "clang++.exe", "clang", "clang.exe"),
        CompilerFamily.MSVC: ("cl.exe", "cl"),
        CompilerFamily.CLANG_CL: ("clang-cl.exe", "clang-cl"),
    }
    selected: list[tuple[CompilerFamily, Path]] = []
    seen: set[Path] = set()
    preferred = os.environ.get("OLR_GPU_LIVE_COMPILER")
    if preferred:
        path = Path(preferred).resolve()
        family_name = os.environ.get("OLR_GPU_LIVE_FAMILY", "gcc")
        selected.append((CompilerFamily(family_name), path))
        seen.add(path)
    for family, names in candidates.items():
        for name in names:
            found = shutil.which(name)
            if found is None:
                continue
            path = Path(found).resolve()
            if path not in seen:
                selected.append((family, path))
                seen.add(path)
            break
    return tuple(selected)


def _report_optional_compiler_absences(
    compilers: tuple[tuple[CompilerFamily, Path], ...],
    printer=print,
) -> None:
    selected = {family for family, _compiler in compilers}
    for family in CompilerFamily:
        if family not in selected:
            printer(
                "SKIP: direct unittest optional live compiler family "
                f"{family.value}: no executable found"
            )


def _contains_token_sequence(
    view: PreprocessedTranslationUnitView,
    expected_tokens: bytes,
) -> bool:
    try:
        expected_text = expected_tokens.decode("ascii", errors="strict")
    except UnicodeDecodeError as error:
        raise AuditInfrastructureError(
            "live expected token sequence must be ASCII"
        ) from error
    expected = tuple(
        match.group(0).encode("ascii")
        for match in TOKEN_PATTERN.finditer(expected_text)
    )
    if not expected:
        raise AuditInfrastructureError("live expected token sequence is empty")
    actual = tuple(token.spelling for token in view.tokens)
    return any(
        actual[start:start + len(expected)] == expected
        for start in range(len(actual) - len(expected) + 1)
    )


def _require_safe_output_oracle(
    name: str,
    view: PreprocessedTranslationUnitView,
) -> None:
    try:
        required, forbidden = SAFE_OUTPUT_ORACLES[name]
    except KeyError as error:
        raise AuditInfrastructureError(
            f"live safe output oracle is missing: {name}"
        ) from error
    spellings = tuple(token.spelling for token in view.tokens)
    missing = tuple(spelling for spelling in required if spelling not in spellings)
    present = tuple(spelling for spelling in forbidden if spelling in spellings)
    if missing or present:
        raise AuditInfrastructureError(
            f"live safe output oracle failed: {name}: "
            f"missing={missing!r}, forbidden-present={present!r}"
        )


def _require_forbidden_finding(
    name: str,
    source: str,
    view: PreprocessedTranslationUnitView,
    findings: list[AggregatedFinding],
) -> None:
    invocation_line = _invocation_line(source)
    shaped = [
        item
        for item in findings
        if item.finding.path == FIXTURE_PATH
        and item.finding.line == invocation_line
        and item.finding.expression == FORBIDDEN_EXPRESSION
        and item.finding.reason == FORBIDDEN_REASON
    ]
    if not shaped:
        raise AuditInfrastructureError(
            f"live forbidden fixture was not rejected at its invocation: {name}"
        )
    expected = (view.configuration.digest,)
    if not any(item.configurations == expected for item in shaped):
        raise AuditInfrastructureError(
            f"live forbidden finding has the wrong configuration digest: {name}"
        )


def _live_command_path(path: Path, family: CompilerFamily) -> str:
    # GCC dependency files use Make escaping. Supplying a slash-normalized
    # Windows source path prevents ordinary separators from becoming escapes.
    return (
        path.as_posix()
        if family in {CompilerFamily.GCC, CompilerFamily.CLANG}
        else str(path)
    )


def _live_compile_arguments(
    compiler: Path,
    family: CompilerFamily,
    fixture: Path,
) -> tuple[str, ...]:
    if family in {CompilerFamily.GCC, CompilerFamily.CLANG}:
        return (
            _live_command_path(compiler, family), "-std=c++20", "-c",
            _live_command_path(fixture, family),
        )
    options = [str(compiler), "/std:c++20", "/c"]
    if family is CompilerFamily.MSVC:
        options.append("/Zc:preprocessor")
    options.append(str(fixture))
    return tuple(options)


def _run_live_fixture_impl(
    compiler: Path,
    family: CompilerFamily,
    source: str,
    expected_tokens: bytes,
    *,
    budget: capability_audit.LiveExecutionBudget | None = None,
) -> tuple[PreprocessedTranslationUnitView, list[AggregatedFinding]]:
    """Run one temporary fixture through the production compiler pipeline."""

    if not isinstance(compiler, Path) or not isinstance(family, CompilerFamily):
        raise AuditInfrastructureError("live compiler selection is invalid")
    if not isinstance(source, str) or not source:
        raise AuditInfrastructureError("live fixture source is empty")
    if budget is None:
        budget = capability_audit.LiveExecutionBudget.start()
    with tempfile.TemporaryDirectory(prefix="gpu-capability-live-") as temporary:
        root = Path(temporary).resolve()
        fixture = root / "playback" / "gpu" / "live_fixture.cpp"
        header = fixture.with_name("gpu_live_macros.h")
        fixture.parent.mkdir(parents=True)
        fixture.write_text(source, encoding="utf-8", newline="\n")
        header.write_text(INCLUDED_MACROS, encoding="utf-8", newline="\n")
        compiler = compiler.resolve(strict=True)
        arguments = _live_compile_arguments(compiler, family, fixture)
        database = root / "compile_commands.json"
        database.write_text(json.dumps(({
            "directory": str(root),
            "file": _live_command_path(fixture, family),
            "arguments": arguments,
        },)), encoding="utf-8", newline="\n")
        environment = dict(os.environ)
        dependency_roots = build_dependency_root_authority(
            root, {"toolchain": compiler.parent}
        )
        configurations = collect_configurations(
            root, (database,), environment, dependency_roots
        )
        if len(configurations) != 1:
            raise AuditInfrastructureError(
                "live fixture did not produce exactly one configuration"
            )
        if configurations[0].family is not family:
            raise AuditInfrastructureError(
                "live compiler family contradicts the selected family"
            )
        production = enumerate_production_identities(root)
        cache = PreprocessCache(root / "gpu-capability-cache")
        remaining_seconds = budget.remaining_seconds()
        views, coverage = preprocess_all(
            configurations,
            dependency_roots,
            production,
            cache,
            AuditLimits(workers=1, total_seconds=remaining_seconds),
        )
        budget.remaining_seconds()
        if len(views) != 1:
            raise AuditInfrastructureError(
                "live fixture did not produce exactly one compiler view"
            )
        if not _contains_token_sequence(views[0], expected_tokens):
            raise AuditInfrastructureError(
                "live compiler output is missing the expected token sequence"
            )
        sources = capability_audit.load_production_sources(root)
        findings, returned_coverage = capability_audit.audit_pipeline(
            sources, views, coverage
        )
        if returned_coverage != coverage:
            raise AuditInfrastructureError("live audit changed compiler coverage")
        view = views[0]
        budget.remaining_seconds()
    return view, findings


def run_live_fixture(
    compiler: Path,
    family: CompilerFamily,
    source: str,
    expected_tokens: bytes,
    *,
    budget: capability_audit.LiveExecutionBudget | None = None,
) -> tuple[PreprocessedTranslationUnitView, list[AggregatedFinding]]:
    """Run one fixture and normalize filesystem failures as infrastructure."""

    try:
        return _run_live_fixture_impl(
            compiler,
            family,
            source,
            expected_tokens,
            budget=budget,
        )
    except OSError as error:
        raise AuditInfrastructureError(
            f"live fixture setup failed: {error}"
        ) from error


def run_live_compiler_suite(
    compiler: Path,
    family: CompilerFamily,
    budget: capability_audit.LiveExecutionBudget | None = None,
) -> None:
    """Require exact forbidden/safe parity for one selected compiler."""

    if budget is None:
        budget = capability_audit.LiveExecutionBudget.start()
    for name, source in FORBIDDEN_FIXTURES.items():
        budget.remaining_seconds()
        view, findings = run_live_fixture(
            compiler,
            family,
            source,
            FORBIDDEN_OUTPUT_ORACLES[name],
            budget=budget,
        )
        budget.remaining_seconds()
        _require_forbidden_finding(name, source, view, findings)
    for name, (source, expected_tokens) in SAFE_FIXTURES.items():
        budget.remaining_seconds()
        view, findings = run_live_fixture(
            compiler, family, source, expected_tokens, budget=budget
        )
        budget.remaining_seconds()
        _require_safe_output_oracle(name, view)
        if findings:
            raise AuditInfrastructureError(
                f"live safe fixture produced capability findings: {name}"
            )


class LiveCompilerContractTests(unittest.TestCase):
    def test_msvc_live_command_enables_only_conforming_msvc_preprocessor(self):
        command_builder = globals().get("_live_compile_arguments")
        self.assertTrue(callable(command_builder))
        if not callable(command_builder):
            return
        compiler = Path("C:/toolchain/cl.exe")
        fixture = Path("C:/fixture/live_fixture.cpp")
        msvc = command_builder(compiler, CompilerFamily.MSVC, fixture)
        clang_cl = command_builder(compiler, CompilerFamily.CLANG_CL, fixture)
        self.assertIn("/Zc:preprocessor", msvc)
        self.assertNotIn("/Zc:preprocessor", clang_cl)

    def test_exact_fixture_tables_and_live_api_exist(self):
        self.assertEqual(
            set(FORBIDDEN_FIXTURES),
            {
                "nested-self", "original-tail", "object-tail", "va-opt",
                "inactive-undef", "late-undef",
            },
        )
        self.assertEqual(
            set(SAFE_FIXTURES),
            {
                "raw-paste-suppression", "inactive-forbidden",
                "stringification", "empty-placemarker",
                "benign-self-recursion", "benign-mutual-recursion",
                "ordinary-forwarding",
            },
        )
        self.assertTrue(callable(globals().get("run_live_fixture")))
        self.assertTrue(callable(globals().get("run_live_compiler_suite")))
        self.assertIn("...", SAFE_FIXTURES["empty-placemarker"][0])

    def test_each_fixture_source_and_safe_output_oracle_is_locked_exactly(self):
        expected_forbidden = {
            "nested-self": """\
#define F(x) x
#define A PASTE
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.F(F(A))(native,Handle)();
""",
            "original-tail": """\
#define F(x) F_I(x)
#define F_I(x) F_##x
#define F_call PASTE
#define F_native native
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.F(call)(F(native),Handle)();
""",
            "object-tail": """\
#define OPEN PASTE(
#define PASTE(a,b) PASTE_I(a,b)
#define PASTE_I(a,b) a##b
lease.OPEN native,Handle)();
""",
            "va-opt": """\
#define CAT(a,...) a ## __VA_OPT__(__VA_ARGS__)
lease.CAT(native,Handle)();
""",
            "inactive-undef": """\
#include "gpu_live_macros.h"
#if 0
#undef CAT
#endif
lease.CAT(native,Handle)();
""",
            "late-undef": """\
#include "gpu_live_macros.h"
lease.CAT(native,Handle)();
#undef CAT
""",
        }
        expected_safe = {
            "raw-paste-suppression": (
                """\
#define CAT(a,b) a##b
#define X nativeHandle
lease.CAT(safe,X)();
""",
                b"lease.safeX()",
            ),
            "inactive-forbidden": (
                """\
#if 0
lease.CAT(native,Handle)();
#endif
lease.safe();
""",
                b"lease.safe()",
            ),
            "stringification": (
                """\
#define STRINGIFY(x) #x
const char* value = STRINGIFY(lease.CAT(native,Handle)());
lease.safe();
""",
                b"lease.safe()",
            ),
            "empty-placemarker": (
                """\
#define CAT(a,...) a##__VA_ARGS__
lease.CAT(safe,)();
""",
                b"lease.safe()",
            ),
            "benign-self-recursion": (
                """\
#define SELF SELF
SELF
lease.safe();
""",
                b"lease.safe()",
            ),
            "benign-mutual-recursion": (
                """\
#define FIRST SECOND
#define SECOND FIRST
FIRST
lease.safe();
""",
                b"lease.safe()",
            ),
            "ordinary-forwarding": (
                """\
#define FORWARD(x) x
FORWARD(lease.safe)();
""",
                b"lease.safe()",
            ),
        }
        expected_oracles = {
            "raw-paste-suppression": ((b"safeX",), (b"nativeHandle",)),
            "inactive-forbidden": ((b"safe",), (b"nativeHandle", b"CAT")),
            "stringification": (
                (b'"lease.CAT(native,Handle)()"', b"safe"),
                (b"nativeHandle",),
            ),
            "empty-placemarker": ((b"safe",), (b"CAT",)),
            "benign-self-recursion": ((b"SELF", b"safe"), ()),
            "benign-mutual-recursion": ((b"FIRST", b"safe"), (b"SECOND",)),
            "ordinary-forwarding": ((b"safe",), (b"FORWARD",)),
        }
        expected_forbidden_oracles = {
            name: b"lease.nativeHandle()"
            for name in expected_forbidden
        }
        self.assertEqual(FORBIDDEN_FIXTURES, expected_forbidden)
        self.assertEqual(SAFE_FIXTURES, expected_safe)
        self.assertEqual(
            globals().get("FORBIDDEN_OUTPUT_ORACLES"),
            expected_forbidden_oracles,
        )
        self.assertEqual(
            globals().get("SAFE_OUTPUT_ORACLES"), expected_oracles
        )

    def test_suite_rejects_safe_fixture_missing_case_specific_output(self):
        view = SimpleNamespace(
            tokens=(SimpleNamespace(spelling=b"safe"),),
            configuration=SimpleNamespace(digest="cfg-live"),
        )
        with (
            mock.patch.dict(FORBIDDEN_FIXTURES, {}, clear=True),
            mock.patch.dict(
                SAFE_FIXTURES,
                {"stringification": ("lease.safe();\n", b"lease.safe()")},
                clear=True,
            ),
            mock.patch(
                __name__ + ".run_live_fixture", return_value=(view, [])
            ),
            self.assertRaisesRegex(
                AuditInfrastructureError, "safe output oracle"
            ),
        ):
            run_live_compiler_suite(Path(sys.executable), CompilerFamily.MSVC)

    def test_suite_rejects_forbidden_finding_with_wrong_configuration_digest(self):
        view = SimpleNamespace(
            configuration=SimpleNamespace(digest="cfg-live"),
        )
        finding = AggregatedFinding(
            Finding(
                FIXTURE_PATH,
                1,
                FORBIDDEN_EXPRESSION,
                FORBIDDEN_REASON,
            ),
            ("cfg-other",),
        )
        with (
            mock.patch.dict(
                FORBIDDEN_FIXTURES,
                {"nested-self": "lease.nativeHandle();\n"},
                clear=True,
            ),
            mock.patch.dict(SAFE_FIXTURES, {}, clear=True),
            mock.patch(
                __name__ + ".run_live_fixture",
                return_value=(view, [finding]),
            ),
            self.assertRaisesRegex(
                AuditInfrastructureError, "configuration digest"
            ),
        ):
            run_live_compiler_suite(Path(sys.executable), CompilerFamily.MSVC)

    def test_suite_shares_budget_across_fixtures_without_reset(self):
        now = [0.0]
        budget = capability_audit.LiveExecutionBudget.start(
            240.0, lambda: now[0]
        )
        seen_budgets = []

        def fixture_runner(
            _compiler, _family, source, _expected, *, budget=None
        ):
            seen_budgets.append(budget)
            now[0] += 130.0
            view = SimpleNamespace(
                configuration=SimpleNamespace(digest="cfg-live")
            )
            finding = AggregatedFinding(
                Finding(
                    FIXTURE_PATH,
                    _invocation_line(source),
                    FORBIDDEN_EXPRESSION,
                    FORBIDDEN_REASON,
                ),
                ("cfg-live",),
            )
            return view, [finding]

        with (
            mock.patch.dict(
                FORBIDDEN_FIXTURES,
                {
                    "first": "lease.nativeHandle();\n",
                    "second": "lease.nativeHandle();\n",
                },
                clear=True,
            ),
            mock.patch.dict(
                FORBIDDEN_OUTPUT_ORACLES,
                {
                    "first": b"lease.nativeHandle()",
                    "second": b"lease.nativeHandle()",
                },
                clear=True,
            ),
            mock.patch.dict(SAFE_FIXTURES, {}, clear=True),
            mock.patch(
                __name__ + ".run_live_fixture", side_effect=fixture_runner
            ),
            self.assertRaisesRegex(
                AuditInfrastructureError, "total execution deadline"
            ),
        ):
            run_live_compiler_suite(
                Path(sys.executable), CompilerFamily.MSVC, budget
            )
        self.assertEqual(seen_budgets, [budget, budget])

    def test_fixture_deducts_setup_and_probe_time_from_preprocess_budget(self):
        now = [10.0]
        budget = capability_audit.LiveExecutionBudget.start(
            240.0, lambda: now[0]
        )
        configuration = SimpleNamespace(
            family=CompilerFamily.MSVC,
            digest="cfg-live",
        )
        view = SimpleNamespace(configuration=configuration)
        coverage = object()

        def collect_after_probe(*_args):
            now[0] = 47.5
            return (configuration,)

        with (
            mock.patch(
                __name__ + ".collect_configurations",
                side_effect=collect_after_probe,
            ),
            mock.patch(
                __name__ + ".enumerate_production_identities",
                return_value={},
            ),
            mock.patch(__name__ + ".PreprocessCache", return_value=object()),
            mock.patch(
                __name__ + ".preprocess_all",
                return_value=((view,), coverage),
            ) as preprocess,
            mock.patch(
                __name__ + "._contains_token_sequence", return_value=True
            ),
            mock.patch.object(
                capability_audit, "load_production_sources", return_value={}
            ),
            mock.patch.object(
                capability_audit,
                "audit_pipeline",
                return_value=([], coverage),
            ),
        ):
            run_live_fixture(
                Path(sys.executable),
                CompilerFamily.MSVC,
                "lease.safe();\n",
                b"lease.safe()",
                budget=budget,
            )

        limits = preprocess.call_args.args[4]
        self.assertAlmostEqual(limits.total_seconds, 202.5)

    def test_fixture_expiry_during_setup_prevents_preprocess_start(self):
        now = [100.0]
        budget = capability_audit.LiveExecutionBudget.start(
            240.0, lambda: now[0]
        )
        configuration = SimpleNamespace(
            family=CompilerFamily.MSVC,
            digest="cfg-live",
        )
        view = SimpleNamespace(configuration=configuration)
        coverage = object()

        def collect_after_expiry(*_args):
            now[0] = 340.0
            return (configuration,)

        with (
            mock.patch(
                __name__ + ".collect_configurations",
                side_effect=collect_after_expiry,
            ),
            mock.patch(
                __name__ + ".enumerate_production_identities",
                return_value={},
            ),
            mock.patch(__name__ + ".PreprocessCache", return_value=object()),
            mock.patch(
                __name__ + ".preprocess_all",
                return_value=((view,), coverage),
            ) as preprocess,
            self.assertRaisesRegex(
                AuditInfrastructureError, "total execution deadline"
            ),
        ):
            run_live_fixture(
                Path(sys.executable),
                CompilerFamily.MSVC,
                "lease.safe();\n",
                b"lease.safe()",
                budget=budget,
            )
        preprocess.assert_not_called()

    def test_fixture_setup_oserror_is_normalized_to_infrastructure_failure(self):
        with (
            mock.patch.object(
                tempfile,
                "TemporaryDirectory",
                side_effect=OSError("fixture disk unavailable"),
            ),
            self.assertRaisesRegex(
                AuditInfrastructureError, "live fixture setup failed"
            ),
        ):
            run_live_fixture(
                Path(sys.executable),
                CompilerFamily.MSVC,
                "lease.safe();\n",
                b"lease.safe()",
            )

    def test_direct_unittest_reports_every_unavailable_family_explicitly(self):
        reporter = globals().get("_report_optional_compiler_absences")
        self.assertTrue(callable(reporter))
        if not callable(reporter):
            return
        messages = []
        reporter(
            ((CompilerFamily.GCC, Path(sys.executable)),),
            messages.append,
        )
        self.assertEqual(messages, [
            "SKIP: direct unittest optional live compiler family clang: "
            "no executable found",
            "SKIP: direct unittest optional live compiler family msvc: "
            "no executable found",
            "SKIP: direct unittest optional live compiler family clang-cl: "
            "no executable found",
        ])

    def test_safe_live_sources_are_also_safe_in_the_raw_lane(self):
        path = PurePosixPath("playback/gpu/live_fixture.cpp")
        for name, (source, _expected) in SAFE_FIXTURES.items():
            with self.subTest(name=name):
                self.assertEqual(
                    capability_audit.audit_raw_sources({
                        path: source
                    }),
                    [],
                )

    def test_gnu_live_paths_are_make_dependency_safe(self):
        path = Path("C:/temporary/live fixture.cpp")
        self.assertEqual(
            _live_command_path(path, CompilerFamily.GCC),
            "C:/temporary/live fixture.cpp",
        )
        self.assertNotIn(
            "\\", _live_command_path(path, CompilerFamily.CLANG)
        )

    def test_forward_slash_depfile_rebinds_to_source_identity(self):
        with tempfile.TemporaryDirectory(prefix="gpu-live-dep-") as temporary:
            root = Path(temporary).resolve()
            source = root / "playback" / "gpu" / "fixture.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("lease.safe();\n", encoding="utf-8")
            production = enumerate_production_identities(root)
            depfile = root / "fixture.d"
            depfile.write_text(
                f"fixture.o: {source.as_posix()}\n", encoding="utf-8"
            )
            parsed = parse_gcc_dependencies(depfile)
            rebound = validate_dependency_identities(
                parsed, root, production
            )
        self.assertEqual(
            rebound,
            (production[PurePosixPath("playback/gpu/fixture.cpp")],),
        )

    def test_forbidden_table_uses_locked_finding_shape_at_each_invocation(self):
        for name, source in FORBIDDEN_FIXTURES.items():
            invocation_line = _invocation_line(source)
            expanded = "\n" * (invocation_line - 1) + "lease.nativeHandle();\n"
            findings = capability_audit.audit_capability_uses(
                FIXTURE_PATH, expanded, compiler_view=True
            )
            with self.subTest(name=name):
                self.assertTrue(any(
                    item.path == FIXTURE_PATH
                    and item.line == invocation_line
                    and item.expression == FORBIDDEN_EXPRESSION
                    and item.reason == FORBIDDEN_REASON
                    for item in findings
                ))


class LiveCompilerParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.compilers = _optional_live_compilers()
        _report_optional_compiler_absences(
            cls.compilers,
            lambda message: print(message, file=sys.stderr),
        )
        cls.budget = capability_audit.LiveExecutionBudget.start()

    def test_forbidden_expansions_are_rejected_on_the_invocation_line(self):
        if not self.compilers:
            self.skipTest("optional live compiler absent: no supported executable found")
        runner = globals().get("run_live_fixture")
        self.assertTrue(callable(runner))
        for family, compiler in self.compilers:
            for name, source in FORBIDDEN_FIXTURES.items():
                with self.subTest(family=family.value, name=name):
                    _view, findings = runner(
                        compiler,
                        family,
                        source,
                        FORBIDDEN_OUTPUT_ORACLES[name],
                        budget=self.budget,
                    )
                    _require_forbidden_finding(name, source, _view, findings)

    def test_safe_expansions_emit_expected_tokens_without_findings(self):
        if not self.compilers:
            self.skipTest("optional live compiler absent: no supported executable found")
        runner = globals().get("run_live_fixture")
        self.assertTrue(callable(runner))
        for family, compiler in self.compilers:
            for name, (source, expected) in SAFE_FIXTURES.items():
                with self.subTest(family=family.value, name=name):
                    view, findings = runner(
                        compiler,
                        family,
                        source,
                        expected,
                        budget=self.budget,
                    )
                    _require_safe_output_oracle(name, view)
                    self.assertEqual(findings, [])
                    self.assertFalse(view.configuration.source.canonical.exists())


if __name__ == "__main__":
    unittest.main()
