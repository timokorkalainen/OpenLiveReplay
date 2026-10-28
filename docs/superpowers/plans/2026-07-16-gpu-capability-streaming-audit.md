# GPU Capability Streaming Audit Implementation Plan

> Historical planning document. Commands, branch references, checklists, and validation claims describe the plan at the time of writing; they do not establish current implementation or merge status.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the retain-all compiler-view pipeline with a bounded streaming, process-parallel GPU capability audit whose compact validated result cache passes the real 251-configuration Windows cold/warm and conservative whole-tree RSS contracts while Linux and macOS pass their honestly named native containment/accounting contracts.

**Architecture:** Each cold configuration executes the audit compiler twice: a discovery run records only a bounded raw-output digest and validated dependency snapshot, then an accepted run records the same digest while performing the sole full provenance parse. Compiler-inspection probes are counted separately. The explicit spawn coordinator is worker-count agnostic. After the exact tokenizer and final keyed code are complete, manual/key-change real-251 calibrations on Windows, Linux and macOS measure the entire cold top-level pipeline and emit three strictly discriminated, versioned worker-decision artifacts. Implementation stops before final acceptance unless every platform has at least one `<180s` measured candidate satisfying its native contract; Windows additionally requires fail-closed conservative `aggregate_peak_rss_upper_bound_bytes <448 MiB`, preserving 64 MiB below 512 MiB. Linux uses kernel cgroup-v2 accounted-memory enforcement. Native macOS scopes containment to registered worker/compiler PGIDs launched through trusted fingerprinted drivers, proves the registered sets are empty with no known unreconciled descendants, and reports observed residency without claiming global descendant containment or a race-free peak. Normal CI strictly validates its checked-in native decision and applies its measured runtime parameters from generation one; Windows additionally runs its selected cold run to a bounded canonical summary, releases every cold result, then runs warm and compares the same summary, so cold and warm result tuples never coexist. The parent validates warm entries through held dependency handles before dispatch, workers audit and discard accepted views, then the parent closes one final production-source observation boundary and runs raw/source-only policy from the validated snapshot.

**Tech Stack:** CPython 3.11.9 on the Windows reference lane, explicit spawn workers, generation-bound custom overlapped/nonblocking frame and payload endpoints, SHA-256 streaming digests, existing fail-closed cache carriers/locks, GCC 13.1.0/MinGW, Clang/AppleClang, MSVC `cl`, clang-cl, CMake/CTest, `unittest`, GitHub Actions.

## Global Constraints

- Execute every relative command from the repository root of the GPU surface-lease worktree.
- This plan modifies `tests/gpu/`, `tests/CMakeLists.txt`, `.github/workflows/ci.yml`, `docs/build-and-run.md`, this plan, and only these six named platform evidence artifacts: `docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md`, `docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json`, `docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-calibration.md`, `docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-decision.json`, `docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-calibration.md`, and `docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-decision.json`. It does not modify `playback/`, `recorder_engine/`, application runtime behavior, GPU frame paths, or production APIs.
- Preserve the current superseding runtime design without reinterpretation: `GpuSurface::nativeHandle()` stays protected, `GpuReadLease` stays its only friend, `retainUntilFenceRetired()` and `pendingFenceValue()` stay public, and the existing `GpuOpScope`, `GpuRetireRegistry`, `GpuSyncReadScope`, and `withRead` lifetime patterns remain unchanged and in use. Historical omission/manual-`complete()` notes are superseded; this plan neither removes nor relitigates current runtime behavior.
- Preserve the existing capability policy and all raw, authoritative, source-only, provenance, negative-compile, and mutation expectations. The architecture change may not filter compile configurations or weaken a failure into a skip.
- Audit all 251 distinct semantic configurations in the current Windows GPU-on `compile_commands.json`; exact semantic deduplication remains the only configuration deduplication before compilation.
- A cold miss performs exactly two observed audit-purpose subprocess launches and exactly one `PreprocessedStreamBuilder` parse. Typed launch events at the subprocess boundary, never a hard-coded return value, produce `audit_compiler_invocations`; each miss must have one discovery and one accepted event, and any missing, duplicate, retry, or extra launch fails. Raw output byte count and SHA-256, dependency closure, and dependency content snapshots must match across discovery and accepted runs. Collection-time driver/version inspection increments only `inspection_probe_invocations`; it never changes the exact `2 * miss_count` audit total.
- Compiler failure, timeout, output overflow, malformed markers/directives, fresh-run dependency mismatch, unstable concurrent dependency mutation, different production-source identity/content at the final policy boundary, raw-output mismatch, worker crash, platform memory-contract violation, incomplete process accounting, deadline expiry, unsafe cache namespace/carrier/link/replacement state, and a worker result whose engine differs from its requested engine are infrastructure failures. A stable cached dependency whose final held-handle identity/content differs is a miss and rebuild. An ordinary audit-engine change selects a different combined key and coexists with the old entry; a same-key authentic manifest/payload engine mismatch is also corrupt cache data, therefore a miss and rebuild. Full identity/content restoration before the applicable final boundary is equivalent. Partial fresh work never passes or publishes.
- Per-audit-invocation limits remain 60 seconds, 128 MiB stdout, and 1 MiB retained stderr. Compile-database/argument aggregates are bounded before parsing. Windows acceptance is cold `<180s`/`<448 MiB`, warm `<20s`, outer emergency `<512 MiB`; its clock/accountant starts before enumeration/inspection and snapshots after all cold stages. Linux enforces cgroup max/high/OOM, and macOS uses honest registered observed residency. Real Windows/Linux/macOS calibration occurs only after final Task-10 keyed code/workflow/tests and before final acceptance/artifact commit; any platform without a `<180s` passing candidate stops for redesign.
- `_PIPELINE_DEADLINE_SECONDS = 180.0` creates one absolute deadline before enumeration. Compiler work, locks, bounded frame/payload I/O, joins and native waits use it; only failure cleanup gets a separate 30-second emergency deadline.
- No `PreprocessedTranslationUnitView` or compact result object crosses a process boundary or is pickled. At most one accepted full view exists per worker and it is deleted before bounded canonical payload advertisement. The parent decodes only charged compact payloads. An in-order result is validated, merged into the charged streaming aggregate, and released immediately; an out-of-order result may remain only in the charged deterministic reorder window capped at the selected worker count, then is merged/released as soon as its ordinal is next. No uncharged or unbounded result batch exists.
- The logical 128-MiB `CompactResultMemoryBudget` is exclusively for compact result objects and their count/encode/cache-publication/pipe/receive/decode/retained phases. It never charges `PreprocessedTranslationUnitView`, `AuditBuffer`, `CompilerAuditAnalysis`, token/provenance state, or `AuditLimits.retained_token_bytes`. Those are worker audit working set governed by the simultaneous native process-memory contract: conservative fail-closed observed whole-tree RSS upper-bound cancellation on Windows, kernel-enforced 512-MiB `memory.max` on Linux, calibrated Windows cold `<448 MiB`, and honestly observed macOS residency. Windows Job commit charge is a separate per-Job backstop, not a kernel hard aggregate RSS limit. Calibration may reject a worker count whose audit working set violates the physical gate.
- Final worker-count calibration is the explicit Task 10 developer/manual and key-change operation, run only after Task 9 is green. It visits two, one, then increasing counts allowed by the host. For each count it first runs an unrecorded pilot to derive recycling parameters, then launches a separate top-level measured run with a new empty cache and starts clock/native accounting before production-authority enumeration. It applies those exact parameters across enumerate-production, configuration collection, production snapshot, cache validation, compilation, compact audit/publication, authoritative aggregation, final source revalidation, raw audit, source-only audit, shutdown/reap, and summary. Only the second run can become a platform sample or evidence. It emits the checked-in keyed artifact. Normal CI never repeats pilots or the count sweep: it validates the current key, runs exactly the selected sample's recorded parameters for the selected cold/warm pair, and fails stale keys with the exact regeneration command. If no calibrated count passes, stop and redesign instead of relaxing limits.
- The cache/root outer overhead is exact: compact result partition 472 MiB + compiler-inspection partition 32 MiB + carrier/manifest/temporary-and-evidence overhead 8 MiB = 512 MiB. The 8-MiB equation is closed numerically: `round_up(4 MiB payload, 4 KiB) + round_up(64 KiB manifest, 4 KiB) + 2 * round_up(64 KiB carrier/root metadata, 4 KiB) + round_up(3,473,408 bytes simultaneous UUID-temp/directory/fsync metadata, 4 KiB) + 524,288 bytes bounded canonical summary/evidence allocation = 8,388,608 bytes`. The 524,288 bytes are reserved once upfront as one shared `EvidenceSuballocator`, then nonoverlapping/overlap-aware subreservations cover cold summary, warm summary, builder, canonical machine record and codec scratch; no object has both a direct outer-budget owner and a suballocator owner. Builder abort precedes suballocator/outer-budget abort. Tests exercise simultaneous peak and double-ownership rejection. Root-wide publication admission has capacity one, so no second temporary overlap is permitted. `1,024` remains an independent entry-count ceiling.
- `ConfigurationAuditCache` uses the existing eight preprocess coordination carrier filenames and exact current lane formula `hashlib.sha256(f"{namespace}:{key}".encode("utf-8")).digest()[0] % 8`, stores entries below the exact `results/` subdirectory, and does not add a seventeenth carrier or a new lock namespace. The CLI stops constructing `PreprocessCache` and uses a fresh result-cache root; it never opens a root containing old full-view entries.
- The active pipeline refuses a cache root that contains active full-view entries; migration uses a fresh `gpu-capability-result-cache` root, so result and full-view partitions never coexist under the same 512 MiB root.
- Cache identity is one SHA-256 over length-framed cache schema bytes, configuration digest bytes, and audit-engine fingerprint bytes. That same 64-hex key selects the entry path, is supplied unchanged to the existing namespace/key lane formula, and is stored as the manifest/load/publish key.
- Authentic regular cache entries with corrupt, truncated, or over-limit payloads are misses and rebuild. Unsafe namespace state—invalid carrier/root marker, link/reparse, replacement, filesystem identity change, or non-regular entry—is an infrastructure failure and never becomes a miss.
- The audit-engine fingerprint attests a staged exhaustive live semantic graph. Each task extends fixed module roots/schema in its introducing commit; one cycle-safe module-root walker covers every module-defined function/class, code/default/closure/descriptor/base, immutable table and transitive module global. Portable digest inputs are normalized module name/origin role and loaded semantic bytes only—never absolute origin or native identity. Import-time origin/native identity may be recorded for diagnostics only; recomputation performs no filesystem read or path-identity check. Only exact real runtime state is excluded; unknown mutable state fails closed.
- The decision-engine fingerprint applies that same exhaustive module-root walker to process-tree/calibration roots plus portable audit digest and decision tables. There is no hand function manifest and no absolute/native origin input, so checked-in decisions reproduce across worktrees/hosts while semantic changes invalidate them.
- Warm dependency validation is parent-owned and defines validity at a final held-handle observation boundary. `ConfigurationAuditCache.load_many()` authenticates candidate entries, deduplicates dependency paths, opens each unique path exactly once, binds native identity, and retains every handle/fd. It hashes each held object initially, then immediately before dispatch re-seeks and rehashes the same held handle while checking stable pre/post handle stat and current path-to-handle identity. Windows opens with write/delete sharing denied for the held lifetime; POSIX uses final `lstat` identity comparison and detects differing final bytes. Exact identity/content restoration before the final rehash is equivalent; final differing identity/content misses, and unstable pre/hash/post observation is an infrastructure failure. Tests distinguish one path open from two full reads.
- Cache dependency validation has an exact aggregate ceiling of 32,768 unique dependency paths/held handles and 32 MiB of retained path, identity, digest, handle-wrapper and indexing metadata. Allocation-free manifest preparse first deduplicates/counts paths and computes checked metadata bounds for the whole batch; it also verifies the process descriptor/handle limit can satisfy the full retained set. Any overflow fails before opening the first dependency. Only after that preflight may validation open/hash in bounded chunks of 256 while retaining the already charged handles through the final boundary; a partial-open failure closes every handle.
- One frozen attested `DependencyRootAuthority` is constructed before collection. Its object identity is preserved across every in-process API; spawn codecs reproduce exact equality plus the same portable authority digest in children. `PreprocessConfiguration` stores that digest, and compiler inspection, discovery/accepted parsing, cache key/local codec, `WorkerTask`, publication, result validation, audit/decision fingerprints and acceptance keys all require it. Any replacement, equal-digest/different-local-root carrier, ambiguous/unmapped root or identity mismatch fails before compiler/cache work.
- Before workers start, snapshot every production source through one retained native handle/fd: identity, content digest, and bounded immutable bytes, and freeze an immutable `initial_digest_map`. Cache loading and every fresh worker compare every production `DependencyDigest` identity and SHA-256 to that initial map before hit classification, publication, or use; a generation mismatch is an infrastructure failure for a fresh result and a miss for a cached candidate. After all worker results are collected, re-seek/rehash those same held descriptors, revalidate current path identity and stable pre/post stat at one final policy boundary, and fail on different identity/content. Raw and source-only audit only the validated immutable snapshot after that boundary. Exact restoration before the final rehash cannot mask a compiler run against a different production generation; policy never mixes source generations.
- Cancellation consumers depend only on `is_set() -> bool`; the same protocol accepts `threading.Event` and a real spawn-context `multiprocessing.Event` through real subprocess launch/cancellation tests.
- Preserve exact cache carrier names, exact carrier content validation, full-file lock ranges, link/reparse rejection, root-before-key ordering, crash-temp cleanup, and atomic publication from the already reviewed cache substrate that this plan extends in Task 3. Replace every unbounded/root-lock wait and every per-key fresh 240-second timer with the caller's absolute deadline capped by the existing 240-second internal ceiling.
- Preserve the unrelated dirty `tests/unit/tst_realcodecbenchmark.cpp`. Use targeted `git add <paths>`, never `git add -A`, and never stage private handoff notes.
- Do not touch `expectedDecodeSurfaceBytesForTrack`; local MinGW builds use `-DOLR_WERROR=OFF` only for the documented pre-existing warning.
- Do not launch Windows test binaries directly. Use `tools/run_ctest.py`.
- Every implementation task ends with a targeted commit and a fresh-context review. Fix every reproduced Critical or Important finding test-first before continuing.
- For each Task 1-9 commit, stage together the semantic implementation, the fingerprint-owner module's stage/schema/root/exclusion update, the stage-evolution and rebinding tests, and no unrelated files. Immediately after that commit, obtain a new-agent fresh-context review of that task's attestation coverage and implementation; fix reproduced findings in a new targeted commit before beginning the next task. One exhaustive module-root walker is shared by audit and decision attestation: the fixed root set is allowed and tested, while any hand-maintained function/wrapper manifest is forbidden.

---

## Measured Baseline and Failure Evidence

- The real database contains 251 GCC configurations.
- Before O(1) retained-byte accounting, parsing 262,100 real MinGW bytes took 19.720s and 79,326,215 calls; retained-table rescans consumed 18.301s. The counter fix reduced the same sample to 1.275s and 3,189,783 calls.
- The first real cold audit then reached the 180-second global deadline after publishing only 18 stabilized entries. No per-invocation timeout remained.
- Single-configuration stabilized timings: `iosgpupolicy.cpp` 2.075s for 73,222 tokens/1.17 MiB packed; `h26xseitimecode.cpp` 10.391s for 486,477 tokens/7.78 MiB; `gpubudget.cpp` 11.801s for 581,208 tokens/9.30 MiB.
- Discovery and accepted parse costs are symmetric: `iosgpupolicy.cpp` 1.028s/1.108s; `gpubudget.cpp` 5.850s/6.137s. Semantic digesting costs only 3-31ms, so the second full parse is the avoidable cost.
- Warm `gpubudget.cpp` cache load takes 1.77-1.92s and audit takes 17.42-17.80s. Its 581,208 tokens include 268,674 identifiers, 303,604 punctuators, 6,834 numbers, and 2,096 literals with 13,317 unique spellings.
- A batched per-run normalization prototype produced byte-for-byte identical 2,949,084-byte audit text in 0.250s versus 2.615s for the current builder. Full audit remained expensive because token/shadow analysis was rebuilt multiple times.
- Loading only five real cached views raised process RSS from 30,384,128 to 537,055,232 bytes. Linear projection is about 25.4 GiB allocator RSS; packed columns alone project about 1.72 GiB. Retain-all cannot meet 512 MiB.
- Eighteen old full-view payloads consumed 109.9 MiB. The old 256-entry/472 MiB partition cannot guarantee all 251 views stay warm.

## File Structure and Fixed Interfaces

- Modify `tests/gpu/gpu_capability_model.py`: add immutable digest, dependency snapshot, compact finding/result, runtime outcome, and worker telemetry types.
- Modify `tests/gpu/gpu_capability_runner.py`: add discovery-only execution, accepted one-parse stabilization, worker-side audit entry point, typed spawn-worker protocol, compact process outcomes, cancellation, and whole-process-tree accounting.
- Modify `tests/gpu/gpu_capability_cache.py`: replace full-view publication in the active pipeline with `ConfigurationAuditCache`, reusing the reviewed lock/carrier substrate and publishing compact JSON payloads.
- Modify `tests/gpu/gpu_capability_provenance.py`: expose exact stream digest observation and implement the measured exact tokenizer acceleration only after differential parity is red.
- Modify `tests/gpu/gpu_capability_source_audit.py`: add audit-engine fingerprinting, dependency-closure coverage, pre-buffer candidate filtering, batched `AuditBuffer` construction, reusable parsed analysis, compact finding conversion, streaming coordinator aggregation, and final CLI measurements.
- Create `tests/gpu/gpu_capability_process_tree.py` and `tests/gpu/gpu_capability_calibration.py`: native owned-tree accounting, full-pipeline calibration, decision-key construction, atomic artifact publication, and strict artifact loading.
- Modify `tests/gpu/gpu_capability_runner.py`: collect an immutable decision-record sidecar from raw compile-database entries and launcher-effective environment assignments while preserving the Task-2 authority/capability-bound `PreprocessConfiguration` exact compiler/cache identity.
- Modify `tests/gpu/fixtures/fake_preprocessor.py`: add deterministic raw mismatch, dependency mismatch, worker delay/crash, and large-view modes.
- Create `tests/gpu/test_gpu_capability_process_tree.py` and `tests/gpu/test_gpu_capability_calibration.py`; modify `tests/gpu/test_gpu_capability_model.py`, `test_gpu_capability_command.py`, `test_gpu_capability_provenance.py`, `test_gpu_capability_runner.py`, `test_gpu_capability_cache.py`, `test_gpu_capability_audit_lanes.py`, and `test_gpu_capability_live_compilers.py`: exact TDD, decision-artifact validation, and cross-family parity.
- Modify `tests/CMakeLists.txt`, `.github/workflows/ci.yml`, and `docs/build-and-run.md`: register and document Windows compact-cache cold/warm/RSS-upper-bound acceptance, Linux cgroup-accounted memory enforcement, and macOS containment/observed-residency correctness without cross-labeling the metrics.
- Create the six exact platform evidence artifacts named in Global Constraints: Windows, Linux and macOS each produce one human-readable calibration `.md` and one strictly discriminated machine-readable decision `.json` consumed by its native acceptance command.

The following names and signatures are fixed across tasks:

```python
class CancellationSignal(Protocol):
    def is_set(self) -> bool: ...

@dataclass(frozen=True)
class AuditLimits:
    response_depth: int = 8
    response_files: int = 32
    response_bytes: int = 4 * 1024 * 1024
    invocation_seconds: float = 60.0
    total_seconds: float = 180.0
    stdout_bytes: int = 128 * 1024 * 1024
    stderr_bytes: int = 1024 * 1024
    retained_token_bytes: int = 384 * 1024 * 1024
    rss_bytes: int = 512 * 1024 * 1024
    workers: int = min(8, os.cpu_count() or 1)
    production_raw_per_file_bytes: int = 8 * 1024 * 1024
    production_raw_aggregate_bytes: int = 64 * 1024 * 1024
    production_decoded_transient_bytes: int = 256 * 1024 * 1024
    production_decoded_retained_bytes: int = 128 * 1024 * 1024
    compile_database_count: int = 16
    compile_database_encoded_bytes: int = 64 * 1024 * 1024
    compile_database_entries: int = 65_536
    command_arguments_per_entry: int = 1_024
    command_argument_utf8_bytes: int = 4_096
    command_arguments_aggregate_bytes: int = 64 * 1024 * 1024
    unique_dependency_handles: int = 32_768
    unique_generation_guard_directories: int = 65_536
    generation_guard_metadata_bytes: int = 64 * 1024 * 1024
    dependency_handle_metadata_bytes: int = 32 * 1024 * 1024

@dataclass(frozen=True, slots=True)
class StreamDigest:
    sha256: str
    byte_count: int

@dataclass(frozen=True, slots=True)
class DependencyDigest:
    stable_role: str
    role_relative_path: PurePosixPath
    identity: FileIdentity
    sha256: str

@dataclass(frozen=True, slots=True)
class AuditResultFinding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str

@dataclass(frozen=True, slots=True)
class AggregatedFinding:
    path: PurePosixPath
    line: int
    expression: str
    reason: str
    configuration_digests: tuple[str, ...]

@dataclass(frozen=True, slots=True)
class ConfigurationAuditResult:
    configuration_digest: str
    audit_engine_fingerprint: str
    dependencies: tuple[DependencyDigest, ...]
    reached_production: tuple[PurePosixPath, ...]
    findings: tuple[AuditResultFinding, ...]

@dataclass(frozen=True, slots=True)
class ConfigurationAuditOutcome:
    result: ConfigurationAuditResult
    stdout_bytes: int
    stages: "WorkerStageTimings"

@dataclass(frozen=True, slots=True)
class DecisionConfigurationRecord:
    configuration_digest: str
    compiler_family: CompilerFamily
    compiler_digest: str
    production_source: PurePosixPath
    normalized_decision_arguments: tuple[str, ...]
    decision_environment_digest: str
    working_directory_role: str
    compiler_capability_digest: str

@dataclass(frozen=True, slots=True)
class ConfigurationCollection:
    configurations: tuple[PreprocessConfiguration, ...]
    decision_records: Mapping[str, DecisionConfigurationRecord]
    inspection_probe_invocations: int
    dependency_root_authority: "DependencyRootAuthority"
    capability_registry: "CompilerCapabilityRegistry"

@dataclass(frozen=True, slots=True)
class DependencyRootBinding:
    stable_role: str
    resolved_root: Path
    root_identity: FileIdentity

@dataclass(frozen=True, slots=True)
class DependencyRootAuthority:
    source_root: DependencyRootBinding
    external_roots: tuple[DependencyRootBinding, ...]
    portable_authority_digest: str

@dataclass(frozen=True, slots=True)
class PreprocessConfiguration:
    # Existing command/source fields are unchanged. Task 2 adds this mandatory
    # authority binding; its length-framed bytes participate in the exact
    # semantic/configuration digest and every local/cache/wire validation.
    dependency_root_authority_digest: str
    compiler_capability_digest: str
    compiler_capability: "CompilerExecutableCapability" = field(compare=False)

@dataclass(frozen=True, slots=True)
class ProductionIdentityAuthority:
    source_root: DependencyRootBinding
    ordered_relative_paths: tuple[PurePosixPath, ...]
    enumeration_policy_digest: str
    authority_digest: str
    def enumerate(
        self, source_root: Path, accountant: "RunScopedNativeAccountant",
        deadline: float,
    ) -> Mapping[PurePosixPath, FileIdentity]: ...

# The authority is created once, before enumeration, by
# build_production_identity_authority(). Its source-root identity and ordered
# policy are immutable; enumerate() may be called only with that exact root and
# the run's identical accountant. The exact local codec preserves every field
# and rejects unknown/duplicate paths, a changed root identity, or a digest
# mismatch before any filesystem enumeration.

@dataclass(frozen=True, slots=True)
class CompilerInspection:
    compiler_family: CompilerFamily
    executable_identity: FileIdentity
    executable_sha256: str
    normalized_version: str
    driver_fingerprint: str
    inspection_arguments_digest: str
    executable_capability_digest: str

@dataclass(frozen=True, slots=True)
class CompilerExecutableCapability:
    platform_kind: Literal["windows", "linux", "macos"]
    executable_identity: FileIdentity
    executable_sha256: str
    capability_digest: str
    # Local-only retained native handle/fd and vnode/launch guard. Exact codecs
    # carry only the digest/identity/hash; decode must reopen and revalidate.
    native_owner: object = field(compare=False, repr=False)
    trusted_toolchain_root: DependencyRootBinding
    directory_chain_owners: tuple[object, ...] = field(compare=False, repr=False)
    directory_chain_identities: tuple[FileIdentity, ...]
    resolved_runtime_closure_digest: str
    resolved_runtime_closure: tuple[DependencyDigest, ...]

class CompilerCapabilityRegistry(Protocol):
    def register(self, capability: CompilerExecutableCapability) -> str: ...
    def duplicate_for_generation(self, digest: str, worker_index: int,
                                 generation: int) -> object: ...
    def transfer_duplicate_to_child(self, duplicate: object,
                                    startup_endpoint: object) -> None: ...
    def release_generation(self, worker_index: int, generation: int) -> None: ...
    def close_collection_owner(self) -> None: ...
    def close(self) -> None: ...

@dataclass(frozen=True, slots=True)
class DependencyGenerationGuard:
    dependency: DependencyDigest
    held_file_owner: object = field(compare=False, repr=False)
    held_parent_owner: object = field(compare=False, repr=False)
    armed_stat: FileIdentity
    armed_size: int
    armed_mtime_ns: int
    armed_ctime_ns: int
    platform_watch: object = field(compare=False, repr=False)
    directory_chain_owners: tuple[object, ...] = field(compare=False, repr=False)
    directory_chain_identities: tuple[FileIdentity, ...]

@dataclass(frozen=True, slots=True)
class CacheRootIdentity:
    resolved_root: Path
    native_identity: FileIdentity
    namespace_cookie: str

class CompilerInspectionCache(Protocol):
    root_identity: CacheRootIdentity
    def load(self, compiler: Path, compiler_family: CompilerFamily,
             launcher_environment: Mapping[str, str],
             dependency_roots: DependencyRootAuthority,
             expected_audit_engine_fingerprint: str,
             pipeline_deadline: float) -> CompilerInspection | None: ...
    def publish(self, compiler: Path, compiler_family: CompilerFamily,
                launcher_environment: Mapping[str, str],
                dependency_roots: DependencyRootAuthority,
                expected_audit_engine_fingerprint: str,
                inspection: CompilerInspection,
                pipeline_deadline: float) -> CompilerInspection: ...

@dataclass(frozen=True, slots=True)
class CommandArgumentSpan:
    role: str
    option_index: int
    operand_index: int
    attachment: str
    forwarded_depth: int

@dataclass(frozen=True, slots=True)
class CommandRewriteClassification:
    response_expanded_launcher_stripped_arguments: tuple[str, ...]
    compiler: CommandArgumentSpan
    sources: tuple[CommandArgumentSpan, ...]
    nonsemantic_outputs: tuple[CommandArgumentSpan, ...]
    nonsemantic_dependencies: tuple[CommandArgumentSpan, ...]
    semantic_scalars: tuple[CommandArgumentSpan, ...]
    semantic_paths: tuple[CommandArgumentSpan, ...]

@dataclass(frozen=True)
class RewrittenCommand:
    arguments: tuple[str, ...]
    dependency_output: Path
    dependency_format: str
    classification: CommandRewriteClassification

@dataclass(frozen=True, slots=True)
class CacheLoadBatch:
    hit_count: int
    misses: tuple[PreprocessConfiguration, ...]
    cold_slot_reserved_bytes: int

@dataclass(frozen=True, slots=True)
class CompactBudgetOwnership:
    generation: int
    owner: str
    charge_ids: tuple[str, ...]

@dataclass(frozen=True, slots=True)
class CompactResultPreparseBounds:
    encoded_bytes: int
    conservative_decoded_bytes: int
    conservative_retained_bytes: int

@dataclass(frozen=True, slots=True)
class ConservativeAllocationSchema:
    schema_version: int
    reference_python_version: tuple[int, int, int]
    pointer_bytes_upper_bound: int
    allocator_alignment_bytes: int
    object_header_bytes_upper_bound: int
    bytes_header_bytes_upper_bound: int
    bytearray_header_bytes_upper_bound: int
    bytesio_header_bytes_upper_bound: int
    str_header_bytes_upper_bound: int
    list_header_bytes_upper_bound: int
    tuple_header_bytes_upper_bound: int
    dict_header_bytes_upper_bound: int
    dict_index_bytes_upper_bound: int
    dict_entry_bytes_upper_bound: int
    pylong_header_bytes_upper_bound: int
    pylong_digit_bits: int
    pylong_digit_bytes_upper_bound: int
    json_scanner_fixed_bytes_upper_bound: int
    json_decoder_fixed_bytes_upper_bound: int
    canonical_encoder_fixed_bytes_upper_bound: int
    production_decoder_fixed_bytes_upper_bound: int

class CompactAccountingObserver:
    current_bytes: int
    peak_bytes: int
    semantic_events: list[str]
    def transition(self, owner: str, phase: str, delta_bytes: int) -> None: ...
    def release(self, owner: str) -> None: ...

@dataclass(frozen=True, slots=True)
class TransportAllocationLayout:
    counting_pass_peak_bytes: int
    canonical_encoder_scratch_bytes: int
    sender_payload_bytes: int
    pipe_frame_bytes: int
    pipe_kernel_capacity_bytes: int
    receiver_payload_bytes: int
    json_decoded_transient_bytes: int
    retained_result_bytes: int
    permit_reservation_bytes: int
    decode_reservation_bytes: int
    peak_pending_bytes: int

@dataclass(frozen=True, slots=True)
class CompactSlotTemplate:
    worst_case_live_bytes: int
    includes_publication_bytes: int
    includes_transport_bytes: int
    includes_decode_and_retained_bytes: int
    included_components: tuple[str, ...]

@dataclass(frozen=True, slots=True)
class PerTaskCompactReservation:
    task_id: int
    generation: int
    template: CompactSlotTemplate

@dataclass(frozen=True, slots=True)
class CachePublicationPermit:
    task_id: int
    generation: int
    reserved_bytes: int

CONTROL_WIRE_SCHEMA_BYTES = b"olr-gpu-capability-control-v1"
CONTROL_MAX_BYTES = 64 * 1024
CONTROL_DIAGNOSTIC_MAX_BYTES = 4 * 1024
TASK_WIRE_SCHEMA_BYTES = b"olr-gpu-capability-task-v1"
TASK_MAX_BYTES = 256 * 1024
COMMAND_WIRE_SCHEMA_BYTES = b"olr-gpu-capability-command-v1"
COMMAND_MAX_BYTES = 16 * 1024
STARTUP_WIRE_SCHEMA_BYTES = b"olr-gpu-capability-startup-v1"
STARTUP_MAX_BYTES = 256 * 1024

@dataclass(frozen=True, slots=True)
class WorkerStartupBootstrap:
    worker_index: int
    generation: int
    random_cookie: str
    dependency_root_authority: DependencyRootAuthority
    compiler_capability_digests: tuple[str, ...]

@dataclass(frozen=True, slots=True)
class ControlWireBounds:
    message_tag: str
    worker_index: int
    generation: int
    task_id: int | None
    diagnostic_utf8_bytes: int

@dataclass(frozen=True, slots=True)
class CachePublicationRequested:
    worker_index: int
    generation: int
    task_id: int

@dataclass(frozen=True, slots=True)
class PreprocessStageTimings:
    discovery_seconds: float
    accepted_parse_seconds: float

@dataclass(frozen=True, slots=True)
class WorkerStageTimings:
    discovery_seconds: float
    accepted_parse_seconds: float
    audit_seconds: float
    publish_seconds: float

@dataclass(frozen=True, slots=True)
class WindowsRunMemoryMeasurements:
    parent_peak_rss_bytes: int
    worker_peak_rss_bytes: int
    maximum_simultaneous_working_set_bytes: int
    aggregate_peak_rss_upper_bound_bytes: int
    job_peak_commit_charge_bytes: int
    job_total_process_count: int
    retained_process_identity_count: int
    inspection_peak_tree_bytes: int
    retained_inspection_process_identity_count: int
    surviving_job_process_count: int
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class LinuxRunMemoryMeasurements:
    cgroup_current_accounted_memory_bytes: int
    cgroup_peak_accounted_memory_bytes: int
    cgroup_memory_max_bytes: int
    cgroup_memory_high_bytes: int
    cgroup_oom_count_delta: int
    cgroup_oom_kill_count_delta: int
    cgroup_max_event_count_delta: int
    service_root_oom_count_delta: int
    service_root_oom_kill_count_delta: int
    service_root_max_event_count_delta: int
    surviving_cgroup_process_count: int
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class MacOSRunMemoryMeasurements:
    maximum_observed_aggregate_resident_bytes: int
    maximum_observed_parent_resident_bytes: int
    maximum_observed_owned_group_resident_bytes: int
    surviving_registered_process_count: int
    known_unreconciled_descendant_count: int
    accounting_complete: bool

PlatformRunMemoryMeasurements = (
    WindowsRunMemoryMeasurements
    | LinuxRunMemoryMeasurements
    | MacOSRunMemoryMeasurements
)

@dataclass(frozen=True, slots=True)
class WorkerTelemetry:
    worker_index: int
    generation: int
    worker_pid: int
    sequence: int
    owned_process_count: int
    worker_current_resident_bytes: int
    worker_peak_resident_bytes: int
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class WindowsPostReturnSample:
    platform_kind: Literal["windows"]
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    aggregate_peak_rss_upper_bound_bytes: int
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class LinuxPostReturnSample:
    platform_kind: Literal["linux"]
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    cgroup_current_accounted_memory_bytes: int
    cgroup_peak_accounted_memory_bytes: int
    cgroup_memory_high_bytes: int
    cgroup_memory_max_bytes: int
    service_and_run_event_deltas_zero: bool
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class MacOSPostReturnSample:
    platform_kind: Literal["macos"]
    worker_slot: int
    worker_generation: int
    task_ordinal_in_generation: int
    worker_current_rss_bytes: int
    maximum_observed_owned_group_resident_bytes: int
    registered_survivors: int
    known_unreconciled_descendants: int
    accounting_complete: bool

PlatformPostReturnSample = (
    WindowsPostReturnSample | LinuxPostReturnSample | MacOSPostReturnSample
)

@dataclass(frozen=True, slots=True)
class WorkerCalibrationSample:
    platform_kind: Literal["windows"]
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    memory: WindowsRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[WindowsPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: "WindowsPhaseSnapshot"
    task_phase_snapshot: "WindowsPhaseSnapshot"

@dataclass(frozen=True, slots=True)
class ReferenceCalibrationEnvelope:
    runner_image: str
    python_version: str
    qt_package: str
    mingw_package: str
    worker_capacity: int
    affinity_cpu_indices: tuple[int, ...]

@dataclass(frozen=True, slots=True)
class WorkerDecisionKey:
    platform_tag: str
    compiler_digest: str
    configuration_set_digest: str
    uninspected_configuration_digest: str
    decision_engine_fingerprint: str
    effective_worker_capacity: int
    reference_envelope_digest: str
    key_digest: str

@dataclass(frozen=True, slots=True)
class WorkerCountDecision:
    platform_kind: Literal["windows"]
    artifact_schema: int
    key: WorkerDecisionKey
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple["CalibrationRejection", ...]
    samples: tuple[WorkerCalibrationSample, ...]
    evidence_record_sha256: str

@dataclass(frozen=True, slots=True)
class LinuxWorkerCalibrationSample:
    platform_kind: Literal["linux"]
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    memory: LinuxRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[LinuxPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: "LinuxPhaseSnapshot"
    task_phase_snapshot: "LinuxPhaseSnapshot"

@dataclass(frozen=True, slots=True)
class MacOSWorkerCalibrationSample:
    platform_kind: Literal["macos"]
    worker_count: int
    configuration_count: int
    elapsed_seconds: float
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    memory: MacOSRunMemoryMeasurements
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    cache_bytes: int
    cache_entries: int
    pilot_eligible: bool
    surviving_owned_process_count: int
    included_stages: tuple[str, ...]
    post_return_samples: tuple[MacOSPostReturnSample, ...]
    stage_p50_seconds: WorkerStageTimings
    stage_p95_seconds: WorkerStageTimings
    inspection_phase_snapshot: "MacOSPhaseSnapshot"
    task_phase_snapshot: "MacOSPhaseSnapshot"

@dataclass(frozen=True, slots=True)
class LinuxCalibrationEnvelope:
    runner_image: str
    kernel_release: str
    python_version: str
    clang_version: str
    systemd_version: str
    cgroup_v2: bool
    effective_worker_capacity: int

@dataclass(frozen=True, slots=True)
class MacOSCalibrationEnvelope:
    runner_image: str
    macos_version: str
    python_version: str
    appleclang_version: str
    effective_worker_capacity: int

@dataclass(frozen=True, slots=True)
class LinuxWorkerCountDecision:
    platform_kind: Literal["linux"]
    artifact_schema: int
    key: WorkerDecisionKey
    envelope: LinuxCalibrationEnvelope
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple["CalibrationRejection", ...]
    samples: tuple[LinuxWorkerCalibrationSample, ...]
    evidence_record_sha256: str

@dataclass(frozen=True, slots=True)
class MacOSWorkerCountDecision:
    platform_kind: Literal["macos"]
    artifact_schema: int
    key: WorkerDecisionKey
    envelope: MacOSCalibrationEnvelope
    selected_workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    attempted_worker_counts: tuple[int, ...]
    rejections: tuple["CalibrationRejection", ...]
    samples: tuple[MacOSWorkerCalibrationSample, ...]
    evidence_record_sha256: str

PlatformWorkerDecision = (
    WorkerCountDecision | LinuxWorkerCountDecision | MacOSWorkerCountDecision
)

@dataclass(frozen=True, slots=True)
class CalibrationRejection:
    platform_kind: Literal["windows", "linux", "macos"]
    worker_count: int
    failure_stage: str
    failure_code: str
    cleanup_complete: bool
    surviving_owned_process_count: int

@dataclass(frozen=True, slots=True)
class PrevalidatedWorkerDecision:
    path: Path
    platform_kind: Literal["windows", "linux", "macos"]
    audit_engine_fingerprint: str
    uninspected_configuration_digest: str
    bounded_payload_sha256: str

@dataclass(frozen=True, slots=True)
class WorkerRuntimeContract:
    workers: int
    maximum_tasks_per_worker: int
    recycle_rss_bytes: int
    pipeline_deadline: float

class CompilerLaunchPurpose(enum.Enum):
    INSPECTION = "inspection"
    AUDIT_DISCOVERY = "audit-discovery"
    AUDIT_ACCEPTED = "audit-accepted"

@dataclass(frozen=True, slots=True)
class ProcessStartIdentity:
    pid: int
    native_creation_token: str

@dataclass(frozen=True, slots=True)
class CompilerLaunchEvent:
    purpose: CompilerLaunchPurpose
    configuration_digest: str | None
    worker_index: int | None
    generation: int | None
    task_id: int | None
    pid: int
    process_start_identity: ProcessStartIdentity
    windows_process_handle_cookie: str | None

@dataclass(frozen=True, slots=True)
class WindowsWorkerContainment:
    duplicated_job_handle: int
    expected_job_cookie: str

@dataclass(frozen=True, slots=True)
class MacOSWorkerContainment:
    trusted_compiler_driver_fingerprints: tuple[str, ...]

@dataclass(frozen=True, slots=True)
class LinuxWorkerContainment:
    cgroup_run_path: str
    cgroup_leaf_path: str
    generation_token: str

WorkerContainment = (
    WindowsWorkerContainment | LinuxWorkerContainment | MacOSWorkerContainment
)

@dataclass(frozen=True, slots=True)
class WorkerContained:
    worker_index: int
    generation: int
    containment_identity: str

@dataclass(frozen=True, slots=True)
class MacOSWorkerSessionReported:
    worker_index: int
    generation: int
    pid: int
    pgid: int
    bsd_start_identity: str

@dataclass(frozen=True, slots=True)
class CompilerPgidReported:
    worker_index: int
    generation: int
    task_id: int
    purpose: Literal[CompilerLaunchPurpose.AUDIT_DISCOVERY,
                     CompilerLaunchPurpose.AUDIT_ACCEPTED]
    pid: int
    pgid: int
    bsd_start_identity: str
    executable_identity: FileIdentity
    executable_sha256: str
    driver_fingerprint: str

@dataclass(frozen=True, slots=True)
class CompilerExecPermit:
    worker_index: int
    generation: int
    task_id: int
    pgid: int

@dataclass(frozen=True, slots=True)
class MacOSInspectionPgidReported:
    inspection_id: str
    pid: int
    pgid: int
    bsd_start_identity: str
    executable_identity: FileIdentity
    executable_sha256: str

@dataclass(frozen=True, slots=True)
class MacOSInspectionExecPermit:
    inspection_id: str
    pid: int
    pgid: int
    driver_fingerprint: str

@dataclass(frozen=True, slots=True)
class WorkerEngineReady:
    worker_index: int
    generation: int
    recomputed_audit_engine_fingerprint: str

@dataclass(frozen=True, slots=True)
class PendingAuditTask:
    task_id: int
    configuration: PreprocessConfiguration
    deadline: float

@dataclass(frozen=True, slots=True)
class WorkerTask:
    task_id: int
    configuration: PreprocessConfiguration
    deadline: float
    compact_reservation: PerTaskCompactReservation
    publication_entitlement_bytes: int
    dependency_root_authority_digest: str
    # Not serialized. decode_task_frame(payload, expected_authority) validates
    # the wire digest then binds this field to the child's already-held object.
    dependency_root_authority: DependencyRootAuthority = field(compare=False)
    compiler_capability_digest: str
    compiler_capability: CompilerExecutableCapability = field(compare=False)

@dataclass(slots=True)
class GenerationState:
    worker_index: int
    generation: int
    tree: OwnedProcessTree
    containment: WorkerContainment
    task_sender: BoundedFrameSender
    command_sender: BoundedFrameSender
    control_receiver: BoundedFrameReceiver
    payload_receiver: BoundedPayloadReceiver
    live: bool

@dataclass(frozen=True, slots=True)
class WorkerPayloadReady:
    worker_index: int
    generation: int
    task_id: int
    encoded_bytes: int
    encoded_sha256: str
    conservative_decoded_bytes: int
    conservative_retained_bytes: int
    counting_pass_peak_bytes: int
    stdout_bytes: int
    stages: WorkerStageTimings

@dataclass(frozen=True, slots=True)
class WorkerPayloadPermit:
    worker_index: int
    generation: int
    task_id: int

@dataclass(frozen=True, slots=True)
class WorkerFailure:
    worker_index: int
    generation: int
    task_id: int
    diagnostic: str

@dataclass(frozen=True, slots=True)
class WorkerRetire:
    worker_index: int
    generation: int
    reason: str

@dataclass(frozen=True, slots=True)
class WorkerRetireAck:
    worker_index: int
    generation: int

@dataclass(frozen=True, slots=True)
class WorkerStop:
    worker_index: int
    generation: int

@dataclass(frozen=True, slots=True)
class WorkerStopped:
    worker_index: int
    generation: int

@dataclass(frozen=True, slots=True)
class StreamingAuditMeasurements:
    pipeline_started_at: float
    elapsed_seconds: float
    configuration_count: int
    cache_hits: int
    cache_misses: int
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    memory: PlatformRunMemoryMeasurements
    cache_bytes: int
    cache_entries: int
    compact_result_peak_live_bytes: int
    compact_result_retained_bytes: int
    maximum_encoded_result_bytes: int
    maximum_conservative_decoded_bytes: int
    maximum_conservative_retained_bytes: int
    runtime_contract: WorkerRuntimeContract
    cache_root: Path
    worker_counts_started: tuple[int, ...]
    included_stages: tuple[str, ...]
    inspection_phase_snapshot: "NativePhaseSnapshot"
    task_phase_snapshot: "NativePhaseSnapshot"
    @property
    def memory_backend(self) -> Literal["windows", "linux", "macos"]: ...

@dataclass(frozen=True, slots=True)
class CompilerAuditRun:
    summary: "CanonicalAuditContentSummary"
    measurements: StreamingAuditMeasurements

@dataclass(frozen=True, slots=True)
class CanonicalAuditContentSummary:
    canonical_sha256: str
    configuration_digests: tuple[str, ...]
    finding_count: int
    authoritative_path_count: int
    source_only_path_count: int

OUTER_EVIDENCE_OVERHEAD_BYTES = 8 * 1024 * 1024

class OuterEvidenceMemoryBudget(Protocol):
    # Covers the bounded 251-digest/count-only summary, cold/warm evidence
    # records, canonical machine record and codec scratch; compact aggregate
    # collections never transfer into this owner.
    def reserve_evidence_subbudget(self, exact_bytes: int,
                                   deadline: float) -> "EvidenceSuballocator": ...
    def abort(self, cleanup_deadline: float) -> None: ...

class EvidenceSuballocator(Protocol):
    maximum_bytes: Literal[524288]
    def reserve_summary(self, configuration_count: int,
                        deadline: float) -> object: ...
    def commit_summary(self, reservation: object,
                       summary: CanonicalAuditContentSummary) -> None: ...
    def release_summary(self, summary: CanonicalAuditContentSummary) -> None: ...
    def reserve_builder(self, deadline: float) -> object: ...
    def reserve_machine_record_and_scratch(self, maximum_bytes: int,
                                           deadline: float) -> object: ...
    def transfer_to_evidence(self, deadline: float) -> "EvidenceOwnership": ...
    def abort(self, cleanup_deadline: float) -> None: ...

class EvidenceOwnership(Protocol):
    # Holds the suballocator and its parent outer budget after successful
    # builder finish. close() is mandatory after serialization/consumption.
    def close(self, deadline: float) -> None: ...

@dataclass(frozen=True, slots=True)
class StreamingResultAggregate:
    ordered_configuration_digests: tuple[str, ...]
    canonical_result_digest: str
    authoritative_coverage: frozenset[PurePosixPath]
    aggregated_findings: tuple[AggregatedFinding, ...]
    budget_ownership: CompactBudgetOwnership

@dataclass(frozen=True, slots=True)
class StreamingAggregateGrowth:
    configuration_index_bytes: int
    coverage_index_bytes: int
    finding_index_bytes: int
    digest_state_bytes: int
    merge_working_bytes: int

@dataclass(frozen=True, slots=True)
class WindowsAccountantSnapshot:
    memory: WindowsRunMemoryMeasurements
    compact_result_peak_live_bytes: int
    accounting_complete: bool

@dataclass(frozen=True, slots=True)
class WindowsPhaseSnapshot:
    platform_kind: Literal["windows"]
    phase: Literal["inspection", "tasks"]
    memory: WindowsRunMemoryMeasurements
    archived_generation_count: int

@dataclass(frozen=True, slots=True)
class LinuxPhaseSnapshot:
    platform_kind: Literal["linux"]
    phase: Literal["inspection", "tasks"]
    memory: LinuxRunMemoryMeasurements
    archived_generation_count: int

@dataclass(frozen=True, slots=True)
class MacOSPhaseSnapshot:
    platform_kind: Literal["macos"]
    phase: Literal["inspection", "tasks"]
    memory: MacOSRunMemoryMeasurements
    archived_generation_count: int

NativePhaseSnapshot = (
    WindowsPhaseSnapshot | LinuxPhaseSnapshot | MacOSPhaseSnapshot
)

@dataclass(frozen=True, slots=True)
class AcceptanceEvidence:
    cold_summary: CanonicalAuditContentSummary
    cold_measurements: StreamingAuditMeasurements
    cold_end_snapshot: WindowsAccountantSnapshot
    warm_summary: CanonicalAuditContentSummary
    warm_measurements: StreamingAuditMeasurements
    outer_snapshot: WindowsAccountantSnapshot
    cache_child: Path
    schema_version: int
    ownership: EvidenceOwnership

    def close(self, deadline: float) -> None:
        self.ownership.close(deadline)

class BoundedAcceptanceEvidenceBuilder(Protocol):
    # Lifecycle is OPEN -> COLD_RECORDED -> WARM_RECORDED -> FINISHED; every
    # out-of-order/duplicate call fails. add_cold retains the exact `cold_end`
    # object supplied by the verifier (identity, not reconstructed measurements).
    def add_cold(self, summary: CanonicalAuditContentSummary,
                 measurements: StreamingAuditMeasurements,
                 cold_end: WindowsAccountantSnapshot,
                 deadline: float) -> None: ...
    def add_warm(self, summary: CanonicalAuditContentSummary,
                 measurements: StreamingAuditMeasurements,
                 deadline: float) -> None: ...
    def finish(self, cache_child: Path,
               monitor: WindowsAccountantSnapshot,
               evidence_suballocator: EvidenceSuballocator,
               deadline: float) -> AcceptanceEvidence: ...
    # finish() commits the final builder state and atomically calls
    # evidence_suballocator.transfer_to_evidence(); the returned evidence owns
    # that closeable token. After transfer, builder.abort() and allocator.abort()
    # are invalid, and only AcceptanceEvidence.close() releases success state.
    def abort(self, cleanup_deadline: float) -> None: ...

class WindowsWholeOrchestratorMonitor(Protocol):
    # Lifecycle is STARTED -> COLD_SNAPSHOTTED -> FINISHED, or ABORTED. All
    # registered launches, snapshots and the returned evidence use the exact
    # RunScopedNativeAccountant object given to start().
    @classmethod
    def start(cls, accountant: "RunScopedNativeAccountant", deadline: float,
              first_slice_deadline: float) -> "WindowsWholeOrchestratorMonitor": ...
    @property
    def accountant(self) -> "RunScopedNativeAccountant": ...
    def register_process(self, launch: CompilerLaunchEvent,
                         process_handle: int, deadline: float) -> None: ...
    def snapshot(self, deadline: float) -> WindowsAccountantSnapshot: ...
    def finish(self, deadline: float) -> WindowsAccountantSnapshot: ...
    def abort(self, cleanup_deadline: float) -> None: ...

class _ScheduledAuditSession(Protocol):
    cache_hits: int
    cache_misses: int
    inspection_probe_invocations: int
    audit_compiler_invocations: int
    stdout_bytes: int
    memory: PlatformRunMemoryMeasurements
    cache_bytes: int
    cache_entries: int
    compact_result_peak_live_bytes: int
    compact_result_retained_bytes: int
    maximum_encoded_result_bytes: int
    maximum_conservative_decoded_bytes: int
    maximum_conservative_retained_bytes: int
    runtime_contract: WorkerRuntimeContract
    cache_root: Path
    worker_counts_started: tuple[int, ...]
    included_stages: tuple[str, ...]
    pending_result_reference_count: int
    compact_result_budget: CompactResultMemoryBudget
    def consume_aggregate(self) -> "StreamingResultAggregate": ...
    def shutdown_reap(self, pipeline_deadline: float) -> None: ...
    def abort_and_reap(self, emergency_cleanup_deadline: float) -> None: ...

@dataclass(frozen=True, slots=True)
class PreprocessDiscovery:
    stream: StreamDigest
    dependencies: tuple[FileIdentity, ...]
    snapshots: tuple[DependencyDigest, ...]

@dataclass(frozen=True, slots=True)
class ProductionSourceSnapshot:
    identity: FileIdentity
    sha256: str
    content: bytes

class HeldProductionSnapshot(Protocol):
    @property
    def initial_digest_map(
        self,
    ) -> Mapping[PurePosixPath, DependencyDigest]: ...
    def __enter__(self) -> "HeldProductionSnapshot": ...
    def finalize_policy_boundary(
        self,
        pipeline_deadline: float,
    ) -> Mapping[PurePosixPath, ProductionSourceSnapshot]: ...
    def __exit__(self, exc_type, exc, traceback) -> None: ...
```

Compact result limits are fixed: encoded payload 4 MiB, 16,384 dependencies,
4,096 reached paths, 65,536 findings, 4,096 UTF-8 bytes per path, 16,384 UTF-8
bytes per expression/reason, and 16 MiB conservative decoded allocation per
entry. One shared compact-result memory contract applies to warm cache loads,
mixed hit/miss runs, and fully cold worker transport: at most 128 MiB is live
across compact result objects and their count/encode/publication/pipe/receive,
transient decoded, and retained immutable phases. Full views, audit buffers,
analysis/provenance/token state, and `retained_token_bytes` are excluded and
belong to OS-owned worker process memory. Read or receive at most encoded-limit-plus-one, run
the allocation-free canonical-JSON preparse, and reserve its conservative
decoded and retained upper bounds before `json.loads`; every collection/string
bound is validated before immutable result construction. Queued and transient
charges are released only after the validated immutable result replaces them,
while its retained charge remains through policy completion.

- `audit_engine_fingerprint() -> str`
- `_attest_loaded_audit_engine(expected: str | None = None) -> str`, which always recomputes from the fixed loaded-object manifest and rejects a differing expected value
- `decision_engine_fingerprint(expected_audit_engine_fingerprint: str | None = None) -> str`, which first calls `_attest_loaded_audit_engine()` and rejects a differing expected value
- `classify_command_rewrite(raw_arguments: tuple[str, ...], cwd: Path, compiler_family: CompilerFamily) -> CommandRewriteClassification`
- `rewrite_preprocess_command(configuration: PreprocessConfiguration, dependency_output: Path) -> RewrittenCommand`
- `decision_environment_digest(compiler_family: CompilerFamily, effective_environment: Mapping[str, str], explicit_launcher_assignments: Mapping[str, str], cwd: Path, working_directory_role: str, source_root: Path, build_root: Path, toolchain_root: Path) -> str`
- `normalize_decision_arguments(rewritten: RewrittenCommand, cwd: Path, working_directory_role: str, source_root: Path, build_root: Path, toolchain_root: Path) -> tuple[str, ...]`
- `compiler_digest(compiler_family: CompilerFamily, executable: Path, normalized_version: str) -> str`
- `build_production_identity_authority(source_root: Path, production_globs: tuple[str, ...]) -> ProductionIdentityAuthority`
- `encode_production_identity_authority(authority: ProductionIdentityAuthority) -> bytes` and `decode_production_identity_authority(payload: bytes) -> ProductionIdentityAuthority`, an exact ordered local codec
- `encode_acceptance_evidence(evidence: AcceptanceEvidence) -> bytes` and `decode_acceptance_evidence(payload: bytes) -> AcceptanceEvidence`, an exact bounded codec that rejects unknown/missing fields and preserves the separately recorded cold-end and outer snapshots
- `build_dependency_root_authority(source_root: Path, external_roots: Mapping[str, Path]) -> DependencyRootAuthority`; freezes exact roles/resolved roots/native root identities and portable role-layout digest, rejecting overlap/ambiguity
- `encode_dependency_root_authority(authority: DependencyRootAuthority) -> bytes` and `decode_dependency_root_authority(payload: bytes) -> DependencyRootAuthority`, exact ordered local codec with all binding/root identity fields
- `create_preprocess_configuration(..., dependency_roots: DependencyRootAuthority) -> PreprocessConfiguration`; the mandatory authority digest is length-framed into the exact semantic digest, and direct construction without it is rejected
- `portable_dependency_key(dependency: DependencyDigest) -> bytes`
- `encode_local_dependency_digest(dependency: DependencyDigest) -> bytes` and `decode_local_dependency_digest(payload: bytes) -> DependencyDigest`
- `collect_configurations_with_decision_records(source_root: Path, compile_commands: tuple[Path, ...], launcher_environment: Mapping[str, str], production: Mapping[PurePosixPath, FileIdentity], dependency_roots: DependencyRootAuthority, capability_registry: CompilerCapabilityRegistry, inspection_cache: CompilerInspectionCache, expected_audit_engine_fingerprint: str, limits: AuditLimits, pipeline_deadline: float, launch_accountant: RunScopedNativeAccountant) -> ConfigurationCollection`; caller owns the registry, result preserves its identity, and the function re-attests on entry
- `inspect_compiler(compiler: Path, compiler_family: CompilerFamily, launcher_environment: Mapping[str, str], dependency_roots: DependencyRootAuthority, inspection_cache: CompilerInspectionCache, expected_audit_engine_fingerprint: str, pipeline_deadline: float, limits: AuditLimits, launch_accountant: RunScopedNativeAccountant) -> CompilerInspection`; re-attests immediately before inspection-cache lookup and registers/contains every probe before launch
- `open_compiler_executable_capability(inspection: CompilerInspection, dependency_roots: DependencyRootAuthority, pipeline_deadline: float) -> CompilerExecutableCapability`; validates the trusted toolchain root. Windows retains the validated file/directory handles, Linux launches the exact hashed fd through the fixed `fexecve`/`execveat(AT_EMPTY_PATH)` helper, and macOS uses an exact-fd exec helper only when the runner proves support; otherwise it honestly fails closed or uses the reviewed trusted path-chain guard without claiming fd-bound exec. Path-only unguarded launch is forbidden.
- `CompilerCapabilityRegistry.create(deadline: float) -> CompilerCapabilityRegistry`; the closeable owner deduplicates by exact capability digest/local identity, reference-counts collection/configuration owners, creates only explicit per-generation native duplicates, marks each duplicate transferred exactly once, and rejects double transfer/release/close or use after close
- `preflight_dependency_generation_guards(discovery: PreprocessDiscovery, dependency_roots: DependencyRootAuthority, limits: AuditLimits) -> GenerationGuardPreflight`; deduplicates the complete directory chains from each authority root's required root-parent sentinel through every dependency parent, checks unique file/directory owner/watch ceilings and exact metadata bytes with checked arithmetic, and fails before the first open/watch
- `arm_dependency_generation_guards(preflight: GenerationGuardPreflight, dependency_roots: DependencyRootAuthority, deadline: float) -> tuple[DependencyGenerationGuard, ...]` and `validate_and_hash_guarded_dependencies(guards: tuple[DependencyGenerationGuard, ...], deadline: float) -> tuple[DependencyDigest, ...]`; owns every deduplicated directory component and declared file, binds pre/post path↔owner identity/full stat, drains all component events before hashing the held fd, and closes all owners on every path
- `compiler_inspection_cache_key(compiler: Path, compiler_family: CompilerFamily, launcher_environment: Mapping[str, str], dependency_roots: DependencyRootAuthority, expected_audit_engine_fingerprint: str) -> str`, plus exact manifest encode/decode functions that carry `dependency_roots.portable_authority_digest` and require the caller's local authority bindings before load/publish
- `configuration_set_digest(configurations: tuple[PreprocessConfiguration, ...], decision_records: Mapping[str, DecisionConfigurationRecord], dependency_roots: DependencyRootAuthority) -> str`
- `effective_worker_capacity(limits: AuditLimits) -> int`
- `reference_calibration_envelope() -> ReferenceCalibrationEnvelope`
- `constrain_reference_affinity(envelope: ReferenceCalibrationEnvelope) -> None`
- `runtime_contract_from_decision(decision: WorkerCountDecision, pipeline_deadline: float) -> WorkerRuntimeContract`
- `load_platform_worker_decision(path: Path, expected_platform: Literal["windows", "linux", "macos"], expected_key: WorkerDecisionKey) -> PlatformWorkerDecision`; strict discriminated loading rejects generic/foreign/unknown fields
- `prevalidate_platform_worker_decision(path: Path, expected_platform: Literal["windows", "linux", "macos"], expected_audit_engine_fingerprint: str, uninspected_configuration_digest: str, deadline: float) -> PrevalidatedWorkerDecision`; bounded structural/schema/platform/audit/config validation performs no process launch or compiler inspection
- `finalize_platform_worker_decision(prevalidated: PrevalidatedWorkerDecision, expected_key: WorkerDecisionKey, deadline: float) -> PlatformWorkerDecision`; rechecks the same bounded payload hash and exact final inspected key before any audit worker/compiler launch
- `runtime_contract_from_platform_decision(decision: PlatformWorkerDecision, pipeline_deadline: float) -> WorkerRuntimeContract`; copies only the selected sample's measured worker/recycle parameters
- `RunScopedNativeAccountant.begin_phase(phase: Literal["inspection", "tasks"], deadline: float) -> None` and `seal_phase(phase: Literal["inspection", "tasks"], deadline: float) -> NativePhaseSnapshot`; sealing uses the native backend's authoritative set/events, includes all archived generation maxima, drains notifications, and returns the exact discriminated immutable snapshot only after stable empty/equal reconciliation. Any delayed process/member/event or unequal identity count keeps draining or fails at the deadline.
- `schedule_configuration_audits(source_root: Path, configurations: tuple[PreprocessConfiguration, ...], dependency_roots: DependencyRootAuthority, capability_registry: CompilerCapabilityRegistry, initial_digest_map: Mapping[PurePosixPath, DependencyDigest], cache: ConfigurationAuditCache, limits: AuditLimits, expected_audit_engine_fingerprint: str, runtime_contract: WorkerRuntimeContract, inspection_probe_invocations: int, run_accountant: RunScopedNativeAccountant) -> _ScheduledAuditSession`; re-attests immediately before result-cache lookup and preserves registry/accountant identity through reactor/tree/measurements
- `decode_validated_production_sources(sources: Mapping[PurePosixPath, ProductionSourceSnapshot], limits: AuditLimits, pipeline_deadline: float) -> Mapping[PurePosixPath, str]`
- `allowed_worker_counts(effective_worker_capacity: int) -> tuple[int, ...]`
- `worker_decision_key(platform_tag: str, compiler_digest: str, configuration_set_digest: str, uninspected_configuration_digest: str, decision_engine_fingerprint: str, effective_worker_capacity: int) -> WorkerDecisionKey`
- `canonical_platform_kind(sys_platform: str) -> Literal["windows", "linux", "macos"]`; exactly maps `win32` to `windows`, `linux` to `linux`, and `darwin` to `macos`, rejecting every other value
- `windows_calibration_envelope() -> ReferenceCalibrationEnvelope`, `linux_calibration_envelope() -> LinuxCalibrationEnvelope`, and `macos_calibration_envelope() -> MacOSCalibrationEnvelope`; each exact envelope is canonical-encoded into its decision key
- `platform_worker_decision_key(platform: Literal["windows", "linux", "macos"], compiler_digest: str, configuration_set_digest: str, uninspected_configuration_digest: str, decision_engine_fingerprint: str, effective_worker_capacity: int, envelope: ReferenceCalibrationEnvelope | LinuxCalibrationEnvelope | MacOSCalibrationEnvelope) -> WorkerDecisionKey`
- `load_worker_decision(path: Path, expected: WorkerDecisionKey) -> WorkerCountDecision`
- `snapshot_production_sources(production: Mapping[PurePosixPath, FileIdentity], limits: AuditLimits, pipeline_deadline: float) -> HeldProductionSnapshot`
- `audit_cache_key(configuration_digest: str, audit_engine_fingerprint: str) -> str`
- `conservative_allocation_schema() -> ConservativeAllocationSchema`
- `_transport_allocation_bound(encoded_bytes: int, bounds: CompactResultPreparseBounds, schema: ConservativeAllocationSchema, pipe_kernel_capacity_bytes: int) -> TransportAllocationLayout`
- `stream_compact_result(result: ConfigurationAuditResult, consumer: Callable[[bytes], None]) -> StreamDigest`
- `encode_compact_result(result: ConfigurationAuditResult) -> bytes`
- `preparse_compact_result(payload: bytes) -> CompactResultPreparseBounds`
- `decode_compact_result(payload: bytes, bounds: CompactResultPreparseBounds) -> ConfigurationAuditResult`
- `maximum_compact_result_slot(limits: AuditLimits, schema: ConservativeAllocationSchema, pipe_kernel_capacity_bytes: int) -> CompactSlotTemplate`
- `StreamingResultAggregator(configurations: tuple[PreprocessConfiguration, ...], result_budget: CompactResultMemoryBudget, limits: AuditLimits)` with `accept_validated_result(configuration: PreprocessConfiguration, result: ConfigurationAuditResult, ownership: CompactBudgetOwnership) -> None` and `finish() -> StreamingResultAggregate`; before mutating any index it preparses the result into an exact checked `StreamingAggregateGrowth`, reserves that growth, then inserts only the next configuration order and releases result ownership. `finish()` freezes/moves the charged aggregate backing structures in place and returns their ownership; it does not own the later small canonical summary.
- `CompactResultMemoryBudget(maximum_bytes: int, observer: CompactAccountingObserver)` with exact `reserve_cache_encoded(entry_id: str, encoded_bytes: int)`, `transition_cache_entry_to_decode(entry_id: str, conservative_decoded_bytes: int, conservative_retained_bytes: int)`, `commit_cache_retained(entry_id: str)`, `capture_ownership(owner: str) -> CompactBudgetOwnership`, `transfer_ownership(ownership: CompactBudgetOwnership, from_owner: str, to_owner: str) -> CompactBudgetOwnership`, `try_reserve_before_dispatch(task_id: int, reservation: PerTaskCompactReservation) -> bool`, `activate_root_publication(task_id: int, generation: int) -> CachePublicationPermit`, `require_within_pre_dispatch_reservation(task_id: int, encoded_bytes: int, bounds: CompactResultPreparseBounds, counting_pass_peak_bytes: int) -> None`, `release_root_publication_after_atomic_cleanup(task_id: int)`, `transition_to_decode(task_id: int, conservative_decoded_bytes: int, conservative_retained_bytes: int)`, `commit_retained_result(task_id: int)`, `release_pending(task_id: int)`, `release_retained_owner(ownership: CompactBudgetOwnership)`, `reserve_aggregate_growth(ownership: CompactBudgetOwnership, growth: StreamingAggregateGrowth)`, `commit_aggregate_growth(ownership: CompactBudgetOwnership)`, `prevalidate_aggregate_release(ownership: CompactBudgetOwnership)`, and `release_aggregate_after_summary(ownership: CompactBudgetOwnership) -> None` operations plus `owns(ownership)`, read-only `remaining_bytes`, `retained_bytes`, and `peak_live_bytes`. Release is prevalidated and atomic: failure leaves the token/live charge unchanged, success invalidates it completely. Every configuration-digest tuple slot, coverage path/set entry, finding key and configuration-digest set, and hash state is charged to this 128-MiB budget. The small final `CanonicalAuditContentSummary` is explicitly excluded and charged to the separate exact 8-MiB outer-evidence overhead. Every transition rejects stale generations, duplicate release, overlapping ownership, and checked-size overflow.
- `encode_control_message(message: WorkerContained | MacOSWorkerSessionReported | CompilerPgidReported | MacOSInspectionPgidReported | WorkerEngineReady | CachePublicationRequested | WorkerPayloadReady | WorkerFailure | WorkerRetire | WorkerStopped | CompilerLaunchEvent) -> bytes`
- `preparse_control_message(payload: bytes) -> ControlWireBounds`, an allocation-free fixed-schema preparse
- `decode_control_message(payload: bytes, bounds: ControlWireBounds) -> WorkerContained | MacOSWorkerSessionReported | CompilerPgidReported | MacOSInspectionPgidReported | WorkerEngineReady | CachePublicationRequested | WorkerPayloadReady | WorkerFailure | WorkerRetire | WorkerStopped | CompilerLaunchEvent`
- `recv_control_message_ready(receiver: BoundedFrameReceiver, expected_generation: int, pipeline_deadline: float) -> object`
- `encode_task_frame(task: WorkerTask) -> bytes` serializes only authority/capability digests, never native root paths/identity/owners; `decode_task_frame(payload: bytes, expected_authority: DependencyRootAuthority, expected_capabilities: Mapping[str, CompilerExecutableCapability]) -> WorkerTask` validates exact digests and binds the authority/capability fields to the child's already-held local objects, with exact schema, aggregate argument bounds and `TASK_MAX_BYTES`. Parent/child object identity is never compared across processes.
- `encode_worker_startup_bootstrap(bootstrap: WorkerStartupBootstrap, authentication_key: bytes) -> bytes` and `decode_worker_startup_bootstrap(payload: bytes, authentication_key: bytes) -> WorkerStartupBootstrap`; exact capped schema carries every local `DependencyRootAuthority` binding/root identity plus capability digests in an authenticated blob, rejects unknown/duplicate/over-limit fields, and is delivered once over a dedicated bounded endpoint transferred only with `DupHandle`/`DupFd`. It is never a multiprocessing pickle.
- `_spawn_worker_generations(..., dependency_roots: DependencyRootAuthority, startup_bootstraps: tuple[WorkerStartupBootstrap, ...], capability_registry: CompilerCapabilityRegistry, ...) -> tuple[GenerationState, ...]`; starts the child with only fixed transferred endpoint wrappers, obtains its stable PID/process handle, then transfers registry duplicates and waits for authenticated child ACK before readiness
- `encode_command_frame(command: CachePublicationPermit | WorkerPayloadPermit | CompilerExecPermit | MacOSInspectionExecPermit | WorkerRetireAck | WorkerStop) -> bytes` and `decode_command_frame(payload: bytes) -> object`, with exact schema and `COMMAND_MAX_BYTES`
- `BoundedFrameChannel.create(maximum_frame_bytes: int, chunk_bytes=64 * 1024) -> tuple[BoundedFrameReceiver, BoundedFrameSender]`: Windows uses overlapped named pipes with deadline waits and `CancelIoEx`; POSIX uses nonblocking `AF_UNIX/SOCK_STREAM` plus selectors. Both send a capped length/schema header and deadline-aware chunks no larger than the queried/verified transport capacity, so no datagram-atomic-size assumption exists; close-on-cancel wakes peers.
- `send_frame_ready(sender: BoundedFrameSender, payload: bytes, pipeline_deadline: float) -> None`, which performs an OS-native cancellable/nonblocking send under the absolute deadline; `receive_frame_ready(receiver, maximum_bytes, deadline)` bounds before decode. Generic `Connection` writable readiness is never assumed.
- `BoundedPayloadChannel.create(maximum_payload_bytes=4 * 1024 * 1024, chunk_bytes=64 * 1024) -> tuple[BoundedPayloadReceiver, BoundedPayloadSender]`: Windows overlapped byte pipe and POSIX nonblocking `SOCK_STREAM` send an authenticated length/hash header followed by deadline/cancel-aware chunks; receive allocates only after bounded header preparse and reads exact chunks.
- `discover_configuration(configuration: PreprocessConfiguration, dependency_roots: DependencyRootAuthority, production: Mapping[PurePosixPath, FileIdentity], limits: AuditLimits, deadline: float, cancel_event: CancellationSignal | None = None) -> PreprocessDiscovery`
- `stabilize_and_parse_configuration(configuration: PreprocessConfiguration, dependency_roots: DependencyRootAuthority, production: Mapping[PurePosixPath, FileIdentity], limits: AuditLimits, deadline: float, cancel_event: CancellationSignal | None = None) -> tuple[PreprocessedTranslationUnitView, PreprocessDiscovery, PreprocessStageTimings]`
- `ConfigurationAuditCache(root: Path)`
- `ConfigurationAuditCache.prepare(pipeline_deadline: float) -> None`
- `ConfigurationAuditCache.root_identity: CacheRootIdentity`, available only after `prepare()`
- `ConfigurationAuditCache.inspection_cache() -> CompilerInspectionCache`, valid only after `prepare()` and bound to the prepared `CacheRootIdentity`
- `ConfigurationAuditCache.load(configuration: PreprocessConfiguration, dependency_roots: DependencyRootAuthority, expected_audit_engine_fingerprint: str, production_snapshot: Mapping[PurePosixPath, DependencyDigest], pipeline_deadline: float) -> ConfigurationAuditResult | None`
- `ConfigurationAuditCache.load_many(configurations: tuple[PreprocessConfiguration, ...], dependency_roots: DependencyRootAuthority, expected_audit_engine_fingerprint: str, production_snapshot: Mapping[PurePosixPath, DependencyDigest], result_budget: CompactResultMemoryBudget, aggregator: StreamingResultAggregator, cold_slot: CompactSlotTemplate | None, pipeline_deadline: float) -> CacheLoadBatch`; if any miss exists it reserves `cold_slot` before decoding the first hit, streams hits in configuration order into the aggregator, and never returns hit objects
- `ConfigurationAuditCache.publish(configuration: PreprocessConfiguration, dependency_roots: DependencyRootAuthority, result: ConfigurationAuditResult, publication_permit: CachePublicationPermit, pipeline_deadline: float) -> ConfigurationAuditResult`
- `audit_configuration_worker(task: WorkerTask, dependency_roots: DependencyRootAuthority, production_snapshot: Mapping[PurePosixPath, DependencyDigest], control_endpoint: BoundedFrameSender, command_endpoint: BoundedFrameReceiver, deadline: float) -> ConfigurationAuditOutcome`
- `_audit_worker_main(worker_index: int, generation: int, source_root: Path, cache_root: Path, startup_receiver: BoundedFrameReceiver, startup_authentication_key: bytes, limits: AuditLimits, parent_engine: str, production_snapshot: Mapping[PurePosixPath, DependencyDigest], containment: WorkerContainment, control_sender: BoundedFrameSender, registration_sender: BoundedFrameSender | None, launch_sender: BoundedFrameSender, payload_sender: BoundedPayloadSender, cancel_event: CancellationSignal, task_receiver: BoundedFrameReceiver, command_receiver: BoundedFrameReceiver, rss_slots: object) -> None`; no native capability owner/map is a spawn argument. The child receives capabilities only after start over the authenticated startup transport, ACKs exact digests/count, validates before readiness, and binds tasks to those local objects.
- `_snapshot_rss_slots(rss_slots: object, worker_count: int) -> tuple[WorkerTelemetry, ...]`
- `calibrate_platform_worker_count(platform: Literal["windows", "linux", "macos"], source_root: Path, compile_commands: tuple[Path, ...], cache_parent: Path, limits: AuditLimits) -> PlatformWorkerDecision`
- `_calibrate_platform_worker_count_with_runner(platform: Literal["windows", "linux", "macos"], runner: CalibrationRunner, source_root: Path, compile_commands: tuple[Path, ...], cache_parent: Path, limits: AuditLimits) -> PlatformWorkerDecision`, the only injected test seam; production constructs the native real spawn runner and delegates once
- `run_compiler_audit_pipeline(source_root: Path, compile_commands: tuple[Path, ...], launcher_environment: Mapping[str, str], production: Mapping[PurePosixPath, FileIdentity], dependency_roots: DependencyRootAuthority, capability_registry: CompilerCapabilityRegistry, cache: ConfigurationAuditCache, inspection_cache: CompilerInspectionCache, limits: AuditLimits, mode: str, decision_path: Path, operation_deadline: float, run_accountant: RunScopedNativeAccountant, evidence_suballocator: EvidenceSuballocator, pipeline_started_at: float, expected_audit_engine_fingerprint: str | None = None) -> CompilerAuditRun`; no ambient reconstruction is allowed. `pipeline_started_at` is caller-owned and precedes production enumeration; measurements use it verbatim.
- `emergency_cleanup_deadline() -> float`, callable only after failure/expiry and returning `time.monotonic() + 30.0`

### Task 1: Lock the compact result model and audit-engine identity

**Files:**
- Modify: `tests/gpu/gpu_capability_model.py`
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Modify: `tests/gpu/test_gpu_capability_model.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Task 1 owns exactly `DependencyDigest(stable_role, role_relative_path, identity, sha256)`, `AuditResultFinding(path, line, expression, reason)`, `ConfigurationAuditResult(configuration_digest, audit_engine_fingerprint, dependencies, reached_production, findings)`, `AUDIT_RESULT_SCHEMA_BYTES`, `AUDIT_ENGINE_GRAPH_SCHEMA_BYTES`, the Task-1 stage/root/exclusion constants, local dependency codec, portable dependency key, and `audit_engine_fingerprint()`. Cache schema/codecs are Task 3-local; `CompilerInspection` is Task 2; later carriers belong to their introducing tasks.

- [ ] **Step 1: Write failing model and fingerprint tests**

```python
def test_configuration_audit_result_is_compact_frozen_and_sorted(self):
    result = ConfigurationAuditResult(
        configuration_digest="c" * 64,
        audit_engine_fingerprint="a" * 64,
        dependencies=(DependencyDigest(
            stable_role="production",
            role_relative_path=PurePosixPath("playback/gpu/a.h"),
            identity=self.production_file_identity("playback/gpu/a.h"),
            sha256="b" * 64),),
        reached_production=(PurePosixPath("playback/gpu/a.h"),),
        findings=(AuditResultFinding(
            path=PurePosixPath("playback/gpu/a.h"), line=7,
            expression="nativeHandle()", reason="outside lease"),))
    self.assertFalse(hasattr(result, "tokens"))
    with self.assertRaises(dataclasses.FrozenInstanceError):
        result.configuration_digest = "other"

def test_dependency_digest_portable_key_and_exact_local_codec_are_separate(self):
    digest = self.dependency_digest_with_every_file_identity_field()
    relocated = self.relocated_same_role_path_and_bytes(digest)
    self.assertEqual(portable_dependency_key(digest),
                     portable_dependency_key(relocated))
    encoded = encode_local_dependency_digest(digest)
    self.assertEqual(tuple(self.decoded_object_pairs(encoded)), (
        "stable_role", "role_relative_path", "canonical", "relative",
        "device", "inode", "line_count", "production", "sha256"))
    self.assertEqual(decode_local_dependency_digest(encoded), digest)
    for mutation in ("missing-field", "unknown-field", "reordered-field",
                     "wrong-type", "replacement-identity"):
        with self.subTest(mutation=mutation), self.assertRaises(
                AuditInfrastructureError):
            self.decode_or_validate_local_dependency_mutation(encoded, mutation)

def test_audit_engine_fingerprint_changes_for_every_framed_component(self):
    baseline = audit_engine_fingerprint()
    graph = _enumerate_live_semantic_graph()
    self.assertEqual(tuple(name for name, _ in graph),
                     tuple(sorted(name for name, _ in graph)))
    for name, loaded_object in graph:
        with self.subTest(name=name), self.rebind_task1_component(
                name, self.semantic_mutation_of(loaded_object)):
            self.assertNotEqual(audit_engine_fingerprint(), baseline)

def test_transitive_helper_global_and_policy_table_rebinding_changes_digest(self):
    baseline = audit_engine_fingerprint()
    for name, replacement in (
        ("gpu_capability_source_audit._header_operand_after_leading_comments",
         self.mutated_obscure_helper),
        ("gpu_capability_source_audit._RAW_DIRECTIVES",
         self.mutated_raw_directives),
        ("gpu_capability_source_audit.AUDIT_RESULT_SCHEMA_BYTES",
         b"different-schema"),
    ):
        with self.subTest(name=name), self.rebind_task1_component(name,
                                                                  replacement):
            self.assertNotEqual(audit_engine_fingerprint(), baseline)

def test_task1_policy_globals_are_deeply_immutable(self):
    self.assertIsInstance(SOURCE_SUFFIXES, frozenset)
    for table in (REVIEWED_NATIVE_HANDLE_SINKS,
                  REVIEWED_NATIVE_HANDLE_METHODS,
                  REVIEWED_NATIVE_HANDLE_MEMBER_SINKS,
                  REVIEWED_NATIVE_HANDLE_TYPES):
        self.assertIsInstance(table, MappingProxyType)
        with self.assertRaises(TypeError):
            table[PurePosixPath("replacement.cpp")] = frozenset({"sink"})
        self.assertTrue(all(isinstance(value, frozenset)
                            for value in table.values()))

def test_path_replacement_after_import_does_not_redefine_loaded_engine(self):
    baseline = audit_engine_fingerprint()
    self.replace_temp_imported_fixture_path_after_import()
    with self.fail_any_fingerprint_filesystem_read():
        self.assertEqual(audit_engine_fingerprint(), baseline)
    self.assertEqual(_IMPORTED_MODULE_IDENTITIES,
                     self.identities_captured_before_replacement)

def test_audit_fingerprint_is_equal_from_two_absolute_import_roots(self):
    first = self.run_fresh_interpreter_import(self.tree_a_absolute_root)
    second = self.run_fresh_interpreter_import(self.tree_b_absolute_root)
    self.assertEqual(first.audit_fingerprint, second.audit_fingerprint)
    mutated = self.run_fresh_interpreter_import(
        self.tree_b_absolute_root, semantic_mutation=True)
    self.assertNotEqual(first.audit_fingerprint, mutated.audit_fingerprint)

def test_parent_recomputes_attestation_and_rejects_caller_mismatch(self):
    actual = audit_engine_fingerprint()
    self.assertEqual(_attest_loaded_audit_engine(actual), actual)
    with self.assertRaisesRegex(AuditInfrastructureError,
                                "loaded audit engine attestation"):
        _attest_loaded_audit_engine("0" * 64)
    with self.rebind_task1_component(
            "gpu_capability_source_audit.cpp_tokens",
            self.mutated_cpp_tokens):
        with self.assertRaisesRegex(AuditInfrastructureError,
                                    "loaded audit engine attestation"):
            _attest_loaded_audit_engine(actual)
```

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_model tests.gpu.test_gpu_capability_audit_lanes -v`

Expected: FAIL because the compact types and fingerprint do not exist.

- [ ] **Step 3: Implement exact validation and fingerprinting**

```python
AUDIT_RESULT_SCHEMA_BYTES = b"olr-gpu-capability-audit-result-v1"
AUDIT_ENGINE_GRAPH_SCHEMA_BYTES = b"olr-gpu-capability-live-graph-v1"
AUDIT_ENGINE_STAGE_BYTES = b"task-1-model-and-source-audit"
# Migrate the current mutable semantic policy globals before graph enumeration:
# SOURCE_SUFFIXES becomes frozenset(...), and the four REVIEWED_NATIVE_HANDLE_*
# dictionaries become MappingProxyType({...}) with frozenset values.
_AUDIT_ENGINE_TARGET_MODULES = (
    gpu_capability_model,
    gpu_capability_source_audit,
)
_AUDIT_RUNTIME_STATE_EXCLUSIONS = {
    "gpu_capability_source_audit": frozenset({
        "_RUNTIME_OBSERVER_LOCK",
    }),
    "gpu_capability_model": frozenset(),
}
_IMPORTED_MODULE_IDENTITIES = _capture_module_origins_and_identities_at_import(
    tuple(module.__name__ for module in _AUDIT_ENGINE_TARGET_MODULES))

def _update_framed(digest, value: bytes) -> None:
    digest.update(struct.pack("<Q", len(value)))
    digest.update(value)

def _enumerate_live_semantic_graph() -> tuple[tuple[str, object], ...]:
    return _walk_live_semantic_graph_cycle_safe(
        target_modules=_AUDIT_ENGINE_TARGET_MODULES,
        runtime_state_exclusions=_AUDIT_RUNTIME_STATE_EXCLUSIONS,
    )

def audit_engine_fingerprint() -> str:
    digest = hashlib.sha256()
    _update_framed(digest, AUDIT_ENGINE_GRAPH_SCHEMA_BYTES)
    _update_framed(digest, AUDIT_ENGINE_STAGE_BYTES)
    for name, loaded_object in _enumerate_live_semantic_graph():
        _update_framed(digest, name.encode("ascii"))
        _update_framed(digest, _marshal_live_semantic_object(loaded_object))
    for module_name, origin_role, _local_identity in _IMPORTED_MODULE_IDENTITIES:
        _update_framed(digest, f"module-root:{module_name}".encode("ascii"))
        _update_framed(digest, origin_role.encode("ascii"))
    return digest.hexdigest()

def _attest_loaded_audit_engine(expected=None):
    actual = audit_engine_fingerprint()
    if expected is not None and not hmac.compare_digest(actual, expected):
        raise AuditInfrastructureError("loaded audit engine attestation mismatch")
    return actual
```

Task 1's graph is deliberately stage-valid: it contains only `gpu_capability_model` and `gpu_capability_source_audit` objects that exist by this commit. In particular, `cpp_tokens` is owned by `gpu_capability_source_audit`; Task 1 does not mention future command, runner, cache, provenance, worker, or summary objects. `_walk_live_semantic_graph_cycle_safe()` begins with every function and class whose `__module__` equals a target module and every immutable module-defined schema/policy/constant/table. It traverses function code, positional/keyword defaults, annotations and closure cells; recursively follows module-defined globals named by bytecode or held in closures; and traverses class bases plus every sorted method, property, static/class method and descriptor. It uses qualified-name/type-tag/length framing and identity-based cycle detection. Imported modules and imported classes/functions are origin-framed but not recursively traversed.

The immutable encoder supports scalars, tuples, frozensets, enums, frozen dataclasses, mapping proxies, recursively immutable maps/tables, `PurePosixPath` as validated POSIX text, and `re.Pattern` as `(pattern bytes/text, flags)`; unsupported reachable objects fail closed. Task 1 imports `MappingProxyType`, converts the current mutable `SOURCE_SUFFIXES` set to a `frozenset`, and converts all four current `REVIEWED_NATIVE_HANDLE_*` policy dictionaries to `MappingProxyType` with already-frozen values before fingerprinting. Tests reject mutation and prove each table remains behaviorally equivalent. `_AUDIT_RUNTIME_STATE_EXCLUSIONS` names the real existing `_RUNTIME_OBSERVER_LOCK` only at Task 1; recomputation has no fingerprint cache. The whole `gpu_capability_source_audit` module is intentionally a conservative semantic root, including CLI/perf helpers, so unrelated semantic changes may over-invalidate but cannot be omitted. Fixed module roots are not a hand function manifest.

Code objects use a structural encoder, not raw `marshal.dumps`: it length-frames argument/local/stack/flag metadata, bytecode, recursively encoded constants (including nested code objects), names, var/free/cell names and line/exception tables. Every `co_filename` is replaced recursively with the owning normalized module-origin role; absolute strings that equal a known module root are likewise role-normalized, while unrelated semantic string constants remain exact. Tests launch two fresh interpreters against byte-identical trees under different absolute roots and require identical audit and decision fingerprints; a one-byte semantic mutation must differ.

The portable digest frames normalized module name plus semantic origin role and loaded object bytes. It never hashes absolute paths, device/inode, mtime or native `FileIdentity`, so checked-in decisions reproduce across hosts/worktrees. Import-time path/native identity is diagnostic only; no held source handle or later path reread can block repository updates or redefine loaded semantics. The path-replacement test mutates a temporary imported fixture, mocks filesystem access during recomputation to fail if called, and proves unchanged loaded objects retain the same digest; it never edits the live repository.

`DependencyDigest` retains the exact fields/order shown above. Its canonical serialization/order key is `(stable_role, role_relative_path.as_posix(), sha256)`; native `identity` is deliberately excluded from portable bytes but retained for held-handle/path replacement validation. `stable_role == "production"` requires source-root-relative path; external dependencies require one collection-derived role from the frozen toolchain/SDK root map (for example `toolchain` or `sdk:<normalized-id>`) plus a role-relative path. Task 2 derives these roles from explicit configured roots, rejects ambiguous/unmapped externals, and revalidates role/path/content through held handles on warm load. Paths reject absolute, dot/dotdot, backslash, invalid suffix or role escape; bool line is rejected. Dependencies/reached/findings are unique and sorted by exact portable keys. All digests are lowercase 64-hex.

Portable ordering/key bytes and local cache bytes are distinct named codecs. `portable_dependency_key()` frames only stable role, role-relative POSIX path and SHA-256. `encode_local_dependency_digest()` uses an exact ordered schema and serializes every current `FileIdentity` field—`canonical`, `relative`, `device`, `inode`, `line_count`, `production`—in addition to the portable fields; `decode_local_dependency_digest()` rejects missing/unknown/reordered keys, bool-as-int, invalid native values and path/role inconsistency. A local hit must round-trip the exact identity and then match the held current object; same portable bytes with replacement native identity is a miss. Native fields never enter cross-host fingerprints/decision keys.

- [ ] **Step 4: Run GREEN and commit**

Run: `python -m unittest tests.gpu.test_gpu_capability_model tests.gpu.test_gpu_capability_audit_lanes -v`

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_model.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "test(gpu): define compact capability audit results" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: Prove two compiler runs with one accepted parse

**Files:**
- Modify: `tests/gpu/gpu_capability_model.py`
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/gpu_capability_command.py`
- Modify: `tests/gpu/gpu_capability_cache.py`
- Modify: `tests/gpu/fixtures/fake_preprocessor.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_live_compilers.py`
- Modify: `tests/gpu/test_gpu_capability_model.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify: `tests/gpu/test_gpu_capability_command.py`
- Modify: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `tests/gpu/test_gpu_capability_provenance.py`

**Interfaces:** Produces exact frozen `CompilerInspection(compiler_family, executable_identity, executable_sha256, normalized_version, driver_fingerprint, inspection_arguments_digest, executable_capability_digest)`, local `CompilerExecutableCapability`, `inspect_compiler()`, `discover_configuration()` and `stabilize_and_parse_configuration()`. Discovery uses `hashlib.sha256().update` as the stdout consumer and never constructs `PreprocessedStreamBuilder`; accepted execution tees each chunk into the digest and the one builder.

Task 2 is the sole introduction boundary for `DependencyRootBinding`/`DependencyRootAuthority`. It adds mandatory `dependency_root_authority_digest` to the exact `PreprocessConfiguration` constructor and length-frames that digest into its semantic/configuration digest. Compiler-inspection cache keys, manifests and exact codecs accept the authority and include its portable digest; load/publish also validate the caller's local binding identities before hit use or publication. Tests reject a missing/changed authority digest and an equal portable digest paired with different local root path/identity before inspection, compilation, or cache access.

In this same commit, advance the attestation stage/schema and add command, runner, provenance, and the compiler-inspection cache semantics changed here to the exact roots. The one exhaustive module-root walker automatically covers every `PreprocessConfiguration` constructor, `classify_command_rewrite()`, `rewrite_preprocess_command()`, inspection-cache key/manifest/codec/load/publish, executable-capability launch, dependency generation guards, discovery/accepted execution, provenance parsing and all transitive helpers/defaults/tables. Task 2 must not anticipate worker or summary objects. Its staged evolution test asserts the exact root set and proves the Task-1 fingerprint no longer validates after this intentional stage change.

Update every repository `PreprocessConfiguration` constructor/copy/fixture in the affected eight implementation/test files in this same task; an `rg` inventory is checked into the test assertion so no intermediate task commit is broken. Run the complete model, command, cache, provenance, runner, live-compiler and audit-lane Python suites before committing.

- [ ] **Step 1: Write failing one-parse and mismatch tests**

Add constructor/codec negatives for every `CompilerInspection` field: reject bool/wrong enum family, non-`FileIdentity`, non-regular or changed held executable identity, non-lowercase/non-64-hex content/fingerprint/argument digests, empty or non-normalized version, and unknown keys. Prove the portable inspection key excludes absolute path/native identity while pre-exec held-handle identity/content validation remains mandatory.

```python
def test_cold_stabilization_invokes_twice_but_builds_once(self):
    with mock.patch("gpu_capability_runner.PreprocessedStreamBuilder",
                    wraps=PreprocessedStreamBuilder) as builder:
        view, discovery, stages = stabilize_and_parse_configuration(
            self.configuration(), self.dependency_roots, self.production,
            self.limits,
            time.monotonic() + 10.0)
    self.assertEqual(builder.call_count, 1)
    self.assertGreater(discovery.stream.byte_count, 0)
    self.assertGreater(stages.discovery_seconds, 0.0)
    self.assertGreater(stages.accepted_parse_seconds, 0.0)
    self.assertEqual(view.configuration.digest, "cfg")

def test_raw_output_mismatch_publishes_nothing(self):
    self.fixture_mode("different-second-output")
    with self.assertRaisesRegex(AuditInfrastructureError,
                                "raw preprocessed output changed"):
        stabilize_and_parse_configuration(
            self.configuration(), self.dependency_roots, self.production,
            self.limits,
            time.monotonic() + 10.0)

def test_real_subprocess_accepts_thread_and_spawn_cancellation_events(self):
    events = (
        threading.Event(),
        multiprocessing.get_context("spawn").Event(),
    )
    for event in events:
        event.set()
        with self.subTest(event=type(event).__name__), self.assertRaisesRegex(
                AuditInfrastructureError, "cancelled"):
            self.run_real_fake_preprocessor("wait-for-cancel", event)
        self.assertFalse(self.any_fake_preprocessor_alive())
```

Add byte-count mismatch, dependency closure reorder/add/remove, mutation/restoration between runs, cancellation before accepted launch, and accepted parser failure assertions.

The discovery dependency set is unknown before its first launch, so the plan does not claim impossible per-file pre-discovery guards. Discovery runs against live roots, then its bounded declared dependency set is immediately resolved/opened/hashed and preflighted. Preflight deduplicates every full directory chain from the mapped authority root's parent sentinel (needed to detect replacement/alias swap of the root itself), through the authority root and every component down to each dependency parent; it checks owner/watch count and exact metadata ceilings before opening anything. Guards for that exact set are then armed before accepted launch. Windows file and directory handles deny delete/rename (files also deny write). Linux watches every chain component and file for move/delete/attrib/write events and fails on overflow/watch loss/unsupported setup. macOS owns every component vnode and watches the applicable `NOTE_WRITE|NOTE_RENAME|NOTE_DELETE|NOTE_ATTRIB`, failing on dropped/unsupported observation. Before launch and again after accepted exit/before hash, drain events, require no relevant event, exact full stat and path↔held identities for every chain component/file, then hash the held file. Accepted dependency set/digests and raw stream must equal discovery. A discovery A→B→A either changes the raw/dependency stream relative to guarded accepted A and fails or was audit-semantically identical; accepted A→B→A always trips the guard even if restored before post-exit hash. Tests exercise both schedules for production/external files and ancestor directory/root swap→restore on all three backends, plus ceiling, overflow, unsupported and complete-owner cleanup failures.

Every compiler launch consumes `CompilerExecutableCapability`, never an unguarded path. The capability owns/deduplicates the full executable chain and the driver-resolved preprocessing/runtime closure (`cc1*`, compiler helper executables, and load-time DLL/dylib/shared-library dependencies applicable to the lane); held identities/hashes and one closure digest enter inspection, configuration and decision keys. Windows keeps executable/runtime handles plus deny-delete directory handles. Linux executes the exact driver fd and guards its resolved closure. macOS does **not** claim vnode checks prevent path-based exec substitution: unless an enforceable exact-fd/signature launcher is proven on the pinned runner, it launches only through the trusted path-chain policy, arms all vnode guards before permit, then detects any pre/during/post launch change and fails the whole run with no publication. The invariant is detect-and-fail, not prevention. Inspection-cache hits reopen/reconstruct the complete capability. Masquerading via replaced `cc1`/runtime dependency, ancestor/root swap, permit→exec substitution and during-exec replacement tests fail closed on all applicable backends.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_model tests.gpu.test_gpu_capability_command tests.gpu.test_gpu_capability_cache tests.gpu.test_gpu_capability_provenance tests.gpu.test_gpu_capability_runner tests.gpu.test_gpu_capability_live_compilers tests.gpu.test_gpu_capability_audit_lanes -v`

Expected: FAIL because both current executions build a full view.

- [ ] **Step 3: Implement discovery and accepted tee**

```python
class _StreamDigestConsumer:
    def __init__(self, downstream=None):
        self._digest = hashlib.sha256()
        self._count = 0
        self._downstream = downstream

    def __call__(self, chunk: bytes) -> None:
        self._digest.update(chunk)
        self._count += len(chunk)
        if self._downstream is not None:
            self._downstream(chunk)

    def finish(self) -> StreamDigest:
        return StreamDigest(self._digest.hexdigest(), self._count)
```

`discover_configuration()` runs the rewritten command with this consumer, parses and validates the dependency file, and hashes every dependency with `force=True`. `stabilize_and_parse_configuration()` snapshots discovery, runs accepted output through `_StreamDigestConsumer(builder.feed)`, validates accepted dependencies, finalizes once, and rejects unless stream digests and ordered dependency snapshots are exactly equal. `run_bounded_preprocessor()` calls only `cancel_event.is_set()` after validating that attribute is callable; it performs no `isinstance(threading.Event)` check, so both in-process and spawn-context events drive the same real subprocess termination path.

- [ ] **Step 4: Prove exact-byte stability on live families**

Run: `python -m unittest tests.gpu.test_gpu_capability_live_compilers -v`

Expected: installed MinGW/GCC passes exact two-run byte equality; optional absent families skip only in local unit execution. CI-required GCC, Clang, AppleClang Objective-C++, MSVC, and clang-cl must pass.

- [ ] **Step 5: Run GREEN and commit**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner tests.gpu.test_gpu_capability_live_compilers -v`

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_runner.py tests/gpu/gpu_capability_command.py tests/gpu/gpu_capability_cache.py tests/gpu/fixtures/fake_preprocessor.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_live_compilers.py tests/gpu/test_gpu_capability_model.py tests/gpu/test_gpu_capability_audit_lanes.py tests/gpu/test_gpu_capability_command.py tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_provenance.py
git commit -m "refactor(gpu): stabilize output with one capability parse" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: Replace active full-view caching with compact validated results

**Files:**
- Modify: `tests/gpu/gpu_capability_model.py`
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_cache.py`
- Modify: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_model.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Produces `audit_cache_key()`, core `CompactResultMemoryBudget`/ownership/growth types, core ordered `StreamingResultAggregator`, `ConfigurationAuditCache.load()`, `.load_many()`, and `.publish()`. Reuses `_CacheRootLock`, `_CacheKeyLock`, `_HeldDirectory`, exact carrier files, dependency hashing, atomic UUID temp directories, replacement detection, and cache partition enforcement from `PreprocessCache`.

Task 3 is the ownership boundary for the complete non-transport compact budget and streaming aggregate core, including reserve/commit/release/error semantics and the 251-disjoint growth tests. This is required for `load_many()` to become GREEN in Task 3; no core type/test is deferred to Task 7/8. In this same commit, advance `AUDIT_ENGINE_STAGE_BYTES`; `gpu_capability_cache` was introduced into attestation in Task 2 for compiler-inspection semantics, and Task 3 now adds its compact-result cache/codec, budget and aggregator graph/schema tables. The exhaustive walker attests all constructors, validators, helpers and immutable tables.

- [ ] **Step 1: Write failing compact cache tests**

```python
def test_result_cache_round_trips_without_token_payload(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(self.configuration, self.dependency_roots, self.result,
                  self.publication_permit,
                  self.pipeline_deadline)
    loaded = cache.load(
        self.configuration, self.dependency_roots,
        self.result.audit_engine_fingerprint,
        self.production_snapshot, self.pipeline_deadline)
    self.assertEqual(loaded, self.result)
    self.assertFalse(any(path.suffix == ".bin"
                         for path in self.cache_root.rglob("*")))

def test_all_251_results_survive_unchanged_warm_load(self):
    cache = ConfigurationAuditCache(self.cache_root)
    for index in range(251):
        cache.publish(self.configuration_for(index), self.dependency_roots,
                      self.result_for(index),
                      self.publication_permit_for(index), self.pipeline_deadline)
    batch = cache.load_many(
        tuple(self.configuration_for(i) for i in range(251)),
        self.dependency_roots, self.engine, self.production_snapshot,
        self.result_budget, self.aggregator, self.maximum_cold_slot,
        self.pipeline_deadline)
    self.assertEqual((batch.hit_count, len(batch.misses)), (251, 0))

def test_load_many_opens_each_unique_dependency_once(self):
    cache = ConfigurationAuditCache(self.cache_root)
    self.publish_251_results_sharing_common_dependencies(cache)
    with mock.patch("gpu_capability_cache._open_dependency_handle",
                    wraps=_open_dependency_handle) as opened, \
         mock.patch("gpu_capability_cache._hash_held_dependency",
                    wraps=_hash_held_dependency) as reads:
        batch = cache.load_many(
            self.configurations_251, self.dependency_roots, self.engine,
            self.production_snapshot, self.result_budget, self.aggregator,
            self.maximum_cold_slot, self.pipeline_deadline)
    self.assertEqual(batch.hit_count, 251)
    self.assertEqual(
        {Path(call.args[0].canonical) for call in opened.call_args_list},
        self.unique_dependency_paths)
    self.assertEqual(opened.call_count, len(self.unique_dependency_paths))
    self.assertEqual(reads.call_count, 2 * len(self.unique_dependency_paths))

def test_combined_key_is_length_framed_and_used_everywhere(self):
    first = audit_cache_key("a" * 64, "bc" + "0" * 62)
    second = audit_cache_key("ab" + "0" * 62, "c" + "0" * 63)
    self.assertNotEqual(first, second)
    configuration = self.configuration_with_digest("a" * 64)
    result = dataclasses.replace(
        self.result, configuration_digest="a" * 64,
        audit_engine_fingerprint="bc" + "0" * 62)
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(configuration, self.dependency_roots, result,
                  self.publication_permit,
                  self.pipeline_deadline)
    self.assertEqual(cache.observed_entry_key, first)
    expected_lane = hashlib.sha256(
        f"preprocess:{first}".encode("utf-8")).digest()[0] % 8
    self.assertEqual(cache.observed_lane, expected_lane)
    self.assertEqual(cache.observed_manifest_key, first)

def test_cache_root_partition_sum_is_exact_and_excludes_full_views(self):
    self.assertEqual(_AUDIT_CACHE_MAXIMUM_BYTES +
                     _INSPECTION_MAX_TOTAL_BYTES +
                     _CACHE_ROOT_OVERHEAD_RESERVE,
                     512 * 1024 * 1024)
    self.create_old_full_view_entry(self.cache_root)
    with self.assertRaisesRegex(AuditInfrastructureError, "full-view partition"):
        ConfigurationAuditCache(self.cache_root).prepare(self.pipeline_deadline)

def test_root_and_key_locks_use_caller_deadline_capped_by_240_seconds(self):
    cache = ConfigurationAuditCache(self.cache_root)
    for operation_started, pipeline_deadline in ((1000.0, 1180.0),
                                                  (1000.0, 1400.0)):
        with self.record_lock_deadlines(now=operation_started) as observed:
            cache.prepare(pipeline_deadline)
            cache.load(self.configuration, self.dependency_roots, self.engine,
                       self.production_snapshot, pipeline_deadline)
        self.assertEqual(observed.root,
                         min(pipeline_deadline, operation_started + 240.0))
        self.assertEqual(observed.key,
                         min(pipeline_deadline, operation_started + 240.0))

def test_expired_or_contended_root_and_key_lock_never_wait_indefinitely(self):
    for lock_kind in ("root", "key"):
        with self.subTest(lock_kind=lock_kind), \
             self.hold_cache_lock_in_other_process(lock_kind), \
             self.assertRaisesRegex(AuditInfrastructureError,
                                    "cache lock deadline"):
            self.run_cache_operation(deadline=time.monotonic() + 0.05)
        self.assertEqual(self.cache_lock_waiter_survivors(), ())

def test_boundary_hash_rejects_different_bytes_with_restored_size_and_mtime(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(self.configuration, self.dependency_roots, self.result,
                  self.publication_permit,
                  self.pipeline_deadline)
    timestamp = self.dependency.stat().st_mtime_ns
    self.dependency.write_bytes(self.same_length_different_bytes)
    os.utime(self.dependency, ns=(timestamp, timestamp))
    self.assertIsNone(cache.load(
        self.configuration, self.dependency_roots, self.engine,
        self.production_snapshot,
        self.pipeline_deadline))

def test_complete_pre_boundary_identity_and_content_restoration_is_equivalent(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(self.configuration, self.dependency_roots, self.result,
                  self.publication_permit,
                  self.pipeline_deadline)
    original = self.dependency.read_bytes()
    timestamp = self.dependency.stat().st_mtime_ns
    self.dependency.write_bytes(self.same_length_different_bytes)
    self.dependency.write_bytes(original)
    os.utime(self.dependency, ns=(timestamp, timestamp))
    self.assertEqual(cache.load(
        self.configuration, self.dependency_roots, self.engine,
        self.production_snapshot,
        self.pipeline_deadline), self.result)

def test_final_held_handle_rehash_detects_different_bytes_without_reopening(self):
    cache = ConfigurationAuditCache(self.cache_root)
    self.publish_251_results_sharing_common_dependencies(cache)
    with self.mutate_between_initial_and_final_held_reads(self.dependency), \
         mock.patch("gpu_capability_cache._open_dependency_handle",
                    wraps=_open_dependency_handle) as opened:
        batch = cache.load_many(
            self.configurations_251, self.dependency_roots, self.engine,
            self.production_snapshot, self.result_budget, self.aggregator,
            self.maximum_cold_slot, self.pipeline_deadline)
    self.assertIn(self.changed_configuration, batch.misses)
    self.assertEqual(self.open_count_for(self.dependency, opened), 1)

def test_exact_restoration_before_final_held_rehash_is_equivalent(self):
    cache = ConfigurationAuditCache(self.cache_root)
    self.publish_251_results_sharing_common_dependencies(cache)
    with self.mutate_and_restore_between_held_reads(self.dependency):
        batch = cache.load_many(
            self.configurations_251, self.dependency_roots, self.engine,
            self.production_snapshot, self.result_budget, self.aggregator,
            self.maximum_cold_slot, self.pipeline_deadline)
    self.assertEqual((batch.hit_count, len(batch.misses)), (251, 0))

def test_cached_production_digest_must_match_initial_snapshot_before_hit(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(self.configuration, self.dependency_roots,
                  self.result_for_production_bytes(b"B"),
                  self.publication_permit, self.pipeline_deadline)
    batch = cache.load_many(
        (self.configuration,), self.dependency_roots, self.engine,
        self.production_snapshot_for_bytes(b"A"), self.result_budget,
        self.aggregator, self.maximum_cold_slot, self.pipeline_deadline)
    self.assertEqual(batch.hit_count, 0)
    self.assertEqual(batch.misses, (self.configuration,))

def test_batch_retained_cap_prevents_many_individually_valid_decodes(self):
    cache = ConfigurationAuditCache(self.cache_root)
    self.publish_individually_valid_results(
        count=9, conservative_decoded_bytes=16 * 1024 * 1024)
    with self.track_memory_reservations() as reservations, \
         self.assertRaisesRegex(AuditInfrastructureError,
                               "batch retained result limit"):
        cache.load_many(
            self.nine_configurations, self.dependency_roots, self.engine,
            self.production_snapshot, self.result_budget, self.aggregator,
            self.maximum_cold_slot, self.pipeline_deadline)
    self.assertLessEqual(reservations.maximum_committed,
                         128 * 1024 * 1024)

def test_unique_dependency_handle_and_metadata_ceiling_preflights_before_open(self):
    cache = ConfigurationAuditCache(self.cache_root)
    with mock.patch("gpu_capability_cache._open_dependency_handle") as opened, \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "dependency handle|metadata ceiling"):
        cache.load_many(
            self.configurations_with_unique_dependencies(32769),
            self.dependency_roots, self.engine, self.production_snapshot,
            self.result_budget, self.aggregator, self.maximum_cold_slot,
            self.pipeline_deadline)
    opened.assert_not_called()
    self.assertLessEqual(self.peak_validation_chunk_size, 256)

def test_250_near_limit_hits_leave_one_cold_slot_for_single_miss_rebuild(self):
    self.populate_250_hits_whose_retained_sum_nears_128_mib()
    aggregator = StreamingResultAggregator(
        self.configurations_251, self.result_budget, self.limits)
    batch = cache.load_many(
        self.configurations_251, self.dependency_roots, self.engine,
        self.production_snapshot, self.result_budget, aggregator,
        self.maximum_cold_slot, self.pipeline_deadline)
    self.assertEqual((batch.hit_count, len(batch.misses)), (250, 1))
    self.assertGreaterEqual(batch.cold_slot_reserved_bytes,
                            self.maximum_cold_slot.worst_case_live_bytes)
    rebuilt = self.run_single_miss_into_same_aggregator(batch.misses[0],
                                                        aggregator)
    self.assertTrue(rebuilt)
    aggregate = aggregator.finish()
    self.assertEqual(aggregate, self.expected_all_cold_aggregate)
    self.assertLessEqual(self.result_budget.peak_live_bytes, 128 << 20)

def test_251_disjoint_max_aggregate_growth_fails_before_insert_without_copy(self):
    aggregator = StreamingResultAggregator(
        self.configurations_251, self.result_budget, self.limits)
    for configuration, result in self.disjoint_maximal_results[:250]:
        aggregator.accept_validated_result(
            configuration, result, self.ownership_for(result))
    before = self.snapshot_aggregate_indices(aggregator)
    configuration, result = self.disjoint_maximal_results[250]
    with self.assertRaisesRegex(AuditInfrastructureError,
                                "aggregate.*128 MiB"):
        aggregator.accept_validated_result(
            configuration, result, self.ownership_for(result))
    self.assertEqual(self.snapshot_aggregate_indices(aggregator), before)
    self.assertFalse(self.final_summary_copy_was_started)

def test_real_cold_and_warm_streaming_summaries_are_byte_deterministic(self):
    cold, warm = self.run_real_251_cold_warm_through_streaming_aggregator()
    self.assertEqual(encode_canonical_summary(cold),
                     encode_canonical_summary(warm))
    self.assertLessEqual(self.result_budget.peak_live_bytes, 128 << 20)

def test_authentic_corruption_is_miss_but_unsafe_namespace_fails(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.publish(self.configuration, self.dependency_roots, self.result,
                  self.publication_permit,
                  self.pipeline_deadline)
    self.corrupt_regular_payload_in_place()
    self.assertIsNone(cache.load(
        self.configuration, self.dependency_roots, self.engine,
        self.production_snapshot,
        self.pipeline_deadline))
    self.replace_entry_with_symlink()
    with self.assertRaisesRegex(AuditInfrastructureError, "namespace|link"):
        cache.load(self.configuration, self.dependency_roots, self.engine,
                   self.production_snapshot,
                   self.pipeline_deadline)

def test_encoded_and_decoded_limits_fail_before_large_construction(self):
    self.write_authentic_payload(b"{" + b"x" * (4 * 1024 * 1024))
    with mock.patch("gpu_capability_cache.json.loads") as loads:
        self.assertIsNone(cache.load(
            self.configuration, self.dependency_roots, self.engine,
            self.production_snapshot,
            self.pipeline_deadline))
    loads.assert_not_called()
    for field, count in (("dependencies", 16385), ("reached", 4097),
                         ("findings", 65537)):
        with self.subTest(field=field), self.assertRaisesRegex(
                AuditInfrastructureError, "result limit"):
            self.publish_oversized_collection(field, count)
```

Add same-configuration/different-engine coexistence, different-configuration/same-engine coexistence, dependency digest mismatch, malformed JSON, duplicate key, truncated payload, overlong path/expression/reason, per-entry decoded allocation overflow, batch retained overflow, symlink/hardlink/reparse, invalid carrier/root marker, concurrent equal winner, concurrent differing winner, crash temp, cache-entry replacement, dependency same-content identity swap, unstable pre/hash/post observation, Windows write/delete denial while held, POSIX dependency path replacement and final-byte change, capacity, and access-time tests. Corruption inside an authentic held regular entry and stable dependency identity/content drift are misses; cache-entry/carrier/root namespace replacement or links are infrastructure errors. Before classifying any authentic candidate as a hit, `load_many()` compares every production dependency's complete `FileIdentity` and SHA-256 to the immutable initial production snapshot map; an absent or unequal production entry is a miss and is never returned for use. The stable 251-entry test proves one path open, two full reads on the same native handle, and zero reopens per unique dependency.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_cache.ConfigurationAuditCacheTests -v`

Expected: FAIL because `ConfigurationAuditCache` is absent.

- [ ] **Step 3: Implement canonical compact JSON publication**

```python
AUDIT_CACHE_SCHEMA_BYTES = b"olr-gpu-capability-audit-cache-v1"
_AUDIT_CACHE_MAXIMUM_BYTES = 472 * 1024 * 1024
_AUDIT_CACHE_MAXIMUM_ENTRIES = 1024
_require(
    _AUDIT_CACHE_MAXIMUM_BYTES + _INSPECTION_MAX_TOTAL_BYTES
    + _CACHE_ROOT_OVERHEAD_RESERVE == 512 * 1024 * 1024,
    "cache partition total is not 512 MiB")

def audit_cache_key(configuration_digest: str, engine: str) -> str:
    digest = hashlib.sha256()
    for value in (AUDIT_CACHE_SCHEMA_BYTES,
                  configuration_digest.encode("ascii"),
                  engine.encode("ascii")):
        digest.update(struct.pack("<Q", len(value)))
        digest.update(value)
    return digest.hexdigest()

payload = json.dumps(document, ensure_ascii=True, sort_keys=True,
                     separators=(",", ":")).encode("ascii")
key = audit_cache_key(configuration.digest, result.audit_engine_fingerprint)
manifest = {
    "schema": AUDIT_CACHE_SCHEMA_BYTES.decode("ascii"),
    "key": key,
    "payload_bytes": len(payload),
    "payload_sha256": hashlib.sha256(payload).hexdigest(),
}
```

The combined key is used without substitution for `results/<key[:2]>/<key>`, manifest key, load, and publish. Lock selection preserves the reviewed substrate exactly: `hashlib.sha256(f"{namespace}:{key}".encode("utf-8")).digest()[0] % 8`; neither key-prefix arithmetic nor a new namespace is permitted. `prepare()`, `load()`, `load_many()`, and `publish()` pass the caller's absolute pipeline deadline into every root/key acquisition, each capped by `operation_started + 240s`; constructors perform no blocking lock acquisition. `load_many()` first authenticates and allocation-free preparses bounded entries, rejects hit classification for any cached production `DependencyDigest` not exactly equal in path, full identity, and SHA-256 to the initial production snapshot map, deduplicates expected dependency paths, then opens each unique path once and retains the native handle/fd until dispatch. The initial read binds handle identity, checks non-link/regular-file policy, captures stable pre/post `fstat`, streams SHA-256, and compares the entry digest. The final boundary re-seeks and rehashes that same handle, repeats stable pre/post handle stat, and verifies the current path still names the held identity. Windows uses a native handle opened with read sharing only, denying write/delete/rename while held; POSIX compares `lstat(path)` device/inode to `fstat(fd)` and therefore detects replacement without reopening. Final differing bytes/identity are cache misses, exact restoration before the final read is equivalent only after the initial production-generation comparison, and unstable reads are infrastructure failures. Handles close only after the final boundary and the hit/miss set is fixed.

The caller constructs one observer, budget and ordered `StreamingResultAggregator` before `load_many()`. It first authenticates/preparses all entry headers to determine misses without decoding hits. If any miss exists, it reserves one complete worst-case cold slot before the first hit decode. Hits then decode one at a time in configuration order, precompute and reserve all configuration-index, coverage-index, finding-index and digest-state growth before any mutation, enter those charged indices, and immediately release their object ownership; they can never consume the cold reservation. Fresh results use the identical aggregator and release after acceptance, with dispatch/order windowing so at most worker-count pending objects exist. The API returns counts/misses, not 251 hit objects. Thus 250 near-limit hits plus one miss rebuilds under 128 MiB and real cold/warm summaries are byte-identical. A 251-way adversarial disjoint maximum that cannot fit fails before inserting its final result; it is not silently spilled or partially merged. Finalization freezes or moves already charged backing structures and transfers ownership to the summary, never allocating an uncharged merged copy. Publish still requires the exact generation permit and releases root charge after atomic cleanup.

- [ ] **Step 4: Run cache and lock suites**

Run: `python -m unittest tests.gpu.test_gpu_capability_cache -v`

Expected: every existing lock, namespace-integrity, and isolation test plus compact result tests passes.

- [ ] **Step 5: Commit**

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_cache.py tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_model.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "refactor(gpu): cache compact capability audit results" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 4: Accelerate exact audit construction and reuse parsed analysis

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_runner.py` (replace production asserts)
- Modify: `tests/gpu/gpu_capability_command.py` (replace production asserts)
- Modify: `tests/gpu/gpu_capability_provenance.py` (replace production asserts)
- Modify: `tests/gpu/gpu_capability_cache.py` (replace production asserts)
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify: `tests/gpu/test_gpu_capability_command.py`
- Modify: `tests/gpu/test_gpu_capability_provenance.py`
- Modify: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`

Advance the attestation stage/schema in this same commit for the new source-audit analysis semantics; `gpu_capability_provenance` has already been a fixed module root since Task 2. The exhaustive graph must include `gpu_capability_source_audit.cpp_tokens` under its actual owner, provenance validators, analysis constructors, capability rules and every transitive helper/table without a manual function list. Its stage-evolution test locks the exact new module roots/schema before commit.

**Interfaces:** Produces mechanically derived `capability_candidate_spellings() -> frozenset[bytes]`, `view_has_capability_spelling(view) -> bool`, bounded-chunk byte-identical `AuditBuffer.from_preprocessed()`, a test-only forced-unfiltered reference path, and one reusable `CompilerAuditAnalysis` per candidate view.

- [ ] **Step 1: Write differential and call-count tests**

```python
def test_candidate_prefilter_does_not_build_audit_buffer(self):
    view = self.view(tokens=(b"int", b"safe", b";"))
    with mock.patch.object(AuditBuffer, "from_preprocessed") as build:
        self.assertEqual(audit_preprocessed_view(view, self.limits, lambda: 0), [])
    build.assert_not_called()

def test_batched_buffer_is_byte_and_location_identical(self):
    expected = reference_audit_buffer(self.mixed_provenance_view)
    actual = AuditBuffer.from_preprocessed(
        self.mixed_provenance_view, self.limits, lambda: 0)
    self.assertEqual(actual.text, expected.text)
    self.assertEqual(actual.location_for_line(7), expected.location_for_line(7))

def test_every_rejecting_fixture_has_derived_candidate_and_same_unfiltered_result(self):
    derived = capability_candidate_spellings()
    for name, view in self.all_mutation_and_live_views():
        filtered = audit_preprocessed_view(view, self.limits, lambda: 0)
        unfiltered = _audit_preprocessed_view_unfiltered(
            view, self.limits, lambda: 0)
        with self.subTest(name=name):
            self.assertEqual(filtered, unfiltered)
            if unfiltered:
                spellings = set(object.__getattribute__(view.tokens, "_spellings"))
                self.assertFalse(derived.isdisjoint(spellings))

def test_dense_view_pre_reserves_final_str_mapping_and_join_temporaries(self):
    view = self.view(tokens=(b"identifier",) * 1_000_000)
    buffer, observations = self.build_with_allocation_observer(view)
    self.assertIs(type(buffer.text), str)
    self.assertTrue(buffer.text)
    self.assertEqual(buffer.text_character_count, len(buffer.text))
    self.assertEqual(buffer.text_max_code_point,
                     max(map(ord, buffer.text), default=0))
    self.assertEqual(observations.events[:2],
                     ("measure-layout", "reserve-all-allocations"))
    self.assertTrue(observations.final_str.reserved_before_allocation)
    self.assertTrue(observations.chunk_list.reserved_before_allocation)
    self.assertTrue(observations.join_transient.reserved_before_allocation)
    self.assertTrue(observations.mapping_arrays.reserved_before_allocation)
    self.assertLessEqual(observations.maximum_chunk_characters, 64 * 1024)
    self.assertEqual(observations.final_str.constructions, 1)
    self.assertLessEqual(observations.peak_charged_bytes,
                         observations.pre_reserved_bytes)
    self.assertEqual(cpp_tokens(buffer.text),
                     cpp_tokens(reference_audit_buffer(view).text))

def test_runtime_allocation_probe_matches_supported_cpython_3119_objects(self):
    layout = probe_cpython_allocation_layout()
    self.assertEqual(layout.python_version, (3, 11, 9))
    for value, reserved in self.actual_objects_and_reserved_bounds(layout):
        with self.subTest(type=type(value), value=repr(value)[:40]):
            self.assertLessEqual(sys.getsizeof(value), reserved)
    with self.fake_python_version((3, 11, 8)), self.assertRaisesRegex(
            AuditInfrastructureError, "CPython 3.11.9 allocation layout"):
        probe_cpython_allocation_layout()
```

Add a call-count assertion that `cpp_tokens()` and `build_shadow_index()` execute once per view even when candidate paths have multiple policy keys. Run the filtered-versus-forced-unfiltered equality assertion over every raw/authoritative/source-only mutation fixture, every legacy macro bypass fixture, every live compiler safe/forbidden fixture, empty input, and candidate-free input; this exhaustive owned corpus is the acceptance proof for the prefilter.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_audit_lanes tests.gpu.test_gpu_capability_command tests.gpu.test_gpu_capability_provenance tests.gpu.test_gpu_capability_cache -v`

Expected: candidate-free views still build a full buffer and multi-policy views rebuild token/shadow analysis.

- [ ] **Step 3: Implement exact prefilter and batched runs**

```python
@dataclass(frozen=True)
class CapabilityRule:
    trigger_spellings: tuple[bytes, ...]
    evaluator: Callable[..., list[Finding]]

def capability_candidate_spellings() -> frozenset[bytes]:
    return frozenset(spelling for rule in _CAPABILITY_RULES
                     for spelling in rule.trigger_spellings)

def view_has_capability_spelling(view: PreprocessedTranslationUnitView) -> bool:
    spellings = object.__getattribute__(view.tokens, "_spellings")
    return not capability_candidate_spellings().isdisjoint(spellings)

layout = _measure_audit_buffer_layout(tokens)  # allocation-free first pass
runtime_layout = probe_cpython_allocation_layout()
reservation = reserve_before_allocation(
    _pep393_str_allocation_bound(
        layout.text_character_count, layout.maximum_code_point, runtime_layout)
    + _mapping_allocation_bound(layout, runtime_layout)
    + _exact_chunk_list_allocation_bound(layout.chunk_count, runtime_layout)
    + layout.aggregate_live_chunk_bytes_and_strs
    + _unicode_join_result_and_transient_bound(layout, runtime_layout)
    + _analysis_and_json_object_bound(layout, runtime_layout))
chunks: list[str | None] = [None] * layout.chunk_count
columns = _allocate_exact_mapping_columns(layout, runtime_layout)
for chunk_index, measured_chunk in enumerate(_iter_measured_text_and_mapping_chunks(
        tokens, layout, maximum_characters=64 * 1024)):
    reservation.require_before_allocation(measured_chunk.allocation_bound)
    # Latin-1 preserves the current byte-to-code-point mapping exactly.
    chunks[chunk_index] = measured_chunk.normalized_bytes.decode("latin-1", "strict")
    _fill_exact_mapping_columns(columns, measured_chunk.mapping_records)
text = "".join(chunks)  # the sole retained final str construction
_require(type(text) is str, "audit text is not str")
_require(len(text) == layout.text_character_count, "audit text length changed")
_require(max(map(ord, text), default=0) == layout.maximum_code_point,
         "audit text storage kind changed")
_require(_mapping_counts(columns) == layout.mapping_counts,
         "audit mapping count changed")
del chunks  # release chunk strings/list/join transients; retain str + mappings
```

Refactor every capability evaluator into `_CAPABILITY_RULES`; its exact trigger spellings are the identifiers that can make that evaluator report, so the prefilter has no separately maintained allowlist. `AuditBuffer.text` remains one final exact `str`, matching the current `TranslationText`, string-regex, `cpp_tokens()`, masking, slicing, and policy APIs; no evaluator or provenance API becomes bytes-like. `_measure_audit_buffer_layout()` is an allocation-free first pass over the packed token runs. It computes the exact final Latin-1 character count, maximum code point, PEP 393 storage kind, run/origin/mapping counts, bounded chunk count, and checked conservative allocation totals before any text, list, mapping, or join object is constructed.

Production always uses `conservative_allocation_schema()`; no runtime/private-layout probe selects or alters a production bound. It returns one versioned portable closed-form upper-bound schema. Every term uses checked addition/multiplication and `round_up(raw_bytes, allocator_alignment_bytes)`. The formulas are explicit: `PyLong(n)` charges the fixed long header plus `max(1, ceil(bit_length(abs(n)) / pylong_digit_bits)) * pylong_digit_bytes_upper_bound`; `bytes(n)` and `bytearray(n)` charge their fixed header plus `n + 1`; `BytesIO(n)` charges its fixed header plus the complete allocator-rounded backing capacity; `str(code_points)` charges its fixed header plus `(code_points + 1) * 4`, regardless of observed PEP-393 kind; list and tuple charge their fixed header plus the exact fixed slot count times the pointer bound; dict charges its header plus the next power-of-two capacity required by the declared maximum load, the full index array, and key/value entry slots. Duplicate-key rejection hooks separately charge their ordered pair list, seen-key dict capacity/index/entries, and all retained key strings. JSON charges scanner and decoder fixed bounds, every token/string/int/container from the preparsed maximum counts and lengths, all input chunks in aggregate, the one joined input, canonical encoder scratch, and the production decode/mapping objects. Large line integers use the same bit-length formula. No formula subtracts sharing or assumes small-int/string interning.

The reservation covers every phase overlap explicitly: all bounded input chunks plus chunk containers, joined text plus join transient, final four-bytes-per-code-point string, fixed list/tuple slots, dict tables, duplicate-key structures, analysis objects, canonical JSON scanner/decoder objects, and production raw/decode/mapping objects. The exact CPython 3.11.9 private-layout probe exists only as a Windows calibration diagnostic recorded beside evidence; it never gates, parametrizes, or substitutes the production schema. All three platforms assert before auditing that representative actual `sys.getsizeof()` values are less than or equal to the portable schema; Linux/macOS never assert exact private sizes. Tests cover maximum string lengths, entry counts, duplicate-key hook structures, large line-number integers, chunk aggregate plus join overlap, JSON scanner/decoder overlap, production decode overlap, Windows diagnostic equality, all-platform less-than-or-equal portability, and one-byte-over failures. The second pass materializes at most 64-Ki-character chunks into preallocated indices only after reservation, performs one `"".join(chunks)`, validates measured counts with explicit exceptions, and drops chunk transients. Move `cpp_tokens`, delimiter pairs, scope bindings, next-nonspace indices, blocks, and shadow index into one immutable `CompilerAuditAnalysis` passed to provenance validation and every policy group. Do not change `TranslationText`, `cpp_tokens()`, regex patterns, finding text/path/line, or provenance checks.

At this Task 4 boundary, add an AST gate only for production modules that already exist by then: `gpu_capability_model.py`, `gpu_capability_command.py`, `gpu_capability_runner.py`, `gpu_capability_provenance.py`, `gpu_capability_cache.py`, and `gpu_capability_source_audit.py`. It fails on every `ast.Assert`; replace each production assertion with an explicit `_require_contract()` or typed infrastructure error. Do not reference `gpu_capability_process_tree.py` or `gpu_capability_calibration.py` before Task 6 creates them. Task 10 adds the complete eight-module gate after both files exist and runs the verifier mutation corpus under `python -O` and `python -OO`.

- [ ] **Step 4: Run mutation, lane, and measured representative gates**

Run: `python -m unittest tests.gpu.test_gpu_capability_audit_lanes -v`

Run: `python -c "import sys; sys.path.insert(0,'tests/gpu'); import gpu_capability_source_audit as a; a.mutation_self_tests(); print('mutation PASS')"`

Run: `python tests/gpu/gpu_capability_source_audit.py --source-root . --profile-view build/gpu/compile_commands.json --profile-source playback/gpu/gpubudget.cpp`

Expected: exact filtered/unfiltered finding equivalence over the exhaustive owned corpus; mechanically derived candidate proof; candidate-free buffer count zero; one final retained `str` plus exact mapping structures, conservative PEP-393/list/chunk/join/mapping reservation before construction, every text chunk at most 64 KiB, exact regex/`cpp_tokens`/location parity, and no bytes-like policy API change; one parsed analysis for `gpubudget.cpp`; normalization equals the reference character-for-character; representative warm view audit is below 5 seconds before process parallelism.

- [ ] **Step 5: Commit**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_runner.py tests/gpu/gpu_capability_command.py tests/gpu/gpu_capability_provenance.py tests/gpu/gpu_capability_cache.py tests/gpu/test_gpu_capability_audit_lanes.py tests/gpu/test_gpu_capability_command.py tests/gpu/test_gpu_capability_provenance.py tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_runner.py
git commit -m "perf(gpu): reuse compact capability audit analysis" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: Audit and discard inside one worker

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_model.py` (introduce `ConfigurationAuditOutcome`)
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_model.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Produces single-miss `audit_configuration_worker()`. Parent-owned `load_many()` is the only warm-hit path; workers never validate or return hits. The function requires a parent-issued `PerTaskCompactReservation` before compiler launch or audit, then obtains a generation-bound `CachePublicationPermit` from the coordinator before compact-result materialization or cache publication.

Task 5 tests the worker in-process with fake bounded control/command permit endpoints and a prebound local `DependencyRootAuthority`; it does not import, instantiate, or anticipate Task 7's coordinator/reactor. Spawn coordination and real endpoint integration remain exclusively Task 7.

Advance the attestation stage/schema in this same commit. Exhaustive enumeration must now include `audit_configuration_worker()`, its production-generation validation, compact-result constructors/validators, publication-permit handling and every transitive helper/default/table; rebinding any one changes the fingerprint.

- [ ] **Step 1: Write failing worker lifecycle tests**

```python
def test_worker_returns_only_compact_outcome_and_drops_view(self):
    outcome = self.run_worker(self.configuration)
    self.assertIsInstance(outcome, ConfigurationAuditOutcome)
    self.assertFalse(hasattr(outcome, "view"))
    self.assertEqual(self.live_view_count(), 0)

def test_worker_is_called_only_for_parent_validated_miss(self):
    with mock.patch.object(_WORKER_CACHE, "load") as load:
        outcome = self.run_worker(self.configuration)
    load.assert_not_called()

def test_audit_count_comes_from_typed_subprocess_boundary_events(self):
    outcome, events = self.run_worker_with_launch_events(self.configuration)
    self.assertEqual(tuple(event.purpose for event in events),
                     (CompilerLaunchPurpose.AUDIT_DISCOVERY,
                      CompilerLaunchPurpose.AUDIT_ACCEPTED))
    self.assertEqual(self.parent_observed_audit_launch_count(events), 2)
    self.assertFalse(hasattr(outcome, "audit_compiler_invocations"))
    for corruption in ("missing", "duplicate", "retry", "extra"):
        with self.subTest(corruption=corruption), self.assertRaisesRegex(
                AuditInfrastructureError, "audit compiler launch protocol"):
            self.run_worker_with_corrupt_launch_events(corruption)

def test_empty_dependency_is_authoritatively_reached(self):
    outcome = self.run_worker(self.configuration_with_empty_header())
    self.assertIn(PurePosixPath("playback/gpu/empty.h"),
                  outcome.result.reached_production)

def test_fresh_worker_rejects_different_production_generation_before_publish(self):
    initial = self.production_snapshot_for_bytes(b"A")
    self.production_path.write_bytes(b"B")
    with mock.patch.object(_WORKER_CACHE, "publish") as publish, \
         self.assertRaisesRegex(AuditInfrastructureError,
                               "production snapshot generation"):
        self.run_worker(self.configuration, production_snapshot=initial)
    publish.assert_not_called()
    self.production_path.write_bytes(b"A")
    self.assertFalse(self.cache_entry_exists(self.configuration))
```

Add candidate-free, forbidden finding, dependency mutation, raw mismatch, audit exception, cache publish exception, cancellation, and no-publication-on-failure tests.
Add tests proving a worker without a dispatch reservation performs no discovery launch or audit allocation, and a worker without a granted root-publication permit performs no compact-result/canonical-string materialization, publication, or send. Two workers cannot overlap root publication; the exact 8-MiB root publication charge covers one 4-MiB payload plus manifest/carrier and UUID-temp/atomic-replacement overlap; every success/failure/cancellation releases the root permit and reservation at the named phase boundary.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.WorkerAuditTests -v`

Expected: no worker-side compact audit function exists.

- [ ] **Step 3: Implement the exact worker sequence**

```python
def audit_configuration_worker(task, dependency_roots, production_snapshot,
                               control_endpoint, command_endpoint, deadline):
    _require(task.dependency_root_authority is dependency_roots,
             "worker dependency-root authority identity changed")
    _require_valid_dispatch_reservation(task.compact_reservation, task,
                                        _WORKER_GENERATION)
    view, discovery, preprocess_stages = stabilize_and_parse_configuration(
        task.configuration, dependency_roots, _WORKER_PRODUCTION,
        _WORKER_LIMITS, deadline,
        _WORKER_CANCEL_EVENT)
    validate_production_dependency_snapshots(
        discovery.snapshots, production_snapshot)
    reached = frozenset(
        identity.relative for identity in discovery.dependencies
        if identity.production and identity.relative is not None)
    with _stage_timer() as audit_time:
        findings = audit_preprocessed_view(view, _WORKER_LIMITS,
                                           _WORKER_RSS.sample)
    draft = _bounded_compact_result_draft(
        task.configuration.digest, _WORKER_ENGINE, discovery.snapshots,
        tuple(sorted(reached, key=PurePosixPath.as_posix)),
        tuple(_compact_findings(findings)))
    del view
    _send_control_frame_before(control_endpoint, CachePublicationRequested(
        _WORKER_INDEX, _WORKER_GENERATION, task.task_id), deadline)
    publication_permit = _receive_bounded_command_frame_before(
        command_endpoint, task, _WORKER_CANCEL_EVENT, deadline,
        maximum_quantum_seconds=0.05)
    result = draft.materialize_under(task.compact_reservation,
                                     publication_permit)
    with _stage_timer() as publish_time:
        accepted = _WORKER_CACHE.publish(
            task.configuration, dependency_roots, result,
            publication_permit, deadline)
    stages = WorkerStageTimings(
        preprocess_stages.discovery_seconds,
        preprocess_stages.accepted_parse_seconds,
        audit_time.elapsed_seconds,
        publish_time.elapsed_seconds)
    return ConfigurationAuditOutcome(
        result=accepted, stdout_bytes=discovery.stream.byte_count * 2,
        stages=stages)
```

Before dispatch the parent reserves one conservative compact-result slot covering the worker draft's maximum dependency tuple, reached-path tuple, compact-finding tuple and object/container/path metadata, then canonical counting/encoding, payload, serialized cache publication, pipe/receiver, parent decode transient and retained immutable-result phases. Exact draft counts/UTF-8 lengths are checked against this reservation before constructing each collection, and draft ownership transitions into the final result without an uncharged overlap. The slot excludes only full view/audit-buffer/analysis/provenance-token working state. A task cannot enter discovery until its generation-bound reservation validates. The worker may build only the charged bounded draft before `CachePublicationRequested`; the permit prohibition applies specifically to canonical payload/string construction, cache carrier/temp creation, publication and send. `activate_root_publication(task_id, generation)` returns the sole generation-bound 8-MiB permit; `require_within_pre_dispatch_reservation(task_id, encoded_bytes, bounds, counting_pass_peak_bytes)` uses the exact fixed API before payload permission. Publication/root charge releases after atomic cleanup; transport/decode/retained charges release at their named transitions.

Every compiler process is created through one subprocess-boundary wrapper that emits exactly one `CompilerLaunchEvent` before returning the process handle. Inspection, discovery, and accepted purposes are distinct; task identity/generation are mandatory for audit events. `ConfigurationAuditOutcome` contains only `result`, `stdout_bytes`, and `stages`; audit invocation counts come exclusively from parent-observed generation-bound launch connections. Missing events, an automatic retry, or any extra discovery/accepted process is an infrastructure failure even if output happens to match. `validate_production_dependency_snapshots()` checks every production dependency path in the fresh discovery against the immutable initial production map and requires equal full `FileIdentity` plus SHA-256. Missing or unequal entries are infrastructure failures before audit result publication or use. The adversarial A→B test keeps B stable through both compiler runs, restores A before the later final descriptor boundary, and still fails with no publication, proving final restoration cannot hide a mixed-generation compile. Reached coverage comes from the validated dependency closure, not token provenance, so empty files, pragma-only files, and candidate-free included production files remain authoritative. Publish only after auditing and dropping the full view. A concurrent winner must be exactly equal. Per-configuration cycle collection is forbidden because it is unmeasured; bounded recycling is decided after real calibration.

Each launch event carries stable typed `ProcessStartIdentity`, not PID alone. On Windows the boundary wrapper opens/retains the process handle before emitting and registers an authenticated handle cookie in the parent association table; Job membership/creation time and post-exit peak queries use that handle even for a short-lived process. Other backends validate their native start token. PID reuse, mismatched cookie/start identity, event-before-handle registration, double close and leak tests freeze ownership through Job registration, exit observation and final phase seal.

- [ ] **Step 4: Run GREEN and commit**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.WorkerAuditTests tests.gpu.test_gpu_capability_audit_lanes -v`

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_runner.py tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_model.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "refactor(gpu): audit and discard views in workers" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: Build process-tree accounting and the versioned decision contract

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_model.py` (worker telemetry and decision carriers)
- Create: `tests/gpu/gpu_capability_process_tree.py`
- Create: `tests/gpu/gpu_capability_calibration.py`
- Modify: `tests/gpu/gpu_capability_command.py`
- Modify: `tests/gpu/gpu_capability_runner.py`
- Create: `tests/gpu/test_gpu_capability_process_tree.py`
- Create: `tests/gpu/test_gpu_capability_calibration.py`
- Modify: `tests/gpu/test_gpu_capability_command.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify: `tests/gpu/test_gpu_capability_model.py`
- Modify: `tests/gpu/fixtures/fake_preprocessor.py`

**Interfaces:** Produces `OwnedProcessTree`, complete-tree `WorkerTelemetry`, immutable collection-time `DecisionConfigurationRecord` sidecars, `WorkerDecisionKey`, fake-runner calibration formulas, atomic decision serialization, and strict decision codecs. Task 6 tests process-tree/accounting primitives and calibration selection only through the injected fake runner; it neither integrates scheduling nor runs the real coordinator. Task 7 owns schedule/reactor/tree integration and its integration tests. Task 10 performs the real post-tokenizer native calibrations and creates the evidence artifacts.

Advance the audit stage/schema for the collection and decision inputs introduced here. Build the decision-engine fingerprint with the same exhaustive, deterministic live-object walker over the now-existing process-tree and calibration modules, plus the loaded audit-engine digest and decision schema/tables. Do not use a manual wrapper manifest; rebind an obscure accounting helper, calibration selector and immutable decision table in negative tests and require a changed digest.

- [ ] **Step 1: Write failing complete-tree accounting fixtures**

```python
def test_windows_job_accounts_every_created_identity_and_rss_upper_bound(self):
    samples = self.sample_two_slots_and_recycled_generations()
    expected_upper = max(
        samples.maximum_simultaneous_working_set_bytes,
        samples.parent_peak_working_set + sum(
            max(generation.worker_root_peak_working_set_bytes
                + max((invocation.compiler_descendant_peak_tree_bytes
                       for invocation in generation.invocations), default=0)
                for generation in slot.generations)
            for slot in samples.slots))
    self.assertEqual(samples.memory.aggregate_peak_rss_upper_bound_bytes,
                     expected_upper)
    self.assertEqual(samples.memory.job_total_process_count,
                     samples.memory.retained_process_identity_count)
    self.assertTrue(samples.memory.accounting_complete)
    self.assertNotEqual(samples.memory.aggregate_peak_rss_upper_bound_bytes,
                        samples.memory.job_peak_commit_charge_bytes)

def test_windows_cold_accountant_precedes_collection_and_contains_inspection(self):
    with self.record_cold_lifecycle() as events:
        sample = self.run_realistic_cold_slice()
    self.assertLess(events.index("create-run-job-accountant"),
                    events.index("enumerate-production"))
    self.assertLess(events.index("create-run-job-accountant"),
                    events.index("inspection-probe-preexec-register"))
    self.assertTrue(sample.all_inspection_and_worker_identities_retained)
    self.assertLess(sample.complete_cold_rss_upper_bound_bytes, 448 << 20)
    self.assertEqual(sample.outer_emergency_ceiling_bytes, 512 << 20)

def test_windows_sequential_compilers_take_max_but_concurrent_slots_sum(self):
    one_slot = self.sample_251_sequential_compiler_invocations()
    self.assertEqual(one_slot.generation_compiler_upper_bound,
                     max(i.compiler_descendant_peak_tree_bytes
                         for i in one_slot.invocations))
    self.assertNotEqual(one_slot.generation_compiler_upper_bound,
                        sum(i.compiler_descendant_peak_tree_bytes
                            for i in one_slot.invocations))
    concurrent = self.sample_two_concurrent_worker_slots()
    self.assertEqual(concurrent.slot_contribution,
                     sum(slot.maximum_generation_upper_bound
                         for slot in concurrent.slots))

def test_windows_missed_short_lived_job_member_fails_closed(self):
    with self.job_total_processes_exceed_retained_identities_by_one(), \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "incomplete Job process accounting"):
        self.observe_windows_forest()

def test_linux_run_cgroup_accounts_coordinator_workers_and_oom_deltas(self):
    memory = self.run_acknowledged_linux_cgroup_fixture()
    self.assertGreaterEqual(memory.cgroup_peak_accounted_memory_bytes,
                            self.fixture_accounted_bytes)
    self.assertEqual(memory.cgroup_memory_max_bytes, 512 << 20)
    self.assertEqual(memory.cgroup_memory_high_bytes, 448 << 20)
    self.assertEqual((memory.cgroup_oom_count_delta,
                      memory.cgroup_oom_kill_count_delta), (0, 0))
    self.assertTrue(memory.accounting_complete)
    self.assertEqual(self.all_leaf_cgroup_procs(), ())

def test_linux_outer_supervisor_uses_supervisor_leaf_and_propagates_status(self):
    result = self.run_linux_supervisor_with_child_exit(17)
    self.assertEqual(result.supervisor_cgroup, self.supervisor_leaf)
    self.assertEqual(result.child_argv, self.exact_ctest_argv)
    self.assertEqual(result.child_initial_cgroup, result.coordinator_leaf)
    self.assertEqual(result.returncode, 17)
    self.assertEqual(result.remaining_run_subtrees, ())

def test_linux_workflow_uses_exact_systemd_delegated_service(self):
    self.assertEqual(self.workflow_systemd_prefix, (
        "sudo", "systemd-run", f"--unit={self.expected_unit}",
        "--wait", "--collect", "--property=Type=exec",
        "--property=Delegate=yes", "--property=MemoryAccounting=yes",
        "--property=MemoryMax=536870912",
        "--property=MemoryHigh=469762048",
        f"--uid={os.getuid()}", f"--gid={os.getgid()}",
        f"--working-directory={os.getcwd()}", "python3",
        "tests/gpu/gpu_capability_process_tree.py", "prepare-linux-cgroup"))
    self.assertSystemdRunSupports(
        "--wait", "--collect", "--property=", "--working-directory=")
    self.assertGreaterEqual(self.parsed_systemd_version, self.minimum_tested_version)
    self.assertEqual(self.actual_ci_unit_type, "service")
    self.assertTrue(self.actual_ci_unit_delegate)
    self.assertTrue(self.supervisor_moved_to_sibling_supervisor_leaf)
    self.assertEqual(self.delegated_service_root_cgroup_procs, ())
    self.assertFalse(self.supervisor_was_ever_inside_run_subtree)
    self.assertTrue(self.supervisor_verified_memory_controller)
    self.assertTrue(self.dynamic_leaf_request_ack_protocol_verified)
    self.assertTrue(self.second_invocation_removed_all_owned_leaves)

def test_linux_worker_acknowledges_exact_generation_leaf_before_ready(self):
    carrier = LinuxWorkerContainment(
        self.run_path, self.worker_leaf_path, self.generation_token)
    events = self.start_linux_worker_with_carrier(carrier)
    self.assertEqual(events[:3],
                     ("move-exact-leaf", "verify-token-and-membership",
                      "containment-ack"))
    self.assertLess(events.index("containment-ack"),
                    events.index("engine-ready"))

def test_macos_launcher_registers_compiler_pgid_before_exec(self):
    with self.record_macos_launcher_handshake() as events:
        memory = self.run_acknowledged_macos_fixture()
    self.assertEqual(events[:5],
                     ("setsid", "report-pid-pgid-start", "libproc-verify",
                      "register-compiler-pgid", "permit-exec"))
    self.assertGreater(memory.maximum_observed_aggregate_resident_bytes, 0)
    self.assertTrue(memory.accounting_complete)
    self.assertEqual(memory.surviving_registered_process_count, 0)
    self.assertEqual(memory.known_unreconciled_descendant_count, 0)
    self.assertEqual(self.registered_compiler_pgids_after_reap(), ())

def test_macos_worker_creates_and_reports_own_session_before_ready(self):
    events = self.start_macos_worker_generation()
    self.assertEqual(events[:5],
                     ("child-setsid", "report-worker-pid-pgid-start",
                      "libproc-verify", "register-worker-pgid",
                      "permit-engine-ready"))
```

Add worker-exits-first, compiler-spawns-grandchild, PID reuse/start-time identity, cancellation, forced termination, and two-tree simultaneous-sum tests. Windows tests cover completion-port wakeup loss, `TotalProcesses` greater/less than retained identities, a short-lived inspection child whose handle is retained at Job notification and whose high RSS occurs and exits between polls, creation-time mismatch, the three-way simultaneous/inspection/worker conservative peak formula, recycled-generation per-slot maximum, and Job commit never populating RSS. Linux tests cover missing delegation/controller with no fallback, supervisor remaining outside the run subtree, exact argv launch in `coordinator/`, coordinator plus every worker leaf, service-root and run-node `memory.events` OOM/max deltas, inherited compiler/grandchild membership, child-status propagation, and every leaf/subtree empty before cleanup. macOS tests cover launcher report/permit timeout, unregistered or untrusted-driver exec rejection, a registered member observed changing group/ancestry while still reconcilable, foreign member, start-time mismatch, full-buffer enumeration retry, configured truncation failure, per-invocation compiler-group termination, pool-wide compiler-groups-then-worker-group order, zero registered survivors, and zero known unreconciled descendants. Do not claim a test detects an already-unobserved `setsid()`/double-fork escape. Acknowledged fixtures hold allocations until observation; unacknowledged macOS transient tests explicitly do not claim a peak upper bound.

- [ ] **Step 2: Implement OS-owned tree accounting and run RED/GREEN**

On Windows, the top-level pipeline creates one run-scoped kill-on-close aggregate Job/accountant before production enumeration. Every inspection wrapper is assigned to it before exec. Each worker generation additionally owns a fresh nested kill-on-close generation Job, created/registered under the run Job before dispatch; the generation Job is verified empty, archived and closed on recycle. The lane first proves nested-Job support and rejects breakaway. Compilers inherit both applicable scopes; independent compiler Jobs are forbidden. Membership authority is repeated PID-list enumeration in the aggregate and per-generation Jobs, with retained PID/creation-time handles and `TotalProcesses` equality. Thus generation telemetry remains isolatable while the aggregate accountant covers collection/inspection and the complete cold lifetime.

Each observation computes parent current working set plus every live member's `WorkingSetSize`; its maximum is the simultaneous sample. Every inspection invocation and every audit compiler invocation receives a distinct phase/index tag. The accountant retains a queryable process handle and creation identity for every inspection process and descendant until the cold inspection phase is closed, queries each process's `PeakWorkingSetSize` even after a short-lived process exits, sums the retained per-process peaks for that inspection tree, and retains `inspection_peak_tree_bytes = max(per_inspection_tree_sum)` across sequential probes. A missed high-RSS process between polling samples therefore remains represented by its retained peak handle; failure to open/retain/query any Job-reported inspection identity makes accounting incomplete.

Inspection and task phases use explicit accountant markers. `seal_phase()` does not trust a single empty poll: it enumerates Job PID identities, `TotalProcesses` and active-process count, opens/retains every newly reported identity, drains all queued completion notifications, and requires two consecutive stable snapshots with active count zero and exact equality between total reported identities and retained identities before sealing. A delayed `NEW_PROCESS` followed by exit that crosses the apparent boundary must still be incorporated. Tests inject delayed new-process/exit notifications across both inspection→task and task→summary boundaries and prove the phase cannot seal early, lose a peak, or leave a handle/survivor.

Every audit compiler invocation tree is tagged `(worker_index, generation, task_id, invocation_index)` and includes that compiler plus descendants. Because invocations within one worker never overlap, a generation upper bound is `worker_root_peak_working_set_bytes + max(per_invocation_compiler_and_descendant_peak_tree_bytes)`, never a sum of its sequential compiler invocations. A slot upper bound is the maximum across recycled generations. Inspection and worker phases are nonoverlapping. The exact conservative observed-RSS cancellation formula is `max(maximum_simultaneous_working_set_bytes, parent_peak_rss_bytes + inspection_peak_tree_bytes, parent_peak_rss_bytes + sum(concurrently_configured_slot_upper_bounds))`. The 251-sequential-invocation test proves maxima are not accumulated, while concurrent worker slots do sum. A dedicated inspection fixture allocates above every poll sample and exits before the next poll; its retained `PeakWorkingSetSize` must raise `inspection_peak_tree_bytes` and the formula. Timing/memory acceptance proves the corrected bound can pass. Job commit charge remains a separately named per-Job committed-memory diagnostic/backstop, never RSS and never a kernel hard aggregate RSS limit. Cold Windows calibration requires the observed upper bound `<448 MiB`; warm requires `<512 MiB`.

On Linux, the delegated service supervisor creates sibling `supervisor/` and empty `run/`, moves itself to `supervisor/`, verifies service-root `cgroup.procs` empty, and enables/verifies `+memory` at the service root. It baselines both service-root and run-node `memory.events`; either scope's `oom`, `oom_kill` or `max` increase fails. While run is empty it sets/verifies run max/high and enables `+memory` in run subtree control, then creates coordinator/workers and verifies effective hierarchical limits. The supervisor never enters run.

Supervisor coordination uses a durable authenticated Unix-domain `SOCK_SEQPACKET` rendezvous, not inherited FDs (CTest may close them). The supervisor creates a private `0700` directory and `0600` socket, listens for the whole transient-service lifetime, and exports only `OLR_CGROUP_RENDEZVOUS_PATH` plus a 256-bit random `OLR_CGROUP_RENDEZVOUS_TOKEN` through systemd→supervisor→CTest→audit. The audit connects, sends one bounded canonical hello with token/run identity, and the supervisor verifies `SO_PEERCRED` UID/PID plus that PID's exact coordinator-leaf membership before accepting dynamic leaf requests. Replies are bounded sequenced packets bound to run/index/generation/nonce. Socket/token unlink occurs only after coordinator exit and run cleanup. A two-invocation test creates distinct rendezvous paths/tokens, proves CTest propagation, rejects replay/foreign credentials, and leaves no socket/cgroup state.

The supervisor alone owns cgroup directory creation/deletion. For initial and recycled generations, the coordinator sends bounded `(create-leaf, worker_index, generation, nonce)` frames; the supervisor creates `run/worker-<index>-g<generation>/`, writes a bound token, verifies limits, and replies with exact path/token before dispatch. Release requires an empty recheck before removal/ack. Final cleanup removes worker leaves, empty coordinator, then run while the supervisor remains in its sibling leaf. A real two-invocation test proves service-root `cgroup.procs` is empty while controllers are enabled, the first transient unit is collected after exit, and the second starts with no stale leaf/controller state. Missing/malformed env/FD/token, occupied removal or stale state fails with no fallback.

On macOS, audit compilers use generation/task-bound `CompilerPgidReported`/`CompilerExecPermit`. Run-scoped collection inspection uses distinct `MacOSInspectionPgidReported`/`MacOSInspectionExecPermit` carrying a random `inspection_id` and no worker, generation or task fields/sentinels. Exact codecs reject audit fields in inspection messages and vice versa. The accountant validates executable held identity/hash and PGID/start identity before deriving the driver fingerprint and issuing the matching one-shot inspection permit. Timeout/cancellation reconciles the registered inspection group before collection can continue.

Membership authority for the registered scope is repeated `proc_listpids(PROC_PGRP_ONLY, pgid)` plus `proc_pidinfo` start identity and recursive PPID reconciliation for every registered worker/compiler group. Enumeration grows/retries whenever its buffer fills and fails on configured maximum or unstable/truncated results. `kqueue EVFILT_PROC NOTE_FORK` is only a rescan wakeup. Foreign membership, identity change, unregistered/untrusted exec, reconciliation failure, a registered survivor, or an escape observed while its ancestry is still visible is an infrastructure failure. Darwin exposes no race-free global descendant controller: a process that double-forks/`setsid()` and disappears from all observed registered ancestry may be unknowable, so the plan does not claim global zero survivors or guaranteed detection of such an escape. Exact macOS metric names remain `maximum_observed_aggregate_resident_bytes`, `maximum_observed_parent_resident_bytes`, and `maximum_observed_owned_group_resident_bytes`, scoped to registered groups and sampled observations only.

Run: `python -m unittest tests.gpu.test_gpu_capability_process_tree -v`

Expected: Windows proves complete Job identity accounting and its conservative RSS upper bound; Linux proves controller-enforced run-subtree accounting with an external supervisor and zero cgroup members; macOS proves its registered PGID set is empty with no known unreconciled descendants and honestly scoped observed residency, without a global-zero claim.

- [ ] **Step 3: Write failing full-pipeline calibration and artifact-contract tests**

```python
def test_calibration_sample_wraps_every_cold_top_level_stage(self):
    decision = _calibrate_platform_worker_count_with_runner(
        "windows", self.fake_top_level_pipeline, self.source_root,
        (self.fake_database_251,), self.empty_cache_parent, self.limits)
    self.assertEqual(decision.attempted_worker_counts[:2], (2, 1))
    expected_stages = (
        "enumerate-production", "collect-configurations", "snapshot-production", "load-cache",
        "compile", "compact-audit-publish", "aggregate-authoritative",
        "validate-policy-boundary", "raw-audit", "source-only",
        "shutdown-reap", "summary")
    self.assertTrue(all(stages == expected_stages
                        for stages in self.fake_top_level_pipeline.calls_by_sample))

def test_each_count_uses_unrecorded_pilot_then_fresh_measured_parameters(self):
    decision = _calibrate_platform_worker_count_with_runner(
        "windows", self.fake_top_level_pipeline, self.source_root,
        (self.fake_database_251,), self.empty_cache_parent, self.limits)
    counts = allowed_worker_counts(self.expected_key.effective_worker_capacity)
    self.assertEqual(self.fake_top_level_pipeline.call_kinds,
                     tuple((count, kind) for count in counts
                           for kind in ("pilot", "measured")))
    self.assertTrue(self.fake_top_level_pipeline.all_cache_roots_distinct)
    self.assertEqual(decision.attempted_worker_counts, counts)
    self.assertEqual(tuple(sample.worker_count for sample in decision.samples),
                     counts)
    self.assertEqual(decision.rejections, ())
    for sample in decision.samples:
        pilot = self.fake_top_level_pipeline.pilot_for(sample.worker_count)
        measured = self.fake_top_level_pipeline.measured_for(sample.worker_count)
        self.assertEqual(
            (measured.maximum_tasks_per_worker, measured.recycle_rss_bytes),
            (sample.maximum_tasks_per_worker, sample.recycle_rss_bytes))
        self.assertNotIn(pilot.run_id, self.serialized_evidence_run_ids(decision))
        self.assertTrue(sample.pilot_eligible)
        self.assertEqual(sample.surviving_owned_process_count, 0)

def test_failed_candidate_is_a_typed_rejection_and_does_not_block_selection(self):
    self.fake_top_level_pipeline.reject_measured_count(
        4, stage="compile", code="native-memory-limit")
    decision = _calibrate_platform_worker_count_with_runner(
        "windows", self.fake_top_level_pipeline, self.source_root,
        (self.fake_database_251,), self.empty_cache_parent, self.limits)
    self.assertEqual(decision.attempted_worker_counts, (2, 1, 3, 4))
    self.assertEqual(tuple(sample.worker_count for sample in decision.samples),
                     (2, 1, 3))
    self.assertEqual(decision.rejections, (CalibrationRejection(
        platform_kind="windows", worker_count=4, failure_stage="compile",
        failure_code="native-memory-limit", cleanup_complete=True,
        surviving_owned_process_count=0),))
    self.assertIn(decision.selected_workers, (2, 1, 3))
    for mutation in ("missing-attempt", "duplicate-count", "overlap",
                     "unknown-stage", "unknown-code", "cleanup-incomplete",
                     "survivor"):
        with self.subTest(mutation=mutation), self.assertRaises(
                AuditInfrastructureError):
            load_platform_worker_decision(
                self.write_rejection_mutation(decision, mutation),
                "windows", decision.key)

def test_production_calibration_signature_constructs_real_runner_and_delegates(self):
    with mock.patch("gpu_capability_calibration.RealCalibrationRunner") as runner, \
         mock.patch("gpu_capability_calibration._calibrate_platform_worker_count_with_runner",
                    return_value=self.decision) as helper:
        actual = calibrate_platform_worker_count(
            "windows", self.source_root, (self.fake_database_251,),
            self.empty_cache_parent, self.limits)
    self.assertIs(actual, self.decision)
    helper.assert_called_once_with(
        "windows", runner.return_value, self.source_root,
        (self.fake_database_251,),
        self.empty_cache_parent, self.limits)

def test_pilot_recycling_formula_is_exact_at_every_boundary(self):
    workers = 4
    samples = self.post_return_samples(
        safe_prefix_lengths=(63, 62, 63, 63),
        maximum_aggregate_peak=(448 << 20) - 1 - workers,
        maximum_worker_current=80 << 20,
        accounting_complete=True)
    derived = derive_pilot_runtime_parameters(samples, workers, 251)
    self.assertEqual(derived.maximum_tasks_per_worker, 62)
    self.assertEqual(derived.recycle_rss_bytes, (80 << 20) + 1)
    self.assertTrue(derived.pilot_eligible)
    for mutation in ("missing-ordinal", "duplicate-ordinal",
                     "accounting-incomplete", "aggregate-equals-448mib"):
        with self.subTest(mutation=mutation):
            ineligible = derive_pilot_runtime_parameters(
                self.mutate_post_return_samples(samples, mutation), workers, 251)
            self.assertEqual(
                (ineligible.pilot_eligible,
                 ineligible.maximum_tasks_per_worker,
                 ineligible.recycle_rss_bytes),
                (False, 1, 1))
    one_byte_too_little = self.post_return_samples(
        safe_prefix_lengths=(1, 1, 1, 1),
        maximum_aggregate_peak=(448 << 20) - workers,
        maximum_worker_current=1,
        accounting_complete=True)
    self.assertEqual(
        derive_pilot_runtime_parameters(
            one_byte_too_little, workers, 251).diagnostic_parameters,
        (1, 1))

def test_decision_artifact_is_versioned_keyed_and_strictly_loaded(self):
    audit_engine = audit_engine_fingerprint()
    baseline = decision_engine_fingerprint(audit_engine)
    graph = _enumerate_live_decision_semantic_graph(audit_engine)
    self.assertEqual(tuple(name for name, _ in graph),
                     tuple(sorted(name for name, _ in graph)))
    for name in ("gpu_capability_process_tree._query_job_member_identity",
                 "gpu_capability_calibration._sample_is_eligible",
                 "gpu_capability_calibration._WORKER_SELECTION_ORDER"):
        with self.subTest(name=name), self.mutate_decision_component(name):
            self.assertNotEqual(decision_engine_fingerprint(audit_engine), baseline)
    decision = self.valid_decision(
        platform_tag="windows-amd64-py311",
        compiler_digest="a" * 64,
        configuration_set_digest="b" * 64,
        decision_engine_fingerprint=baseline,
        effective_worker_capacity=4)
    write_worker_decision_atomic(self.path, decision)
    self.assertEqual(load_worker_decision(self.path, decision.key), decision)
    for field in ("artifact_schema", "platform_tag", "compiler_digest",
                  "configuration_set_digest", "decision_engine_fingerprint",
                  "effective_worker_capacity",
                  "reference_envelope_digest",
                  "key_digest"):
        with self.subTest(field=field), self.assertRaisesRegex(
                AuditInfrastructureError, "worker decision"):
            load_worker_decision(self.mutated_artifact(field), decision.key)

def test_decision_fingerprint_is_equal_across_absolute_import_roots(self):
    first = self.run_fresh_decision_interpreter(self.tree_a_absolute_root)
    second = self.run_fresh_decision_interpreter(self.tree_b_absolute_root)
    self.assertEqual(first.decision_fingerprint, second.decision_fingerprint)
    mutated = self.run_fresh_decision_interpreter(
        self.tree_b_absolute_root, semantic_mutation=True)
    self.assertNotEqual(first.decision_fingerprint,
                        mutated.decision_fingerprint)

def test_decision_configuration_set_digest_is_relocatable_but_semantic(self):
    collection = collect_configurations_with_decision_records(
        self.source_root, (self.raw_compile_database,),
        self.launcher_effective_environment, self.production,
        self.dependency_roots, self.capability_registry,
        self.inspection_cache, self.engine, self.limits, self.pipeline_deadline,
        self.run_accountant)
    configurations = collection.configurations
    records = collection.decision_records
    exact_environment_digests = tuple(
        configuration.environment_digest for configuration in configurations)
    exact_cache_keys = tuple(self.cache_key(configuration)
                             for configuration in configurations)
    baseline = configuration_set_digest(
        configurations, records, self.dependency_roots)
    relocated = collect_configurations_with_decision_records(
        self.relocated_source_root,
        (self.relocated_raw_compile_database,),
        self.relocated_launcher_effective_environment,
        self.relocated_production, self.relocated_dependency_roots,
        self.capability_registry,
        self.relocated_inspection_cache,
        self.engine, self.limits,
        self.pipeline_deadline, self.run_accountant)
    relocated_configurations = relocated.configurations
    relocated_records = relocated.decision_records
    self.assertEqual(
        baseline,
        configuration_set_digest(relocated_configurations, relocated_records,
                                 self.relocated_dependency_roots))
    self.assertEqual(
        tuple(configuration.environment_digest for configuration in configurations),
        exact_environment_digests)
    self.assertEqual(tuple(self.cache_key(configuration)
                           for configuration in configurations),
                     exact_cache_keys)
    with self.assertRaises(TypeError):
        records[configurations[0].digest] = records[configurations[0].digest]
    changed = collect_configurations_with_decision_records(
        self.relocated_source_root,
        (self.relocated_raw_compile_database,),
        self.mutate_non_root_launcher_assignment(
            self.relocated_launcher_effective_environment),
        self.relocated_production, self.relocated_dependency_roots,
        self.capability_registry,
        self.relocated_inspection_cache,
        self.engine, self.limits,
        self.pipeline_deadline, self.run_accountant)
    changed_configurations = changed.configurations
    changed_records = changed.decision_records
    self.assertNotEqual(
        baseline,
        configuration_set_digest(changed_configurations, changed_records,
                                 self.relocated_dependency_roots))
    for key in ("GITHUB_RUN_ID", "RUNNER_TEMP", "ACTIONS_RUNTIME_TOKEN"):
        noisy_configurations, noisy_records = self.collect_with_ambient_change(
            key, "different")
        self.assertEqual(
            baseline,
            configuration_set_digest(
                noisy_configurations, noisy_records, self.dependency_roots))
    for family, key in (("gcc", "CPATH"), ("clang", "SDKROOT"),
                        ("clang", "MACOSX_DEPLOYMENT_TARGET"),
                        ("msvc", "INCLUDE"),
                        ("clang-cl", "INCLUDE")):
        semantic_configurations, semantic_records = (
            self.collect_with_family_environment_change(family, key))
        self.assertNotEqual(
            baseline,
            configuration_set_digest(
                semantic_configurations, semantic_records,
                self.dependency_roots))
    launcher_configurations, launcher_records = (
        self.collect_with_explicit_launcher_assignment_change())
    self.assertNotEqual(
        baseline,
        configuration_set_digest(
            launcher_configurations, launcher_records, self.dependency_roots))
    self.assertNotEqual(
        configuration_set_digest(
            configurations[:1], self.records_for_first(records),
            self.dependency_roots),
        configuration_set_digest(
            configurations[:2], self.records_for_first_two(records),
            self.dependency_roots))
    for rejected in ("CL", "_CL_"):
        with self.subTest(rejected=rejected), self.assertRaisesRegex(
                AuditInfrastructureError, "unsupported compiler environment"):
            self.collect_with_family_environment_change("msvc", rejected)

def test_dependency_root_authority_is_frozen_attested_and_unambiguous(self):
    authority = build_dependency_root_authority(
        self.source_root, self.explicit_external_roots)
    relocated = build_dependency_root_authority(
        self.relocated_source_root, self.relocated_external_roots)
    self.assertEqual(authority.portable_authority_digest,
                     relocated.portable_authority_digest)
    self.assertEqual(decode_dependency_root_authority(
        encode_dependency_root_authority(authority)), authority)
    for mutation in ("overlapping-roots", "duplicate-role", "unmapped-path",
                     "ambiguous-path", "unknown-codec-key",
                     "root-identity-replacement"):
        with self.subTest(mutation=mutation), self.assertRaises(
                AuditInfrastructureError):
            self.use_mutated_dependency_root_authority(mutation)

def test_repeated_compile_commands_are_preserved_in_order_and_deduplicated_semantically(self):
    databases = (self.compile_commands_a, self.compile_commands_b)
    collection = collect_configurations_with_decision_records(
        self.source_root, databases, self.launcher_effective_environment,
        self.production, self.dependency_roots, self.capability_registry,
        self.inspection_cache,
        self.engine, self.limits,
        self.pipeline_deadline, self.run_accountant)
    configurations = collection.configurations
    records = collection.decision_records
    self.assertEqual(self.observed_compile_database_arguments, databases)
    self.assertEqual(configurations, self.expected_semantic_deduplication)
    self.assertEqual(set(records), {item.digest for item in configurations})
    parsed = self.parse_cli(
        "--compile-commands", str(databases[0]),
        "--compile-commands", str(databases[1]))
    self.assertEqual(parsed.compile_commands, databases)

def test_collection_probe_is_separate_from_exact_audit_invocation_count(self):
    with self.record_compiler_process_purposes() as purposes:
        run = self.run_one_configuration_pipeline()
    self.assertGreater(run.measurements.inspection_probe_invocations, 0)
    self.assertEqual(run.measurements.audit_compiler_invocations, 2)
    self.assertEqual(purposes.count(CompilerLaunchPurpose.AUDIT_DISCOVERY)
                     + purposes.count(CompilerLaunchPurpose.AUDIT_ACCEPTED), 2)
    self.assertEqual(purposes.count(CompilerLaunchPurpose.INSPECTION),
                     run.measurements.inspection_probe_invocations)

def test_prepared_shared_inspection_cache_makes_warm_collection_process_free(self):
    cache = ConfigurationAuditCache(self.cache_root)
    cache.prepare(self.pipeline_deadline)
    capability = cache.inspection_cache()
    cold = collect_configurations_with_decision_records(
        self.source_root, (self.raw_compile_database,),
        self.launcher_effective_environment, self.production,
        self.dependency_roots, self.capability_registry,
        capability, self.engine, self.limits, self.pipeline_deadline,
        self.run_accountant)
    with self.record_compiler_process_purposes() as purposes:
        warm = collect_configurations_with_decision_records(
            self.source_root, (self.raw_compile_database,),
            self.launcher_effective_environment, self.production,
            self.dependency_roots, self.capability_registry,
            capability, self.engine, self.limits, self.pipeline_deadline,
            self.run_accountant)
    self.assertEqual(purposes, ())
    self.assertGreater(cold.inspection_probe_invocations, 0)
    self.assertEqual(warm.inspection_probe_invocations, 0)
    self.assertEqual(warm.configurations, cold.configurations)
    self.assertEqual(capability.root_identity, cache.root_identity)

def test_inspection_cache_capability_is_explicit_and_root_bound(self):
    with self.assertRaisesRegex(AuditInfrastructureError, "prepared cache"):
        self.unprepared_cache.inspection_cache()
    with self.assertRaisesRegex(AuditInfrastructureError, "cache root identity"):
        collect_configurations_with_decision_records(
            self.source_root, (self.raw_compile_database,),
            self.launcher_effective_environment, self.production,
            self.dependency_roots, self.capability_registry,
            self.capability_from_replaced_root, self.engine,
            self.limits, self.pipeline_deadline, self.run_accountant)

def test_inspection_probe_uses_minimum_of_pipeline_and_60_second_deadline(self):
    with self.record_subprocess_deadline(now=1000.0) as observed:
        inspect_compiler(
            self.compiler, CompilerFamily.CLANG, self.launcher_environment,
            self.dependency_roots, self.inspection_cache, self.engine,
            1040.0, self.limits, self.run_accountant)
    self.assertEqual(observed.value, 1040.0)
    with self.record_subprocess_deadline(now=1000.0) as observed:
        inspect_compiler(
            self.compiler, CompilerFamily.CLANG, self.launcher_environment,
            self.dependency_roots, self.inspection_cache, self.engine,
            1100.0, self.limits, self.run_accountant)
    self.assertEqual(observed.value, 1060.0)
    with self.assertRaisesRegex(AuditInfrastructureError,
                               "pipeline deadline"):
        inspect_compiler(
            self.compiler, CompilerFamily.CLANG, self.launcher_environment,
            self.dependency_roots, self.inspection_cache, self.engine,
            999.0, self.limits, self.run_accountant)

def test_decision_arguments_relocate_but_preserve_semantic_paths_and_flags(self):
    def normalized(fixture):
        return normalize_decision_arguments(
            fixture.rewritten, fixture.cwd, fixture.working_directory_role,
            fixture.source_root, fixture.build_root, fixture.toolchain_root)

    for spelling in ("separate", "attached"):
        original = self.rewritten_decision_fixture(
            spelling=spelling, include=self.source_root / "include",
            sysroot=self.toolchain_root / "sysroot",
            output=self.build_root / "out.o", depfile=self.build_root / "out.d")
        relocated = self.relocate_rewritten_decision_fixture(original)
        self.assertEqual(normalized(original), normalized(relocated))
    baseline = normalized(self.semantic_rewritten_fixture())
    self.assertNotEqual(
        baseline, normalized(
            self.semantic_rewritten_fixture(include_suffix="other")))
    self.assertNotEqual(
        baseline, normalized(
            self.semantic_rewritten_fixture(define="OLR_GPU=0")))
    with self.assertRaisesRegex(AuditInfrastructureError,
                               "unclassified path-bearing option"):
        normalized(self.rewritten_fixture_with_unknown_path_option())

def test_rewrite_and_decision_share_response_expanded_typed_spans(self):
    classification = classify_command_rewrite(
        self.response_file_launcher_command, self.cwd, CompilerFamily.CLANG)
    self.assertEqual(
        tuple(span.role for span in (
            (classification.compiler,) + classification.sources
            + classification.nonsemantic_outputs
            + classification.nonsemantic_dependencies
            + classification.semantic_scalars
            + classification.semantic_paths)),
        self.expected_typed_roles)
    self.assertIn("forwarded", tuple(
        span.attachment for span in classification.semantic_paths))
    with mock.patch("gpu_capability_command.classify_command_rewrite",
                    return_value=classification) as classify:
        rewritten = rewrite_preprocess_command(
            self.configuration, self.dependency_output)
    classify.assert_called_once_with(
        self.configuration.arguments, self.configuration.working_directory,
        self.configuration.family)
    normalized = normalize_decision_arguments(
        rewritten, self.cwd, self.working_directory_role,
        self.source_root, self.build_root, self.toolchain_root)
    self.assertEqual(rewritten.classification, classification)
    self.assertEqual(
        normalized,
        self.expected_cwd_resolved_typed_root_and_working_role_suffixes)

def test_compiler_digest_is_path_independent_but_content_and_version_bound(self):
    first, relocated = self.equal_compiler_bytes_at_different_paths()
    self.assertEqual(compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.8"),
                     compiler_digest(CompilerFamily.CLANG, relocated, "clang 18.1.8"))
    self.assertNotEqual(compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.8"),
                        compiler_digest(CompilerFamily.CLANG, self.changed_binary,
                                        "clang 18.1.8"))
    self.assertNotEqual(compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.8"),
                        compiler_digest(CompilerFamily.CLANG, first, "clang 18.1.9"))

def test_decision_loader_requires_current_capacity_complete_samples_and_optimum(self):
    self.assertEqual(allowed_worker_counts(1), (1,))
    self.assertEqual(allowed_worker_counts(4), (2, 1, 3, 4))
    decision = self.valid_decision(
        effective_worker_capacity=4,
        sample_worker_counts=(2, 1, 3, 4), selected_workers=3,
        elapsed_seconds={2: 80.0, 1: 120.0, 3: 70.0, 4: 70.0})
    write_worker_decision_atomic(self.path, decision)
    self.assertEqual(load_worker_decision(self.path, decision.key), decision)
    with mock.patch("gpu_capability_calibration.effective_worker_capacity",
                    return_value=3), \
         self.assertRaisesRegex(AuditInfrastructureError,
                               "effective worker capacity"):
        load_worker_decision(self.path, decision.key)
    for artifact in (
            self.with_selected_over_capacity(decision),
            self.without_sample(decision, 4),
            self.with_selected_workers(decision, 4)):
        with self.subTest(artifact=artifact), self.assertRaisesRegex(
                AuditInfrastructureError, "worker decision"):
            load_worker_decision(self.write_artifact(artifact), decision.key)

def test_reference_envelope_is_exact_and_affinity_constrained(self):
    envelope = reference_calibration_envelope()
    self.assertEqual(envelope, ReferenceCalibrationEnvelope(
        runner_image="windows-2022", python_version="3.11.9",
        qt_package="qt-6.10.2-win64_mingw",
        mingw_package="tools_mingw1310-13.1.0",
        worker_capacity=4, affinity_cpu_indices=(0, 1, 2, 3)))
    with self.reference_host(logical_capacity=8) as host:
        constrain_reference_affinity(envelope)
        self.assertEqual(host.process_affinity(), (0, 1, 2, 3))
    with self.reference_host(logical_capacity=3), self.assertRaisesRegex(
            AuditInfrastructureError, "reference worker capacity"):
        constrain_reference_affinity(envelope)

def test_loader_requires_reaped_samples_and_exact_selected_parameters(self):
    decision = self.valid_decision(
        sample_worker_counts=(2, 1, 3, 4), selected_workers=3,
        sample_maximum_tasks={2: 20, 1: 30, 3: 18, 4: 12},
        sample_recycle_rss={2: 96 << 20, 1: 128 << 20,
                            3: 80 << 20, 4: 64 << 20},
        surviving_owned_process_count=0)
    selected = next(sample for sample in decision.samples
                    if sample.worker_count == decision.selected_workers)
    self.assertEqual(decision.maximum_tasks_per_worker,
                     selected.maximum_tasks_per_worker)
    self.assertEqual(decision.recycle_rss_bytes, selected.recycle_rss_bytes)
    for artifact in (
            self.with_sample_field_missing(
                decision, "surviving_owned_process_count"),
            self.with_sample_field_missing(decision, "pilot_eligible"),
            self.with_sample_field(decision, "pilot_eligible", 1),
            self.with_sample_field(decision, "surviving_owned_process_count", 1),
            self.with_sample_field(decision, "surviving_owned_process_count", True),
            self.with_sample_field(decision, "surviving_owned_process_count", "0"),
            self.with_sample_missing_stage(decision, "shutdown-reap"),
            self.with_sample_field_missing(decision, "memory"),
            self.with_sample_memory_field_missing(
                decision, "aggregate_peak_rss_upper_bound_bytes"),
            self.with_unknown_sample_field(
                decision, "aggregate_peak_rss_bytes", 1),
            self.with_unknown_sample_field(decision, "compiler_invocations", 502),
            self.with_sample_field_missing(decision,
                                           "inspection_probe_invocations"),
            self.with_sample_field_missing(decision,
                                           "audit_compiler_invocations"),
            self.with_sample_memory_field(
                decision, "aggregate_peak_rss_upper_bound_bytes", 448 << 20),
            self.with_decision_maximum_tasks(decision,
                                             selected.maximum_tasks_per_worker + 1),
            self.with_decision_recycle_rss(decision,
                                           selected.recycle_rss_bytes + 1)):
        with self.subTest(artifact=artifact), self.assertRaisesRegex(
                AuditInfrastructureError, "worker decision"):
            load_worker_decision(self.write_artifact(artifact), decision.key)
```

`WORKER_DECISION_SCHEMA_BYTES` and `WORKER_SELECTION_SEMANTICS_BYTES` are explicit versioned constants. The decision fingerprint frames the freshly attested audit digest, constants and exhaustive process-tree/calibration module-root graph; local module identity is checked separately and never enters the portable digest. The existing configuration/environment digests remain unchanged. `calibrate_platform_worker_count(platform, source_root, compile_commands, cache_parent, limits)` constructs the matching native runner and exact platform envelope/key; tests inject only through `_calibrate_platform_worker_count_with_runner(...)`.

`collect_configurations_with_decision_records()` preserves ordered compile databases, computes the decision projection from explicit inputs, and never enumerates production or reconstructs cache/accounting authority. Its caller passes the prepared root-bound inspection capability, immutable production table, attested engine, run accountant and deadline. Every inspection process is registered/contained before exec and emits `INSPECTION`; cold/warm counters are observed, not inferred. The inspection carrier key is portable (executable content, normalized environment/family/arguments/engine/platform role), while held executable native identity/content is revalidated operationally.

`classify_command_rewrite()` is the single frozen parser shared by the existing preprocess rewrite and decision normalization. It expands bounded response files, strips classified launchers/assignments, and returns typed spans over the resulting immutable argument tuple: compiler, source, nonsemantic output/depfile/dependency-target, semantic scalar, and semantic path, including attached, separate, and forwarded option spellings. `normalize_decision_arguments()` accepts the `RewrittenCommand` and consumes only `rewritten.classification` plus its explicit cwd/role/root context. It excludes the compiler span because `compiler_digest` owns driver identity and removes only classifier-proven nonsemantic spans. It resolves every relative semantic path against the recorded cwd before replacing complete roots with `<SOURCE_ROOT>/<suffix>`, `<BUILD_ROOT>/<suffix>`, `<TOOLCHAIN_ROOT>/<suffix>`, or `<WORKING_DIRECTORY:{role}>/<suffix>`. It preserves order and multiplicity of definitions, language/standard flags, include/sysroot/framework/forced-include options, target/architecture flags, module/PCH flags, and all other classified semantic operands. A path-bearing option or operand not classified by the shared table is an infrastructure failure; substring replacement and best-effort dropping are forbidden. Tests cover response expansion, launcher stripping, forwarded/separate/attached spans, cwd-relative paths, working-directory-role suffixes, relocation equality, output/depfile invariance, semantic path/definition changes, duplicated flags, and unknown path-bearing failure.

The public API remains exactly `rewrite_preprocess_command(configuration, dependency_output) -> RewrittenCommand`. Its implementation calls `classify_command_rewrite(configuration.arguments, configuration.working_directory, configuration.family)` once, performs the existing rewrite from those spans, and stores the same frozen object in the new `RewrittenCommand.classification` field. Decision normalization accepts the returned `RewrittenCommand` and consumes `.classification`; no caller-facing rewrite overload or parallel parser is introduced.

`DECISION_ENVIRONMENT_SCHEMA_BYTES` versions the family-specific semantic allowlist. GCC/MinGW includes path-list variables `CPATH`, `C_INCLUDE_PATH`, `CPLUS_INCLUDE_PATH`, `OBJC_INCLUDE_PATH`, `COMPILER_PATH`, and `GCC_EXEC_PREFIX`. The existing `CompilerFamily.CLANG` allowlist conservatively includes those plus path-valued `SDKROOT` and scalar `MACOSX_DEPLOYMENT_TARGET` on every platform. All relative path or path-list entries resolve against the recorded cwd before typed-root substitution. No additional compiler-family enum/string is introduced; AppleClang remains the actual probed driver represented by Clang. MSVC and clang-cl include `INCLUDE` and `WindowsSdkDir`; the existing fail-closed rejection of nonempty `CL` or `_CL_` remains, so those unreachable variables are excluded from the decision allowlist and mutation tests. Explicit non-secret launcher assignments remain included. Ambient keys outside the allowlist are excluded, including CI metadata, temp locations, and secret/token/credential names. The exact full effective-environment digest in `PreprocessConfiguration` remains unchanged for compiler/cache identity.

`decision_environment_digest()` length-frames the schema, compiler family, allowed collection-time entries, and allowed explicit launcher assignments after cwd-aware typed-root substitution; it never rewrites an arbitrary substring. `configuration_set_digest()` verifies every configuration has one matching record and one common path-independent compiler digest, uses `configuration_digest` only for the join, and hashes the sorted multiset of normalized semantic fields with multiplicity. Tests prove CI/temp/token invariance, `INCLUDE`/`CPATH`/`SDKROOT`/deployment-target/relevant-launcher sensitivity, nonempty `CL`/`_CL_` rejection before projection, compiler relocation equality/content-version sensitivity, and exact environment/cache identity preservation.

`effective_worker_capacity()` clamps to at least one and takes the minimum of `AuditLimits.workers`, affinity-aware logical CPUs (`GetProcessAffinityMask` on Windows, `os.sched_getaffinity()` on POSIX, and `os.cpu_count()` only as fallback), and any applicable process/Job Object/cgroup CPU quota. The pinned reference envelope is exactly `windows-2022`, CPython `3.11.9`, Qt package `qt-6.10.2-win64_mingw`, MinGW package `tools_mingw1310-13.1.0`, worker capacity four, and affinity indices `(0,1,2,3)`. Calibration and normal reference CI first require host capacity at least four and then constrain the top-level process and inherited children to exactly those processors; extra host CPUs never expand the decision key or measured count set. `allowed_worker_counts(capacity)` therefore uses the reference capacity and returns exactly `(2,1,3,4)`. `platform_tag` includes OS, architecture, pointer width, and exact Python major/minor/patch. `reference_envelope_digest` length-frames every pinned field and is part of `key_digest` beside artifact schema, platform tag, compiler digest, configuration-set digest, decision-engine fingerprint, and effective worker capacity. The canonical JSON has an exact schema/version and no unknown keys.

Pilot derivation is exact and decision-keyed by `WORKER_SELECTION_SEMANTICS_BYTES`. For worker count `W` and configuration count `N`, every platform uses only each slot's consecutive native post-return prefix beginning at task ordinal one; the prefix stops at the first missing/duplicate ordinal, protocol/accounting failure, or platform-unsafe sample. Let `k_i` be each slot's safe length and set `maximum_tasks_per_worker = min(ceil(N / W), min(k_i))`. Zero is ineligible and uses the explicit diagnostic measured-run floor `(1, 1)` only to retain the sample.

Windows safety remains `accounting_complete and aggregate_peak_rss_upper_bound_bytes < 448 MiB`; with `U=max(aggregate_peak_rss_upper_bound_bytes)` and `R0=max(worker_current_rss_bytes)`, it requires `headroom=(448 MiB)-1-U >= W` and derives `recycle_rss_bytes = R0 + headroom // W`. Linux never reads the Windows RSS-upper-bound field: a sample is safe only when accounting is complete, service/run OOM/OOM-kill/max deltas are zero, and both cgroup current and peak are strictly below `memory.high` and `memory.max`; with `U=max(cgroup_peak_accounted_memory_bytes)`, `R0=max(worker_current_rss_bytes)`, it requires `headroom=memory.high-1-U >= W` and derives the same checked `R0 + headroom // W`. macOS makes no hard memory claim: safety requires complete registration/reconciliation with zero survivors/unreconciled descendants, uses exact task-count generation recycling, and serializes `recycle_rss_bytes = UINT64_MAX` as an explicitly disabled RSS-recycle sentinel; runtime conversion recognizes only that macOS sentinel and never compares observed residency to a hard gate. Every platform also requires pilot shutdown/reap and zero survivors. No percentages, regression, minimum clamp, alternate rounding, or post-selection re-derivation is allowed. Tests freeze every native formula, discriminant/field rejection, zero-prefix, nonconsecutive ordinal, boundary byte, uneven-prefix, macOS sentinel, and diagnostic floor.

Every serialized Windows `WorkerCalibrationSample` contains the exact applied `maximum_tasks_per_worker`/`recycle_rss_bytes`, typed `pilot_eligible`, complete `WindowsRunMemoryMeasurements`, separate `inspection_probe_invocations` and `audit_compiler_invocations`, plus `surviving_owned_process_count`, captured only after successful normal `shutdown-reap` before the one acceptance deadline. A passing 251-configuration measured sample requires `pilot_eligible is True`, `audit_compiler_invocations == 502`, finite nonnegative `inspection_probe_invocations` matching the collection fixture, cold `<180.000s`, `memory.accounting_complete is True`, equal Job total/retained identity counts, `memory.aggregate_peak_rss_upper_bound_bytes < 448 MiB`, the exact included-stage tuple, and zero survivors. A measured attempt that cannot reach normal shutdown is represented only by `CalibrationRejection`; the fixed `CALIBRATION_FAILURE_STAGES`/`CALIBRATION_FAILURE_CODES` enums exclude free-form exception text, and cleanup must be complete with zero survivors. Loading independently derives current capacity, requires `attempted_worker_counts` to equal the exact allowed order, requires completed samples and rejections to partition those counts without overlap, rejects unknown/legacy flat memory or ambiguous `compiler_invocations` fields, recomputes passing status over completed samples, then selects minimum elapsed with an exact tie toward lower worker count. At least one completed passing sample is mandatory. `job_peak_commit_charge_bytes` is validated/reported separately and never satisfies RSS. The decision's two runtime parameters must equal the selected measured sample byte-for-byte. Malformed hashes, nonfinite values, platform/compiler/configuration/decision-engine/reference-envelope mismatch, altered key digest, non-atomic carrier state, or a selected nonpassing/nonoptimal count fails closed.

- [ ] **Step 4: Implement the fake-pipeline harness and run GREEN**

Run: `python -m unittest tests.gpu.test_gpu_capability_process_tree tests.gpu.test_gpu_capability_calibration tests.gpu.test_gpu_capability_command tests.gpu.test_gpu_capability_runner tests.gpu.test_gpu_capability_audit_lanes -v`

Expected: native accounting fixtures pass; fake calibration covers every top-level stage; decision codecs fail closed. Task 6 emits no real decision; only final Task-10 Step 7 calibration creates checked artifacts.

- [ ] **Step 5: Commit calibration infrastructure without a fabricated decision**

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_process_tree.py tests/gpu/gpu_capability_calibration.py tests/gpu/gpu_capability_command.py tests/gpu/gpu_capability_runner.py tests/gpu/test_gpu_capability_model.py tests/gpu/test_gpu_capability_process_tree.py tests/gpu/test_gpu_capability_calibration.py tests/gpu/test_gpu_capability_command.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_audit_lanes.py tests/gpu/fixtures/fake_preprocessor.py
git commit -m "test(gpu): define capability worker decisions" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: Add calibrated spawn-process coordination and native platform memory enforcement

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/gpu_capability_model.py`
- Modify: `tests/gpu/fixtures/fake_preprocessor.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Consumes a frozen `WorkerRuntimeContract`, initial production digest map, and `OwnedProcessTree`; decision/correctness callers construct the contract outside the scheduler. Produces an internal `_ScheduledAuditSession` containing deterministic compact results plus counters and owned resources. The scheduler uses parent-owned spawned processes, warm validation, generation-bound one-way task/command/control/registration/launch/payload Connections under one reactor, one absolute deadline, complete process trees, measured recycling, and crash cleanup.

Advance the attestation stage/schema in this same commit to include the coordinator, generation reactor, control-wire codec/schema, Task-7 transport/decode/publication extensions to the Task-3 budget, containment adapters and all their transitive helpers. Core budget/growth/aggregator semantics remain owned and already green in Task 3. Worker and parent independently use the identical exhaustive algorithm and stage identity; neither side rereads module path bytes.

- [ ] **Step 1: Write failing process-boundary tests**

```python
def test_251_configurations_never_serialize_or_retain_views(self):
    decision = self.passing_decision()
    pipeline_deadline = time.monotonic() + 180.0
    runtime_contract = runtime_contract_from_decision(
        decision, pipeline_deadline)
    session = schedule_configuration_audits(
        self.source_root, self.configurations(251), self.dependency_roots,
        self.capability_registry,
        self.initial_digest_map,
        self.prepared_cache,
        self.limits, self.engine, runtime_contract,
        inspection_probe_invocations=0,
        run_accountant=self.run_accountant)
    aggregate = session.consume_aggregate()
    self.assertEqual(len(aggregate.ordered_configuration_digests), 251)
    self.assertEqual(session.cache_hits, 251)
    self.assertEqual(session.cache_misses, 0)
    self.assertEqual(opened, self.unique_dependency_count)
    session.shutdown_reap(pipeline_deadline)
    self.assertEqual(self.maximum_parent_live_views, 0)
    self.assertEqual(self.maximum_live_views_per_worker, 1)
    self.assertLessEqual(self.maximum_aggregate_worker_live_views,
                         runtime_contract.workers)

def test_parent_load_many_validates_warm_251_before_spawning(self):
    self.populate_cold_cache_with_forced_worker_recycling()
    with mock.patch("multiprocessing.Process.start") as start, \
         self.count_dependency_opens() as opened:
        session = self.schedule_warm_251()
    start.assert_not_called()
    aggregate = session.consume_aggregate()
    self.assertEqual(len(aggregate.ordered_configuration_digests), 251)

def test_collection_counter_is_threaded_to_scheduler_without_recounting(self):
    collection = self.collect_real_fake_configurations()
    session = self.schedule_collection(collection)
    self.assertEqual(session.inspection_probe_invocations,
                     collection.inspection_probe_invocations)
    self.assertEqual(session.audit_compiler_invocations,
                     2 * len(collection.configurations))

def test_slow_first_configuration_bounds_ordinal_dispatch_window(self):
    run = self.run_real_reactor_251(first_blocked=True, remaining_fast=True)
    self.assertTrue(run.completed)
    for snapshot in run.dispatch_snapshots:
        self.assertTrue(all(
            snapshot.next_unaggregated <= ordinal
            <= snapshot.next_unaggregated + snapshot.worker_count - 1
            for ordinal in snapshot.dispatched_ordinals))
    self.assertLessEqual(run.maximum_pending_results, run.worker_count)
    self.assertFalse(run.deadlocked)

def test_platform_memory_violation_cancels_all_workers(self):
    self.launch_worker_compiler_grandchild_trees((200, 250), (180, 260))
    with self.assertRaisesRegex(AuditInfrastructureError,
                                "process-tree memory contract"):
        self.run_scheduler()
    self.assertEqual(self.all_platform_accounted_or_registered_survivors(), ())

def test_emergency_cleanup_deadline_is_failure_only_and_never_makes_acceptance_pass(self):
    with self.record_stage_deadlines(start=1000.0) as observed, \
         self.force_pipeline_expiry(at=1180.0), \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "pipeline deadline"):
        self.run_scheduler_and_outer_policy()
    self.assertEqual(set(observed.normal_stage_deadlines), {1180.0})
    self.assertEqual(observed.abort_and_reap_deadline, 1210.0)
    self.assertNotIn(1210.0, observed.normal_stage_deadlines)
    self.assertFalse(observed.acceptance_passed)

def test_successful_shutdown_reap_finishes_before_pipeline_deadline_without_emergency(self):
    with self.record_stage_deadlines(start=1000.0) as observed:
        run = self.run_successful_pipeline()
    self.assertLess(run.finished_at, 1180.0)
    self.assertEqual(observed.shutdown_reap_deadline, 1180.0)
    self.assertIsNone(observed.abort_and_reap_deadline)

def test_cold_results_use_bounded_canonical_pipe_transport(self):
    with self.record_decoded_wire_message_types() as wire, \
         self.record_payload_connections() as payloads:
        session = self.run_cold_251()
    self.assertNotIn(ConfigurationAuditResult, wire)
    self.assertNotIn(ConfigurationAuditOutcome, wire)
    self.assertEqual(
        wire.result_messages, (WorkerPayloadReady, WorkerFailure,
                               WorkerRetire, WorkerStopped))
    self.assertEqual(payloads.maximum_payload_bytes,
                     (4 * 1024 * 1024,) * 251)
    self.assertTrue(all(size <= 64 * 1024 for size in payloads.chunk_sizes))
    self.assertLessEqual(session.compact_result_peak_live_bytes,
                         128 * 1024 * 1024)

def test_transport_allocation_layout_counts_each_live_copy_once(self):
    bounds = self.preparse_bounds(encoded=4 << 20,
                                  decoded=16 << 20, retained=3 << 20)
    layout = _transport_allocation_bound(
        bounds.encoded_bytes, bounds, self.allocation_schema,
        self.pipe_kernel_capacity)
    self.assertEqual(layout.permit_reservation_bytes,
                     layout.canonical_encoder_scratch_bytes
                     + layout.sender_payload_bytes + layout.pipe_frame_bytes
                     + layout.pipe_kernel_capacity_bytes
                     + layout.receiver_payload_bytes)
    self.assertEqual(layout.decode_reservation_bytes,
                     layout.receiver_payload_bytes
                     + layout.json_decoded_transient_bytes
                     + layout.retained_result_bytes)
    self.assertEqual(layout.peak_pending_bytes,
                     max(layout.counting_pass_peak_bytes,
                         layout.permit_reservation_bytes,
                         layout.decode_reservation_bytes))
    self.assertEqual(self.instrument_actual_peak(layout),
                     self.expected_nonduplicated_component_peak)
    self.exercise_transport_budget_boundary((128 << 20) - 1, accepted=True)
    self.exercise_transport_budget_boundary(128 << 20, accepted=True)
    self.exercise_transport_budget_boundary((128 << 20) + 1, accepted=False)

def test_mixed_hit_miss_budget_charges_ready_decode_and_retained_before_accept(self):
    self.seed_hits_just_below_compact_result_cap()
    with self.record_compact_result_budget_events() as events, \
         mock.patch("gpu_capability_runner.decode_compact_result",
                    wraps=decode_compact_result) as decode:
        with self.assertRaisesRegex(AuditInfrastructureError,
                                   "compact result memory limit"):
            self.run_one_cold_miss_exceeding_cap_by_one()
    self.assertEqual(events[-3:],
                     ("reserve-queued", "preparse",
                      "reject-before-decode-and-retain"))
    decode.assert_not_called()
    self.assertEqual(self.all_platform_accounted_or_registered_survivors(), ())

def test_declared_or_actual_oversized_worker_payload_fails_before_decode(self):
    for corruption in ("declared-over-limit", "pipe-over-limit",
                       "length-mismatch", "digest-mismatch"):
        with self.subTest(corruption=corruption), \
             mock.patch("gpu_capability_runner.decode_compact_result") as decode, \
             self.assertRaisesRegex(AuditInfrastructureError,
                                    "worker result payload"):
            self.run_corrupt_payload_worker(corruption)
        decode.assert_not_called()
        self.assertEqual(self.all_platform_accounted_or_registered_survivors(), ())

def test_payload_pipe_wait_is_deadline_and_generation_bound(self):
    for platform in ("windows", "posix"):
        with self.subTest(platform=platform), \
             self.record_native_endpoint_waits(platform) as waits:
            self.run_one_delayed_payload(platform)
        if platform == "windows":
            self.assertTrue(waits.used_overlapped_event_and_cancel_io_ex)
        else:
            self.assertTrue(waits.used_nonblocking_selector)
        self.assertTrue(waits.polled_tree_health_between_waits)
        self.assertFalse(waits.called_blocking_io)
    for failure in ("deadline", "worker-exit", "generation-change", "eof"):
        with self.subTest(failure=failure), self.assertRaisesRegex(
                AuditInfrastructureError, "worker result payload"):
            self.run_payload_wait_failure(failure)
        self.assertEqual(self.all_platform_accounted_or_registered_survivors(), ())

def test_worker_containment_and_engine_fingerprint_precede_ready(self):
    for platform in ("windows", "linux", "macos"):
        with self.subTest(platform=platform), self.record_worker_startup() as events:
            self.run_one_worker(platform)
        self.assertEqual(events[:3],
                         ("containment-ack", "recompute-engine", "engine-ready"))
    with self.mutate_child_engine_module(), self.assertRaisesRegex(
            AuditInfrastructureError, "worker audit engine fingerprint"):
        self.run_one_worker("windows")

def test_compact_slot_excludes_worker_audit_working_set(self):
    slot = maximum_compact_result_slot(
        self.limits, self.allocation_schema, self.pipe_kernel_capacity)
    self.assertLessEqual(slot.worst_case_live_bytes, 128 << 20)
    self.assertNotIn("PreprocessedTranslationUnitView", slot.included_components)
    self.assertNotIn("AuditBuffer", slot.included_components)
    self.assertNotIn("CompilerAuditAnalysis", slot.included_components)
    self.assertNotIn("retained_token_bytes", slot.included_components)
    self.assertEqual(self.worker_audit_working_set_budget_backend,
                     "os-owned-process-memory")

def test_impossible_compact_slot_or_stranded_remaining_task_fails_immediately(self):
    with mock.patch("multiprocessing.Process.start") as start, \
         mock.patch("gpu_capability_process_tree.OwnedProcessTree.create") as tree, \
         self.record_channel_cgroup_and_job_construction() as native, \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "single compact-result slot"):
        self.run_with_maximum_slot((128 << 20) + 1)
    start.assert_not_called()
    tree.assert_not_called()
    self.assertEqual(native.created, ())
    with self.retained_hits_leave_no_dispatch_capacity(), \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "remaining compact task cannot be admitted"):
        self.run_mixed_without_pending_workers()
    self.assertEqual(self.native_endpoint_waits_after_impossible_admission, 0)

def test_retire_or_exit_with_any_work_never_leaves_zero_live_generations(self):
    for work in ("remaining", "pending"):
        with self.subTest(work=work), self.force_last_generation_retire(work):
            self.run_to_completion()
        self.assertTrue(self.replacement_spawned_before_next_wait)
    with self.force_replacement_spawn_failure(), self.assertRaisesRegex(
            AuditInfrastructureError, "replacement generation"):
        self.run_last_worker_exit_with_remaining_work()
    self.assertEqual(self.wait_calls_after_zero_live_generation, 0)

def test_recycled_generation_gets_fresh_tree_and_archives_empty_telemetry(self):
    first, second = self.run_one_recycled_worker_slot()
    self.assertIsNot(first.tree, second.tree)
    self.assertTrue(first.tree_verified_empty_before_close)
    self.assertTrue(second.tree_created_and_registered_before_dispatch)
    self.assertEqual(self.slot_peak_upper,
                     max(first.archived_generation_upper,
                         second.archived_generation_upper))

def test_one_run_accountant_identity_reaches_every_layer_and_cold_snapshot(self):
    with self.record_run_accountant_consumers() as observed:
        self.run_cold_pipeline()
    self.assertEqual(set(observed.object_ids), {id(self.run_accountant)})
    self.assertEqual(observed.layers, (
        "pipeline", "schedule", "reactor", "tree-create",
        "measurements", "cold-end-snapshot"))

def test_every_worker_control_channel_uses_bounded_fixed_wire_not_pickle(self):
    with self.forbid_multiprocessing_connection_construction(), \
         self.record_bounded_frame_receive_limits() as calls:
        self.run_all_control_message_kinds()
    self.assertTrue(calls)
    self.assertEqual(set(calls), {CONTROL_MAX_BYTES + 1})
    for payload in self.oversized_malformed_unknown_and_pickled_control_frames():
        with self.subTest(payload=payload[:16]), \
             mock.patch("gpu_capability_runner.decode_control_message") as decode, \
             self.assertRaises(AuditInfrastructureError):
            self.receive_control_frame(payload)
        decode.assert_not_called()

def test_parent_uses_only_bounded_wire_writes_and_explicit_stop_handshake(self):
    with self.record_worker_protocol_io() as io:
        self.run_one_cold_task_to_shutdown()
    self.assertTrue(io.all_task_and_command_writes_used_bounded_send_bytes)
    self.assertTrue(io.all_sends_waited_for_readiness_with_deadline)
    self.assertEqual(io.shutdown_tail,
                     (WorkerStop, WorkerStopped, "close-connections",
                      "zero-survivors"))
    self.assertFalse(io.waited_on_any_output_endpoint)
    self.assertFalse(io.used_connection_send_for_parent_commands)
```

Add true PID separation, simultaneous CPU jobs at every calibrated count, deterministic result order, only-miss dispatch, first-failure cancellation, real spawn-`Event` cancellation, global deadline, worker crash, bounded task/command pipe backpressure, compiler/grandchild cleanup, PID/start-time replacement, RSS sequence rollback, recycling, and shutdown bounds. Force non-writable task/command pipes, closed peers, worker death, event/payload EOF, generation replacement and deadline expiry; no send/receive may deadlock. Protocol tests reject pickle objects, unknown tags, booleans/out-of-range values, oversized compile arguments, duplicate/wrong task IDs or generations, length/hash mismatch, retirement with outstanding work, missing acknowledgement, and exit before `WorkerStopped`.

Authority protocol tests prove the task frame contains only the portable authority digest, `decode_task_frame(payload, expected_authority)` binds the receiver's already-held exact local object, and all in-child calls preserve that identity. They reject a parent/child digest mismatch and an equal-digest carrier whose resolved local root or native root identity differs, before readiness, dispatch, compiler launch, inspection-cache lookup, or result-cache access. No test compares Python object identity across a spawn boundary.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.ProcessCoordinatorTests -v`

Expected: current coordinator cannot prove parent-only warm validation, PID separation, typed lifecycle acknowledgement, or the native platform memory contract.

- [ ] **Step 3: Implement spawn workers and telemetry loop**

```python
engine = _attest_loaded_audit_engine(expected_audit_engine_fingerprint)
accounting = _new_compact_accounting_observer()
result_budget = CompactResultMemoryBudget(
    maximum_bytes=128 * 1024 * 1024, observer=accounting)
slot_template = maximum_compact_result_slot(
    limits, conservative_allocation_schema(), _PIPE_KERNEL_CAPACITY_BYTES)
if slot_template.worst_case_live_bytes > 128 * 1024 * 1024:
    raise AuditInfrastructureError("single compact-result slot exceeds 128 MiB")
aggregator = StreamingResultAggregator(configurations, result_budget, limits)
batch = cache.load_many(
    configurations, dependency_roots, engine, production_snapshot,
    result_budget, aggregator, slot_template,
    runtime_contract.pipeline_deadline)
if not batch.misses:
    return _ScheduledAuditSession(
        aggregate=aggregator.finish(), states=(), reactor=None, trees=(),
        counters=_warm_scheduler_counters(
            batch, inspection_probe_invocations),
        runtime_contract=runtime_contract,
        cache_root=cache.root_identity.resolved_root,
        result_budget=result_budget)

workers = runtime_contract.workers
global_deadline = runtime_contract.pipeline_deadline
context = multiprocessing.get_context("spawn")
cancel_event = context.Event()
task_receivers, task_senders = zip(*(
    BoundedFrameChannel.create(TASK_MAX_BYTES) for _ in range(workers)))
command_receivers, command_senders = zip(*(
    BoundedFrameChannel.create(COMMAND_MAX_BYTES) for _ in range(workers)))
event_receivers, event_senders = zip(*(
    BoundedFrameChannel.create(CONTROL_MAX_BYTES) for _ in range(workers)))
registration_receivers, registration_senders = zip(*(
    BoundedFrameChannel.create(CONTROL_MAX_BYTES) for _ in range(workers)))
launch_receivers, launch_senders = zip(*(
    BoundedFrameChannel.create(CONTROL_MAX_BYTES) for _ in range(workers)))
payload_receivers, payload_senders = zip(*(
    BoundedPayloadChannel.create() for _ in range(workers)))
trees = tuple(OwnedProcessTree.create(index, run_accountant)
              for index in range(workers))
containment = tuple(_generation_containment(tree, engine) for tree in trees)
startup_bootstraps = tuple(_bounded_authenticated_startup_bootstrap(
    index, trees[index].generation, dependency_roots,
    capability_registry) for index in range(workers))
states = _spawn_worker_generations(
    context, workers, trees, cancel_event, task_receivers, command_receivers,
    event_senders, registration_senders, launch_senders, payload_senders,
    containment, dependency_roots, startup_bootstraps,
    capability_registry,
    source_root, cache.root, limits, engine, production_snapshot)
remaining = collections.deque(_pending_tasks_for_sorted_misses(batch.misses))
next_unaggregated = aggregator.next_unaggregated_ordinal
publication_requests = collections.deque()
active_publication = None
reactor = GenerationReactor(
    event_receivers=event_receivers,
    registration_receivers=registration_receivers,
    launch_receivers=launch_receivers,
    payload_receivers=payload_receivers,
    task_senders=task_senders, command_senders=command_senders,
    states=states, trees=trees, cancel_event=cancel_event,
    deadline=global_deadline, run_accountant=run_accountant)
while remaining or pending_tasks:
    while (remaining and reactor.has_idle_worker
           and remaining[0].ordinal
               <= next_unaggregated + runtime_contract.workers - 1):
        pending = remaining[0]
        generation = reactor.idle_generation_for(pending)
        reservation = _bind_compact_slot_to_task(
            slot_template, pending, generation)
        if not result_budget.try_reserve_before_dispatch(
                pending.task_id, reservation):
            if not pending_tasks:
                raise AuditInfrastructureError(
                    "remaining compact task cannot be admitted")
            break
        remaining.popleft()
        task = WorkerTask(
            task_id=pending.task_id, configuration=pending.configuration,
            deadline=pending.deadline, compact_reservation=reservation,
            publication_entitlement_bytes=8 * 1024 * 1024,
            dependency_root_authority_digest=
                dependency_roots.portable_authority_digest,
            dependency_root_authority=dependency_roots,
            compiler_capability_digest=
                pending.configuration.compiler_capability_digest,
            compiler_capability=
                pending.configuration.compiler_capability)
        reactor.send_task_frame_ready_or_cancel(generation, task)

    event = reactor.next_readable_event_or_payload(maximum_quantum_seconds=0.05)
    match event:
        case WorkerContained() | MacOSWorkerSessionReported() \
                | WorkerEngineReady():
            reactor.validate_startup_event(event, expected_engine=engine)
        case CompilerPgidReported():
            reactor.verify_register_and_put_compiler_permit_nowait(event)
        case CompilerLaunchEvent(purpose, configuration_digest,
                                 worker_index, generation, task_id, pid):
            reactor.record_observed_launch(event)
            _require_exact_launch_prefix_for_task(
                reactor.observed_launch_events(task_id))
        case CachePublicationRequested(worker_index, generation, task_id):
            reactor.validate_task_generation(event)
            reactor.validate_unique_publication_request(event)
            publication_requests.append(event)
        case WorkerPayloadReady(worker_index, generation, task_id,
                                encoded_bytes, encoded_sha256,
                                conservative_decoded_bytes,
                                conservative_retained_bytes,
                                counting_pass_peak_bytes,
                                stdout_bytes, stages):
            reactor.validate_task_generation(event)
            result_budget.release_root_publication_after_atomic_cleanup(task_id)
            active_publication = None
            _validate_bounded_payload_header(event, pending_tasks)
            declared_bounds = CompactResultPreparseBounds(
                encoded_bytes, conservative_decoded_bytes,
                conservative_retained_bytes)
            transport = _transport_allocation_bound(
                encoded_bytes, declared_bounds,
                conservative_allocation_schema(),
                _PIPE_KERNEL_CAPACITY_BYTES)
            _require(transport.counting_pass_peak_bytes == counting_pass_peak_bytes,
                     "worker counting allocation declaration changed")
            result_budget.require_within_pre_dispatch_reservation(
                task_id, encoded_bytes, declared_bounds,
                counting_pass_peak_bytes)
            reactor.send_command_frame_ready_or_cancel(WorkerPayloadPermit(
                worker_index, generation, task_id))
            payload = reactor.receive_bounded_ready_payload(
                event, maxlength=4 * 1024 * 1024 + 1)
            _validate_payload_length_and_sha256(event, payload)
            bounds = preparse_compact_result(payload)  # no json allocation
            _require(bounds == declared_bounds,
                     "worker payload allocation bounds changed")
            result_budget.transition_to_decode(
                task_id, bounds.conservative_decoded_bytes,
                bounds.conservative_retained_bytes)  # before json.loads
            result = decode_compact_result(payload, bounds)
            _validate_result_for_task(result, event, pending_tasks)
            result_budget.commit_retained_result(task_id)
            _require_exact_two_parent_observed_launches(
                reactor.observed_launch_events(task_id),
                expected=(CompilerLaunchPurpose.AUDIT_DISCOVERY,
                          CompilerLaunchPurpose.AUDIT_ACCEPTED))
            reactor.accept_result_in_order_or_hold_bounded(
                event, result,
                result_budget.capture_ownership(f"result:{task_id}"),
                aggregator, maximum_held=runtime_contract.workers)
            # Advances only across the contiguous accepted prefix; an
            # out-of-order completion never moves the dispatch window.
            next_unaggregated = aggregator.next_unaggregated_ordinal
        case WorkerFailure(worker_index, generation, task_id, diagnostic):
            _validate_live_generation(states, worker_index, generation)
            raise AuditInfrastructureError(_bounded_diagnostic(diagnostic))
        case WorkerRetire(worker_index, generation, reason):
            reactor.put_command_nowait_or_cancel(WorkerRetireAck(
                worker_index, generation))
            reactor.join_reaped_generation_after_ack(event)
            if remaining or pending_tasks:
                reactor.spawn_next_generation(event.worker_index)
        case _:
            raise AuditInfrastructureError("invalid worker protocol message")
    reactor.remove_stale_generation_publication_requests(publication_requests)
    if active_publication is None and publication_requests:
        request = publication_requests.popleft()
        publication_permit = result_budget.activate_root_publication(
            request.task_id, request.generation)
        reactor.send_command_frame_ready_or_cancel(publication_permit)
        active_publication = request
    tree_sample = _sample_live_and_archived_owned_forest(
        states=reactor.live_generation_states,
        archived_generations=reactor.archived_generation_telemetry,
        run_accountant=run_accountant)
    _enforce_platform_memory_contract(tree_sample)
    _enforce_deadline_and_health(
        reactor.live_generation_states,
        reactor.archived_generation_telemetry,
        run_accountant, cancel_event, global_deadline)
    if (remaining or pending_tasks) and reactor.live_generation_count == 0:
        reactor.spawn_replacement_or_raise_immediately()

return _ScheduledAuditSession(
    aggregate=aggregator.finish(),
    states=states, reactor=reactor, task_senders=task_senders,
    command_senders=command_senders, run_accountant=run_accountant,
    counters=_scheduler_counters(
        ..., inspection_probe_invocations=inspection_probe_invocations,
        audit_compiler_invocations=reactor.authoritative_audit_launch_count,
        expected_audit_compiler_invocations=2 * len(batch.misses)),
    runtime_contract=runtime_contract,
    cache_root=cache.root_identity.resolved_root,
    result_budget=result_budget)
```

The scheduler accepts explicit snapshot/cache/runtime/accounting inputs and returns only an internal closeable session. It constructs observer/budget before `load_many()`, transfers warm ownership, and computes slot/residual feasibility before creating any frame channel, Job/cgroup/tree or worker. Impossible admission has zero native/process starts. Launch counts come only from authenticated control frames; warm has zero workers/probes.

Workers never serialize result objects; only fixed control frames and bounded canonical payload bytes cross process boundaries. `_transport_allocation_bound()` names every live draft/encoder/payload/publication/receiver/decode/retained phase; worker audit working set remains native physical accounting.

Each worker generation gets fresh one-way bounded task, command and control frame channels plus a separate compact-payload channel, bound to `(worker_index, generation)`. Canonical schemas and allocation-free preparsers forbid pickle/`Connection.send/recv`. Windows uses overlapped named-pipe I/O plus `CancelIoEx`; POSIX uses nonblocking stream sockets plus selectors. Every frame is capped and chunked at or below queried kernel capacity, so 256-KiB task frames never rely on datagram atomicity. Tests fill transports, prove deadline/cancel/peer-close cleanup, and verify replacement startup. Compile-database/argument bounds precede pending-task construction and are revalidated by the encoder.

Endpoint spawn transfer is explicit and one-shot. The parent creates all endpoints before `Process.start`; Windows transfers only child ends through `multiprocessing.reduction.DupHandle`, POSIX through `DupFd`, with random cookie/direction/limit. The child detaches once and validates; the parent closes transferred copies after spawn. All other descriptors are non-inheritable. Payload uses the custom cancellable chunked endpoint. OS tests cover maximum frames, blocked peers, cancel/deadline/close/recycle and leak-free cleanup.

The startup endpoint is distinct from task/control traffic. The child starts first with fixed `DupHandle`/`DupFd` endpoint wrappers only; the plan does not claim Python spawn itself is pickle-free, only that no variable/untrusted protocol object or native capability map is pickled. After stable child identity exists, Windows duplicates each registry handle into that known child process and sends an authenticated bounded numeric handle/digest table. POSIX transfers exact fds with `SCM_RIGHTS` over the private startup Unix socket (or an equivalently reviewed pass-fd primitive), and the child validates exact fd count/order/digests. The child bounded-decodes the authority/bootstrap, reconstructs capabilities, and sends an authenticated ACK naming every accepted digest. Parent transfer owners remain live until that ACK validates, then close deterministically; failure closes both sides. Only afterward may `WorkerEngineReady` occur. Missing/duplicate/replayed/oversized/tampered bootstrap, wrong credentials/count/cookie/generation, changed local root or unknown capability fails before ready with every endpoint/native owner reaped.

`CompilerCapabilityRegistry` is the single parent owner. Collection registers/deduplicates capabilities, configuration records hold logical references only, and each generation receives explicit duplicated native owners transferred once through startup. Child generation shutdown closes task-local refs, executable/directory/watch owners, then startup/task endpoints; parent generation reap closes its duplicates before archived telemetry. Warm process-free completion closes collection/config refs without duplication. Success closes registry only after all sessions/policy; any bootstrap, collection, spawn, worker or policy failure unwinds child duplicates, parent generation duplicates, collection refs, then the registry. Tests assert exact ref/duplicate/transfer counts, double-operation rejection, zero native handles/watches, and that executable/toolchain ancestor rename succeeds immediately after final close on every backend.

`GenerationReactor` is the only reader of protocol Connections. It waits on readable lifecycle/control/registration/launch/payload receivers with a deadline-bounded 50-ms slice, revalidates health and exact generation immediately before each bounded receive, and never uses an unbounded or pickle receive. Backpressure deadline, EOF, worker exit, generation change or health failure cancels and enters failure-only cleanup. After any retire acknowledgement or observed worker exit, if `remaining or pending_tasks` and live generation count is zero, the reactor immediately spawns a replacement or fails; it never waits with work stranded. Normal shutdown sends bounded-wire `WorkerStop`, requires bounded-wire `WorkerStopped`, closes endpoints, joins/reaps, and proves the native survivor set empty.

Publication requests enter one bounded FIFO queue capped by live workers. Exactly one generation-bound permit may be active. The reactor keeps servicing deadlines, health, cancellation and other frames while requests wait; after atomic publication cleanup it releases the active permit and grants the next valid FIFO request. Retirement/crash/cancellation removes that generation's queued request and rejects a stale active completion. Tests cover simultaneous requests, FIFO order, no busy wait/deadlock, stale-generation removal and complete permit/queue cleanup.

Dispatch is an ordinal sliding window. If `next_unaggregated` is the lowest result not yet accepted into the streaming aggregator and `W` is the selected worker count, the only dispatchable ordinals are the inclusive interval `[next_unaggregated, next_unaggregated + W - 1]`. No later ordinal is sent until the gap closes and advances `next_unaggregated`; completion may be out of order only inside that window. The slow-first/250-fast real-reactor fixture proves the window remains bounded, resumes immediately when ordinal zero arrives, and finishes without deadlock or stranded work.

`GenerationState` owns exactly one fresh `OwnedProcessTree`/Job/cgroup leaf/registered-group carrier and its fresh Connections; no tree is reused across generations. Retirement/exit first stops and reconciles the generation, proves its native scope empty, snapshots immutable telemetry into that worker slot's archived maximum, closes the old tree/endpoints, then creates and registers a new tree/carrier before replacement dispatch. Slot memory uses the maximum archived/live generation upper bound. Failure at any create/register step fails immediately with zero dispatch into an uncontained generation.

Accountant phase calls occur at the real boundaries: inspection begins before the first inspection probe and seals after collection; task phase begins before first generation creation and seals only after final shutdown/reap. Scheduler memory samples always combine the run Job/accountant, every current `GenerationState.tree`, and immutable telemetry from every archived recycled generation; the initial `trees` tuple is never an accounting source after replacement. Tests delay Job notifications across each phase seal and recycle a first generation with higher RSS than its replacement, proving the final snapshot retains that archived maximum and equal complete identity counts.

Neither seal result is discarded. The pipeline stores exact discriminated `inspection_phase_snapshot` and `task_phase_snapshot`, passes both into final `StreamingAuditMeasurements`, serializes them in every native calibration sample/machine record, and recomputes the final native memory measurement from their phase maxima plus the outer accountant snapshot. Backend/type/phase mismatch fails. Delayed-notification and recycled-high-RSS tests on Windows, Linux and macOS exact-compare the sealed snapshots, final measurements, serialized sample and reloaded evidence.

Every `CompactResultMemoryBudget` in cold, warm, mixed, calibration, and acceptance construction receives the same run-scoped `CompactAccountingObserver`. Budget transitions emit semantic deltas (`retain-hit`, `reserve-dispatch`, `activate-publication`, `release-publication`, `send-pipe`, `decode`, `retain-result`, `release-result`) and reject negative, duplicate, or overlapping ownership. The whole-orchestrator monitor enforces two independent simultaneous lifetime gates and never adds them: the observer proves logical compact-result ownership/phase overlap stays at or below 128 MiB, while Windows Job accounting independently proves the physical process-tree RSS upper bound stays below 512 MiB. Job RSS already includes the verifier/parent process, workers, compilers, and physical buffer/object pages, so current verifier RSS and observer logical bytes are never added again. An injected semantic-overlap test charges one logical allocation to two observer owners and requires rejection; a separate monitor test proves the observer and Job gates are both checked without summation or cross-source attribution.

Decision and correctness entry points both construct the same runtime contract; the scheduler has no decision-specific branch. A worker retires when either runtime limit is reached, never with an outstanding payload. Typed message/acknowledgement and cancellation remain unchanged; teardown uses Windows Job emptiness, Linux cgroup emptiness, or macOS registered-set/known-reconciliation emptiness according to the native contract.

Platform enforcement samples the parent plus each platform's authoritative registered/accounted scope. Worker audit state is physical memory, never compact-budget charge. Windows performs conservative fail-closed observed-RSS upper-bound cancellation below 512 MiB and accepts cold calibration only below 448 MiB; this is not a kernel hard aggregate RSS limit. Per-Job commit charge remains a separate backstop. Linux alone applies controller-enforced 512-MiB `memory.max`/448-MiB `memory.high`; macOS records scoped observed residency and gates registered/reconciled survivors without a hard bound.

Every frame/payload/native wait, join and successful shutdown receives the one pipeline deadline. Failure alone gets the fixed 30-second emergency abort/reap deadline; cleanup cannot turn failure into success.

- [ ] **Step 4: Run cross-process and cache-lock suites**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.ProcessCoordinatorTests tests.gpu.test_gpu_capability_cache -v`

Expected: all process, cancellation, lock, corruption, platform-memory, success-deadline, and emergency-cleanup tests pass with Windows Job/Linux cgroup membership empty and macOS registered membership/known reconciliation empty.

- [ ] **Step 5: Commit**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_runner.py tests/gpu/gpu_capability_model.py tests/gpu/fixtures/fake_preprocessor.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "test(gpu): bound capability audit process workers" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 8: Stream deterministic coverage and findings without full views

**Files:**
- Modify: `tests/gpu/gpu_capability_model.py`
- Modify: `tests/gpu/gpu_capability_runner.py`
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Produces `compute_active_sources()`, outer-owned `HeldProductionSnapshot`, strict bounded UTF-8 decoding, and public `run_compiler_audit_pipeline() -> CompilerAuditRun`. The internal session exposes only `consume_aggregate()`, never a result tuple; the public run contains summary and measurements only.

Advance the attestation stage/schema in this same commit to cover active-source selection, production validation, `consume_aggregate()` integration, policy/final-summary extensions to the already-owned Task-3 aggregator, coverage/finding aggregation, outer-pipeline policy and every transitive helper/default/table. Task 8 does not redefine core budget/growth/aggregator semantics.

- [ ] **Step 1: Write failing equivalence and ordering tests**

```python
def test_streaming_pipeline_equals_reference_findings_and_coverage(self):
    expected = self.reference_pipeline(self.views)
    actual = audit_pipeline_streaming(
        self.sources, self.compact_results, self.configured_sources)
    self.assertEqual(actual, expected)

def test_public_compiler_audit_run_contains_only_summary_and_measurements(self):
    run = self.run_streaming_pipeline()
    self.assertEqual(tuple(field.name for field in dataclasses.fields(run)),
                     ("summary", "measurements"))
    self.assertIsInstance(run.summary, CanonicalAuditContentSummary)
    for forbidden in ("results", "findings", "coverage"):
        self.assertFalse(hasattr(run, forbidden))

def test_completion_order_does_not_change_output(self):
    first = self.run_with_completion_order(("cfg-c", "cfg-a", "cfg-b"))
    second = self.run_with_completion_order(("cfg-a", "cfg-b", "cfg-c"))
    self.assertEqual(first, second)

def test_a_to_b_compile_then_restore_a_cannot_publish_or_pass_final_boundary(self):
    self.production_path.write_bytes(b"A")
    with self.hold_initial_production_snapshot() as held, \
         self.keep_bytes_stable_for_both_compiler_runs(b"B"), \
         mock.patch.object(self.cache, "publish") as publish:
        with self.assertRaisesRegex(AuditInfrastructureError,
                                   "production snapshot generation"):
            self.run_miss_worker(
                production_snapshot=held.initial_digest_map,
                restore_bytes=b"A",
                restore_before="finalize_policy_boundary")
        self.assertEqual(self.compiler_invocation_bytes, (b"B", b"B"))
        self.assertEqual(self.production_path.read_bytes(), b"A")
        publish.assert_not_called()
        self.assertFalse(self.cache_entry_exists(self.configuration))

def test_policy_decode_is_strict_bounded_and_text_equivalent(self):
    with self.validated_snapshot(self.utf8_source_bytes) as sources, \
         self.track_memory_reservations() as reservations:
        text = decode_validated_production_sources(
            sources, self.limits, self.pipeline_deadline)
    self.assertEqual(
        self.audit_policy_text(text),
        self.reference_policy_findings_from_utf8_text)
    self.assertLessEqual(reservations.maximum_committed,
                         self.limits.production_decoded_transient_bytes)
    self.assertLessEqual(reservations.retained,
                         self.limits.production_decoded_retained_bytes)
    with self.validated_snapshot({self.path: b"\xff"}) as invalid, \
         self.assertRaisesRegex(AuditInfrastructureError, "UTF-8"):
        decode_validated_production_sources(
            invalid, self.limits, self.pipeline_deadline)

def test_production_raw_and_decoded_caps_are_exact_and_pre_reserved(self):
    expected = {
        "production_raw_per_file_bytes": 8 << 20,
        "production_raw_aggregate_bytes": 64 << 20,
        "production_decoded_transient_bytes": 256 << 20,
        "production_decoded_retained_bytes": 128 << 20,
    }
    for field, limit in expected.items():
        self.assertEqual(getattr(self.limits, field), limit)
        self.assertTrue(self.exercise_cap(field, limit).accepted)
        with self.assertRaisesRegex(AuditInfrastructureError, "production.*limit"):
            self.exercise_cap(field, limit + 1)
    self.assertEqual(
        self.recorded_order,
        ("reserve-raw", "read", "reserve-decode", "decode",
         "reserve-retained-strings", "construct-strings",
         "reserve-mapping", "construct-mapping"))

def test_outer_pipeline_owns_snapshot_finalize_and_policy_order(self):
    with self.record_pipeline_events() as events:
        self.run_streaming_outer_pipeline()
    self.assertEqual(events, (
        "snapshot-enter", "schedule-compact", "finalize-descriptors",
        "decode-utf8", "compute-coverage", "raw-policy",
        "source-only-policy", "shutdown-reap", "snapshot-exit",
        "finalize-measurements"))
    self.assertFalse(self.scheduler_called_finalize_or_policy)

def test_outer_enumerates_one_immutable_production_table_before_collection(self):
    with self.record_outer_production_table_consumers() as observed:
        self.run_streaming_outer_pipeline()
    self.assertEqual(observed.events[:3],
                     ("enumerate-production", "collect", "snapshot"))
    self.assertEqual(observed.enumeration_call_count, 1)
    self.assertIs(observed.collection_table, observed.snapshot_table)
    with self.assertRaises(TypeError):
        observed.collection_table[self.new_path] = self.new_identity

def test_aggregate_findings_preserves_exact_sorted_configuration_provenance(self):
    duplicated = self.same_finding_from_configurations("b" * 64, "a" * 64)
    self.assertEqual(aggregate_findings(duplicated), (
        AggregatedFinding(
            path=self.finding.path, line=self.finding.line,
            expression=self.finding.expression, reason=self.finding.reason,
            configuration_digests=("a" * 64, "b" * 64)),))
    self.assertNotEqual(
        aggregate_findings(self.same_location_with_different_reason()),
        aggregate_findings(duplicated))

def test_one_absolute_deadline_covers_collection_through_shutdown(self):
    run = self.run_with_recorded_stage_deadlines(start=1000.0)
    self.assertEqual(set(run.stage_deadlines.values()), {1180.0})
    self.assertEqual(
        tuple(run.stage_deadlines),
        ("enumerate-production", "collect", "snapshot", "cache", "schedule", "finalize",
         "decode", "coverage", "raw", "source-only", "shutdown-reap"))
    self.assertEqual(run.cache_lock_deadline,
                     min(1180.0, run.cache_lock_started_at + 240.0))
    self.assertLess(run.measurements.elapsed_seconds, 180.0)

def test_final_key_stale_allows_only_contained_inspection_events(self):
    with self.record_compiler_launch_events() as events:
        self.run_with_stale_final_inspected_key()
    self.assertTrue(events)
    self.assertTrue(all(event.purpose is CompilerLaunchPurpose.INSPECTION
                        for event in events))
    self.assertEqual(self.audit_worker_start_count, 0)
    self.assertEqual(self.audit_compiler_launch_count, 0)

def test_structural_stale_decision_launches_no_process(self):
    with self.record_compiler_launch_events() as events, \
         self.assertRaises(AuditInfrastructureError):
        self.run_with_structurally_invalid_decision()
    self.assertEqual(events, ())

def test_outer_failure_uses_only_emergency_abort_and_preserves_primary_error(self):
    with self.fail_policy_at(
             1100.0, AuditInfrastructureError("primary policy failure")), \
         self.fail_emergency_abort(RuntimeError("cleanup failure")), \
         self.record_session_teardown() as teardown, \
         self.assertRaisesRegex(AuditInfrastructureError,
                                "primary policy failure") as raised:
        self.run_streaming_outer_pipeline(start=1000.0)
    self.assertEqual(teardown.shutdown_reap_calls, ())
    self.assertEqual(teardown.abort_and_reap_deadlines, (1130.0,))
    self.assertFalse(teardown.acceptance_passed)
    self.assertIn("emergency abort/reap", " ".join(raised.exception.__notes__))

def test_consume_aggregate_has_no_retained_result_objects_before_shutdown(self):
    with self.result_weakrefs_and_accounting_observer() as observed:
        session = self.completed_internal_session()
        aggregate = session.consume_aggregate()
        self.assertEqual(session.pending_result_reference_count, 0)
        with self.assertRaisesRegex(AuditInfrastructureError, "already consumed"):
            session.consume_aggregate()
        self.summarize_aggregate(aggregate)
        gc.collect()
        self.assertTrue(all(ref() is None for ref in observed.result_weakrefs))
        self.assertLess(observed.release_event_index,
                        observed.shutdown_event_index)

def test_disjoint_251_summary_failure_releases_all_aggregate_ownership(self):
    with self.disjoint_maximal_251_results(), \
         self.assertRaisesRegex(AuditInfrastructureError, "summary.*128 MiB"):
        self.run_full_streaming_summary()
    self.assertEqual(self.result_budget.current_bytes, 0)
    self.assertEqual(self.result_budget.live_owners, ())
    self.assertFalse(self.uncharged_tuple_copy_was_started)

def test_summary_uses_outer_overhead_and_aggregate_release_is_atomic(self):
    aggregate = self.completed_maximal_aggregate()
    with self.fail_atomic_aggregate_release() as release:
        with self.assertRaisesRegex(AuditInfrastructureError, "release"):
            self.summarize_aggregate(aggregate)
        self.assertTrue(release.original_token_still_live_and_fully_charged)
        self.assertFalse(release.partial_release_observed)
    summary = self.summarize_aggregate(aggregate)
    self.assertLessEqual(self.outer_evidence_budget.current_bytes, 524288)
    self.assertFalse(self.compact_budget.owns_summary(summary))
```

`aggregate_findings()` groups only an exact `(path, line, expression, reason)` key, emits one `AggregatedFinding` with unique lexicographically sorted `configuration_digests`, and sorts the final tuple deterministically by the four finding fields followed by the digest tuple. Different reasons or expressions at the same location remain distinct. Add crossed main provenance, empty active main, source-only fallback, conditional translation units, cancellation with partial results, and complete configuration digest reporting.

Freeze active-source behavior with a table-driven test covering all eight exact members of `_CONDITIONALLY_SELECTED_TRANSLATION_UNITS`, ordinary required sources, Apple Objective-C++ present/absent, Windows backend present/absent, configured-but-inactive sources, an active source missing its compile command, and an empty or crossed main dependency. Assert that missing active commands fail before cache lookup or worker spawn and that each compact result reaches its own configured main identity.
Separately assert `coverage.source_only == frozenset(production) - coverage.authoritative` for every table row, including inactive conditional/platform sources; never use `active - authoritative` for policy coverage.
Add worker-time source mutation, same-content identity replacement, exact mutate/restore, and unstable-read tests. Prove every production path opens once, is read twice through the same held descriptor, `HeldProductionSnapshot.initial_digest_map` is captured before cache lookup and passed unchanged through real spawn workers, raw/source-only are not called before `finalize_policy_boundary()`, different final identity/content fails the whole run, and exact restoration before the final read produces byte-identical findings only when all compiler dependency digests also matched the initial map. Include A→B mutation held stable through both compiler invocations followed by restoration to A before the final descriptor boundary; it must still raise an infrastructure failure and publish no compact result.

- [ ] **Step 2: Run RED**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner.OrchestrationTests tests.gpu.test_gpu_capability_audit_lanes -v`

Expected: current pipeline requires full views.

- [ ] **Step 3: Implement compact aggregation**

```python
# `pipeline_started_at`, `operation_deadline` and `run_accountant` are explicit
# caller inputs; the start was captured before production enumeration.
pipeline_deadline = operation_deadline
session_needing_abort = None
completed_session = None
aggregate_ownership = None
summary = None
try:
    _require(cache.is_prepared, "result cache is not prepared")
    _require(cache.root_identity == inspection_cache.root_identity,
             "inspection cache root identity mismatch")
    _require(run_accountant.started_before_production_enumeration,
             "run accountant started too late")
    engine = _attest_loaded_audit_engine(expected_audit_engine_fingerprint)
    platform_kind = canonical_platform_kind(sys.platform)
    uninspected_digest = bounded_uninspected_configuration_digest(
        compile_commands, launcher_environment, dependency_roots,
        pipeline_deadline)
    prevalidated_decision = prevalidate_platform_worker_decision(
        decision_path, platform_kind, engine, uninspected_digest,
        pipeline_deadline)
    run_accountant.begin_phase("inspection", pipeline_deadline)
    collection = collect_configurations_with_decision_records(
        source_root, compile_commands, launcher_environment, production,
        dependency_roots, capability_registry, inspection_cache, engine,
        limits, pipeline_deadline,
        launch_accountant=run_accountant)
    _require(collection.dependency_root_authority is dependency_roots,
             "collection replaced dependency-root authority")
    _require(collection.capability_registry is capability_registry,
             "collection replaced compiler-capability registry")
    inspection_phase_snapshot = run_accountant.seal_phase(
        "inspection", pipeline_deadline)
    _require(_attest_loaded_audit_engine(engine) == engine,
             "loaded audit engine changed before cache lookup")
    ordered = _validated_orchestration_inputs(
        collection.configurations, production, cache, limits, pipeline_deadline)
    active, configured_sources = compute_active_sources(ordered, production)
    missing_commands = tuple(sorted(active - configured_sources,
                                    key=PurePosixPath.as_posix))
    if missing_commands:
        raise AuditInfrastructureError(
            "active source has no compile command: "
            + ", ".join(path.as_posix() for path in missing_commands))
    decision = finalize_platform_worker_decision(
        prevalidated_decision,
        expected_key=_current_key(collection.decision_records,
                                  dependency_roots, pipeline_deadline),
        deadline=pipeline_deadline)
    runtime_contract = runtime_contract_from_platform_decision(
        decision, pipeline_deadline)
    with snapshot_production_sources(
            production, limits, pipeline_deadline) as held_sources:
        production_snapshot = held_sources.initial_digest_map
        run_accountant.begin_phase("tasks", pipeline_deadline)
        session_needing_abort = schedule_configuration_audits(
            source_root, ordered, dependency_roots, capability_registry,
            production_snapshot,
            cache, limits, engine,
            runtime_contract, collection.inspection_probe_invocations,
            run_accountant)
        session = session_needing_abort
        result_budget = session.compact_result_budget
        aggregate = session.consume_aggregate()
        aggregate_ownership = aggregate.budget_ownership
        _require(result_budget.owns(aggregate_ownership),
                 "aggregate returned foreign compact-result ownership")
        sources = held_sources.finalize_policy_boundary(pipeline_deadline)
        source_text = decode_validated_production_sources(
            sources, limits, pipeline_deadline)
        coverage = _coverage_from_streaming_aggregate(
            production, aggregate, pipeline_deadline)
        summary_builder = BoundedCanonicalAuditContentSummaryBuilder(
            limits, result_budget)
        final_growth = summary_builder.premeasure_policy_merge_working_state(
            source_text, aggregate, coverage, pipeline_deadline)
        result_budget.reserve_aggregate_growth(
            aggregate_ownership, final_growth)
        observations = _run_raw_compact_and_source_only_policy_into_reserved_state(
            source_text, aggregate, coverage, summary_builder,
            pipeline_deadline)
        summary_builder.merge_findings_into_reserved_backing_state(
            aggregate.aggregated_findings, observations)
        result_budget.commit_aggregate_growth(aggregate_ownership)
        summary_reservation = evidence_suballocator.reserve_summary(
            len(aggregate.ordered_configuration_digests), pipeline_deadline)
        result_budget.prevalidate_aggregate_release(aggregate_ownership)
        summary = summary_builder.build_bounded_digest_and_count_summary(
            aggregate, coverage, summary_reservation)
        evidence_suballocator.commit_summary(summary_reservation, summary)
        result_budget.release_aggregate_after_summary(aggregate_ownership)
        aggregate_ownership = None
        del observations, coverage, aggregate
        _require(session.pending_result_reference_count == 0,
                 "scheduled session retained consumed results")
        session.shutdown_reap(pipeline_deadline)
        task_phase_snapshot = run_accountant.seal_phase(
            "tasks", pipeline_deadline)
        completed_session = session
        session_needing_abort = None
    measurements = _finalize_streaming_measurements(
        pipeline_started_at, pipeline_deadline, completed_session,
        cache.root_identity.resolved_root,
        inspection_phase_snapshot, task_phase_snapshot)
    if measurements.elapsed_seconds >= 180.0:
        raise AuditInfrastructureError("pipeline deadline expired")
    return CompilerAuditRun(summary, measurements)
except BaseException as primary_error:
    if aggregate_ownership is not None:
        try:
            result_budget.release_aggregate_after_summary(aggregate_ownership)
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"aggregate ownership release also failed: {cleanup_error!r}")
    if summary is not None:
        try:
            evidence_suballocator.release_summary(summary)
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"summary ownership release also failed: {cleanup_error!r}")
    if session_needing_abort is not None:
        try:
            session_needing_abort.abort_and_reap(emergency_cleanup_deadline())
        except BaseException as cleanup_error:
            primary_error.add_note(
                f"emergency abort/reap also failed: {cleanup_error!r}")
    raise
```

The outer pipeline receives explicit authorities/capabilities and an already-started accountant. Cache hits and fresh misses enter the same ordered streaming aggregator. In-order per-result objects are released immediately after merge; out-of-order objects remain charged only inside Task 7's deterministic worker-count reorder window and release as soon as the missing prefix closes. `_ScheduledAuditSession.consume_aggregate()` requires that window to be empty, moves only bounded digest/coverage/finding state plus its live `CompactBudgetOwnership`, and exposes the exact shared `compact_result_budget`; the caller rejects a foreign token and proves zero pending result references before shutdown. Before any raw/source policy output or merged-finding working index is allocated, the builder premeasures checked growth and reserves it against that same token. The returned `CanonicalAuditContentSummary` contains only the bounded 251 configuration digests plus scalar digest/counts; it is explicitly outside the compact budget and constructed under the pre-reserved 524,288-byte summary/evidence suballocation of the exact 8-MiB outer overhead. After that small summary is committed, `release_aggregate_after_summary()` atomically invalidates/releases the compact aggregate token; prevalidation or injected transition failure leaves the entire original token charged, never partially released. No live returned summary is claimed by a released compact token. The full 251-disjoint adversary runs through final policy/summary, fails before an uncharged allocation if necessary, and ends with zero compact owners. Cold/warm canonical summaries must be identical.

Decision loading is deliberately two-stage. Before opening the inspection phase, bounded prevalidation checks carrier/schema/platform/audit engine and the process-free raw configuration-set digest with zero launches. Only a structurally viable carrier may enter accountant-contained typed compiler inspection. The final inspected compiler/environment/configuration key must then validate against the exact same payload hash before cache lookup, worker spawn or audit-purpose compiler launch. A stale final key may therefore produce only typed `INSPECTION` events; every audit event/start count remains zero.

`uninspected_configuration_digest` is a required canonical field of `WorkerDecisionKey`, every platform decision JSON, the prevalidated carrier and their schema/hash inputs. It length-frames bounded raw compile-database semantics available without compiler inspection. A missing/changed value is structural-stale and launches nothing; it is not a diagnostic-only side value.

`snapshot_production_sources()` reserves descriptor and bounded immutable-content retention before opening/reading, opens every production path once, and applies the same Windows share-denial/POSIX identity policy as dependency handles. `finalize_policy_boundary()` re-seeks and rehashes the same descriptors and verifies final identity/content. Only afterward, `decode_validated_production_sources()` uses the same `ConservativeAllocationSchema` as compact JSON: it premeasures code-point count and reserves raw bytes, decoder scratch, the four-bytes-per-code-point string bound, mapping/dict/list/tuple/PyLong objects, and retained values before construction. Runtime probes only prove actual objects fit beneath schema constants. Invalid UTF-8, probe failure, or arithmetic overflow fails before policy. Handles remain live through decode/raw/source-only and close only when the outer context exits. Source-only remains `set(production) - authoritative`; main reachability is checked for cached and fresh results before final validation. Tests compare every representative `sys.getsizeof()` object and simultaneous phase overlap against the conservative reservation on each lane.

The four new `AuditLimits` fields are exact: per-file raw 8 MiB, aggregate retained raw 64 MiB, decoded transient allocation 256 MiB, and retained decoded mapping/strings 128 MiB. File size is checked and aggregate/raw reservation acquired before each read; decoded transient reservation is acquired before UTF-8 validation or string construction; retained string plus mapping/object reservation is acquired before publishing the immutable mapping. At decode peak, 64 MiB raw + the existing 128-MiB compact-result batch + 256 MiB decoded transient = 448 MiB, preserving the existing 64-MiB reserve under 512 MiB. Transient reservation is released before the 128-MiB retained decoded mapping becomes live. Tests hit each cap exactly, exceed each by one byte, force multi-file aggregate overflow, verify reservation precedes read/decode/object/mapping construction, and prove all reservations release on invalid UTF-8 and exceptions.

Delete the retain-all active path after equivalence is green. Keep compatibility helpers only in unit fixtures that explicitly build reference views.

- [ ] **Step 4: Run GREEN and commit**

Run: `python -m unittest tests.gpu.test_gpu_capability_runner tests.gpu.test_gpu_capability_audit_lanes -v`

```powershell
git add tests/gpu/gpu_capability_model.py tests/gpu/gpu_capability_runner.py tests/gpu/gpu_capability_source_audit.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "refactor(gpu): stream capability findings and coverage" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 9: Replace the measured Python byte tokenizer with an exact native scanner

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py` (fingerprint owner/stage)
- Modify: `tests/gpu/gpu_capability_provenance.py`
- Modify: `tests/gpu/test_gpu_capability_provenance.py`
- Modify: `tests/gpu/test_gpu_capability_live_compilers.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`

**Interfaces:** Keeps `PreprocessedStreamBuilder.feed()` and every produced token/provenance value unchanged. Uses a compiled bytes regex only for maximal token candidate scanning; marker parsing, comments, literal termination, chunk buffering, interning, line advancement, and provenance remain explicit fail-closed Python state.

Advance the attestation stage/schema in this same commit to include the exact scanner implementation, compiled-pattern source bytes, punctuator tables and every scanner helper. A scanner helper/table rebind must invalidate both audit and decision fingerprints before work.

- [ ] **Step 1: Freeze the old scanner as a test-only differential oracle**

```python
def assert_scanners_equal(self, stream: bytes):
    expected = self.capture_reference_success_or_failure(stream)
    for splits in self.feasible_differential_matrix(stream):
        actual = self.capture_native_success_or_failure(stream, splits)
        if expected.succeeded:
            self.assertTrue(actual.succeeded)
            self.assertEqual(self.compact_columns(actual.value),
                             self.compact_columns(expected.value))
        else:
            self.assertFalse(actual.succeeded)
            self.assertIs(type(actual.error), type(expected.error))
            self.assertEqual(str(actual.error), str(expected.error))
```

The corpus labels every case as expected success or expected matched failure. It includes comments, block-comment chunk splits, escaped literals, pp-numbers (explicitly `1e+2` and `0x1p-3`), pp-number splits between `e`/`p` and the following sign, prefixed ordinary literals (`u8`, `u`, `U`, `L`) whose frozen boundary remains identifier token plus literal token, and multiline raw-string spellings whose frozen behavior remains the same `AuditInfrastructureError("compiler output contains an unterminated literal")`, never a newly merged raw literal. It also covers every alternative punctuator, UTF-8 identifier bytes, GCC/MSVC markers, GCC pragmas, recursive includes, working-directory markers, one-million dense tokens, and all Task 10 macro fixtures. For each small corpus entry, the matrix runs unsplit, every single split offset, byte-at-a-time, and 32 deterministic three-way splits derived from a fixed SHA-256 seed. For each large/live stream, it runs chunk widths 1, 2, 3, 7, and 4096 plus splits immediately before/inside/after every marker, comment delimiter, escape, literal prefix/terminator, raw spelling delimiter, pp-number exponent sign, UTF-8 sequence, and multi-byte punctuator. It does not enumerate all partitions.

- [ ] **Step 2: Run RED performance assertion**

Run: `python -m unittest tests.gpu.test_gpu_capability_provenance.ProvenanceScannerParityTests -v`

Expected: semantic parity passes with the reference scanner while the explicit real-output scaling assertion fails the required linear throughput floor.

- [ ] **Step 3: Implement the compiled scanner**

```python
_PUNCTUATOR_PATTERN = b"|".join(re.escape(value) for value in _PUNCTUATORS)
_TOKEN_CANDIDATE = re.compile(
    rb'(?P<whitespace>[ \t\f\v\r]+)'
    rb'|(?P<line_comment>//[^\r\n]*)'
    rb'|(?P<block_comment>/\*(?:[^*]|\*(?!/))*\*/)'
    rb'|(?P<identifier>[_A-Za-z\x80-\xff][_A-Za-z0-9\x80-\xff]*)'
    rb'|(?P<number>(?:[0-9]|\.[0-9])'
    rb'(?:[eEpP][+-]|[_A-Za-z0-9.\'\x80-\xff])*)'
    rb'|(?P<string>"(?:\\.|[^"\\])*")'
    rb'|(?P<character>\'(?:\\.|[^\'\\])*\')'
    rb'|(?P<punctuator>' + _PUNCTUATOR_PATTERN + rb')'
    rb'|(?P<unknown>[^\r\n])')
```

The `[eEpP][+-]` alternative precedes the general pp-number continuation class, implementing the frozen C/C++ rule that `+` or `-` belongs to a pp-number only when the immediately preceding character is `e`, `E`, `p`, or `P`; line buffering retains a trailing exponent marker across a feed boundary before deciding. Identifier precedes literal alternatives exactly as the reference byte scanner does, preserving `u8`/`u`/`U`/`L` as a separate identifier before the following quoted literal. Do not add raw-string recognition: the scanner processes `R` as an identifier and preserves the reference unterminated-literal failure on multiline raw spellings. The existing `_PUNCTUATORS` tuple is longest-first; tests assert that property and exact escaping. `_scan_line()` skips whitespace/comments and emits only identifier/number/literal/punctuator/unknown candidates. Feed retains incomplete physical input chunks but applies the same logical-line termination/failure rules as the reference and never scans a marker/directive line. The final one-byte fallback handles an otherwise unknown non-newline byte without unbounded backtracking.

- [ ] **Step 4: Run differential, live, and scaling gates**

Run: `python -m unittest tests.gpu.test_gpu_capability_provenance tests.gpu.test_gpu_capability_live_compilers -v`

Expected: every successful compact column is byte-identical to the oracle and every rejected stream has the identical exception class/message for the complete feasible matrix; prefixed literals retain identifier-plus-literal boundaries, multiline raw spellings retain the frozen unterminated-literal rejection, and real MinGW accepted parsing is at least twice as fast as the 2026-07-17 O(1)-counter baseline without changing findings.

- [ ] **Step 4.5: Prove Linux/macOS calibration harness readiness without emitting decisions**

On pinned Linux/macOS runners, exercise the real full-pipeline calibration harness with a one-configuration smoke and fake multi-count codec fixtures only. Prove native measurement/containment and candidate ordering work, but emit and stage **no** real decision/evidence: Task 10 still changes the final fingerprint/decision key, so any Task-9 artifact would be stale by construction.

- [ ] **Step 5: Commit**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_provenance.py tests/gpu/test_gpu_capability_provenance.py tests/gpu/test_gpu_capability_live_compilers.py tests/gpu/test_gpu_capability_audit_lanes.py
git commit -m "perf(gpu): accelerate exact provenance token scanning" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10: Wire real cold/warm acceptance, CI, documentation, and closure review

**Files:**
- Modify: `tests/gpu/gpu_capability_source_audit.py`
- Modify: `tests/gpu/gpu_capability_calibration.py`
- Modify: `tests/gpu/test_gpu_capability_runner.py`
- Modify: `tests/gpu/test_gpu_capability_cache.py`
- Modify: `tests/gpu/test_gpu_capability_audit_lanes.py`
- Modify: `tests/gpu/test_gpu_capability_calibration.py`
- Modify: `tests/gpu/test_gpu_capability_live_compilers.py`
- Modify: `tests/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/build-and-run.md`
- Modify: `docs/superpowers/plans/2026-07-16-gpu-capability-streaming-audit.md`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-calibration.md`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-decision.json`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-calibration.md`
- Create: `docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-decision.json`

**Interfaces:** Final CLI summary reports `CanonicalAuditContentSummary` (canonical digest, ordered configuration digests, finding count, authoritative path count, and source-only path count) beside common operational `StreamingAuditMeasurements` (`cache_hits`, `cache_misses`, `inspection_probe_invocations`, `audit_compiler_invocations`, `elapsed_seconds`, `stdout_bytes`, compact cache bytes/entries, and a discriminated `memory_backend`). Its exact nested `memory` object is `WindowsRunMemoryMeasurements`, `LinuxRunMemoryMeasurements`, or `MacOSRunMemoryMeasurements`; platform-inapplicable flat RSS fields and public result/finding/coverage collections are forbidden.

`memory_backend` is a computed property derived solely from the exact runtime type of the closed `memory` union (`WindowsRunMemoryMeasurements→windows`, Linux→linux, macOS→macos). It is not a separately serialized mutable field. Unknown subclasses/types and disagreement with either phase-snapshot discriminant fail encoding/validation.

`encode_platform_worker_decision()`/`decode_platform_worker_decision()` use exact ordered discriminated codecs. `platform_kind` selects Windows samples with Windows memory/cold `<448`, Linux samples with cgroup fields/OOM gates, or macOS samples with observed-residency/reconciliation fields; unknown/cross-platform fields and wrong sample types fail. The generic-named original pair is explicitly the Windows artifact; Linux/macOS use the exact paths above. No real artifact is generated until all Task-10 code, fingerprint roots/schema/exclusions, workflow and codec tests are final.

Linux and macOS samples carry the same complete measured contract as Windows: pilot eligibility, applied recycling parameters, inspection/audit invocation counts, stdout/cache byte and entry totals, exact included stages, post-return samples, stage p50/p95, native memory/reconciliation fields and zero-survivor result. Their decisions embed the exact `LinuxCalibrationEnvelope` or `MacOSCalibrationEnvelope`; that envelope's canonical digest is the key's `reference_envelope_digest`, and loaders independently reconstruct the current runner/compiler/Python/capacity envelope before accepting it. The Linux codec requires cgroup current/peak/max/high plus both service-root and run-node OOM/max-event deltas; the macOS codec requires all registered-group observed-residency and reconciliation counts. The generic loader and runtime conversion above are the only native decision consumers.

Every evidence Markdown file begins byte zero with one bounded canonical machine-record block (fixed sentinel, length, canonical JSON bytes and terminating newline) containing platform, full decision key, complete `attempted_worker_counts`, ordered completed samples, ordered typed rejections, selected count/parameters, native measurements for completed samples only, exact `calibration_sha`, GitHub workflow run id and attempt. Human prose follows only after that block. The decision JSON stores `evidence_record_sha256 = sha256(exact machine-record bytes)`. The hosted verifier requires workflow input SHA == checked-out `GITHUB_SHA` == record SHA before upload. The artifact verifier bounded-parses the first record, checks its hash link, exact-decodes and compares every attempt/sample/rejection/selection/native-measurement/run field to the decision and workflow metadata, and rejects stale/unrelated/reordered content. The later artifact-only commit does not enter any semantic/decision key.

Lock the final attestation stage here: tests assert the exact ordered target-module set, stage/schema bytes, import-time module identities and exact per-module runtime-state exclusions. They enumerate every module-defined function/class plus immutable semantic table and prove that rebinding representative obscure helpers, globals and tables from Tasks 1-9 changes the fingerprint. Unknown mutable state, an unused exclusion, a missing target module, or a parent/child stage mismatch fails closed before cache lookup or worker readiness.

- [ ] **Step 1: Add the explicit post-tokenizer calibration verifier**

```python
def verify_real_calibration_contract(
        decision_path: Path, evidence_path: Path,
        platform: Literal["windows", "linux", "macos"],
        compile_commands: tuple[Path, ...]) -> None:
    verifier_started_at = time.monotonic()
    verifier_deadline = verifier_started_at + 60.0
    decision, evidence = read_calibration_outputs(
        decision_path, evidence_path, verifier_deadline)
    machine_record = parse_bounded_leading_evidence_machine_record(
        evidence_path, verifier_deadline)
    _require_contract(hashlib.sha256(machine_record.exact_bytes).hexdigest()
                      == decision.evidence_record_sha256,
                      "evidence machine-record hash differs")
    _require_contract(machine_record.platform_kind == decision.platform_kind
                      and machine_record.key == decision.key
                      and machine_record.attempted_worker_counts
                          == decision.attempted_worker_counts
                      and machine_record.rejections == decision.rejections
                      and machine_record.samples == decision.samples
                      and machine_record.selected_workers
                          == decision.selected_workers
                      and machine_record.native_measurements
                          == tuple(s.memory for s in decision.samples),
                      "evidence machine record differs from decision")
    expected_counts = allowed_worker_counts(
        decision.key.effective_worker_capacity)
    expected_prefix = (1,) if decision.key.effective_worker_capacity == 1 \
        else (2, 1)
    _require_contract(decision.attempted_worker_counts
                      [:len(expected_prefix)] == expected_prefix,
                      "calibration order prefix changed")
    _require_contract(
        decision.attempted_worker_counts == expected_counts,
        "calibration attempted-count set is incomplete")
    completed_counts = tuple(s.worker_count for s in decision.samples)
    rejected_counts = tuple(r.worker_count for r in decision.rejections)
    _require_contract(
        len(set(completed_counts + rejected_counts))
            == len(completed_counts) + len(rejected_counts)
        and set(completed_counts) | set(rejected_counts) == set(expected_counts),
        "completed and rejected calibration attempts do not partition counts")
    for rejection in decision.rejections:
        _require_contract(
            rejection.platform_kind == platform
            and rejection.failure_stage in CALIBRATION_FAILURE_STAGES
            and rejection.failure_code in CALIBRATION_FAILURE_CODES
            and rejection.cleanup_complete
            and rejection.surviving_owned_process_count == 0,
            "calibration rejection record is incomplete")
    selected = next(s for s in decision.samples
                    if s.worker_count == decision.selected_workers)
    current_configuration_count = current_native_configuration_set_cardinality(
        compile_commands, verifier_deadline)
    expected_sample_count = 251 if platform == "windows" \
        else current_configuration_count
    for sample in decision.samples:
        _require_contract(sample.platform_kind == platform
                          and sample.configuration_count == expected_sample_count
                          and sample.audit_compiler_invocations
                              == 2 * expected_sample_count,
                          "serialized calibration sample cardinality/launches differ")
    _require_contract(selected.inspection_probe_invocations >= 0,
                      "negative inspection count")
    _require_contract(selected.audit_compiler_invocations
                      == 2 * selected.configuration_count,
                      "calibration audit launch count changed")
    _require_contract(selected.elapsed_seconds < 180.0,
                      "calibration deadline failed")
    _require_contract(decision.platform_kind == platform
                      and selected.platform_kind == platform,
                      "calibration platform discriminant differs")
    if platform == "windows":
        _require_contract(selected.configuration_count == 251
                          and current_configuration_count == 251
                          and isinstance(selected, WorkerCalibrationSample)
                          and selected.memory.accounting_complete
                          and selected.memory.job_total_process_count
                          == selected.memory.retained_process_identity_count
                          and selected.memory.aggregate_peak_rss_upper_bound_bytes
                          < 448 * 1024 * 1024,
                          "Windows calibration accounting/RSS failed")
    elif platform == "linux":
        _require_contract(selected.configuration_count
                          == current_configuration_count
                          and isinstance(selected, LinuxWorkerCalibrationSample)
                          and selected.memory.accounting_complete
                          and selected.memory.cgroup_peak_accounted_memory_bytes
                          < selected.memory.cgroup_memory_high_bytes
                          and all(delta == 0 for delta in (
                              selected.memory.cgroup_oom_count_delta,
                              selected.memory.cgroup_oom_kill_count_delta,
                              selected.memory.cgroup_max_event_count_delta,
                              selected.memory.service_root_oom_count_delta,
                              selected.memory.service_root_oom_kill_count_delta,
                              selected.memory.service_root_max_event_count_delta)),
                          "Linux calibration cgroup contract failed")
    else:
        _require_contract(selected.configuration_count
                          == current_configuration_count
                          and isinstance(selected, MacOSWorkerCalibrationSample)
                          and selected.memory.accounting_complete
                          and selected.memory.surviving_registered_process_count == 0
                          and selected.memory.known_unreconciled_descendant_count == 0
                          and selected.recycle_rss_bytes == UINT64_MAX,
                          "macOS calibration reconciliation failed")
    _require_contract(selected.surviving_owned_process_count == 0,
                      "calibration survivor")
    _require_contract(
        decision.maximum_tasks_per_worker == selected.maximum_tasks_per_worker
        and decision.recycle_rss_bytes == selected.recycle_rss_bytes,
        "selected runtime parameters differ")
    _require_contract(selected.included_stages == (
        "enumerate-production", "collect-configurations", "snapshot-production", "load-cache",
        "compile", "compact-audit-publish", "aggregate-authoritative",
        "validate-policy-boundary", "raw-audit", "source-only",
        "shutdown-reap", "summary"), "calibration stages incomplete")
    expected = platform_worker_decision_key_for_current_run(
        platform, compile_commands, verifier_deadline)
    _require_contract(load_platform_worker_decision(
        decision_path, platform, expected) == decision,
                      "reloaded calibration decision differs")
    _require_contract(time.monotonic() < verifier_deadline,
                      "calibration verifier deadline expired")
```

`verify_real_calibration_contract()` lives in `gpu_capability_calibration.py`, has no `test*` name, and is called only by the explicit calibration CLI and the Task 10 Step 7 developer gate. It creates its own absolute verifier deadline; bounded reads, current-key compiler inspection, and validation receive that deadline and fail if it expires. It preserves the ordered compile-database tuple. It is not imported or invoked by `unittest discover`, CTest, or normal CI. Normal `test_gpu_capability_calibration.py` cases use only the small fake pipeline to validate ordering, artifact serialization, and rejection behavior.

Calibration order is exactly `allowed_worker_counts(effective_worker_capacity(AuditLimits()))`: capacity one yields only `(1,)`; otherwise the prefix is `(2,1)` followed by three through the constrained capacity, with duplicates removed. `attempted_worker_counts` records that complete order. For each count, launch an unrecorded pilot in its own top-level process and empty cache under the same hard deadline/RSS enforcement. Every native pilot and measured runner captures its clock and creates its run accountant before `ProductionIdentityAuthority.enumerate()`; serialized elapsed/native measurements therefore include `enumerate-production`, which is the first exact included-stage marker. Derive `maximum_tasks_per_worker` and `recycle_rss_bytes` only from consecutive post-return samples that stayed below the hard budget. A pilot that aborts may contribute only its verified safe prefix. If no safe prefix exists, set `pilot_eligible=False`, apply the fail-closed diagnostic floor `(maximum_tasks_per_worker=1, recycle_rss_bytes=1)` to the separate measured attempt, and never select that count even if the diagnostic run happens to complete. Pilot elapsed/RSS/cache/configuration totals and run IDs are never copied into a completed sample or evidence.

Then launch a new top-level measured process with another new empty result-cache root and apply exactly the derived native parameters (or explicit ineligible diagnostic floor) from its first worker generation onward. With clock/accountant already active, enumerate production, collect the compile database, snapshot all production sources; validate cache; run the native configuration-set cardinality (exactly 251 on the Windows reference lane) with exactly `2 * configuration_count` audit compiler invocations and one accepted parse per miss; audit/publish/discard in workers; aggregate authoritative coverage; close the final source identity/content boundary; run raw audit and source-only over `set(production) - authoritative` from the validated snapshot; stop/close/join/reap the complete owned tree; capture `surviving_owned_process_count`; and only then construct and serialize the discriminated summary/sample. Only a normally completed second run populates the matching Windows/Linux/macOS sample type, whole-pipeline elapsed and native memory measurements, separate inspection/audit counters, stdout/cache totals, applied recycling parameters, the exact stage tuple beginning `enumerate-production`, and per-worker discovery/accepted/audit/publish p50/p95. A native violation, deadline, or other aborted/incomplete measured run instead emits one bounded `CalibrationRejection` only after emergency cleanup proves zero owned survivors; its failure stage/code come from closed enums and it carries no fabricated timing, memory, cardinality, or launch totals. If cleanup itself is incomplete, emit no artifact. Completed samples and rejection records must partition `attempted_worker_counts` exactly, and at least one completed sample must pass its native formula before a platform decision can be emitted.

- [ ] **Step 2: Freeze the exact manual key-change calibration command**

Record this command now, but do not execute it until Step 7 after all decision-keyed code and wiring are final:

```powershell
python tests/gpu/gpu_capability_calibration.py --source-root . --compile-commands build/gpu/compile_commands.json --cache-parent build/gpu/gpu-capability-calibration --reference-envelope windows-2022-py3.11.9-qt6.10.2-mingw13.1.0-cap4 --reference-affinity 0,1,2,3 --evidence-out docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md --decision-out docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json
```

The final workflow exposes required `calibration_sha` and enum `calibration_platform` (`windows`, `linux`, `macos`) `workflow_dispatch` inputs and adds two non-Windows jobs named exactly `calibrate-linux-gpu-capability` (`ubuntu-24.04`) and `calibrate-macos-gpu-capability` (`macos-15`); exactly the selected platform job runs. Both manual calibration and normal native CI use `actions/setup-python` exact `3.11.9`. Linux downloads the LLVM `18.1.8` release archive by fixed URL and checked SHA-256, uses only its resolved `clang`/`clang++`, and records the complete version string in `LinuxCalibrationEnvelope`; system `clang` is forbidden. macOS sets `DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer`, resolves `xcrun clang`, requires the exact recorded AppleClang version/build string, and records it in `MacOSCalibrationEnvelope`; a missing/different Xcode fails before key derivation. Workflow tests reject floating Python, system/floating compilers, changed archive digest, or a normal/manual environment mismatch. After their native build/containment setup they run, respectively:

```bash
python3 tests/gpu/gpu_capability_calibration.py --calibrate-platform linux --source-root . --compile-commands build/gpu-linux-clang/compile_commands.json --cache-parent build/gpu/gpu-capability-linux-calibration --evidence-out docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-calibration.md --decision-out docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-decision.json
python3 tests/gpu/gpu_capability_calibration.py --calibrate-platform macos --source-root . --compile-commands build/gpu-macos-appleclang/compile_commands.json --cache-parent build/gpu/gpu-capability-macos-calibration --evidence-out docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-calibration.md --decision-out docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-decision.json
```

The Linux command runs inside the delegated `systemd-run` supervisor/rendezvous described in Task 6; the macOS command runs under the registered-PGID launcher. Before upload, the producing native job invokes `verify_real_calibration_contract(..., platform=<native>)` and strict-reloads the artifact against the current native envelope/key; failure uploads nothing. Each native job uploads exactly one artifact named `gpu-capability-windows-calibration`, `gpu-capability-linux-calibration`, or `gpu-capability-macos-calibration`, containing only that platform's `.md`/`.json` pair. Retrieve with `gh run download <run-id> -n <exact-name>`; on Windows/local review only schema, discriminant, canonical bytes and hashes for foreign artifacts, then target-stage the two exact files. Do not pretend a foreign host can recompute the native current key. Normal Linux/macOS CI performs the authoritative strict current-envelope/key load before any spawn. Workflows never commit or push generated evidence. Native correctness CTests pass the matching checked artifact through `--worker-decision`; a missing artifact is a pre-spawn failure.

The eventual native run is the first real calibration for **all three** platforms and occurs only after final Task-10 decision fingerprint/key changes are pushed. Each platform tests its allowed counts with pilot-derived thresholds followed by fresh-cache full measured runs. Windows reference cardinality is exactly 251. Linux/macOS use their current native configuration-set cardinality from the exact final key/artifact and require `audit_compiler_invocations == 2 * native_configuration_count`; they never inherit the Windows 251 constant. Windows requires complete cold `<448` accounting; Linux requires cgroup limits/events/zero OOM/empty leaves; macOS requires registered reconciliation and honestly named observed residency without an RSS bound. Every selected sample requires exact real configuration/launch counts, all stages and `<180.000s`. Encode each discriminated decision/evidence atomically at its exact path and immediately strict-reload it. If any platform lacks a passing candidate, upload none for that platform and stop to redesign; never reuse a Task-9 or cross-platform artifact.

- [ ] **Step 3: Add the explicit real acceptance verifier and fake unit contracts**

```python
def _validate_windows_run(run: CompilerAuditRun, mode: str) -> None:
    measurements = run.measurements
    expected_hits, expected_misses, expected_audits = (
        (0, 251, 502) if mode == "cold" else (251, 0, 0))
    elapsed_limit = 180.0 if mode == "cold" else 20.0
    rss_limit = (448 if mode == "cold" else 512) << 20
    _require_contract(measurements.configuration_count == 251,
                      f"{mode} configuration count changed")
    _require_contract((measurements.cache_hits, measurements.cache_misses)
                      == (expected_hits, expected_misses),
                      f"{mode} cache counts changed")
    _require_contract(measurements.audit_compiler_invocations == expected_audits,
                      f"{mode} audit launch count changed")
    expected_inspections = 0 if mode == "warm" else None
    _require_contract(
        measurements.inspection_probe_invocations >= 0
        and (expected_inspections is None
             or measurements.inspection_probe_invocations == expected_inspections),
        f"{mode} inspection count changed")
    _require_contract(mode != "warm" or measurements.worker_counts_started == (),
                      "warm run started a worker")
    _require_contract(measurements.elapsed_seconds < elapsed_limit,
                      f"{mode} elapsed limit failed")
    _require_contract(measurements.compact_result_peak_live_bytes <= 128 << 20
                      and measurements.compact_result_retained_bytes <= 128 << 20,
                      f"{mode} compact-result memory failed")
    _require_contract(isinstance(measurements.memory,
                                 WindowsRunMemoryMeasurements),
                      f"{mode} memory backend is not Windows Job")
    _require_contract(measurements.memory.accounting_complete
                      and measurements.memory.surviving_job_process_count == 0,
                      f"{mode} Windows process accounting incomplete")
    _require_contract(measurements.memory.job_total_process_count
                      == measurements.memory.retained_process_identity_count,
                      f"{mode} Windows identity count differs")
    _require_contract(measurements.memory.aggregate_peak_rss_upper_bound_bytes
                      < rss_limit, f"{mode} RSS bound failed")
    _require_contract(measurements.maximum_encoded_result_bytes <= 4 << 20,
                      f"{mode} encoded result bound failed")
    _require_contract(measurements.maximum_conservative_decoded_bytes <= 16 << 20,
                      f"{mode} decoded result bound failed")
    _require_contract(measurements.cache_entries == 251
                      and measurements.cache_entries <= 1024
                      and measurements.cache_bytes <= 472 << 20,
                      f"{mode} cache capacity failed")

def run_and_verify_real_cold_warm_contract(
        source_root: Path,
        compile_commands: tuple[Path, ...],
        launcher_environment: Mapping[str, str],
        production_authority: ProductionIdentityAuthority,
        dependency_roots: DependencyRootAuthority,
        capability_registry: CompilerCapabilityRegistry,
        prepared_cache: ConfigurationAuditCache,
        inspection_cache: CompilerInspectionCache,
        limits: AuditLimits,
        decision_path: Path,
        evidence_builder_factory: Callable[
            [EvidenceSuballocator, OuterEvidenceMemoryBudget],
            BoundedAcceptanceEvidenceBuilder],
        verifier_deadline: float) -> AcceptanceEvidence:
    verifier_started_at = time.monotonic()
    outer_deadline = min(verifier_deadline, verifier_started_at + 270.0)
    _require(prepared_cache.root_identity == inspection_cache.root_identity,
             "inspection cache is not bound to prepared result-cache root")
    cache_child = prepared_cache.root_identity.resolved_root
    cold_started_at = time.monotonic()  # before enumeration; evidence start
    cold_deadline = min(outer_deadline, cold_started_at + 180.0)
    run_accountant = None
    monitor = None
    outer_evidence_budget = None
    evidence_subbudget = None
    evidence_builder = None
    evidence = None
    try:
        run_accountant = RunScopedNativeAccountant.create(outer_deadline)
        outer_evidence_budget = ExactOuterEvidenceMemoryBudget(
            OUTER_EVIDENCE_OVERHEAD_BYTES)
        evidence_subbudget = outer_evidence_budget.reserve_evidence_subbudget(
            524_288, outer_deadline)
        evidence_builder = evidence_builder_factory(
            evidence_subbudget, outer_evidence_budget)
        monitor = WindowsWholeOrchestratorMonitor.start(
            run_accountant, outer_deadline,
            first_slice_deadline=cold_deadline)
        production = MappingProxyType(dict(
            production_authority.enumerate(source_root, run_accountant,
                                           cold_deadline)))
        cold = run_compiler_audit_pipeline(
            source_root, compile_commands, launcher_environment, production,
            dependency_roots, capability_registry, prepared_cache,
            inspection_cache, limits,
            "cold", decision_path, cold_deadline, run_accountant,
            evidence_subbudget, cold_started_at)
        _require_contract(cold.measurements.pipeline_started_at
                          == cold_started_at,
                          "cold measurement start excluded enumeration")
        _validate_windows_run(cold, mode="cold")
        cold_summary = cold.summary
        cold_measurements = cold.measurements
        del cold
        cold_end = monitor.snapshot(cold_deadline)
        _require_contract(cold_end.compact_result_peak_live_bytes <= 128 << 20,
                          "cold compact-result lifetime exceeded 128 MiB")
        _require_contract(cold_end.accounting_complete,
                          "cold accountant snapshot incomplete")
        _require_contract(
            cold_end.memory.aggregate_peak_rss_upper_bound_bytes < 448 << 20,
            "complete cold slice RSS bound failed")
        evidence_builder.add_cold(
            cold_summary, cold_measurements, cold_end, cold_deadline)

        warm_started_at = time.monotonic()
        warm_deadline = min(outer_deadline, warm_started_at + 20.0)
        warm = run_compiler_audit_pipeline(
            source_root, compile_commands, launcher_environment, production,
            dependency_roots, capability_registry, prepared_cache,
            inspection_cache, limits,
            "warm", decision_path, warm_deadline, run_accountant,
            evidence_subbudget, warm_started_at)
        _validate_windows_run(warm, mode="warm")
        _require_contract(warm.summary == cold_summary,
                          "cold/warm content summaries differ")
        evidence_builder.add_warm(
            warm.summary, warm.measurements, warm_deadline)
        del warm, cold_summary

        verifier_deadline = min(outer_deadline, time.monotonic() + 40.0)
        monitor_result = monitor.finish(verifier_deadline)
        _require_contract(
            monitor_result.compact_result_peak_live_bytes <= 128 << 20,
                          "orchestrator compact-result lifetime exceeded 128 MiB")
        _require_contract(monitor_result.memory.accounting_complete,
                          "orchestrator Job accounting incomplete")
        _require_contract(
            monitor_result.memory.aggregate_peak_rss_upper_bound_bytes < 512 << 20,
            "orchestrator lifetime RSS bound failed")
        evidence = evidence_builder.finish(
            cache_child=cache_child, monitor=monitor_result,
            evidence_suballocator=evidence_subbudget,
            deadline=verifier_deadline)
        _remove_acceptance_child_after_verified_success(
            cache_child, deadline=outer_deadline)
        _require_contract(time.monotonic() < outer_deadline,
                          "acceptance outer deadline expired after cleanup")
    except BaseException as primary_error:
        cleanup_deadline = emergency_cleanup_deadline()
        if monitor is not None:
            try:
                monitor.abort(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"orchestrator monitor abort also failed: {cleanup_error!r}")
        elif run_accountant is not None:
            try:
                run_accountant.abort_and_close(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"partial accountant abort also failed: {cleanup_error!r}")
        if evidence is not None:
            try:
                evidence.close(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"successful evidence ownership close also failed: {cleanup_error!r}")
        elif evidence_builder is not None:
            try:
                evidence_builder.abort(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"evidence builder abort also failed: {cleanup_error!r}")
        if evidence is None and evidence_subbudget is not None:
            try:
                evidence_subbudget.abort(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"evidence suballocator abort also failed: {cleanup_error!r}")
        if evidence is None and outer_evidence_budget is not None:
            try:
                outer_evidence_budget.abort(cleanup_deadline)
            except BaseException as cleanup_error:
                primary_error.add_note(
                    f"outer evidence budget abort also failed: {cleanup_error!r}")
        _retain_and_report_failed_acceptance_child(cache_child)
        raise
    return evidence

def test_normal_ci_uses_checked_decision_once_and_never_calibrates(self):
    with mock.patch("gpu_capability_calibration.calibrate_platform_worker_count") as calibrate, \
         self.record_bounded_run_returns() as observed:
        evidence = self.run_normal_windows_ci(self.checked_decision)
    try:
        calibrate.assert_not_called()
        self.assertEqual(observed.cold.measurements.worker_counts_started,
                         (self.checked_decision.selected_workers,))
        self.assertEqual(
            (observed.cold.measurements.runtime_contract.maximum_tasks_per_worker,
             observed.cold.measurements.runtime_contract.recycle_rss_bytes),
            (self.checked_decision.maximum_tasks_per_worker,
             self.checked_decision.recycle_rss_bytes))
        self.assertFalse(observed.cold_and_warm_full_results_ever_coexisted)
        self.assertEqual(evidence.cold_summary.canonical_sha256,
                         evidence.warm_summary.canonical_sha256)
        self.assertIs(evidence.cold_end_snapshot,
                      observed.evidence_builder_recorded_cold_end)
        self.assertEqual(
            {field.name for field in dataclasses.fields(observed.cold.measurements)},
            {"pipeline_started_at", "elapsed_seconds", "configuration_count",
             "cache_hits", "cache_misses",
             "inspection_probe_invocations", "audit_compiler_invocations",
             "stdout_bytes", "memory", "cache_bytes", "cache_entries",
             "compact_result_peak_live_bytes", "compact_result_retained_bytes",
             "maximum_encoded_result_bytes",
             "maximum_conservative_decoded_bytes",
             "maximum_conservative_retained_bytes",
             "runtime_contract", "cache_root",
             "worker_counts_started", "included_stages",
             "inspection_phase_snapshot", "task_phase_snapshot"})
        self.assertIsInstance(observed.cold.measurements.memory,
                              WindowsRunMemoryMeasurements)
        self.assertEqual(observed.warm.measurements.worker_counts_started, ())
    finally:
        evidence.close(time.monotonic() + 10.0)
    self.assertTrue(observed.evidence_success_ownership_released)

def test_success_evidence_owns_budget_until_explicit_close(self):
    evidence = self.run_normal_windows_ci(self.checked_decision)
    self.assertTrue(self.evidence_budget.is_transferred)
    self.assertFalse(self.evidence_budget.is_released)
    self.serialize_acceptance_evidence(evidence)
    evidence.close(time.monotonic() + 10.0)
    self.assertTrue(self.evidence_budget.is_released)
    self.assertEqual(self.evidence_budget.live_bytes, 0)
    with self.assertRaises(AuditInfrastructureError):
        evidence.close(time.monotonic() + 10.0)

def test_acceptance_has_outer_and_independent_run_deadlines(self):
    with self.monotonic_script(2000.0, cold_seconds=100.0,
                               warm_seconds=10.0) as clock, \
         self.record_acceptance_deadlines() as deadlines:
        evidence = run_and_verify_real_cold_warm_contract(
            self.source_root, self.compile_commands,
            self.launcher_environment, self.production_authority,
            self.dependency_roots,
            self.capability_registry,
            self.prepared_cache, self.inspection_cache, self.limits,
            self.decision_path, self.evidence_builder_factory,
            verifier_deadline=2270.0)
    evidence.close(time.monotonic() + 10.0)
    self.assertEqual(deadlines.outer, 2270.0)
    self.assertEqual(deadlines.cold, 2180.0)
    self.assertEqual(deadlines.warm, 2120.0)
    self.assertLessEqual(deadlines.verifier, 2150.0)
    self.assertTrue(clock.summary_evidence_monitor_and_cleanup_finished)
    self.assertLess(clock.final_deadline_check_at, 2270.0)
    for run_name, mutation in (
            ("cold", "audit-count"), ("cold", "448mib"),
            ("warm", "audit-count"), ("warm", "512mib"),
            ("warm", "survivor"), ("warm", "inspection-probe"),
            ("warm", "worker-start"), ("pair", "cleanup-overrun")):
        with self.subTest(run=run_name, mutation=mutation), \
             self.assertRaisesRegex(AuditInfrastructureError, run_name):
            self.verify_mutated_real_pair(run_name, mutation)

def test_every_acceptance_phase_failure_aborts_builder_monitor_and_native_state(self):
    for phase in ("enumeration", "inspection", "cold-task", "cold-policy",
                  "cold-evidence", "warm-cache", "warm-evidence", "finish"):
        with self.subTest(phase=phase), self.fail_acceptance_phase(phase), \
             self.record_acceptance_cleanup() as cleanup, \
             self.assertRaisesRegex(AuditInfrastructureError, phase):
            self.run_real_acceptance()
        self.assertEqual(cleanup.evidence_abort_calls, 1)
        self.assertEqual(cleanup.monitor_abort_calls, 1)
        self.assertEqual(cleanup.surviving_owned_processes, 0)
        self.assertEqual(cleanup.open_endpoint_count, 0)
        self.assertTrue(cleanup.failed_cache_child_retained)

def test_cleanup_failures_are_notes_and_never_replace_primary(self):
    with self.fail_acceptance_phase("cold-policy"), \
         self.fail_evidence_abort("evidence cleanup"), \
         self.fail_monitor_abort("monitor cleanup"), \
         self.assertRaisesRegex(AuditInfrastructureError, "cold-policy") as raised:
        self.run_real_acceptance()
    self.assertIn("evidence builder abort", " ".join(raised.exception.__notes__))
    self.assertIn("orchestrator monitor abort", " ".join(raised.exception.__notes__))

def test_staged_acceptance_startup_closes_exact_partial_owner(self):
    for step in ("accountant-create", "outer-budget-create",
                 "evidence-reservation", "evidence-builder-construction",
                 "monitor-start", "first-enumeration"):
        with self.subTest(step=step), self.fail_startup_step(step), \
             self.record_acceptance_cleanup() as cleanup, \
             self.assertRaises(AuditInfrastructureError):
            self.run_real_acceptance()
        self.assertEqual(cleanup.live_job_count, 0)
        self.assertEqual(cleanup.open_endpoint_count, 0)
        self.assertEqual(cleanup.accountant_abort_calls,
                         1 if step in ("outer-budget-create",
                                       "evidence-reservation",
                                       "evidence-builder-construction",
                                       "monitor-start") else 0)
        self.assertEqual(cleanup.monitor_abort_calls,
                         1 if step == "first-enumeration" else 0)
        self.assertEqual(cleanup.evidence_abort_calls,
                         1 if step in ("monitor-start", "first-enumeration") else 0)
        self.assertEqual(cleanup.outer_budget_abort_calls,
                         1 if step in ("evidence-reservation",
                                       "evidence-builder-construction",
                                       "monitor-start", "first-enumeration") else 0)
        self.assertTrue(cleanup.failed_cache_child_retained)

def test_verifiers_remain_fail_closed_under_python_optimize(self):
    for optimize in ("1", "2"):
        completed = self.run_verifier_subprocess(
            optimize=optimize, mutation="warm-audit-count")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("AuditInfrastructureError", completed.stderr)

def test_production_audit_modules_contain_no_assert_nodes(self):
    production_modules = (
        "gpu_capability_model.py", "gpu_capability_command.py",
        "gpu_capability_runner.py", "gpu_capability_provenance.py",
        "gpu_capability_cache.py", "gpu_capability_source_audit.py",
        "gpu_capability_process_tree.py", "gpu_capability_calibration.py")
    violations = []
    for name in production_modules:
        tree = ast.parse((GPU_TEST_ROOT / name).read_text(encoding="utf-8"),
                         filename=name)
        violations.extend((name, node.lineno) for node in ast.walk(tree)
                          if isinstance(node, ast.Assert))
    self.assertEqual(violations, [])

def test_correctness_only_streams_every_production_configuration_with_native_decision(self):
    configurations = self.production_compile_database.semantic_configurations
    result = self.run_correctness_only(
        self.production_compile_database,
        worker_decision=self.native_decision_path)
    self.assertEqual(result.measurements.runtime_contract,
                     runtime_contract_from_platform_decision(
                         self.native_decision,
                         result.measurements.pipeline_started_at + 180.0))
    self.assertEqual(result.summary.configuration_digests,
                     tuple(sorted(c.digest for c in configurations)))
    self.assertEqual(result.measurements.audit_compiler_invocations,
                     2 * len(configurations))
    self.assertEqual(result.summary.canonical_sha256,
                     self.expected_complete_production_summary_sha256)
    for incompatible in ("--cold-warm-acceptance", "--performance-only"):
        with self.subTest(incompatible=incompatible), self.assertRaisesRegex(
                AuditInfrastructureError, "correctness-only"):
            self.run_cli("--correctness-only", incompatible)

def test_each_real_native_cli_strict_loads_matching_decision_before_spawn(self):
    for platform, path in self.native_decision_paths.items():
        with self.subTest(platform=platform), \
             self.run_on_native_platform(platform), \
             self.record_process_starts() as starts:
            self.run_cli("--correctness-only", f"--worker-decision={path}")
            self.assertTrue(starts.after_strict_decision_load)
        for corruption in ("missing", "cross-platform", "structural-stale",
                           "wrong-envelope", "malformed"):
            with self.subTest(platform=platform, corruption=corruption), \
                 self.run_on_native_platform(platform), \
                 self.record_process_starts() as starts, \
                 self.assertRaises(AuditInfrastructureError):
                self.run_native_cli_with_decision_corruption(corruption)
            self.assertEqual(starts.count, 0)
        with self.subTest(platform=platform, corruption="final-key-stale"), \
             self.run_on_native_platform(platform), \
             self.record_typed_process_starts() as starts, \
             self.assertRaises(AuditInfrastructureError):
            self.run_native_cli_with_decision_corruption("final-key-stale")
        self.assertTrue(starts)
        self.assertTrue(all(start.purpose is CompilerLaunchPurpose.INSPECTION
                            for start in starts))
        self.assertEqual(self.audit_worker_start_count, 0)
        self.assertEqual(self.audit_compiler_launch_count, 0)

def test_correctness_only_reports_native_platform_memory_without_false_rss_claims(self):
    linux = self.run_correctness_only_on_fake_linux()
    self.assertIsInstance(linux.measurements.memory, LinuxRunMemoryMeasurements)
    self.assertEqual(linux.measurements.memory.cgroup_memory_max_bytes, 512 << 20)
    self.assertEqual(linux.measurements.memory.cgroup_memory_high_bytes, 448 << 20)
    self.assertEqual((linux.measurements.memory.cgroup_oom_count_delta,
                      linux.measurements.memory.cgroup_oom_kill_count_delta),
                     (0, 0))
    mac = self.run_correctness_only_on_fake_macos()
    self.assertIsInstance(mac.measurements.memory, MacOSRunMemoryMeasurements)
    self.assertTrue(mac.measurements.memory.accounting_complete)
    self.assertEqual(mac.measurements.memory.surviving_registered_process_count, 0)
    self.assertEqual(mac.measurements.memory.known_unreconciled_descendant_count, 0)
    self.assertFalse(hasattr(mac.measurements.memory,
                             "aggregate_peak_rss_upper_bound_bytes"))

def test_acceptance_and_correctness_cache_roots_are_isolated_and_fresh(self):
    stable_parent = self.create_stable_scratch_parent_with_prior_successes()
    with self.record_acceptance_children() as children:
        self.run_cold_warm_acceptance(stable_parent)
        self.run_cold_warm_acceptance(stable_parent)
    self.assertEqual(len(set(children.created)), 2)
    self.assertTrue(all(child.is_relative_to(stable_parent)
                        for child in children.created))
    self.assertTrue(all(children.was_new_at_creation))
    self.assertTrue(all(children.cold_warm_shared_child))
    self.assertTrue(all(not child.exists() for child in children.created))
    failed_child = self.run_failing_acceptance_and_capture_child(stable_parent)
    self.assertTrue(failed_child.exists())
    self.assertIn(str(failed_child), self.failure_diagnostic)
    with self.capture_correctness_temp_cache() as cache_root:
        self.run_correctness_only(self.production_compile_database)
    self.assertFalse(cache_root.exists())
    self.assertNotEqual(self.manual_diagnostic_root(1),
                        self.manual_diagnostic_root(2))

def test_stale_decision_fails_with_exact_manual_regeneration_command(self):
    with self.assertRaisesRegex(
            AuditInfrastructureError,
            re.escape(self.documented_calibration_command)):
        self.run_compiler_authoritative(self.stale_decision)
```

`run_and_verify_real_cold_warm_contract()` receives source root, ordered databases, launcher environment, authorities/registry, prepared caches, limits, decision, an `evidence_builder_factory` and absolute verifier deadline. It constructs one shared 524,288-byte `EvidenceSuballocator`, then binds the factory-created builder to it. One `RunScopedNativeAccountant` owns the outer cold/warm lifetime and the monitor only observes it. The exact `cold_started_at` precedes enumeration and flows into measurements/evidence; the same `cold_end` snapshot is retained. On successful builder finish, the suballocator atomically transfers itself and its strong parent-budget owner into the closeable `EvidenceOwnership` carried by `AcceptanceEvidence`; callers serialize/consume inside `try/finally` and call `AcceptanceEvidence.close()`, which releases both summaries, builder/machine-record scratch, suballocator, and outer budget exactly once. No successful owner falls out of scope untracked. On pre-transfer failure, close native monitor/accountant owners, abort the builder first, then its shared suballocator/outer budget; on post-transfer failure, close the evidence token instead. Retain/report the failed cache child and attach cleanup failures to the primary exception. Tests cover transfer, serialization while live, exact release, double-close rejection, and every failure boundary; no builder allocation is double-owned.

The verifier receives an `evidence_builder_factory`, not a preconstructed builder. It creates the outer budget, reserves the exact 524,288-byte evidence subbudget before invoking the factory, and binds the returned builder to that reservation. Reservation failure never calls the factory; constructor failure releases the reservation/budget and native owners. Dedicated injection tests cover both boundaries, zero builder/result retention, zero Jobs/endpoints and failed-child retention.

`--correctness-only` strictly loads the final Task-10-produced Linux or macOS discriminated worker decision and applies it from generation one. Missing/stale/cross-platform artifacts fail before spawn.

Correctness-only owns a `TemporaryDirectory` containing fresh result and compiler-inspection partitions and removes it after handle/process cleanup. Authoritative CTest receives a stable scratch parent suitable for repeated invocations. The CLI atomically creates a cryptographically unique child beneath it and requires that child not to have existed; cold and warm share only that child. A successful fully verified pair removes its child. Failure retains the exact child and prints its path for diagnosis. Prior sibling children are ignored and cannot manufacture hits. Repeating the same CTest is therefore idempotent. Manual diagnostics use separate UUID children and never reuse acceptance/calibration/correctness children.

All calibration/acceptance validation uses explicit `_require_contract()` checks that raise `AuditInfrastructureError`; Python `assert` is forbidden in production/verifier paths. Task 10 installs the complete eight-module AST gate only after Task 6 has created `gpu_capability_process_tree.py` and `gpu_capability_calibration.py`; every earlier task's smaller gate references only files that exist at that task boundary. Subprocess tests with `PYTHONOPTIMIZE=1` and `2` prove mutations still fail. The bounded evidence builder receives exact cold/warm summaries, counters, platform memory measurements, cache bytes/entries, and per-entry maximum encoded/conservative-decoded/conservative-retained fields; it never retains a result object.
Add a CMake registration matrix test/configure assertion: Windows + MinGW + GPU + GNU exposes the reference and perf tests. Only `gpu_capability_source_audit` receives the checked decision, `--cold-warm-acceptance`, generated compile database, and a stable scratch parent; the CLI creates the per-invocation child. Linux/macOS GPU-on production builds expose only production-database correctness. GPU-off registers no production-database acceptance/correctness/performance CTest introduced by this plan; existing GPU capability unit and live-compiler tests remain under their current independent conditions and are not disabled or reclassified. Cross-platform live-family lanes remain required.

- [ ] **Step 4: Run focused RED before final artifact generation**

Run: `python -m unittest tests.gpu.test_gpu_capability_calibration tests.gpu.test_gpu_capability_runner tests.gpu.test_gpu_capability_cache -v`

Expected before final wiring: fail named decision fingerprint, held-boundary, one-generation source, CI-no-recalibration, or conditional-registration assertions. Do not generate a real decision from incomplete keyed code merely to make RED advance.

- [ ] **Step 5: Wire CLI, CTest, and CI**

Conditionally register `gpu_capability_source_audit` and `gpu_capability_source_audit_perf` only under `WIN32 AND MINGW AND OLR_GPU_PIPELINE` (and require GNU) with CTest timeout **360 seconds**: 270-second outer verifier + 30-second emergency cleanup + 30 seconds launcher/reporting overhead plus a strict positive harness margin. A configure test asserts `TIMEOUT > 270 + 30 + 30`, not equality. Linux/macOS native calibrated correctness CTests use the same 360-second process timeout while retaining their internal 180-second cold deadline. GPU-off adds none.

The reference job uses `runs-on: windows-2022` (never `windows-latest`), `actions/setup-python` with exact `3.11.9`, Qt `6.10.2` `win64_mingw`, exact `tools_mingw1310`/MinGW `13.1.0`, and capacity/affinity `(4, 0,1,2,3)`. Both calibration and normal CI use the same frozen configure environment and command: install-qt-action's resolved `QT_ROOT_DIR` (validated as Qt `6.10.2` `mingw_64`), its exact sibling `Tools/mingw1310_64` and `Tools/Ninja/ninja.exe`, setup-python's resolved executable, controlled `${GITHUB_WORKSPACE}/windows_build/dist/ffmpeg` and `/srt`, `Release`, Ninja, tests/GPU probes/GPU pipeline/export enabled, and `OLR_WERROR=OFF`. Every absolute root is resolved and existence-checked before configure; `CMAKE_PREFIX_PATH`, compiler, make program, Python executables, `OLR_FFMPEG_ROOT`, and `OLR_SRT_ROOT` are passed explicitly. The command never lexically selects a newest MinGW/Ninja directory. A dedicated `workflow_dispatch` calibration input selects a calibration-only job with that identical setup, runs the pinned-envelope command, and uploads the JSON and Markdown as review artifacts; it does not directly commit them. Normal CI repeats the same setup, fails if the host exposes fewer than four eligible processors, constrains affinity to `0,1,2,3` before key derivation, and validates/uses the reviewed checked-in artifact. Workflow tests reject `windows-latest`, floating `3.x`, mismatched Qt/MinGW/dependency roots or configure flags, unconstrained affinity, a calibration job on push/PR, or a normal job that regenerates the decision.

The following setup sequence appears byte-for-byte in both the normal reference job and the manual calibration job before the PowerShell configure/audit block; the block is not described as self-contained without these steps:

```yaml
- uses: actions/setup-python@v5
  with:
    python-version: '3.11.9'
- name: Install Qt MinGW and tools
  uses: jurplel/install-qt-action@v4
  with:
    version: '6.10.2'
    host: windows
    target: desktop
    arch: win64_mingw
    modules: qtmultimedia qtwebsockets qtshadertools
    tools: tools_mingw1310 tools_ninja
    cache: true
- name: Cache controlled Windows FFmpeg and SRT
  uses: actions/cache@v4
  with:
    path: |
      windows_build/dist/ffmpeg
      windows_build/dist/srt
    key: ${{ runner.os }}-winffmpeg-srt-${{ hashFiles('build-scripts/build_ffmpeg_windows_srt.sh', 'build-scripts/single_ffmpeg_policy.json') }}
    restore-keys: |
      ${{ runner.os }}-winffmpeg-srt-
- name: Build controlled Windows FFmpeg and SRT (idempotent; cached)
  shell: bash
  run: |
    set -euo pipefail
    export OLR_QT_ROOT="${QT_ROOT_DIR//\\//}"
    ./build-scripts/build_ffmpeg_windows_srt.sh
```

The workflow also constrains the top-level normal/calibration process and inherited children to affinity `0,1,2,3` before key derivation. A workflow-structure test compares the setup and configure step payloads across both jobs and fails any drift in Python/Qt/modules/tools, dependency cache/build, Ninja, `Release`, affinity, GPU/test/export flags, or controlled dependency roots.

Linux correctness configures/builds only `build/gpu-linux-clang` with resolved Clang/Clang++, GPU/tests/export enabled; macOS uses only `build/gpu-macos-appleclang` with `xcrun` Clang/Clang++. Each job asserts its own `${build_dir}/compile_commands.json` is nonempty before CTest, and the registered CTest passes exactly `${CMAKE_BINARY_DIR}/compile_commands.json`. Matrix tests fail a generic `build-tests` path, mismatched CTest directory, missing export, GCC-configured Linux production audit, or a fixture database.

```powershell
$workspaceInput = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$workspace = (Resolve-Path -LiteralPath $workspaceInput).Path.Replace('\', '/')
$qtInput = if ($env:QT_ROOT_DIR) { $env:QT_ROOT_DIR } else { 'C:/Qt/6.10.2/mingw_64' }
$qtRoot = (Resolve-Path -LiteralPath $qtInput).Path.Replace('\', '/')
$qtToolsRoot = (Resolve-Path -LiteralPath "$qtRoot/../..").Path.Replace('\', '/') + '/Tools'
$mingwRoot = "$qtToolsRoot/mingw1310_64"
$ninja = "$qtToolsRoot/Ninja/ninja.exe"
$python = (Get-Command python.exe -ErrorAction Stop).Source
$currentProcess = [System.Diagnostics.Process]::GetCurrentProcess()
$availableAffinity = $currentProcess.ProcessorAffinity.ToInt64()
if (($availableAffinity -band 0xF) -ne 0xF) {
    throw 'reference runner exposes fewer than affinity CPUs 0,1,2,3'
}
$currentProcess.ProcessorAffinity = [IntPtr]0xF
$ffmpegRoot = (Resolve-Path -LiteralPath "$workspace/windows_build/dist/ffmpeg").Path.Replace('\', '/')
$srtRoot = (Resolve-Path -LiteralPath "$workspace/windows_build/dist/srt").Path.Replace('\', '/')
$gcc = "$mingwRoot/bin/gcc.exe"
$mingw = "$mingwRoot/bin/g++.exe"
foreach ($required in ($qtRoot, "$qtRoot/bin/qtpaths6.exe",
                        $gcc, $mingw, $ninja, $python,
                        "$ffmpegRoot/include", "$ffmpegRoot/lib",
                        "$srtRoot/include", "$srtRoot/lib")) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "reference dependency is missing: $required"
    }
}
$qtVersion = (& "$qtRoot/bin/qtpaths6.exe" --qt-version).Trim()
if ($qtVersion -ne '6.10.2') { throw "expected Qt 6.10.2, got $qtVersion" }
$mingwVersion = (& $mingw -dumpfullversion -dumpversion).Trim()
if ($mingwVersion -ne '13.1.0') { throw "expected MinGW 13.1.0, got $mingwVersion" }
$env:OLR_QT_ROOT = $qtRoot
$env:CMAKE_PREFIX_PATH = $qtRoot
$env:PATH = "$mingwRoot/bin;$qtRoot/bin;$(Split-Path -Parent $ninja);$ffmpegRoot/bin;$ffmpegRoot/lib;$srtRoot/bin;$srtRoot/lib;$env:PATH"
cmake -S . -B build/gpu -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DOLR_BUILD_TESTS=ON -DOLR_BUILD_GPU_PROBES=ON -DOLR_GPU_PIPELINE=ON `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DOLR_WERROR=OFF `
  -DCMAKE_PREFIX_PATH=$qtRoot `
  -DCMAKE_C_COMPILER=$gcc -DCMAKE_CXX_COMPILER=$mingw `
  -DCMAKE_MAKE_PROGRAM=$ninja `
  -DPython3_EXECUTABLE=$python -DOLR_PYTHON3_EXECUTABLE=$python `
  -DOLR_FFMPEG_ROOT=$ffmpegRoot -DOLR_SRT_ROOT=$srtRoot
cmake --build build/gpu
if (-not (Test-Path -LiteralPath build/gpu/compile_commands.json)) {
    throw 'build/gpu/compile_commands.json is missing'
}
python tests/gpu/gpu_capability_source_audit.py --source-root . --live-only --live-compiler "gcc=$mingw" --require-live-family gcc

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe is missing' }
$vsInstall = (& $vswhere -latest -products * `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath).Trim()
if (-not $vsInstall) { throw 'Visual Studio C++ toolchain is missing' }
$vsDevCmd = "$vsInstall/Common7/Tools/VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $vsDevCmd)) { throw 'VsDevCmd.bat is missing' }
$vsEnvironment = & cmd.exe /d /s /c `
  "`"$vsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
foreach ($line in $vsEnvironment) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        Set-Item -Path "Env:$($line.Substring(0, $separator))" `
          -Value $line.Substring($separator + 1)
    }
}
$msvc = (Get-Command cl.exe -ErrorAction Stop).Source
if (-not $msvc.Replace('\', '/').StartsWith($vsInstall.Replace('\', '/') + '/')) {
    throw "cl.exe did not come from activated Visual Studio: $msvc"
}
if ((& $msvc 2>&1 | Out-String) -notmatch 'Compiler Version 19\.') {
    throw 'unexpected cl.exe toolchain'
}
python tests/gpu/gpu_capability_source_audit.py --source-root . --live-only --live-compiler "msvc=$msvc" --require-live-family msvc

$clangCl = (Resolve-Path -LiteralPath `
  (Get-Command clang-cl.exe -ErrorAction Stop).Source).Path
$clangClVersion = (& $clangCl --version | Select-Object -First 1).Trim()
if ($clangClVersion -notmatch '^clang version [0-9]+\.[0-9]+\.[0-9]+') {
    throw 'unexpected clang-cl.exe toolchain'
}
Write-Host "Using exact clang-cl: $clangCl ($clangClVersion)"
python tests/gpu/gpu_capability_source_audit.py --source-root . --live-only --live-compiler "clang-cl=$clangCl" --require-live-family clang-cl

ctest --test-dir build/gpu -R '^gpu_capability_source_audit$' --output-on-failure
```

```bash
set -euo pipefail
cc="$LLVM_18_1_8_ROOT/bin/clang"
cxx="$LLVM_18_1_8_ROOT/bin/clang++"
test "$(python3 -c 'import platform; print(platform.python_version())')" = 3.11.9
"$cc" --version | grep -F 'clang version 18.1.8'
cmake -S . -B build/gpu-linux-clang -G Ninja \
  -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
cmake --build build/gpu-linux-clang
test -s build/gpu-linux-clang/compile_commands.json
python3 tests/gpu/gpu_capability_source_audit.py --source-root . --live-only \
  --live-compiler "clang=$cxx" --require-live-family clang
systemd-run --help | grep -E -- '--wait|--collect|--property=|--working-directory='
systemd-run --version
unit="olr-gpu-capability-${GITHUB_RUN_ID:-local}-$$"
sudo systemd-run --unit="$unit" --wait --collect \
  --property=Type=exec --property=Delegate=yes \
  --property=MemoryAccounting=yes \
  --property=MemoryMax=536870912 --property=MemoryHigh=469762048 \
  --uid="$(id -u)" --gid="$(id -g)" --working-directory="$PWD" \
  python3 tests/gpu/gpu_capability_process_tree.py prepare-linux-cgroup \
    --memory-max 536870912 --memory-high 469762048 -- \
    ctest --test-dir build/gpu-linux-clang \
      -R '^gpu_capability_source_audit_correctness$' --output-on-failure
```

```bash
set -euo pipefail
export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
cc="$(xcrun --find clang)"
cxx="$(xcrun --find clang++)"
test "$(python3 -c 'import platform; print(platform.python_version())')" = 3.11.9
"$cc" --version | grep -F 'Apple clang version 17.0.0'
cmake -S . -B build/gpu-macos-appleclang -G Ninja \
  -DOLR_BUILD_TESTS=ON -DOLR_GPU_PIPELINE=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx"
cmake --build build/gpu-macos-appleclang
test -s build/gpu-macos-appleclang/compile_commands.json
python3 tests/gpu/gpu_capability_source_audit.py --source-root . --live-only \
  --live-compiler "clang=$cxx" --require-live-family clang \
  --require-live-objcxx
ctest --test-dir build/gpu-macos-appleclang \
  -R '^gpu_capability_source_audit_correctness$' --output-on-failure
```

`--worker-decision` is mandatory for every compiler-authoritative execution, including `--correctness-only`, which must receive the matching discriminated native artifact. Before collection, the CLI enumerates production identities once under the outer deadline, freezes the table, and passes that same object to `collect_configurations_with_decision_records()` and the later held snapshot. Collection runs while raw compile-database and launcher-effective environment inputs are live, threads its immutable sidecar with the unchanged configurations into `configuration_set_digest()`, inspects the compiler, derives the exact current `WorkerDecisionKey`, strictly loads the platform artifact, and fails before cache lookup or worker spawn on a missing/stale/cross-platform/mismatched/malformed decision; there is no default worker count or hard-coded two-worker path. The failure prints the exact Task 10 Step 2 native regeneration command and never silently recalibrates. Exhaustive multi-count calibration is run manually after any decision-key input changes and each artifact is reviewed/checked in; normal CI invokes the dedicated native authoritative CTest once using the selected sample's exact recorded `maximum_tasks_per_worker` and `recycle_rss_bytes`, while generic labels exclude it. `--require-live-objcxx` is valid only with a required `clang` family and runs the bounded live parity fixture through the selected compiler with `-x objective-c++`; unit tests inspect the launched command and prove omission, wrong family, failure, or skipped execution fails the lane. Each OS job runs the complete-tree fixture for its native accounting backend. Linux provisions the delegated cgroup-v2 subtree, then runs both the production correctness CTest and additional Clang parity. macOS runs the inherited-process-group/libproc production correctness CTest plus additional AppleClang/Objective-C++ parity. Only Windows MinGW registers cold/warm reference and perf CTests. The Windows warm phase reuses the matching decision/cache and requires zero spawned worker/compiler processes.

- [ ] **Step 6: Document exact developer commands**

```powershell
python tests/gpu/gpu_capability_calibration.py --source-root . --compile-commands build/gpu/compile_commands.json --cache-parent build/gpu/gpu-capability-calibration --reference-envelope windows-2022-py3.11.9-qt6.10.2-mingw13.1.0-cap4 --reference-affinity 0,1,2,3 --evidence-out docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md --decision-out docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json
$acceptanceScratch = "build/gpu/gpu-capability-acceptance-scratch"
New-Item -ItemType Directory -Force -Path $acceptanceScratch | Out-Null
python tests/gpu/gpu_capability_source_audit.py --source-root . --compile-commands build/gpu/compile_commands.json --cold-warm-acceptance --cache-parent $acceptanceScratch --worker-decision docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json
$diagnostic = "build/gpu/gpu-capability-diagnostic/$([guid]::NewGuid())"
python tests/gpu/gpu_capability_source_audit.py --source-root . --compile-commands build/gpu/compile_commands.json --cache-dir $diagnostic --worker-decision docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json
python -m unittest discover -s tests/gpu -p 'test_gpu_capability_*.py' -v
python tests/gpu/gpu_capability_source_audit.py --source-root . --performance-only
```

Document that calibration must run after tokenizer changes and that both Windows reference audits require the resulting exactly keyed decision artifact. The local calibration command is valid only on the pinned Windows/Python/Qt/MinGW envelope, requires at least four eligible CPUs, and constrains affinity to `0,1,2,3`; an unconstrained local run refuses to emit evidence. The first audit must report 251 misses, `audit_compiler_invocations == 502`, its separate inspection-probe count, `<180s`, and Windows `aggregate_peak_rss_upper_bound_bytes <448 MiB`; the second must report 251 hits, zero audit compiler invocations, `<20s`, no worker start, no eviction, and the Windows upper bound `<512 MiB`. Evidence labels the RSS upper bound and Job commit-charge backstop separately. Linux documentation names cgroup-accounted current/peak/max/high/OOM metrics; macOS documentation names only observed residency and explicitly disclaims a hard or upper-bound RSS gate. Document the selected calibrated worker count and measured recycle thresholds from the evidence/artifact, never a fixed count copied into source.

- [ ] **Step 7: Run artifact-independent unit/config/native-harness readiness**

Run (PowerShell): `$env:GPU_CAPABILITY_TEST_PHASE='artifact-independent'; python -m unittest discover -s tests/gpu -p 'test_gpu_capability_*.py' -v; Remove-Item Env:GPU_CAPABILITY_TEST_PHASE`

This step is artifact-independent local verification only: unit/config/codec/process-tree/live-compiler and one-configuration native harness readiness. The Python suite uses a shared decorator that skips every artifact-consuming case unless `GPU_CAPABILITY_TEST_PHASE=artifact-dependent`; the readiness value above therefore cannot consume a checked-in decision. `tests/CMakeLists.txt` gives every readiness CTest the label `gpu-capability-artifact-independent`, while correctness, cold/warm acceptance and selected-decision performance tests have only artifact-dependent labels. Artifact-dependent tests are explicitly excluded until Step 10 artifacts exist. Do not dispatch any hosted calibration workflow yet: the exact keyed code/workflow must first be committed and pushed.

Run: `python tests/gpu/gpu_capability_source_audit.py --source-root . --performance-only`

Run: `cmake --build build/gpu`

Run:

```powershell
python tools/run_ctest.py --test-dir build/gpu -L gpu-capability-artifact-independent --output-on-failure
```

Run:

```powershell
python tools/run_ctest.py --test-dir build/gpu -L unit --output-on-failure
```

Run: `python tools/roadmap/audit.py`

Run: `git diff --check`

Expected: all artifact-independent Python, live compiler, negative compile, GPU/unit, native containment smoke and roadmap/diff gates pass; no selected-decision correctness/acceptance command has run yet.

- [ ] **Step 8: Commit and push final keyed code/workflow before calibration**

```powershell
git add tests/gpu/gpu_capability_source_audit.py tests/gpu/gpu_capability_calibration.py tests/gpu/test_gpu_capability_runner.py tests/gpu/test_gpu_capability_cache.py tests/gpu/test_gpu_capability_audit_lanes.py tests/gpu/test_gpu_capability_calibration.py tests/gpu/test_gpu_capability_live_compilers.py tests/CMakeLists.txt .github/workflows/ci.yml docs/build-and-run.md docs/superpowers/plans/2026-07-16-gpu-capability-streaming-audit.md
git commit -m "test(gpu): wire native capability calibration" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin $(git branch --show-current)
```

The interim push is required solely so hosted native calibration can run the exact immutable commit. All hooks run; never use `--no-verify`. This is not authorization to merge, and the PR remains not ready until artifacts, reviews and current-head CI land.

- [ ] **Step 9: Dispatch native calibration at the exact pushed SHA**

The workflow exposes required `calibration_sha` and enum `calibration_platform` (`windows`, `linux`, `macos`) inputs; exactly the selected calibration job runs. Execute this sequence from the pushed branch:

```powershell
$branch = (git branch --show-current).Trim()
$calibrationSha = (git rev-parse HEAD).Trim()
git fetch origin $branch
git merge-base --is-ancestor $calibrationSha "origin/$branch"
if ($LASTEXITCODE -ne 0) { throw "calibration SHA is not present on origin/$branch" }
gh workflow run ci.yml --ref $branch -f calibration_sha=$calibrationSha -f calibration_platform=windows
gh workflow run ci.yml --ref $branch -f calibration_sha=$calibrationSha -f calibration_platform=linux
gh workflow run ci.yml --ref $branch -f calibration_sha=$calibrationSha -f calibration_platform=macos
```

Record the three resulting run IDs, require each run's `headSha` to equal `$calibrationSha`, wait for success, and download by exact name:

```powershell
gh run view <windows-run-id> --json headSha,status,conclusion,event,workflowName
gh run view <linux-run-id> --json headSha,status,conclusion,event,workflowName
gh run view <macos-run-id> --json headSha,status,conclusion,event,workflowName
gh run watch <windows-run-id> --exit-status
gh run watch <linux-run-id> --exit-status
gh run watch <macos-run-id> --exit-status
$downloadRoot = "build/gpu-calibration-downloads/$calibrationSha"
if (Test-Path -LiteralPath $downloadRoot) { throw "calibration download root already exists" }
New-Item -ItemType Directory -Path "$downloadRoot/windows","$downloadRoot/linux","$downloadRoot/macos" | Out-Null
gh run download <windows-run-id> -n gpu-capability-windows-calibration -D "$downloadRoot/windows"
gh run download <linux-run-id> -n gpu-capability-linux-calibration -D "$downloadRoot/linux"
gh run download <macos-run-id> -n gpu-capability-macos-calibration -D "$downloadRoot/macos"
$expected = @{
  windows = @("2026-07-17-gpu-capability-worker-calibration.md","2026-07-17-gpu-capability-worker-decision.json")
  linux = @("2026-07-17-gpu-capability-linux-worker-calibration.md","2026-07-17-gpu-capability-linux-worker-decision.json")
  macos = @("2026-07-17-gpu-capability-macos-worker-calibration.md","2026-07-17-gpu-capability-macos-worker-decision.json")
}
foreach ($platform in $expected.Keys) {
  $actual = @(Get-ChildItem -LiteralPath "$downloadRoot/$platform" -File -Recurse | ForEach-Object { $_.FullName.Substring((Resolve-Path "$downloadRoot/$platform").Path.Length + 1).Replace('\','/') } | Sort-Object)
  if (Compare-Object $actual @($expected[$platform] | Sort-Object)) { throw "unexpected $platform artifact file set" }
}
python tests/gpu/gpu_capability_calibration.py --verify-downloaded-artifact-set --download-root $downloadRoot --calibration-sha $calibrationSha --windows-run-id <windows-run-id> --linux-run-id <linux-run-id> --macos-run-id <macos-run-id>
if ($LASTEXITCODE -ne 0) { throw "downloaded calibration verification failed" }
Copy-Item -Force -LiteralPath "$downloadRoot/windows/2026-07-17-gpu-capability-worker-calibration.md" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md"
Copy-Item -Force -LiteralPath "$downloadRoot/windows/2026-07-17-gpu-capability-worker-decision.json" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json"
Copy-Item -Force -LiteralPath "$downloadRoot/linux/2026-07-17-gpu-capability-linux-worker-calibration.md" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-calibration.md"
Copy-Item -Force -LiteralPath "$downloadRoot/linux/2026-07-17-gpu-capability-linux-worker-decision.json" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-decision.json"
Copy-Item -Force -LiteralPath "$downloadRoot/macos/2026-07-17-gpu-capability-macos-worker-calibration.md" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-calibration.md"
Copy-Item -Force -LiteralPath "$downloadRoot/macos/2026-07-17-gpu-capability-macos-worker-decision.json" -Destination "docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-decision.json"
```

Each job checks out the supplied SHA, asserts workflow input == `GITHUB_SHA` == canonical record `calibration_sha`, strict-reloads on its native runner, and uploads only its pair. No hosted workflow is invoked before Step 8's push. Task 10 adds the bounded `--verify-downloaded-artifact-set` CLI shown above; it performs only cross-platform-safe schema/discriminant/canonical-byte/hash/machine-record/run-metadata validation and never pretends to recompute a foreign native key. The producing native job has already performed authoritative current-key validation. Require each platform download directory to contain exactly its two expected basenames and no nested/extra entry; verify schema/hash/machine-record links, run id/attempt and `$calibrationSha` from those exact six downloaded files. Only after verification, copy each explicitly named source to its explicitly named `docs/superpowers/evidence/...` target (no glob and no directory-wide copy), strict-reload those six target paths again, compare source/target SHA-256, and review them before staging. A missing, pre-existing download root, extra file, wrong basename, or target/source hash mismatch fails.

- [ ] **Step 10: Commit calibrated artifacts and closure evidence**

Target-stage exactly the six named evidence files below, no other closure path, commit them with the required trailer, and push with the credential-helper form and all hooks. Never regenerate or edit a native record on a foreign host.

```powershell
git add docs/superpowers/evidence/2026-07-17-gpu-capability-worker-calibration.md docs/superpowers/evidence/2026-07-17-gpu-capability-worker-decision.json docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-calibration.md docs/superpowers/evidence/2026-07-17-gpu-capability-linux-worker-decision.json docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-calibration.md docs/superpowers/evidence/2026-07-17-gpu-capability-macos-worker-decision.json
git commit -m "test(gpu): record native capability calibration" -m "Co-Authored-By: Claude <noreply@anthropic.com>"
```

- [ ] **Step 11: Obtain fresh reviews, fix findings, push, and require current-head CI**

First run artifact-dependent native correctness on Windows/Linux/macOS, Windows cold/warm acceptance and selected-decision performance at the artifact commit. Then obtain the two fresh correctness/concurrency and performance reviews. For every reproduced finding, add RED and implement the smallest fix. Any keyed fix invalidates the artifacts and must repeat the entire Step 8 code commit/push → Step 9 exact-SHA calibration → Step 10 artifact-only commit loop before these gates/reviews resume. Push with hooks and credential helper; never use `--no-verify`:

```powershell
git -c credential.helper= -c credential.helper='!gh auth git-credential' push -u origin $(git branch --show-current)
```

If and only if a documented local gate cannot run, use its narrow repository flag (`SKIP_UNIT=1`, `SKIP_TIDY=1`, or `OLR_PREPUSH_LIGHT=1`; full-mode gates use their named `SKIP_*` flag) and keep every other hook. After push, wait for the current head SHA—not an earlier run—and require green status for the Windows pinned reference cold/warm and perf gates; Linux real production-database correctness, Clang live parity, and cgroup accounting; macOS real production-database correctness, AppleClang Objective-C++ parity, and process-group/libproc accounting; plus every repository-required build/test/audit job. A skipped, neutral, stale-SHA, cancelled, or missing required job is not green. Record the PR URL, Actions run URLs, head SHA, and exact check conclusions.

- [ ] **Step 12: Stop before merge**

Report commit SHAs, exact test counts, compiler versions, Windows cold/warm Job-RSS summaries, Linux cgroup-accounted summaries, macOS observed-residency/containment summaries, compact cache entries/bytes, coverage, PR/CI URLs and current statuses, and both independent review verdicts. Stop and hand control to the user; do not auto-merge.
