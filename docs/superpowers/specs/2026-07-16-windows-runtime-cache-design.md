# Protected Windows Runtime Cache Design

## Scope and goal

The controlled Windows CTest and app launchers currently rebuild and delete
read/execute-only runtime snapshots for every invocation. A representative
Debug app launch writes about 239.7 MiB and takes about 14 seconds even when no
source input changed. This design replaces those repeated payload copies with a
per-user, content-addressed immutable cache while preserving the existing
closed loader, plugin, QML, executable, graph, and source namespaces.

The cache is launcher-only. It does not change production application code,
packaging, runtime hot paths, test registration, or the requirement to launch
Windows tests through `tools/run_ctest.py`.

## Security invariants

Every cache use must satisfy all of these invariants before a child process can
start:

1. The cache root comes from the Windows `FOLDERID_LocalAppData` known-folder
   API, not an inherited environment variable. The lexical root and every
   created parent resolve to the same handle-final path and contain no reparse
   point.
2. Cache management directories are private to the current user. Published
   object directories and their descendants deny write, delete, rename, and
   child insertion while permitting read and execute.
3. Source files are opened with the existing held-handle policy before keying
   or copying. Their key descriptors bind the handle-final path, filesystem
   identity, byte size, and SHA-256 digest. Copying reads those same held
   streams.
4. A published object is visible only after its complete tree and manifest have
   been converted to the immutable ACL and atomically renamed into place.
5. A cache hit is accepted only after its directory is held against
   retarget/delete and its manifest, contents, generated entries, final paths,
   ACL denial, and exact file set are revalidated. Missing, extra, partial,
   corrupted, writable, or aliased objects fail closed.
6. Active objects remain held for the complete child process-tree lifetime.
   Cleanup may skip them but may never invalidate or replace them.
7. Every staged executable receives a launcher-owned `qt.conf` beside it.
   Source `qt.conf` files are never copied. The generated file points only to
   the selected protected plugin and QML cache objects.

The threat boundary is the existing launcher boundary: ordinary same-user
filesystem mutation and concurrent launcher processes must not change audited
loader resolution between verification and process completion. An account
owner can ultimately take ownership and replace ACLs; the launcher detects that
state on the next validation and refuses to use it.

## Cache location and structure

The default root is:

```text
<FOLDERID_LocalAppData>/OpenLiveReplay/launch-cache/v2/
  objects/<sha256>/
    <layout-name>/...
    manifest.json
  staging/<uuid>/...
  quarantine/<sha256>-<uuid>/...
  access/<sha256>.stamp
  locks/repair-<sha256>/
  locks/cleanup/
  cleanup.stamp
```

Tests inject an explicit temporary root into `RuntimeAuditLease`; production
callers cannot select the root through environment variables. The root,
`objects`, `staging`, `quarantine`, `access`, and `locks` remain management
directories with private current-user access. Each child of `objects` is an
independently immutable published object.

## Object keys and manifests

The cache schema and closure policy have explicit version strings. An object
key is the SHA-256 digest of canonical UTF-8 JSON containing:

- schema and closure-policy versions;
- the layout purpose and launcher-supplied context;
- dependency object keys and canonical configured-root identities;
- each case-normalized destination path;
- for a held source: handle-final path, filesystem device/inode identity,
  size, and SHA-256;
- for literal bytes: size and SHA-256; and
- for generated content: generator policy name/version plus canonical declared
  inputs.

Destination spelling is also retained in the manifest so the published layout
preserves the requested case while still rejecting case-insensitive
collisions. Context binds material choices that may not otherwise appear in a
file path, including selected plugin families, QML closure policy, platform
plugins, configured roots, and graph rewrite policy.

`manifest.json` contains the key input, object key, layout name, creation time,
and the final size and SHA-256 of every published file. Time is metadata and is
not part of the content key. The manifest is written last in staging, before
the whole object receives its immutable ACL. Atomic publication means a reader
can observe either no final object or one complete object, never an in-place
partial update.

### Generated entries

Path-dependent files cannot use an unlabelled Python callable because callable
identity is not a stable cache input. A `GeneratedLayoutEntry` carries:

- a stable policy name/version;
- canonical declared inputs; and
- a renderer receiving the final layout path.

The key binds the policy and inputs. During publication the renderer produces
bytes for the final object path, even though output is first written under the
staging directory. During every cache-hit validation the renderer runs again
for the same final path; the expected bytes and digest must exactly match the
manifest and cached file. This supports rewritten `pyvenv.cfg`, generated
CTest graph files, and launcher-owned `qt.conf` without circular content keys.

## Publication and concurrent creators

Publication follows this sequence:

1. Hold and hash all sources, normalize destinations, validate limits, and
   compute the object key.
2. If `objects/<key>` exists, hold and fully validate it.
3. Otherwise create a unique `staging/<uuid>` tree, apply the build ACL, create
   directories, copy held sources, render generated entries for the final
   object path, and write `manifest.json` last.
4. Validate the staging tree, convert it to the immutable ACL, and atomically
   rename it to `objects/<key>` without replacement.
5. If another creator won the rename race, remove the losing staging tree and
   fully validate the winner. No creator trusts another process merely because
   the final directory exists.

Normal cache creation needs no global lock. Unique staging plus a no-replace
atomic rename allows concurrent creators without exposing partial state.

If a final object exists but fails validation, a creator acquires the atomic
`locks/repair-<key>` directory. It revalidates after acquiring the lock, changes
the corrupt object's ACL only for quarantine, atomically moves it out of
`objects`, and publishes a rebuilt object. A held active-object directory
denies that move; repair then fails closed instead of disturbing the active
launch. Repair-lock waits are bounded, stale repair locks are removable after
ten minutes, and a timeout is an actionable launcher error.

Interrupted staging directories are never considered cache entries. Cleanup
removes them after one hour.

## Hit validation and lease behavior

Hit validation performs all checks from the manifest rather than trusting
timestamps:

- the object and layout directories have canonical in-root handle-final paths;
- no directory or file is a symlink, junction, or other reparse alias;
- the exact expected directories and files exist, with no extras;
- canonical manifest JSON reproduces the directory key and requested context;
- every file size and SHA-256 matches;
- every generated file rerenders to identical bytes;
- opening a file for write/delete is denied; and
- opening a directory with add-file, add-subdirectory, or delete-child access
  is denied.

The lease retains directory handles and read-locked file handles after
validation. Cached objects are not deleted by `RuntimeAuditLease.close()`;
temporary non-cache views, if any, retain the current cleanup behavior.

`RuntimeAuditLease` keeps the existing logical `staged_file_count` and
`staged_byte_count` measurements and adds cache hit, miss, and payload-written
counts. Performance tests use payload-written bytes to distinguish a warm hit
from merely restaging the same logical payload.

## Launcher integration

The existing layout boundaries remain intact:

- configured runtime DLL/tool closure;
- Qt plugin closure;
- selected complete QML module closure;
- CTest tool closure;
- external tool and Python/venv closures;
- staged build executable/adjacent-DLL/CTest graph closure;
- QuickTest and approved source/QML property trees; and
- staged application executable/first-party adjacent DLL closure.

Each protected tree is stored as its own object. Dependencies publish first,
so later objects can bind their final paths and keys. The app object binds the
runtime, plugin, and QML keys. CTest build and external-tool objects bind the
same protected dependencies plus their graph-rewrite or external-closure
policy.

Before an executable-bearing layout is keyed, the launcher inserts one
generated `qt.conf` into every directory containing a staged `.exe`. Existing
entries at that name are replaced, not merged. The generated configuration
uses only the immutable plugin and QML object paths. This applies to the app,
CTest itself, build-tree tests, selected runtime tools, and external tools,
including Qt utilities such as `qmllint`.

Compiled runtime DLLs already verified as identical configured peers remain
deduplicated from executable directories. Their content is supplied by the
protected runtime object.

## Bounded lifecycle

The cache uses these fixed defaults:

- maximum total published size: 8 GiB;
- maximum published objects: 256;
- maximum unused age: 14 days;
- interrupted staging retention: one hour;
- access-stamp write throttle: one hour; and
- cleanup throttle: one day.

Access stamps live outside immutable objects. An immediate warm reuse therefore
writes no payload and normally no stamp. Publication triggers cleanup; ordinary
hits attempt cleanup at most once per day. Cleanup takes a nonblocking atomic
`locks/cleanup` directory, removes expired staging/quarantine entries, removes
objects unused for 14 days, then evicts least-recently-used objects until both
the byte and count limits hold. Failed removal of an active held object is
skipped. Cleanup never weakens an object's ACL unless it has selected that
object for removal while holding the cleanup lock.

If cleanup cannot reach the bounds because all candidates are active, the
current launch may proceed with already validated objects, but a publication
that would further exceed either bound fails with an actionable cache-capacity
error.

## Failure behavior

The cache never silently falls back to mutable source trees or an unvalidated
cache object. Invalid roots, aliases, malformed manifests, key mismatches,
content changes, ACL changes, partial publication, repair contention, and
capacity failures stop before process creation. A source change produces a new
key from held content; it cannot reuse the prior object. Diagnostic messages
identify the cache key, object path, and failed invariant without printing file
contents.

## Test and evidence strategy

All implementation follows red/green tests.

Static and cache tests cover:

- deterministic keys and destination collision rejection;
- held source identity/hash binding and source-change misses;
- literal and generated entry validation;
- concurrent creators publishing one valid object;
- interrupted staging and incomplete final-object rejection;
- corrupted manifest, content, extra file, missing file, and loosened ACL
  rejection;
- repair races and active-object-safe cleanup;
- access-stamp throttling, age eviction, LRU byte/count eviction, and capacity
  failure;
- no payload writes on an immediate warm hit;
- launcher-owned `qt.conf` beside every staged executable, replacement of
  source configs, and exclusively protected plugin/QML paths; and
- denied late insertion into imageformat, multimedia, QML asset, executable,
  runtime, and graph namespaces.

Representative performance gates call the real preparation functions without
starting a child:

- real built app: cold and immediate warm `prepare_windows_app_runtime()`;
- representative real focused CTest graph: cold and warm
  `prepare_windows_runtime()`; and
- representative real full CTest graph: cold and warm
  `prepare_windows_runtime()`.

Each gate records absolute elapsed time, logical files/bytes, cache hits/misses,
and payload-written files/bytes. The initial committed ceilings are:

- app cold: at most 30 seconds, 256 MiB, and 2,048 logical files;
- focused CTest cold: at most 30 seconds, 256 MiB, and 4,096 logical files;
- full CTest cold: at most 120 seconds, 2 GiB, and 16,384 logical files;
- app and focused CTest warm: at most 5 seconds; and
- full CTest warm: at most 8 seconds.

Every warm gate additionally requires zero cache misses and zero
payload-written files or bytes. These are hard upper bounds, not benchmark
baselines; evidence may tighten them but may not silently relax them. The warm
ceilings reject the former 239.7 MiB/~14 second copy/delete path. Cold limits
account for real executables, graph rewriting, source snapshots, external
tools, Qt assets, and generated configurations.

After static/cache completion, the exclusive runtime lane supplies real Windows
file-alias and junction evidence, CTest execution, and app launch evidence. No
runtime test may bypass the controlled launcher, error-mode containment, or
process-tree job object.

## Rejected alternatives

Hardlink or reflink temporary views were rejected because they either share
mutable source identity, depend on filesystem/volume support, or still require
a protected view lifecycle. A process-local retained temporary tree was
rejected because each supported launcher invocation starts a new Python
process, so it cannot provide warm reuse. A monolithic app/CTest cache image was
rejected because it couples unrelated closures, increases invalidation, and
makes generated graph paths harder to bind without circular keys.
