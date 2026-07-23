# GPU Capability Compiler-Preprocessing Design Addendum

## Status and relationship to the GPU design

This addendum replaces only the macro-expansion portion of the production GPU
capability source audit. It supplements the GPU lifetime and capability designs
dated 2026-07-12 and 2026-07-14; it does not change their production API or
runtime architecture.

The following locked decisions remain unchanged:

- `GpuSurface::nativeHandle()` is protected and `GpuReadLease` is its only
  friend.
- `retainUntilFenceRetired()` and `pendingFenceValue()` remain public monotonic
  watermark operations.
- Native backing acquisition continues through `GpuSyncReadScope`.
- The public synchronous `read()` compatibility path remains narrowly audited,
  while `withRead()` is preferred for new production code.

The audit is a build/test gate. This work adds no production call, allocation,
lock, fence operation, GPU wait, or application-startup work.

## Problem

The current audit parses compiler macro dumps and emulates expansion. Repeated
hardening has made the emulator bounded, but not compiler-equivalent. Exact
counterexamples remain:

- a nested invocation of the macro currently being expanded is hidden at the
  wrong time;
- a macro argument suppressed by `##` is expanded independently and can reject
  safe code;
- `__VA_OPT__` is not implemented;
- inactive `#undef` directives are applied as if active; and
- `-dM` describes final macro state, not the state at each invocation.

These are properties of preprocessing as a whole, including include order and
conditional evaluation. Adding more local hide-set rules cannot make a macro
dump authoritative.

## Decision

The configured C++ compiler's preprocessed token stream is authoritative for
macro expansion and conditional selection in every production region reached
from a compile command. The audit reconstructs production-file views from line
markers, applies the existing capability grammar to those views, and maps each
finding back to a canonical repository path and original line.

The complete gate has three lanes:

1. **Raw/directive lane:** scans every production source and header, regardless
   of platform, for direct capability use, declaration visibility, physical
   line splicing, malformed directives, and provenance-changing `#line` use.
2. **Compiler-authoritative lane:** preprocesses every unique relevant compile
   configuration and audits every reached production-file region after actual
   macro expansion and conditional selection.
3. **Source-only lane:** audits platform-inactive translation units and
   production headers not reached by a supplied configuration, without claiming
   compiler equivalence. It evaluates source-local constructs conservatively and
   rejects a capability-sensitive macro construct when it cannot prove that
   construct safe within its fixed bounds.

Findings from all lanes and all configurations are unioned. A construct that is
forbidden in one configuration fails the gate even when it is absent or safe in
another.

## Alternatives considered

### Complete the custom macro emulator

Rejected. Correct expansion requires replacing/unavailable-token state,
contextual argument suppression for `#` and `##`, placemarkers, variadics and
`__VA_OPT__`, include-order macro state, conditional evaluation, implementation
extensions, and point-in-time state rather than final `-dM` state. This would be
a second preprocessor whose correctness burden grows with every supported
compiler.

### Reject all macro use near a guarded capability

Rejected. It would reject valid code such as `CAT(safe, X)` where `X` is not
expanded because it is a raw paste operand. It would also make ordinary SDK and
configuration macros unusable in reviewed adapters. The compiler-authoritative
lane distinguishes those cases without weakening the capability boundary.

### Add more generated compile-negative probes

Rejected as the primary mechanism. Compile-negative probes prove public access
control but cannot enumerate all production macro call sites, conditional
configurations, synchronous-scope completion paths, or non-local control flow.
They remain a complementary gate.

## Inputs and coverage

The audit accepts one or more `compile_commands.json` files. A CTest invocation
passes the current build tree's database; the CI matrix collectively supplies
the supported Windows, macOS, Linux, sanitizer, and configuration variants.

Each database entry is normalized into a `PreprocessConfiguration` containing:

- database path and entry index;
- canonical working directory and main-source identity;
- compiler family and compiler fingerprint;
- fully expanded semantic arguments;
- environment digest; and
- a stable configuration digest used in diagnostics and caching.

Every distinct semantic command for a production translation unit is run.
Duplicate byte-identical configurations are coalesced, but differing defines,
include paths, language modes, targets, architectures, forced includes, or
compiler identities are not. Macro definitions that differ across
configurations are expected; their findings are unioned rather than treated as
a loader error.

An active-platform production translation unit must have at least one usable
entry. Missing, ambiguous, or failed coverage is a gate failure. The coverage
classifier preserves the existing explicit rules: generic `.c`, `.cc`, `.cpp`,
and `.cxx` files are active on every configured host; `.mm` and `_apple` files
are active when the databases contain an Objective-C++ entry; and paths under a
`win` component plus `_win` and `_mediafoundation` files are active when the
databases contain a Windows backend entry. The classifier is a reviewed constant
with path-by-path tests, not an inference from file contents. Platform-inactive
translation units use the source-only lane and must receive compiler-
authoritative coverage in their native CI job.

Headers do not require standalone compile entries. Each reached production
header is audited in every inclusion/configuration in which the compiler emits
it. A production header not reached by any supplied configuration also receives
the source-only lane. Coverage output reports authoritative and source-only
paths separately so native CI can prove that platform headers became active.

## Command decoding and compiler identification

Structured `arguments` entries are preferred. A `command` entry is decoded with
POSIX shell word rules on POSIX and `CommandLineToArgvW` rules on Windows. The
audit never sends the command through a shell. Shell operators, an empty command,
stdin as the source, more than one source input, or a source that does not match
the database entry are rejected.

Supported launcher chains are `ccache`, `sccache`, `distcc`, and `icecc`,
including the already supported ccache value options, flags, and `KEY=VALUE`
configuration arguments. Launchers are parsed and removed; the audit invokes
the resolved compiler directly so launcher output, cache mode, and remote
execution cannot alter the preprocessed stream. An unknown wrapper or unknown
launcher option fails closed and names the offending token.

Compiler family is selected from the resolved executable name and verified with
a bounded version probe:

- `gcc`, `g++`, and MinGW variants use the GCC driver rules;
- `clang`, `clang++`, and AppleClang use the Clang driver rules;
- `cl` uses the MSVC rules; and
- `clang-cl` uses the clang-cl rules, not the GNU-style Clang rules.

The fingerprint hashes the canonical compiler executable, its size and
modification metadata, its content, and normalized bounded version output. A
failed or contradictory family probe rejects the entry.

## Response files

All `@response` files are expanded before command rewriting. Relative paths are
resolved against the compile entry's working directory. GCC/Clang response
files use their driver quoting rules; MSVC/clang-cl response files use Windows
command-line quoting and accept UTF-8 with or without BOM and UTF-16LE with BOM.

Expansion is recursive with these fixed bounds:

- maximum depth: 8;
- maximum distinct files per command: 32;
- maximum aggregate response bytes: 4 MiB; and
- no canonical-path cycle.

Unreadable files, undecodable content, cycles, unsupported quoting, and bound
exhaustion fail the configuration. Expansion occurs before source validation
and flag removal, so compile/output flags cannot remain hidden in a response
file.

## GCC and Clang rewriting

For GCC-family and GNU-style Clang commands, rewriting preserves every argument
that can affect preprocessing: language and standard mode, `-D`/`-U`, include
and framework paths, forced includes, sysroots, target and architecture flags,
module/PCH inputs, and warning flags that affect preprocessing. Module and PCH
arguments are preserved byte-for-byte after response expansion; if the selected
compiler cannot preprocess with them, that configuration fails rather than
silently discarding them.

The rewriter removes compile-only, dependency-output, diagnostics-output, and
object-output options, including paired and attached forms of `-c`, `-o`,
`-MD`, `-MMD`, `-MF`, `-MT`, `-MQ`, and `-MJ`. It then adds `-E` without `-P`
so compiler line markers remain on stdout. A private dependency-output path is
added using full dependency reporting, including system headers. Any unknown
option that names an output file, enables a second source, or makes preprocessing
output ambiguous is rejected rather than guessed.

The original source argument remains exactly once. The process runs in the
compile entry's canonical working directory with the entry's effective
environment.

## MSVC and clang-cl rewriting

For `cl` and `clang-cl`, rewriting preserves preprocessing semantics including
`/D`, `/U`, `/I`, `/external:I`, `/FI`, `/std:`, language selection, target and
architecture options, compatibility mode, and supported PCH/module inputs.

The rewriter removes `/c`, object/program/PDB output options (`/Fo`, `/Fe`,
`/Fd` and paired forms), include-reporting output, source-dependency output, and
other compile-only outputs. It adds `/nologo /E`, which writes preprocessed text
with line directives to stdout. A private `/sourceDependencies` output is added
for the dependency manifest. `clang-cl` slash options are processed with these
rules; explicit `/clang:` payloads are additionally checked for hidden GNU-style
compile, dependency, marker-suppression, or output options.

Unknown options with output or source-selection semantics fail closed. Options
known to be diagnostics-only may remain. The original source appears exactly
once and no `/P` or marker-suppression option survives.

## Bounded execution

Preprocessing uses a process API, never a shell. Stdout is parsed as a stream so
the complete preprocessed translation unit is not retained in memory. Stderr is
kept as a bounded tail for diagnostics.

The fixed limits are:

- 60 seconds per compiler invocation;
- 240 seconds for all cold invocations in one audit process;
- 128 MiB of stdout per invocation;
- 1 MiB of retained stderr per invocation;
- 384 MiB of retained packed token tables and columns per invocation;
- 512 MiB peak coordinator resident memory; and
- at most `min(8, logical_cpu_count)` compiler processes concurrently.

Timeout, output overflow, decode failure, dependency-manifest failure, abnormal
exit, or coordinator-limit exhaustion terminates the compiler process group and
fails the configuration. Diagnostics include the configuration digest, compiler
family, canonical main source, exit status, elapsed time, observed bytes, and the
bounded stderr tail. No partial output is audited as success.

Stdout remains bytes through marker parsing and tokenization. Guarded C++ tokens
and directive syntax are ASCII; a non-ASCII identifier is retained as one opaque
token and cannot be confused with an ASCII guarded spelling. GCC/Clang marker
filenames are C-unescaped and decoded with the host filesystem encoding in strict
mode. MSVC/clang-cl marker filenames are decoded with the command's explicit
source/execution charset when present, otherwise with the Windows active code
page through strict `MultiByteToWideChar`. An invalid byte sequence fails
provenance. Stderr is rendered as escaped bytes when it cannot be decoded and is
never parsed as compiler output.

## Canonical identity and line-marker provenance

At startup, the audit enumerates production files under `playback/` and
`recorder_engine/`. Each regular file receives one canonical identity consisting
of its resolved path, repository-relative POSIX path, filesystem identity where
available, and physical line count. A production path that resolves outside the
repository, crosses a symlink/reparse alias, aliases another production file, or
has ambiguous case is rejected.

Dependency manifests are canonicalized with the same rules. Compiler pseudo-
files are not dependencies. A preprocessor
region can be attributed to production code only when its marker resolves to an
enumerated production identity that is present in that configuration's
dependency manifest. Relative markers are resolved against the compiler working
directory first and the current real file's directory second. Exactly one
candidate must match the dependency manifest; zero or multiple matches reject
the whole configuration.

GCC/Clang linemarkers are parsed with their enter-file, return-file, system-
header, and extern-C flags. Enter and return flags must form a balanced include
stack. A cross-file transition without a valid enter/return relationship, a
return to a non-ancestor, or a marker for a non-dependency rejects the
configuration. Unflagged markers may advance only within the current canonical
file. The standard initial sequence through `<built-in>`, `<command-line>`, or
Clang's `<command line>` spelling is accepted only before the first non-directive
code token and is never attributed to production code. Line zero is valid only
in that bootstrap sequence. `<stdin>`, an unknown pseudo-file, or a pseudo-file
transition after code tokens begin is rejected.

MSVC/clang-cl `#line` transitions do not carry GCC stack flags. Their parser
maintains inclusion instances from canonical dependency identities: a marker to
a new dependency starts a child instance, and a marker to an existing ancestor
returns to that instance. A transition that can be interpreted as both a new
inclusion and a return, or that is inconsistent with dependency membership and
the current inclusion stack,
rejects the configuration rather than merging regions.

Raw scanning rejects `#line` directives in production files, including line-
spliced spellings. Every real dependency is also scanned in its raw form; a
source-authored `#line` target that canonicalizes to any production identity is
rejected. Compiler-emitted line markers exist only in the subprocess stream and
are validated separately by the marker state machine, so a dependency cannot
manufacture production provenance.

For every production marker, the logical line must be in the range
`1..physical_line_count + 1`. Tokens after an out-of-range marker, malformed
quoted filename, embedded NUL, undecodable path, or unmatched stack transition
make provenance ambiguous and fail the configuration.

## Preprocessed translation-unit views

Each configuration produces one `PreprocessedTranslationUnitView`. Its `tokens`
member is a `CompactTokenSequence`, not a tuple of token dataclasses. The
sequence interns spelling and file identity once, then stores four parallel
32-bit `array` columns per token: spelling-table ID, identity-table ID,
inclusion-instance ID, and original line. The digest string exists only in the
view's `PreprocessConfiguration`; the sequence retains a reference to that same
configuration and reads its digest only when materializing a token view.
Implementations require four-byte `array('I')` items;
a host with a different item size fails explicitly rather than changing the
memory calculation. A spelling/identity/inclusion ID or original line above
`UINT32_MAX` also fails before append; integer wrap is never accepted.

`CompactTokenSequence` implements `collections.abc.Sequence`. `__getitem__` and
`__iter__` materialize immutable `PreprocessedToken`/`SourceLocation` views only
on demand. Its packed interface exposes `packed_bytes`, spelling/identity table
lookups, indexed spelling IDs, and runs of equal identity/inclusion/line so the
audit can consume packed storage without allocating one Python object per token.
Preprocessor marker/directive records are consumed by the provenance parser and
are never treated as C++ code. The raw subprocess text and per-token dataclass
objects are not retained.

The streaming builder reserves and appends packed columns in bounded blocks,
tracks column/table bytes and sampled process RSS after each block, and raises an
infrastructure failure before either the 384 MiB retained-token bound or the
512 MiB coordinator-RSS bound is crossed. A partial sequence is never finalized
or cached.

The view keeps tokens from non-production dependencies because their balanced
scopes and declarations can provide syntactic context around a production
header. Candidate operations and reportable declarations must originate in a
canonical production identity; non-production tokens can affect parsing but
cannot themselves produce a finding. A capability expression whose required
tokens carry inconsistent or ambiguous production identities fails provenance
instead of being assigned to one file.

When an include temporarily leaves a file, later tokens resume the same
inclusion instance. A repeated inclusion receives a new instance identifier.
This retains the compiler's complete translation-unit context without
concatenating independent header inclusions or allowing a finding to inherit the
wrong source location.

The existing direct capability, synchronous-scope, callback, completion,
non-local-control, typed sink/method, public-member, and binding grammars are
refactored to consume packed runs, spelling IDs, and identity lookups directly.
`AuditBuffer` must not enumerate `PreprocessedToken` objects or copy the four
packed columns. It constructs normalized audit bytes in bounded blocks from
packed runs/table lookups, stores run-length offsets back to packed runs, and
checks coordinator RSS before committing each block. Its position API
materializes a location only for a reportable candidate and uses the compiler's
original-line mapping rather than normalized output line numbers. Compiler-
expanded tokens are therefore reported at the invocation/source location
selected by the compiler.

Findings are deduplicated by canonical path, original line, forbidden
expression, and reason. The diagnostic also lists every configuration digest in
which the finding occurred.

## Responsibilities of the raw and source-only lanes

The raw lane remains authoritative for facts preprocessing erases or rewrites:

- forbidden direct spellings in C++ code outside macro replacement directives,
  and public declarations;
- physical backslash/newline reconstruction;
- source-authored `#line` and malformed directives;
- platform-internal declaration allowlists; and
- the public `registerRetire()` and `track()` surface checks.

For compiler-covered code, the raw lane does not reject an otherwise ordinary
macro merely because its definition contains `#`, `##`, variadics, or a guarded
fragment. Macro meaning comes from the compiler view. This removes the safe raw-
paste false positive without weakening direct-source checks.

The source-only lane runs the raw grammar across every conditional branch and a
bounded source-local macro analysis. It rejects source-local reconstruction of a
guarded identifier, malformed expansion, exhausted depth/token/paste bounds, and
an unresolved macro in a capability-sensitive receiver, member-name, callback,
completion, or control-flow position. It does not reject unrelated macros. A
source-only result is never used to claim native-platform compiler coverage;
the coverage report must be replaced by an authoritative view in that platform's
CI job.

The compiler macro-dump loader and its full custom expansion path are removed
from the authoritative lane. A small source-local checker may remain solely for
the explicitly conservative source-only behavior.

## Cache and performance

The cache is test-build-local and contains no production artifacts. Its base key
hashes:

- schema version;
- canonical compiler fingerprint;
- compiler family;
- normalized expanded preprocessing arguments;
- canonical working directory and source identity; and
- a digest of the complete effective subprocess environment. Environment values
  are hashed but not written to cache manifests.

Each successful entry records the canonical dependency list and SHA-256 of every
dependency, the compact translation-unit view, provenance metadata, and bounded
diagnostics. On lookup, dependencies are content-hashed once per audit process
and shared across configurations. A changed, missing, aliased, or unreadable
dependency invalidates the entry. Compiler failure and partial output are never
cached.

Cache serialization streams intern tables and packed columns in bounded blocks.
It neither enumerates token views nor constructs a monolithic serialized copy;
deserialization rebuilds arrays directly and applies the same packed-byte/RSS
limits before publishing a hit.

Entries publish by write-to-temporary plus atomic rename after the manifest and
payload are complete. Cache corruption is a miss followed by rebuild; inability
to rebuild is a gate failure. Cleanup removes incomplete entries older than one
hour, then least-recently-used complete entries until the cache is at most
512 MiB and 256 entries. Complete entries unused for 14 days are eligible for
removal. Cleanup never follows links and never modifies the source tree.

The performance suite streams one million high-density repeated tokens and
proves that packed retained bytes and RSS remain below injected limits without
retaining raw stdout or one dataclass per token. A second control injects a
smaller packed-byte limit and proves the builder fails before crossing it.

On the repository's supported Windows reference build, the acceptance limits are
180 seconds for an empty-cache audit, 20 seconds for an unchanged warm-cache
audit, and 512 MiB peak coordinator RSS. Other CI platforms use the same hard
limits. The existing pure-Python 4k/8k/16k scaling gate remains for the raw and
source-only grammars. There is no application/runtime benchmark because the new
work is absent from production binaries and execution.

## Failure policy

The audit fails, rather than silently falling back, for an active-platform main
source with no compile entry, an unsupported compiler/wrapper/response construct,
compiler failure, timeout, output/resource overflow, dependency failure,
ambiguous canonical identity, invalid marker transition, incomplete view, or
cache entry that cannot be validated and rebuilt.

Platform-inactive and unreachable files use the documented source-only lane;
this is the only fallback. The final summary names every such path and the native
CI configuration expected to make it authoritative.

## Test strategy

### Exact compiler-parity regressions

Tests use real preprocessing and assert both compiler output and audit result:

- nested same-name argument prescan:
  `F(F(A))(native,Handle)` expands through a callable paste alias and is rejected;
- original-tail rescan:
  `F(call)(F(native),Handle)` is rejected;
- object replacement consuming its original tail is rejected;
- `__VA_OPT__` paste to `nativeHandle` is rejected in the supported language
  mode/compiler extension;
- an inactive `#undef` does not remove an active macro and the forbidden use is
  rejected;
- an active included macro used before a later `#undef` is rejected at its use;
- the same macro in a configuration where its branch is inactive produces no
  finding; and
- `CAT(safe,X)` with raw `##` suppression preprocesses to `safeX` and passes.

These fixtures cover empty variadic operands, placemarkers, stringification,
multi-stage paste, recursive/self-recursive macros, malformed invocations, and
the existing depth/token/output limits. Compiler errors and malformed output
must produce an audit infrastructure failure, never a capability pass.

### Command and provenance regressions

Deterministic tests cover structured and string compile entries, spaces and
quotes, each supported launcher, launcher options and assignments, nested
response files, cycles and size/depth limits, paired/attached output flags,
forced includes, sysroots, targets, multiple source rejection, and compiler-
family selection.

Marker fixtures cover nested includes, repeat inclusion, return transitions,
relative and absolute paths, Windows separators/case, GCC flags 1-4, MSVC
`#line`, source-authored spoof attempts, non-dependency targets, path aliases,
out-of-range lines, malformed markers, and truncated streams.

### Live compiler matrix

The gate includes live smoke fixtures for GCC 13.10/MinGW, Clang/AppleClang,
MSVC `cl`, and `clang-cl` on CI hosts that provide those toolchains. A supported
family is not considered implemented until its live fixture proves command
rewriting, dependency capture, marker mapping, one forbidden macro expansion,
and one safe raw-paste control. Synthetic parser fixtures do not replace this
evidence.

### Repository acceptance

Acceptance requires:

- every exact bypass above is rejected and every safe control passes;
- every supplied compile configuration is audited and findings are unioned;
- active production translation units have authoritative coverage;
- native macOS CI supplies authoritative Objective-C++/Apple header coverage;
- preprocessing failure and ambiguous provenance controls fail closed;
- cold/warm time, output, concurrency, cache, and RSS bounds pass;
- the complete source audit, mutation tests, compile pass/fail tests, GPU scope
  contract, full unit suite, roadmap audit, and diff check pass; and
- a fresh independent review reports no Critical or Important finding in the
  compiler rewriting, provenance mapping, fail-closed policy, or capability
  grammar.

No acceptance step changes the locked GPU API design, and no part of this
addendum adds work to the application's frame or submission paths.
