# GPU Capability Compiler-Preprocessing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace compiler-macro emulation in the production GPU capability audit with bounded, cached, compiler-authoritative preprocessing while preserving raw and platform-inactive source checks.

**Architecture:** Decode and normalize compile commands into typed configurations, run the real configured compiler under hard bounds, parse one compact full-translation-unit token view with per-token provenance, and apply the existing capability grammar only to production-origin candidates. Keep direct/directive checks on every source file, retain a deliberately conservative bounded source-only macro lane for inactive files, and union findings across every supplied configuration.

**Tech Stack:** Python 3.11+, C++17 preprocessors (GCC 13.10/MinGW, Clang/AppleClang, MSVC `cl`, clang-cl), CMake compile databases, CTest, `unittest`, SHA-256 content-addressed cache, GitHub Actions.

## Global Constraints

- `GpuSurface::nativeHandle()` remains protected and `GpuReadLease` remains its only friend.
- `retainUntilFenceRetired()` and `pendingFenceValue()` remain public monotonic watermark operations.
- Native backing acquisition remains through `GpuSyncReadScope`; public synchronous `read()` remains narrowly audited and `withRead()` remains preferred.
- This plan modifies build/test tooling and documentation only. It adds no application call, allocation, lock, fence signal, GPU wait, startup work, or frame-path work.
- The compiler-preprocessed stream is authoritative for macros and conditionals only where a supplied compile command reaches the production code.
- Raw/directive checks run on every production file. Platform-inactive translation units and unreached production headers use the conservative source-only lane and never count as native compiler coverage.
- Audit every distinct semantic configuration and union findings. Never reject merely because macro tables differ across configurations.
- Supported compiler families are GCC/MinGW, Clang/AppleClang, MSVC `cl`, and clang-cl. A family is incomplete until a live smoke test proves rewriting, dependencies, markers, one forbidden expansion, and one safe paste.
- Response expansion limits are depth 8, 32 distinct files, and 4 MiB aggregate bytes; cycles and decoding ambiguity fail closed.
- Source-only macro limits remain depth 96, 256 continuous source tokens, 2,048 generated tokens, and 1,024 paste operations; exhaustion fails closed.
- Execution limits are 60 seconds and 128 MiB stdout per invocation, 1 MiB retained stderr, 240 seconds total cold execution, 512 MiB coordinator RSS, and `min(8, logical_cpu_count)` compiler processes.
- Cache limits are 512 MiB, 256 complete entries, one-hour incomplete-entry cleanup, and 14-day complete-entry retention.
- Acceptance limits are 180 seconds cold and 20 seconds unchanged warm on the supported Windows reference build, with at most 512 MiB coordinator RSS.
- Compiler failure, timeout, output overflow, dependency failure, unsupported command syntax, missing active coverage, ambiguous identity, malformed markers, and incomplete output are infrastructure failures; partial output never passes.
- Preserve the unrelated `tests/unit/tst_realcodecbenchmark.cpp` modification. Use targeted `git add <paths>`, never `git add -A`, and never stage `handoff-notes`.
- Do not touch `expectedDecodeSurfaceBytesForTrack` or add a warning workaround. Use `-DOLR_WERROR=OFF` only for the documented local GCC build.
- Do not run Windows test executables directly. Run CTest through `D:/Development/OpenLiveReplay/windows-runtime-stable/tools/run_ctest.py`.
- End every implementation commit with `Co-Authored-By: Claude <noreply@anthropic.com>`.
- After each task, request a fresh-context review of that task's diff and fix every Critical or Important finding before starting the next task.

---

## File Structure and Interfaces

- Create `tests/gpu/gpu_capability_model.py`: shared immutable types, hard limits, compiler-family enum, file identities, configurations, provenance tokens, compact translation-unit views, coverage reports, and `AuditInfrastructureError`.
- Create `tests/gpu/gpu_capability_command.py`: compile-entry decoding, launcher removal, response-file expansion, compiler identification/fingerprinting, and family-specific preprocessing command rewriting.
- Create `tests/gpu/gpu_capability_provenance.py`: canonical production/dependency identity, byte-stream marker parsing, inclusion-stack validation, dependency parsing, and compact full-TU token construction.
- Create `tests/gpu/gpu_capability_runner.py`: bounded subprocess/process-group execution, concurrency, compiler orchestration, and configuration coverage.
- Create `tests/gpu/gpu_capability_cache.py`: dependency-content validation, atomic cache publication, lookup, and bounded cleanup.
- Modify `tests/gpu/gpu_capability_source_audit.py`: retain capability policy/raw grammar, consume compact compiler views, filter by production provenance, aggregate configurations, keep only bounded source-local macro handling, and expose the final CLI.
- Create `tests/gpu/fixtures/fake_preprocessor.py`: deterministic subprocess fixture for success, failure, timeout, overflow, malformed markers, GCC dependency files, and MSVC dependency JSON.
- Create `tests/gpu/test_gpu_capability_model.py`: model, hard-limit, and path-classification tests.
- Create `tests/gpu/test_gpu_capability_command.py`: command/launcher/response/compiler/rewrite tests.
- Create `tests/gpu/test_gpu_capability_provenance.py`: canonical identity, marker, dependency, byte-token, and spoof-resistance tests.
- Create `tests/gpu/test_gpu_capability_runner.py`: subprocess bounds, process cleanup, orchestration, multi-config, and coverage tests.
- Create `tests/gpu/test_gpu_capability_cache.py`: key, dependency invalidation, corruption, atomic publication, and cleanup tests.
- Create `tests/gpu/test_gpu_capability_audit_lanes.py`: raw, compiler-authoritative, source-only, full-context, path/line, and finding-union tests.
- Create `tests/gpu/test_gpu_capability_live_compilers.py`: real GCC/Clang/MSVC/clang-cl parity smoke fixtures.
- Modify `tests/CMakeLists.txt`: register Python unit/live/audit/performance tests, pass the build-local cache, and raise only the compiler-authoritative audit timeout to 300 seconds.
- Modify `.github/workflows/ci.yml`: provide live GCC/Clang/MSVC/clang-cl evidence and retain native macOS Objective-C++ coverage.
- Modify `docs/build-and-run.md`: document direct Python diagnostics, controlled CTest commands, cache location, coverage summary, and live-family expectations.

Shared public Python interfaces are fixed as follows and must not be renamed between tasks:

```python
class CompilerFamily(enum.Enum):
    GCC = "gcc"
    CLANG = "clang"
    MSVC = "msvc"
    CLANG_CL = "clang-cl"

@dataclass(frozen=True)
class AuditLimits:
    response_depth: int = 8
    response_files: int = 32
    response_bytes: int = 4 * 1024 * 1024
    invocation_seconds: float = 60.0
    total_seconds: float = 240.0
    stdout_bytes: int = 128 * 1024 * 1024
    stderr_bytes: int = 1024 * 1024
    rss_bytes: int = 512 * 1024 * 1024
    workers: int = min(8, os.cpu_count() or 1)

@dataclass(frozen=True)
class FileIdentity:
    canonical: Path
    relative: PurePosixPath | None
    device: int | None
    inode: int | None
    line_count: int
    production: bool

@dataclass(frozen=True)
class PreprocessConfiguration:
    entry_id: str
    family: CompilerFamily
    compiler: Path
    working_directory: Path
    source: FileIdentity
    arguments: tuple[str, ...]
    environment_digest: str
    digest: str

@dataclass(frozen=True)
class SourceLocation:
    identity: FileIdentity | None
    inclusion_instance: int
    line: int
    configuration_digest: str

@dataclass(frozen=True)
class PreprocessedToken:
    spelling: bytes
    location: SourceLocation

@dataclass(frozen=True)
class PreprocessedTranslationUnitView:
    configuration: PreprocessConfiguration
    tokens: tuple[PreprocessedToken, ...]
    dependencies: tuple[FileIdentity, ...]

@dataclass(frozen=True)
class CoverageReport:
    authoritative: frozenset[PurePosixPath]
    source_only: frozenset[PurePosixPath]
    configurations: tuple[str, ...]

class AuditInfrastructureError(RuntimeError):
    pass
```

---

### Task 1: Lock the typed preprocessing model and coverage classifier

**Files:**
- Create: `tests/gpu/gpu_capability_model.py`
- Create: `tests/gpu/test_gpu_capability_model.py`

**Interfaces:**
- Produces: every shared type in the File Structure section; `enumerate_production_identities(root: Path) -> dict[PurePosixPath, FileIdentity]`; `requires_compile_entry(path: PurePosixPath, configured_families: frozenset[CompilerFamily], has_objcpp: bool, has_windows_backend: bool) -> bool`.
- Consumes: repository roots `playback/` and `recorder_engine/` and the existing platform-classification behavior.

- [ ] **Step 1: Write failing immutable-model and classifier tests**

```python
class ModelTests(unittest.TestCase):
    def test_limits_are_exact(self):
        limits = AuditLimits()
        self.assertEqual((limits.response_depth, limits.response_files), (8, 32))
        self.assertEqual(limits.stdout_bytes, 128 * 1024 * 1024)
        self.assertEqual(limits.workers, min(8, os.cpu_count() or 1))

    def test_platform_classifier_is_path_based(self):
        families = frozenset({CompilerFamily.GCC})
        self.assertTrue(requires_compile_entry(
            PurePosixPath("playback/playbackworker.cpp"), families, False, False))
        self.assertFalse(requires_compile_entry(
            PurePosixPath("playback/gpu/gpurhicontext_apple.mm"), families, False, False))
        self.assertFalse(requires_compile_entry(
            PurePosixPath("playback/output/win/wingpuimportedge.cpp"), families, False, False))
        self.assertTrue(requires_compile_entry(
            PurePosixPath("playback/output/win/wingpuimportedge.cpp"), families, False, True))
```

- [ ] **Step 2: Run the model tests and prove the module is absent**

Run: `python -m unittest tests/gpu/test_gpu_capability_model.py -v`

Expected: FAIL with `ModuleNotFoundError: No module named 'gpu_capability_model'`.

- [ ] **Step 3: Implement the exact immutable types and canonical enumeration**

Implement the signatures above. `enumerate_production_identities()` must reject a resolved path outside the root, a symlink/reparse crossing, duplicate filesystem identity, case-fold collision, non-regular file, or unreadable UTF-8 production file by raising `AuditInfrastructureError` with the repository-relative path.

- [ ] **Step 4: Run the model tests**

Run: `python -m unittest tests/gpu/test_gpu_capability_model.py -v`

Expected: PASS for exact limits, frozen dataclasses, generic/Apple/Windows classification, alias rejection, case collision, and physical line counts.

- [ ] **Step 5: Commit the model**

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/test_gpu_capability_model.py
git commit -m "test(gpu): define capability preprocessing model" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Decode compile commands, launchers, compilers, and response files

**Files:**
- Create: `tests/gpu/gpu_capability_command.py`
- Create: `tests/gpu/test_gpu_capability_command.py`

**Interfaces:**
- Consumes: `AuditLimits`, `AuditInfrastructureError`, `CompilerFamily`, `FileIdentity`, and `PreprocessConfiguration` from Task 1.
- Produces: `decode_compile_entry(entry: Mapping[str, object], database: Path, windows: bool) -> tuple[Path, tuple[str, ...]]`; `strip_launchers(arguments: tuple[str, ...]) -> tuple[Path, tuple[str, ...], Mapping[str, str]]`; `identify_compiler(path: Path, version_output: bytes) -> CompilerFamily`; `expand_response_files(arguments: tuple[str, ...], family: CompilerFamily, cwd: Path, limits: AuditLimits) -> tuple[str, ...]`; `make_configuration(entry: Mapping[str, object], database: Path, entry_index: int, source_root: Path, production: Mapping[PurePosixPath, FileIdentity], environment: Mapping[str, str], limits: AuditLimits) -> PreprocessConfiguration`.

- [ ] **Step 1: Write failing command and launcher tests**

```python
def test_ccache_assignments_and_options_are_stripped(self):
    compiler, arguments, assignments = strip_launchers((
        "ccache", "compiler_check=content", "--config-path", "cache.conf",
        "C:/Qt/Tools/mingw1310_64/bin/g++.exe", "-c", "playback/gpu/file.cpp"))
    self.assertEqual(compiler.name, "g++.exe")
    self.assertEqual(arguments[-2:], ("-c", "playback/gpu/file.cpp"))
    self.assertEqual(assignments, {"compiler_check": "content"})

def test_unknown_launcher_option_fails(self):
    with self.assertRaisesRegex(AuditInfrastructureError, "unsupported ccache option"):
        strip_launchers(("ccache", "--mystery", "g++", "file.cpp"))
```

Add exact controls for POSIX quoting, `CommandLineToArgvW` quoting, shell operators, empty commands, source mismatch, stdin, multiple sources, sccache/distcc/icecc chains, compiler-family verification, and contradictory version output.

- [ ] **Step 2: Run command tests and prove decoding is missing**

Run: `python -m unittest tests/gpu/test_gpu_capability_command.py -v`

Expected: FAIL on missing command functions.

- [ ] **Step 3: Implement command decoding and launcher removal**

Decode without a shell. Prefer structured `arguments`. On Windows call `CommandLineToArgvW`; on POSIX use `shlex.split(command, posix=True)`. Reject `|`, `||`, `&&`, `;`, redirection, command substitution, empty commands, stdin source, and multiple/mismatched source inputs. Remove only the four supported launchers and documented options; unknown wrappers and options raise `AuditInfrastructureError` naming the token.

- [ ] **Step 4: Add failing response-file bounds and dialect tests**

```python
def test_nested_gcc_response_expands_in_order(self):
    self.write("inner.rsp", '-DNAME="two words" source.cpp')
    self.write("outer.rsp", '-I"SDK Path/include" @inner.rsp')
    expanded = expand_response_files(("@outer.rsp",), CompilerFamily.GCC,
                                     self.root, AuditLimits())
    self.assertEqual(expanded, ("-ISDK Path/include", "-DNAME=two words", "source.cpp"))

def test_response_cycle_fails(self):
    self.write("a.rsp", "@b.rsp")
    self.write("b.rsp", "@a.rsp")
    with self.assertRaisesRegex(AuditInfrastructureError, "response-file cycle"):
        expand_response_files(("@a.rsp",), CompilerFamily.GCC,
                              self.root, AuditLimits())
```

Add UTF-8 BOM, UTF-16LE BOM, MSVC quoting, depth 9, file count 33, aggregate size over 4 MiB, unreadable, invalid encoding, and canonical alias controls.

- [ ] **Step 5: Implement bounded family-specific response expansion and compiler fingerprints**

Expand before source validation or flag rewriting. Hash the canonical compiler executable contents, size, modification metadata, and normalized version output bounded to 5 seconds and 1 MiB. Hash the complete effective environment into `environment_digest` without serializing environment values. Derive `entry_id` from database path and index and derive `digest` from normalized semantic inputs.

- [ ] **Step 6: Run all command tests**

Run: `python -m unittest tests/gpu/test_gpu_capability_command.py -v`

Expected: PASS for every command, launcher, compiler, response, bound, and rejection case.

- [ ] **Step 7: Commit command normalization**

```powershell
git add tests/gpu/gpu_capability_command.py tests/gpu/test_gpu_capability_command.py
git commit -m "test(gpu): normalize capability compiler commands" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Rewrite preprocessing commands for all four compiler families

**Files:**
- Modify: `tests/gpu/gpu_capability_command.py`
- Modify: `tests/gpu/test_gpu_capability_command.py`

**Interfaces:**
- Consumes: normalized expanded arguments and compiler family from Task 2.
- Produces: the exact immutable command record and rewriter below.

```python
@dataclass(frozen=True)
class RewrittenCommand:
    arguments: tuple[str, ...]  # canonical compiler path is element zero
    dependency_output: Path
    dependency_format: str  # exactly "gcc-depfile" or "msvc-json"
```

`rewrite_preprocess_command(configuration: PreprocessConfiguration, dependency_output: Path) -> RewrittenCommand` is the only public rewriter.

- [ ] **Step 1: Add failing GCC/Clang rewrite-table tests**

```python
def test_gcc_rewrite_preserves_semantics_and_replaces_outputs(self):
    rewritten = self.rewrite(CompilerFamily.GCC, (
        "-std=gnu++17", "-DOLR_GPU=1", "-Iinc", "-include", "forced.h",
        "--sysroot=C:/sdk", "-c", "file.cpp", "-o", "file.obj",
        "-MMD", "-MFdep.d", "-MT", "old"))
    self.assertEqual(rewritten.arguments.count("file.cpp"), 1)
    self.assertIn("-E", rewritten.arguments)
    self.assertNotIn("-P", rewritten.arguments)
    self.assertIn("-MD", rewritten.arguments)
    self.assertIn("-MF", rewritten.arguments)
    self.assertNotIn("file.obj", rewritten.arguments)
    self.assertIn("--sysroot=C:/sdk", rewritten.arguments)
```

Cover attached `-ofoo`, `-MFfoo`, `-MTfoo`, `-MQfoo`, `-MJfoo`, `-MD`/`-MMD`, forced includes, framework paths, target/architecture, PCH/module inputs, marker suppression, and a second source.

- [ ] **Step 2: Run the GCC/Clang rewrite tests and verify RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_command.CommandRewriteTests.test_gcc_rewrite_preserves_semantics_and_replaces_outputs -v`

Expected: FAIL because `rewrite_preprocess_command` is absent.

- [ ] **Step 3: Implement table-driven GCC and Clang rewriting**

Remove compile/dependency/object/diagnostic output arguments in paired and attached forms. Preserve all preprocessing-semantic arguments byte-for-byte. Add `-E -MD -MF <private-path>` after removals and retain the original source exactly once. Reject unknown output/source-selection options rather than passing them through.

- [ ] **Step 4: Add failing MSVC/clang-cl rewrite tests**

```python
def test_clang_cl_rewrite_removes_hidden_outputs(self):
    rewritten = self.rewrite(CompilerFamily.CLANG_CL, (
        "/std:c++17", "/DOLR_GPU=1", "/I", "SDK Path",
        "/FIforced.h", "/c", "file.cpp", "/Foout.obj", "/Fdstate.pdb"))
    self.assertIn("/E", rewritten.arguments)
    self.assertIn("/sourceDependencies", rewritten.arguments)
    self.assertNotIn("/P", rewritten.arguments)
    self.assertFalse(any(value.lower().startswith(("/fo", "/fd", "/fe"))
                         for value in rewritten.arguments))
```

Cover paired and attached `/Fo`, `/Fe`, `/Fd`, `/showIncludes`, existing `/sourceDependencies`, `/P`, `/EP`, PCH/module flags, `/external:I`, `/FI`, `/clang:-o`, `/clang:-MF`, `/clang:-P`, and multiple source inputs.

- [ ] **Step 5: Implement MSVC and clang-cl rewriting**

Preserve slash-style preprocessing semantics, strip compile/output/dependency-report options, inspect every `/clang:` payload, and add `/nologo /E /sourceDependencies <private-json>`. Reject a hidden GNU output/marker/source option or unsupported output-bearing slash option.

- [ ] **Step 6: Run the complete rewrite matrix**

Run: `python -m unittest tests/gpu/test_gpu_capability_command.py -v`

Expected: PASS for exact GCC, Clang, cl, and clang-cl argument lists and all fail-closed controls.

- [ ] **Step 7: Commit family rewriting**

```powershell
git add tests/gpu/gpu_capability_command.py tests/gpu/test_gpu_capability_command.py
git commit -m "test(gpu): rewrite capability preprocess commands" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Build canonical full-TU provenance from line markers

**Files:**
- Create: `tests/gpu/gpu_capability_provenance.py`
- Create: `tests/gpu/test_gpu_capability_provenance.py`

**Interfaces:**
- Consumes: `FileIdentity`, `PreprocessConfiguration`, `PreprocessedToken`, `PreprocessedTranslationUnitView`, and `AuditInfrastructureError`.
- Produces: dependency parsers, spoof rejection, and the exact streaming interface below.

`PreprocessedStreamBuilder(configuration: PreprocessConfiguration, production: Mapping[PurePosixPath, FileIdentity])` exposes `feed(chunk: bytes) -> None` and `finalize(dependencies: tuple[FileIdentity, ...]) -> PreprocessedTranslationUnitView`. The remaining exact functions are `parse_gcc_dependencies(path: Path) -> tuple[Path, ...]`, `parse_msvc_dependencies(path: Path) -> tuple[Path, ...]`, `validate_dependency_identities(paths: tuple[Path, ...], source_root: Path, production: Mapping[PurePosixPath, FileIdentity]) -> tuple[FileIdentity, ...]`, and `reject_source_line_spoofs(path: Path, production: Mapping[PurePosixPath, FileIdentity]) -> None`.

- [ ] **Step 1: Write failing GCC include-stack and full-context tests**

```python
def test_gcc_view_keeps_nonproduction_context_and_production_locations(self):
    stream = (
        b'# 1 "C:/sdk/wrapper.h" 1\nnamespace sdk {\n'
        b'# 7 "D:/repo/playback/gpu/adapter.h" 1\n'
        b'lease.nativeHandle();\n'
        b'# 3 "C:/sdk/wrapper.h" 2\n}\n')
    view = self.parse_gcc(stream)
    self.assertEqual(b"".join(token.spelling for token in view.tokens),
                     b"namespacesdk{lease.nativeHandle();}")
    native = next(token for token in view.tokens if token.spelling == b"nativeHandle")
    self.assertEqual(native.location.identity.relative,
                     PurePosixPath("playback/gpu/adapter.h"))
    self.assertEqual(native.location.line, 7)
```

Add balanced nested/repeated includes, flags 1-4, ancestor return, sibling transition, bootstrap `<built-in>`/`<command-line>`/`<command line>`, post-code pseudo-file, unflagged cross-file transition, non-dependency marker, and truncated-stack tests.

- [ ] **Step 2: Run provenance tests and verify RED**

Run: `python -m unittest tests/gpu/test_gpu_capability_provenance.py -v`

Expected: FAIL because provenance parsing is absent.

- [ ] **Step 3: Implement byte-stream GCC marker parsing and compact tokens**

Parse marker syntax from bytes, C-unescape filenames, resolve relative markers against working directory then current real-file directory, and require exactly one dependency identity. Keep all code tokens but attach production or non-production location. Intern spellings; keep non-ASCII identifiers as opaque byte tokens. Reject line zero outside bootstrap, `<stdin>`, unknown pseudo-files, invalid bytes, out-of-range production lines, and stack inconsistencies.

- [ ] **Step 4: Add failing MSVC transition and path tests**

```python
def test_msvc_repeated_header_gets_distinct_instances(self):
    view = self.parse_msvc(
        b'#line 1 "D:\\\\repo\\playback\\a.cpp"\n'
        b'#line 1 "D:\\\\repo\\playback\\h.h"\nint one;\n'
        b'#line 2 "D:\\\\repo\\playback\\a.cpp"\n'
        b'#line 1 "D:\\\\repo\\playback\\h.h"\nint two;\n')
    instances = {token.location.inclusion_instance for token in view.tokens
                 if token.location.identity and
                    token.location.identity.relative == PurePosixPath("playback/h.h")}
    self.assertEqual(len(instances), 2)
```

Add Windows case/separator normalization, ancestor return, recursive/return ambiguity, malformed quote, embedded NUL, active-code-page decode failure, and absolute/relative collision tests.

- [ ] **Step 5: Implement MSVC stack inference and dependency parsing**

Treat a marker to the current identity as line advance, a new dependency as a child inclusion, and an unambiguous ancestor as return. Reject a target that can be interpreted both ways. Parse GCC escaped depfiles and MSVC dependency JSON with canonical deduplication, including system headers.

- [ ] **Step 6: Add and implement raw `#line` spoof controls**

Test production direct and spliced `#line`, and non-production dependency directives targeting absolute, relative, case-variant, and separator-variant production paths. `reject_source_line_spoofs()` must reject each target while allowing an unrelated generated-file target.

- [ ] **Step 7: Run provenance tests and commit**

Run: `python -m unittest tests/gpu/test_gpu_capability_provenance.py -v`

Expected: PASS for every identity, dependency, marker, byte-token, line, inclusion-instance, and spoof control.

```powershell
git add tests/gpu/gpu_capability_provenance.py tests/gpu/test_gpu_capability_provenance.py
git commit -m "test(gpu): validate capability token provenance" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: Execute preprocessors under hard process and output bounds

**Files:**
- Create: `tests/gpu/gpu_capability_runner.py`
- Create: `tests/gpu/fixtures/fake_preprocessor.py`
- Create: `tests/gpu/test_gpu_capability_runner.py`

**Interfaces:**
- Consumes: rewritten commands from Task 3, provenance parser from Task 4, and exact `AuditLimits`.
- Produces: `ExecutionResult(stderr_tail: bytes, elapsed_seconds: float, observed_stdout_bytes: int)`; `run_bounded_preprocessor(command: RewrittenCommand, configuration: PreprocessConfiguration, limits: AuditLimits, deadline: float, consume_stdout: Callable[[bytes], None]) -> ExecutionResult`; `preprocess_configuration(configuration: PreprocessConfiguration, production: Mapping[PurePosixPath, FileIdentity], limits: AuditLimits, deadline: float) -> PreprocessedTranslationUnitView`.

- [ ] **Step 1: Create a deterministic fake preprocessor and failing success/failure tests**

The fixture accepts `--fixture-mode success|fail|sleep|overflow|malformed|child-sleep`, emits a valid GCC or MSVC marker stream, writes the requested dependency format, and flushes output in 4 KiB chunks. Tests assert success returns bounded chunks and nonzero exit includes only the final 1 MiB stderr tail.

```python
def test_nonzero_exit_never_returns_partial_view(self):
    with self.assertRaisesRegex(AuditInfrastructureError, "exit=9"):
        self.run_fixture("fail")

def test_timeout_terminates_process_group(self):
    started = time.monotonic()
    with self.assertRaisesRegex(AuditInfrastructureError, "timeout"):
        self.run_fixture("child-sleep", invocation_seconds=0.25)
    self.assertLess(time.monotonic() - started, 2.0)
    self.assertFalse(self.fixture_child_is_alive())
```

- [ ] **Step 2: Run runner tests and verify RED**

Run: `python -m unittest tests/gpu/test_gpu_capability_runner.py -v`

Expected: FAIL because the bounded runner is absent.

- [ ] **Step 3: Implement process-group execution and streaming bounds**

Use `start_new_session=True` plus group termination on POSIX and a kill-on-close Job Object/process group on Windows. Feed each stdout chunk directly to `PreprocessedStreamBuilder.feed()`; never accumulate raw stdout chunks. Retain only the bounded stderr tail, sample coordinator RSS, and check invocation/global deadlines and output bytes after every chunk. Terminate and reap on every error path. Do not call `finalize()` or audit a partial stream.

- [ ] **Step 4: Add exact output/RSS/dependency/decode failure tests**

Cover stdout exactly at and one byte over the configured test limit, stderr truncation, malformed UTF/path bytes, missing depfile, malformed dependency JSON, dependency outside the manifest, 60-second/default and injected-short deadline selection, 240-second global deadline, and injected RSS overflow.

- [ ] **Step 5: Connect successful execution to dependency and provenance validation**

`preprocess_configuration()` must require successful exit and complete dependency output before calling `PreprocessedStreamBuilder.finalize()`. Its error includes configuration digest, family, canonical source, exit status, elapsed time, observed bytes, and escaped stderr tail.

- [ ] **Step 6: Run runner/provenance tests and commit**

Run: `python -m unittest tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_provenance.py -v`

Expected: PASS with no surviving fixture child process.

```powershell
git add tests/gpu/gpu_capability_runner.py tests/gpu/fixtures/fake_preprocessor.py tests/gpu/test_gpu_capability_runner.py
git commit -m "test(gpu): bound capability preprocessing" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: Add dependency-validated atomic caching and cleanup

**Files:**
- Create: `tests/gpu/gpu_capability_cache.py`
- Create: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`

**Interfaces:**
- Consumes: `PreprocessConfiguration`, `PreprocessedTranslationUnitView`, dependency identities, and runner callback.
- Produces: `PreprocessCache(root: Path, maximum_bytes: int = 512 * 1024 * 1024, maximum_entries: int = 256)` with `load(configuration: PreprocessConfiguration) -> PreprocessedTranslationUnitView | None`, `publish(view: PreprocessedTranslationUnitView) -> None`, and `cleanup(now: float) -> None`; runner-level `load_or_preprocess(configuration: PreprocessConfiguration, production: Mapping[PurePosixPath, FileIdentity], cache: PreprocessCache, limits: AuditLimits, deadline: float) -> PreprocessedTranslationUnitView`.

- [ ] **Step 1: Write failing cache hit and invalidation tests**

```python
def test_hit_requires_every_dependency_content_hash(self):
    cache.publish(self.view(dependencies=(self.main, self.header)))
    self.assertIsNotNone(cache.load(self.configuration))
    self.header.write_text("changed", encoding="utf-8")
    self.assertIsNone(cache.load(self.configuration))

def test_partial_entry_is_never_a_hit(self):
    self.write_incomplete_manifest()
    self.assertIsNone(cache.load(self.configuration))
```

Add compiler fingerprint, arguments, working directory, environment digest, missing dependency, alias, permission error, malformed manifest, schema mismatch, payload hash, and concurrent winner tests.

- [ ] **Step 2: Run cache tests and verify RED**

Run: `python -m unittest tests/gpu/test_gpu_capability_cache.py -v`

Expected: FAIL because the cache is absent.

- [ ] **Step 3: Implement content-keyed load and atomic publication**

Use a schema-versioned base key from the configuration. Persist no environment values. Store canonical dependency paths and SHA-256 hashes, compact tokens/provenance, and payload digest. Write into a unique temporary directory, fsync files and manifest, then atomically rename. Validate all dependencies before deserializing a hit. Record access time in an atomic sidecar outside the immutable payload so LRU reads cannot corrupt an entry. Cache no failure or partial output.

- [ ] **Step 4: Add failing cleanup and bounded-size tests**

Test incomplete entries older/newer than one hour, complete entries older/newer than 14 days, LRU eviction to both 512 MiB and 256 entries using injected small limits, link refusal, active temp publication, and cleanup idempotence.

- [ ] **Step 5: Implement cleanup and runner integration**

Cleanup never follows links and never touches source paths. `load_or_preprocess()` returns a validated hit or runs the compiler and atomically publishes a success. Hash each canonical dependency once per audit process and share that digest across configurations.

- [ ] **Step 6: Run cache and runner tests and commit**

Run: `python -m unittest tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_runner.py -v`

Expected: PASS for hit/miss, corruption, concurrent publication, cleanup, runner reuse, and failure non-caching.

```powershell
git add tests/gpu/gpu_capability_cache.py tests/gpu/test_gpu_capability_cache.py tests/gpu/gpu_capability_runner.py tests/gpu/test_gpu_capability_runner.py
git commit -m "test(gpu): cache compiler capability views" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: Orchestrate every configuration and enforce coverage

**Files:**
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`

**Interfaces:**
- Consumes: compile databases, command normalization, cache, bounded runner, production identities, and platform classifier.
- Produces: `collect_configurations(root: Path, databases: tuple[Path, ...], environment: Mapping[str, str]) -> tuple[PreprocessConfiguration, ...]`; `preprocess_all(configurations: tuple[PreprocessConfiguration, ...], production: Mapping[PurePosixPath, FileIdentity], cache: PreprocessCache, limits: AuditLimits) -> tuple[tuple[PreprocessedTranslationUnitView, ...], CoverageReport]`.

- [ ] **Step 1: Add failing duplicate/multi-config/coverage tests**

```python
def test_distinct_defines_are_both_audited(self):
    configurations = self.collect(entries=(
        self.entry("file.cpp", defines=("MODE=1",)),
        self.entry("file.cpp", defines=("MODE=2",)),
        self.entry("file.cpp", defines=("MODE=1",)),
    ))
    self.assertEqual(len(configurations), 2)
    self.assertNotEqual(configurations[0].digest, configurations[1].digest)

def test_missing_active_generic_source_fails(self):
    with self.assertRaisesRegex(AuditInfrastructureError,
                                "active source has no compile command"):
        self.preprocess_with_missing("playback/missing.cpp")
```

Add database-array validation, relative working directory/file, source outside root, generic/Apple/Windows classification, header authoritative/source-only coverage, and differing compiler/configuration union tests.

- [ ] **Step 2: Run orchestration tests and verify RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.OrchestrationTests -v`

Expected: FAIL because collection/orchestration is absent.

- [ ] **Step 3: Implement deterministic collection and bounded concurrency**

Sort databases and entries deterministically, coalesce only equal semantic digests, and run at most `AuditLimits.workers`. Preserve all different definitions/configurations. Stop scheduling after infrastructure failure, terminate active compiler groups, collect all completed diagnostics, and raise one deterministic failure report.

- [ ] **Step 4: Implement authoritative/source-only coverage accounting**

Require every active main source. Mark each production identity reached by a validated compiler view authoritative. Put inactive main sources and unreached headers into `CoverageReport.source_only`. Include every configuration digest. Never convert an active missing source into source-only fallback.

- [ ] **Step 5: Run runner, cache, and model tests and commit**

Run: `python -m unittest tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_model.py -v`

Expected: PASS with deterministic ordering under worker counts 1 and 4 and no compiler-table-difference rejection.

```powershell
git add tests/gpu/gpu_capability_runner.py tests/gpu/test_gpu_capability_runner.py
git commit -m "test(gpu): enforce compiler capability coverage" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 8: Refactor capability grammar onto full-TU provenance tokens

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Create: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:**
- Consumes: `PreprocessedTranslationUnitView`, `CoverageReport`, and existing `Finding` capability policy.
- Produces: `AuditBuffer.from_preprocessed(view) -> AuditBuffer`; `AuditBuffer.location_at(offset: int) -> SourceLocation`; `audit_preprocessed_view(view: PreprocessedTranslationUnitView) -> list[Finding]`; `aggregate_findings(findings: Iterable[tuple[Finding, str]]) -> list[AggregatedFinding]`, where `AggregatedFinding` contains one `Finding` and sorted unique `configurations: tuple[str, ...]`.

- [ ] **Step 1: Add failing full-context and production-origin tests**

```python
def test_nonproduction_scope_context_is_kept_but_not_reported(self):
    view = self.view((
        self.external(b"void sdk(nativeHandle); namespace outer {"),
        self.production("playback/gpu/gpufence.h", 41,
                        b"lease.nativeHandle();"),
        self.external(b"}"),
    ))
    findings = audit_preprocessed_view(view)
    self.assertEqual([(item.path, item.line) for item in findings], [
        (PurePosixPath("playback/gpu/gpufence.h"), 41)])

def test_external_guarded_spelling_cannot_report(self):
    self.assertEqual(audit_preprocessed_view(
        self.view((self.external(b"surface.nativeHandle();"),))), [])
```

Add repeated-header instance, scope spanning an include, callback body, `withRead`, `complete`, longjmp, typed sink/method, public member, and mixed-provenance rejection controls.

- [ ] **Step 2: Run lane tests and verify RED**

Run: `python -m unittest tests/gpu/test_gpu_capability_audit_lanes.py -v`

Expected: FAIL because compiler-view auditing is absent.

- [ ] **Step 3: Implement `AuditBuffer` and provenance-aware candidate filtering**

Build normalized audit text from compact tokens while preserving delimiters and token boundaries. Store run-length offset-to-`SourceLocation` mapping. Refactor finding construction to resolve the candidate token's path/line dynamically instead of using one path for the whole translation unit. Keep non-production tokens for grammar context but require reportable candidate tokens to have a production identity. Reject a capability expression whose required tokens have inconsistent production identity.

- [ ] **Step 4: Refactor every existing capability grammar to use candidate provenance**

Cover direct `nativeHandle`, scope/read/complete lifecycle, `withRead` callback selection and non-local control, typed native sinks/methods/types, receiver/binding resolution, declaration allowlists, and public registry/op-scope checks. Select path-specific reviewed names from the candidate's canonical path. Preserve existing raw-source mutation behavior.

- [ ] **Step 5: Add failing multi-configuration aggregation tests**

```python
def test_union_reports_all_forbidden_configurations_once(self):
    aggregated = aggregate_findings((
        (self.finding(line=17), "cfg-b"),
        (self.finding(line=17), "cfg-a"),
        (self.finding(line=17), "cfg-a"),
    ))
    self.assertEqual(len(aggregated), 1)
    self.assertEqual(aggregated[0].configurations, ("cfg-a", "cfg-b"))
```

- [ ] **Step 6: Run lane and existing mutation tests**

Run: `python -m unittest tests/gpu/test_gpu_capability_audit_lanes.py -v`

Run: `python -c "import sys; sys.path.insert(0, 'tests/gpu'); import gpu_capability_source_audit as audit; audit.mutation_self_tests(); print('mutation PASS')"`

Expected: both PASS; every existing direct/scope/callback/control mutation remains killed.

- [ ] **Step 7: Commit provenance grammar support**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "refactor(gpu): audit compiler-expanded capability tokens" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 9: Separate raw, authoritative, and conservative source-only macro lanes

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:**
- Consumes: compiler views and coverage from Tasks 7-8.
- Produces: `audit_raw_sources(sources: Mapping[PurePosixPath, str]) -> list[Finding]`; `audit_source_only(path: PurePosixPath, source: str) -> list[Finding]`; `audit_pipeline(sources, views, coverage) -> tuple[list[AggregatedFinding], CoverageReport]`.

- [ ] **Step 1: Add failing raw-lane safe macro and direct-source tests**

```python
def test_compiler_covered_macro_definition_is_not_rejected_raw(self):
    source = "#define X nativeHandle\n#define CAT(a,b) a##b\nlease.CAT(safe,X)();\n"
    self.assertEqual(audit_raw_sources({self.path: source}), [])

def test_direct_source_access_remains_rejected(self):
    findings = audit_raw_sources({self.path: "surface.nativeHandle();\n"})
    self.assertEqual(findings[0].line, 1)
```

Add public declaration, physical splice, malformed directive, direct `#line`, spliced `#line`, registerRetire/track, and inactive-direct-spelling controls.

- [ ] **Step 2: Run raw-lane tests and verify RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_audit_lanes.RawLaneTests -v`

Expected: FAIL because raw macro replacement bodies still enter authoritative macro checks.

- [ ] **Step 3: Implement the raw/directive responsibility boundary**

Mask macro replacement bodies for guarded direct-spelling and macro-composition decisions in compiler-covered code. Continue scanning directive syntax, physical reconstruction, public declarations, direct C++ code in all branches, and public registerRetire/track surfaces. Do not reject generic `#`, `##`, variadics, or guarded fragments solely for existing in a macro definition.

- [ ] **Step 4: Add failing source-only conservative controls**

Test source-local reconstruction, malformed expansion, exact depth 96/97, token 2048/2049, paste 1024/1025, unknown macro in receiver/member/callback/completion/control-flow positions, and unrelated macro use. The first member of each bound pair passes and the second produces a named source-only complexity finding.

- [ ] **Step 5: Implement bounded source-only analysis and remove authoritative macro dumps**

Retain a small source-local checker only for paths in `CoverageReport.source_only`. It scans every conditional branch and rejects ambiguity in the five capability-sensitive contexts. Delete `load_compiler_macro_tables`, `compiler_macro_definitions`, `compiler_macro_findings`, compiler-table final-state merging, and authoritative calls to `guarded_macro_composition_findings`. Keep no `-dM` command path.

- [ ] **Step 6: Run raw/source-only/full mutation tests and commit**

Run: `python -m unittest tests/gpu/test_gpu_capability_audit_lanes.py -v`

Run: `python -c "import sys; sys.path.insert(0, 'tests/gpu'); import gpu_capability_source_audit as audit; audit.mutation_self_tests(); print('mutation PASS')"`

Expected: PASS; safe paste is not rejected raw, source-only ambiguity fails closed, and every existing policy mutation remains killed.

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "refactor(gpu): separate capability audit lanes" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10: Lock every prior macro bypass and safe control against real compilers

**Files:**
- Create: `tests/gpu/test_gpu_capability_live_compilers.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify: `tests/gpu/gpu_capability_source_audit.py`

**Interfaces:**
- Consumes: full pipeline from Tasks 1-9 and an explicitly selected live compiler command.
- Produces: `run_live_fixture(compiler: Path, family: CompilerFamily, source: str, expected_tokens: bytes) -> tuple[PreprocessedTranslationUnitView, list[AggregatedFinding]]`; CLI options `--live-only`, `--live-compiler FAMILY=PATH` (repeatable), and `--require-live-family FAMILY` (repeatable).

- [ ] **Step 1: Add the exact forbidden compiler-parity table**

Create real source fixtures for all of these cases and assert the compiler emits `lease.nativeHandle()` and the audit rejects the original invocation line:

```text
nested-self: F(x)->x; A->PASTE; PASTE(a,b)->PASTE_I(a,b); PASTE_I(a,b)->a##b;
             lease.F(F(A))(native,Handle)()
original-tail: F(x)->F_I(x); F_I(x)->F_##x; F_call->PASTE; F_native->native;
               PASTE(a,b)->PASTE_I(a,b); PASTE_I(a,b)->a##b;
               lease.F(call)(F(native),Handle)()
object-tail: OPEN->PASTE(; PASTE(a,b)->PASTE_I(a,b); PASTE_I(a,b)->a##b;
             lease.OPEN native,Handle)()
va-opt: CAT(a,...)->a ## __VA_OPT__(__VA_ARGS__); lease.CAT(native,Handle)()
inactive-undef: included CAT(a,b)->a##b; #if 0 / #undef CAT / #endif;
                lease.CAT(native,Handle)()
late-undef: included CAT(a,b)->a##b; lease.CAT(native,Handle)(); #undef CAT
```

- [ ] **Step 2: Add the exact safe compiler-parity table**

Assert compiler output and zero audit findings for raw paste suppression `CAT(a,b)->a##b; X->nativeHandle; lease.CAT(safe,X)()`, inactive forbidden branch, stringification, empty variadic placemarker, benign self/mutual recursion, and ordinary non-paste forwarding.

- [ ] **Step 3: Run the live test on GCC 13.10 and verify at least one RED case**

Run: `python -m unittest tests/gpu/test_gpu_capability_live_compilers.py -v`

Expected before completion: FAIL on a missing CLI/live fixture or a surviving exact bypass; the safe raw-paste control must show the compiler output `lease.safeX()`.

- [ ] **Step 4: Implement live fixture discovery and family requirements**

Use the same command normalization, runner, dependency, provenance, and audit pipeline as production. Add `--live-only` so the audit CLI runs only these fixtures. A named required family with no executable fails. An optional absent family skips with an explicit reason. Test sources and include headers live only in temporary directories and are removed after the result is parsed.

- [ ] **Step 5: Run live and deterministic parity suites**

Run: `python -m unittest tests/gpu/test_gpu_capability_live_compilers.py tests/gpu/test_gpu_capability_audit_lanes.py -v`

Expected: every forbidden table entry is rejected at its original line, every safe control has zero findings, and no test relies on the removed emulator.

- [ ] **Step 6: Commit parity controls**

```powershell
git add tests/gpu/test_gpu_capability_live_compilers.py tests/gpu/test_gpu_capability_audit_lanes.py tests/gpu/gpu_capability_source_audit.py
git commit -m "test(gpu): lock compiler capability parity" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 11: Wire the CLI, CTest, cache performance, and live CI matrix

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Modify: `tests/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `docs/build-and-run.md`

**Interfaces:**
- Consumes: complete compiler-authoritative pipeline and live fixtures.
- Produces: repeatable `--compile-commands`, required `--cache-dir`, `--performance-only`, `--live-only`, coverage summary, `gpu_capability_preprocess_unit`, `gpu_capability_live_compilers`, `gpu_capability_source_audit`, and `gpu_capability_source_audit_perf` CTests.

- [ ] **Step 1: Add failing CLI and performance assertions**

Test two compile databases, missing required cache directory, deterministic coverage output, warm hit with zero compiler invocations, corrupt-cache rebuild, 180-second cold/20-second warm/RSS acceptance reporting, and 4k/8k/16k raw/source-only scaling. Inject clocks/process counters for unit tests; use actual wall time only in the named performance test.

- [ ] **Step 2: Run CLI/performance tests and verify RED**

Run: `python -m unittest tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_cache.py -v`

Expected: FAIL because final CLI/cache performance reporting is not wired.

- [ ] **Step 3: Implement final CLI and deterministic summaries**

Accept one or more `--compile-commands`, require `--cache-dir` for compiler mode, retain `--performance-only`, and print authoritative paths, source-only paths, configurations, cache hits/misses, compiler invocations, elapsed time, stdout bytes, and peak RSS. Infrastructure failures return 2; capability findings return 1; clean audit returns 0.

- [ ] **Step 4: Update CMake test registration**

Register Python unit discovery and live compiler tests. Pass `${CMAKE_BINARY_DIR}/gpu-capability-cache` and `${CMAKE_BINARY_DIR}/compile_commands.json` to the source audit. Map `CMAKE_CXX_COMPILER_ID` to the exact family spelling and invoke the live test with `--live-only --live-compiler FAMILY=${CMAKE_CXX_COMPILER} --require-live-family FAMILY`. Set only `gpu_capability_source_audit` to `TIMEOUT 300`; keep performance `RUN_SERIAL` and `TIMEOUT 180`. Label all four tests `gpu-capability;ci`.

- [ ] **Step 5: Add live compiler-family CI evidence**

In `.github/workflows/ci.yml`, require GCC/MinGW on Windows, Clang on Linux, AppleClang plus Objective-C++ on macOS, and dedicated Windows smoke steps that initialize MSVC `cl` and locate clang-cl. Each named step passes `--require-live-family` and its absolute compiler path. A missing named compiler or skipped live fixture fails that job.

- [ ] **Step 6: Document exact developer commands**

Add these commands and explain the build-local cache/coverage summary in `docs/build-and-run.md`:

```powershell
python tests/gpu/gpu_capability_source_audit.py --source-root . --compile-commands build/gpu/compile_commands.json --cache-dir build/gpu/gpu-capability-cache
python -m unittest discover -s tests/gpu -p 'test_gpu_capability_*.py' -v
python tests/gpu/gpu_capability_source_audit.py --source-root . --performance-only
```

- [ ] **Step 7: Run Python, audit, perf, roadmap, and diff gates**

Run: `python -m unittest discover -s tests/gpu -p 'test_gpu_capability_*.py' -v`

Run: `python tests/gpu/gpu_capability_source_audit.py --source-root . --compile-commands build/gpu/compile_commands.json --cache-dir build/gpu/gpu-capability-cache`

Run twice and require the second summary to report zero compiler invocations and at most 20 seconds.

Run: `python tests/gpu/gpu_capability_source_audit.py --source-root . --performance-only`

Run: `python tools/roadmap/audit.py`

Run: `git diff --check`

Expected: all Python tests pass; audit reports no capability findings, complete active coverage, and warm cache; performance passes 4k/8k/16k plus cold/warm/RSS limits; roadmap reports 107 initiatives and 0 problems; diff check is silent.

- [ ] **Step 8: Commit CLI, CI, performance, and documentation wiring**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_cache.py tests/CMakeLists.txt .github/workflows/ci.yml docs/build-and-run.md
git commit -m "test(gpu): wire compiler-authoritative capability gate" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 12: Run current GPU build, controlled CTest, cross-platform CI, and closure review

**Files:**
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_model.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_command.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_provenance.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_runner.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_cache.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/gpu_capability_source_audit.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_model.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_command.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_provenance.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_runner.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_cache.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/test_gpu_capability_live_compilers.py`
- Modify when selected by an exact reproduced finding: `tests/gpu/fixtures/fake_preprocessor.py`
- Modify when selected by an exact reproduced finding: `tests/CMakeLists.txt`
- Modify when selected by an exact reproduced finding: `.github/workflows/ci.yml`
- Modify when selected by an exact reproduced finding: `docs/build-and-run.md`

No finding in this plan authorizes a change under `playback/` or `recorder_engine/`; a production-code finding stops this plan for a separate design decision.

**Interfaces:**
- Consumes: Tasks 1-11 and the existing GPU branch.
- Produces: build/test/CI evidence and a review-clean compiler-authoritative capability audit; no production API change and no merge.

- [ ] **Step 1: Reconfigure the current Windows GPU build only if its cache is stale**

```powershell
cmake -S . -B build/gpu -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/mingw_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DOLR_GPU_PIPELINE=ON -DOLR_WERROR=OFF -DOLR_BUILD_TESTS=ON -DOLR_FFMPEG_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/ffmpeg -DOLR_SRT_ROOT=D:/Development/OpenLiveReplay/windows_build/dist/srt
```

Expected: configure succeeds and `build/gpu/compile_commands.json` exists. Do not edit the pre-existing GCC warning.

- [ ] **Step 2: Build the complete GPU-on tree**

Run: `cmake --build build/gpu`

Expected: build succeeds with the source audit and Python tests registered.

- [ ] **Step 3: Run focused CTest through the stable runtime launcher**

```powershell
python D:/Development/OpenLiveReplay/windows-runtime-stable/tools/run_ctest.py --test-dir D:/Development/OpenLiveReplay/.claude/worktrees/gpu-surface-lease/build/gpu -R '^(gpu_capability_(preprocess_unit|live_compilers|source_audit|source_audit_perf)|gpu_(negcompile|compile_pass).*)$' --output-on-failure
```

Expected: all capability, live MinGW, negative-compile, compile-pass, and performance gates pass. No raw test executable is launched.

- [ ] **Step 4: Run the full unit suite through the stable runtime launcher**

```powershell
python D:/Development/OpenLiveReplay/windows-runtime-stable/tools/run_ctest.py --test-dir D:/Development/OpenLiveReplay/.claude/worktrees/gpu-surface-lease/build/gpu -L unit --output-on-failure
```

Expected: every unit test passes and the closing process audit reports zero surviving test/application processes.

- [ ] **Step 5: Run final static gates**

Run: `python tools/roadmap/audit.py`

Run: `git diff --check`

Run: `git status --short`

Expected: roadmap reports 107 initiatives and 0 problems; diff check is silent; only intentional task changes plus the preserved unrelated `tests/unit/tst_realcodecbenchmark.cpp` modification appear before targeted commits.

- [ ] **Step 6: Push with the documented hook and require current CI**

```powershell
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin feat/windows-gpu-fault-lane
```

Never use `--no-verify`. If only the documented pre-existing local warning gate blocks, use its narrow documented skip flag and leave all other gates enabled. Require green Linux Clang, Windows MinGW/GCC, Windows cl, Windows clang-cl, macOS AppleClang/Objective-C++, sanitizers, capability audit, and roadmap jobs.

- [ ] **Step 7: Request a fresh independent closure review**

The review must attempt compiler command/response ambiguity, launcher bypass, marker provenance confusion, source-only false accept, safe-code false positive, cache stale/partial reuse, timeout/output/RSS bypass, multi-configuration loss, and every exact macro table from Task 10. It must also verify that no `playback/` or `recorder_engine/` runtime behavior or locked GPU API changed.

- [ ] **Step 8: Fix each reproduced Critical/Important finding test-first**

For every finding, add its exact failing fixture to the owning `test_gpu_capability_*.py`, run that test to RED, apply the smallest correction in the owning Python module, run focused Python tests to GREEN, then repeat Steps 2-7. Stage only the finding's files and commit with the required trailer.

- [ ] **Step 9: Stop before merge and hand off evidence**

Report commit SHAs, focused/full test counts, cold/warm/RSS measurements, live compiler versions, coverage summary, CI URLs/status, and independent review verdict. Do not auto-merge PR #173, #174, #175, or the stacked branch.
