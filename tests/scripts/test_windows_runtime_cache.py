import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from dataclasses import replace
from pathlib import Path
from types import MappingProxyType
from typing import get_args
from unittest import mock

import tools.windows_runtime_cache as runtime_cache
from tools.windows_runtime_cache import (
    CacheLimits,
    CachedLayoutLease,
    GeneratedLayoutEntry,
    HeldSource,
    JsonScalar,
    LayoutEntry,
    LayoutEntries,
    WindowsRuntimeCache,
    _assert_mutation_denied,
    _atomic_move_no_replace,
    _set_tree_acl,
    canonical_json,
    layout_key,
    local_app_data_cache_root,
)
from tools.windows_runtime_guard import _current_user_sid


def held(name: str, payload: bytes, *, inode: int = 1) -> HeldSource:
    return HeldSource(
        source=Path(name),
        final_path=Path("C:/held") / name,
        device=7,
        inode=inode,
        size=len(payload),
        sha256=hashlib.sha256(payload).hexdigest(),
        stream=io.BytesIO(payload),
    )


def _resolved_source(path: Path) -> HeldSource:
    stream = path.open("rb")
    stat_result = os.fstat(stream.fileno())
    final_path = runtime_cache._final_path_from_handle(
        runtime_cache._kernel32(),
        __import__("msvcrt").get_osfhandle(stream.fileno()),
    )
    digest = hashlib.sha256(stream.read()).hexdigest()
    stream.seek(0)
    return HeldSource(
        source=path,
        final_path=final_path,
        device=int(stat_result.st_dev),
        inode=int(stat_result.st_ino),
        size=int(stat_result.st_size),
        sha256=digest,
        stream=stream,
    )


def _hold_path_and_set_acl_sddl(path: Path, sddl: str):
    owner, _canonical = runtime_cache._open_canonical_non_reparse(
        path,
        path.parent,
        desired_access=(
            runtime_cache._FILE_READ_ATTRIBUTES
            | runtime_cache._READ_CONTROL
            | runtime_cache._WRITE_DAC
        ),
        directory=path.is_dir(),
    )
    runtime_cache._set_handle_acl_sddl(owner.value, sddl)
    return owner


def _publication_worker(
    cache_root: str,
    source_path: str,
    ready_path: str,
    peer_ready_path: str,
    start_path: str,
) -> None:
    sid = _current_user_sid()
    cache = WindowsRuntimeCache(
        Path(cache_root),
        sid,
        _resolved_source,
        lambda path: runtime_cache._open_canonical_non_reparse(
            path, path.parent, directory=True
        ),
    )
    descriptor = _resolved_source(Path(source_path))
    Path(ready_path).write_text("ready", encoding="utf-8")
    deadline = time.monotonic() + 30
    while not Path(start_path).exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("publication worker start barrier timed out")
        time.sleep(0.01)

    real_move = runtime_cache._atomic_move_no_replace

    def synchronized_move(source: Path, destination: Path, *, sealed=None) -> None:
        Path(ready_path + ".move").write_text("ready", encoding="utf-8")
        while not Path(peer_ready_path + ".move").exists():
            if time.monotonic() >= deadline:
                raise RuntimeError("publication worker move barrier timed out")
            time.sleep(0.01)
        real_move(source, destination, sealed=sealed)

    try:
        with mock.patch.object(
            runtime_cache, "_atomic_move_no_replace", side_effect=synchronized_move
        ):
            lease = cache.materialize(
                "race",
                ((Path("bin/tool.exe"), descriptor),),
                context={"purpose": "concurrent-test"},
            )
        print(
            json.dumps(
                {
                    "key": lease.key,
                    "cache_hit": lease.cache_hit,
                    "payload_files": lease.payload_files,
                    "payload_bytes": lease.payload_bytes,
                }
            )
        )
        lease.close()
    finally:
        descriptor.stream.close()


def _generated_case_publication_worker(
    cache_root: str,
    layout_name: str,
    ready_path: str,
    peer_ready_path: str,
    start_path: str,
) -> None:
    sid = _current_user_sid()
    cache = WindowsRuntimeCache(
        Path(cache_root),
        sid,
        _resolved_source,
        lambda path: runtime_cache._open_canonical_non_reparse(
            path, path.parent, directory=True
        ),
    )
    generated = GeneratedLayoutEntry(
        "case-race-v1", {}, lambda final_layout: final_layout.as_posix().encode("utf-8")
    )
    Path(ready_path).write_text("ready", encoding="utf-8")
    deadline = time.monotonic() + 30
    while not Path(start_path).exists():
        if time.monotonic() >= deadline:
            raise RuntimeError("publication worker start barrier timed out")
        time.sleep(0.01)

    real_move = runtime_cache._atomic_move_no_replace

    def synchronized_move(source: Path, destination: Path, *, sealed=None) -> None:
        Path(ready_path + ".move").write_text("ready", encoding="utf-8")
        while not Path(peer_ready_path + ".move").exists():
            if time.monotonic() >= deadline:
                raise RuntimeError("publication worker move barrier timed out")
            time.sleep(0.01)
        real_move(source, destination, sealed=sealed)

    with mock.patch.object(
        runtime_cache, "_atomic_move_no_replace", side_effect=synchronized_move
    ):
        lease = cache.materialize(
            layout_name,
            ((Path("Bin/generated.txt"), generated),),
            context={"purpose": "generated-case-race"},
        )
    try:
        print(
            json.dumps(
                {
                    "key": lease.key,
                    "cache_hit": lease.cache_hit,
                    "layout": lease.layout.name,
                    "payload_files": lease.payload_files,
                    "payload_bytes": lease.payload_bytes,
                }
            )
        )
    finally:
        lease.close()


class CacheKeyTests(unittest.TestCase):
    def test_layout_key_is_order_independent_and_binds_every_identity_field(self) -> None:
        first = (
            (Path("bin/tool.exe"), held("tool.exe", b"tool")),
            (Path("literal"), b"x"),
        )
        second = tuple(reversed(first))
        key, manifest = layout_key(
            "tool", first, {"purpose": "ctest"}, ("a" * 64,)
        )
        self.assertEqual(
            layout_key(
                "tool", second, {"purpose": "ctest"}, ("a" * 64,)
            )[0],
            key,
        )
        self.assertEqual(manifest["schema"], "olr-windows-runtime-cache-v2")
        variants = (
            (
                (Path("bin/tool.exe"), held("tool.exe", b"tool", inode=2)),
                (Path("literal"), b"x"),
            ),
            (
                (Path("bin/tool.exe"), held("tool.exe", b"changed")),
                (Path("literal"), b"x"),
            ),
        )
        self.assertTrue(
            all(
                layout_key(
                    "tool", value, {"purpose": "ctest"}, ("a" * 64,)
                )[0]
                != key
                for value in variants
            )
        )
        self.assertNotEqual(
            layout_key("tool", first, {"purpose": "app"}, ("a" * 64,))[0],
            key,
        )
        self.assertNotEqual(
            layout_key("tool", first, {"purpose": "ctest"}, ("b" * 64,))[0],
            key,
        )

    def test_layout_key_independently_binds_all_source_and_layout_fields(self) -> None:
        source = held("tool.exe", b"tool")
        entries = ((Path("bin/tool.exe"), source), (Path("literal"), b"x"))
        key = layout_key("tool", entries, {}, ())[0]
        variants = (
            (
                "tool",
                (
                    (
                        Path("bin/tool.exe"),
                        replace(source, final_path=Path("C:/other/tool.exe")),
                    ),
                    (Path("literal"), b"x"),
                ),
            ),
            (
                "tool",
                (
                    (Path("bin/tool.exe"), replace(source, device=8)),
                    (Path("literal"), b"x"),
                ),
            ),
            (
                "tool",
                (
                    (Path("bin/tool.exe"), replace(source, inode=2)),
                    (Path("literal"), b"x"),
                ),
            ),
            (
                "tool",
                (
                    (Path("bin/tool.exe"), replace(source, size=5)),
                    (Path("literal"), b"x"),
                ),
            ),
            (
                "tool",
                (
                    (Path("bin/tool.exe"), replace(source, sha256="0" * 64)),
                    (Path("literal"), b"x"),
                ),
            ),
            ("other", entries),
            ("tool", ((Path("other/tool.exe"), source), (Path("literal"), b"x"))),
            ("tool", ((Path("bin/tool.exe"), source), (Path("literal"), b"y"))),
            (
                "tool",
                (
                    (Path("bin/tool.exe"), source),
                    (
                        Path("generated"),
                        GeneratedLayoutEntry(
                            "generated-v1",
                            {"value": "changed"},
                            lambda _root: b"x",
                        ),
                    ),
                ),
            ),
        )
        self.assertTrue(
            all(layout_key(name, variant, {}, ())[0] != key for name, variant in variants)
        )
        generated = GeneratedLayoutEntry(
            "generated-v1", {"value": "one"}, lambda _root: b"x"
        )
        changed_generated = GeneratedLayoutEntry(
            "generated-v1", {"value": "two"}, lambda _root: b"x"
        )
        self.assertNotEqual(
            layout_key("tool", ((Path("generated"), generated),), {}, ())[0],
            layout_key("tool", ((Path("generated"), changed_generated),), {}, ())[0],
        )

    def test_layout_key_rejects_unsafe_and_case_colliding_destinations(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unsafe destination"):
            layout_key(
                "tool", ((Path("../tool.exe"), held("tool.exe", b"x")),), {}, ()
            )
        with self.assertRaisesRegex(RuntimeError, "duplicate destination"):
            layout_key(
                "tool",
                (
                    (Path("Bin/tool.exe"), held("one", b"1")),
                    (Path("bin/TOOL.exe"), held("two", b"2")),
                ),
                {},
                (),
            )

    def test_layout_key_rejects_strictly_unsafe_win32_components(self) -> None:
        reserved = ("CON", "PRN", "AUX", "NUL") + tuple(
            f"{prefix}{index}"
            for prefix in ("COM", "LPT")
            for index in range(1, 10)
        ) + (
            "CONIN$",
            "CONOUT$",
            "CLOCK$",
            "COM¹",
            "COM²",
            "COM³",
            "LPT¹",
            "LPT²",
            "LPT³",
        )
        unsafe = (
            Path("foo/foo."),
            Path("foo/foo "),
            Path("foo:stream"),
            Path('foo<bar'),
            Path('foo>bar'),
            Path('foo"bar'),
            Path("foo|bar"),
            Path("foo?bar"),
            Path("foo*bar"),
            Path("foo/\x1fbar"),
            Path("AUX .txt"),
            *(Path(f"{name}.txt") for name in reserved),
        )
        for destination in unsafe:
            with self.subTest(destination=destination):
                with self.assertRaisesRegex(RuntimeError, "unsafe destination"):
                    layout_key("tool", ((destination, held("one", b"1")),), {}, ())

    def test_layout_key_uses_windows_case_normalization_not_unicode_casefold(self) -> None:
        entries = (
            (Path("Bin/stra\u00dfe.exe"), held("one", b"1")),
            (Path("bin/STRASSE.exe"), held("two", b"2")),
        )
        key, manifest = layout_key("tool", entries, {}, ())
        self.assertEqual(len(key), 64)
        self.assertEqual(len(manifest["entries"]), 2)

    def test_layout_key_excludes_case_only_destination_spelling_from_identity(
        self,
    ) -> None:
        source = held("tool.exe", b"tool")
        upper_key, upper_input = layout_key(
            "tool", ((Path("Bin/Tool.exe"), source),), {}, ()
        )
        lower_key, lower_input = layout_key(
            "tool", ((Path("bin/tool.exe"), source),), {}, ()
        )
        self.assertEqual(upper_key, lower_key)
        self.assertEqual(upper_input, lower_input)
        self.assertEqual(
            upper_input["entries"][0]["normalized_destination"],
            "bin/tool.exe",
        )
        self.assertNotIn("destination", upper_input["entries"][0])

    def test_layout_key_rejects_unsafe_layout_names(self) -> None:
        for name in (
            "../escape",
            "x/y",
            "x\\y",
            "C:relative",
            "AUX.txt",
            "layout.",
            "layout ",
        ):
            with self.subTest(name=name):
                with self.assertRaisesRegex(RuntimeError, "unsafe layout name"):
                    layout_key(name, ((Path("value"), b"x"),), {}, ())

    def test_layout_key_normalizes_layout_name_identity_for_windows(self) -> None:
        entries = ((Path("value"), b"x"),)
        lower_key, lower_input = layout_key("tool", entries, {}, ())
        upper_key, upper_input = layout_key("TOOL", entries, {}, ())
        self.assertEqual(lower_key, upper_key)
        self.assertEqual(lower_input["name"], "tool")
        self.assertEqual(upper_input["name"], "tool")

    def test_layout_key_snapshots_nested_mapping_context_and_generated_inputs(self) -> None:
        nested_context = {"values": [1, {"enabled": True}]}
        context = MappingProxyType({"nested": nested_context})
        generated_values = ["one", {"two": (2, 3)}]
        generated_inputs = MappingProxyType({"values": generated_values})
        entry = GeneratedLayoutEntry(
            "generated-v1", generated_inputs, lambda _root: b"x"
        )

        key, key_input = layout_key(
            "tool", ((Path("value"), entry),), context, ()
        )
        nested_context["values"].append(4)
        generated_values.append("changed")

        self.assertEqual(
            key,
            hashlib.sha256(canonical_json(key_input)).hexdigest(),
        )
        self.assertEqual(
            key_input["context"],
            {"nested": {"values": [1, {"enabled": True}]}},
        )
        self.assertEqual(
            key_input["entries"][0]["entry"]["inputs"],
            {"values": ["one", {"two": [2, 3]}]},
        )

    def test_layout_key_rejects_non_json_context_and_generated_inputs(self) -> None:
        invalid_values = (
            {1: "non-string key"},
            {"value": object()},
            {"value": float("nan")},
            {"value": float("inf")},
            {"value": float("-inf")},
            {"value": "\ud800"},
            {"value": "\udfff"},
            {"\ud800": "surrogate key"},
            {"\udfff": "surrogate key"},
        )
        for value in invalid_values:
            with self.subTest(value=value):
                with self.assertRaisesRegex(RuntimeError, "invalid JSON value"):
                    layout_key("tool", ((Path("value"), b"x"),), value, ())
                generated = GeneratedLayoutEntry(
                    "generated-v1", value, lambda _root: b"x"
                )
                with self.assertRaisesRegex(RuntimeError, "invalid JSON value"):
                    layout_key("tool", ((Path("value"), generated),), {}, ())

        cyclic: list[object] = []
        cyclic.append(cyclic)
        with self.assertRaisesRegex(RuntimeError, "invalid JSON value"):
            layout_key("tool", ((Path("value"), b"x"),), {"cycle": cyclic}, ())

    def test_layout_key_validates_and_snapshots_dependency_keys(self) -> None:
        dependencies = ["b" * 64, "a" * 64]
        key, key_input = layout_key(
            "tool", ((Path("value"), b"x"),), {}, dependencies
        )
        dependencies.append("c" * 64)
        self.assertEqual(len(key), 64)
        self.assertEqual(key_input["dependency_keys"], ["a" * 64, "b" * 64])
        with self.assertRaisesRegex(RuntimeError, "invalid dependency key"):
            layout_key("tool", ((Path("value"), b"x"),), {}, (7,))
        malformed = (
            "",
            "a" * 63,
            "a" * 65,
            "A" * 64,
            "g" * 64,
        )
        for dependency in malformed:
            with self.subTest(dependency=dependency):
                with self.assertRaisesRegex(RuntimeError, "invalid dependency key"):
                    layout_key(
                        "tool", ((Path("value"), b"x"),), {}, (dependency,)
                    )
        with self.assertRaisesRegex(RuntimeError, "duplicate dependency key"):
            layout_key(
                "tool",
                ((Path("value"), b"x"),),
                {},
                ("a" * 64, "a" * 64),
            )

    def test_layout_key_rejects_malformed_held_source_descriptors(self) -> None:
        source = held("tool.exe", b"tool")
        malformed = (
            replace(source, final_path=Path("relative/tool.exe")),
            replace(source, final_path=Path("C:relative\\tool.exe")),
            replace(source, final_path=Path("/rooted/tool.exe")),
            replace(source, final_path=Path("C:/held/../tool.exe")),
            replace(source, final_path=Path("C:/held/component./tool.exe")),
            replace(source, final_path=Path("C:/held/component /tool.exe")),
            replace(source, final_path=Path("C:/held/name:stream")),
            replace(source, final_path=Path("C:/held/bad<name/tool.exe")),
            replace(source, final_path=Path("C:/held/\x01name/tool.exe")),
            replace(source, final_path=Path("C:/held/AUX.txt/tool.exe")),
            replace(source, final_path=Path("C:/held/CONIN$.txt/tool.exe")),
            replace(source, final_path=Path("C:/held/COM¹.txt/tool.exe")),
            replace(source, final_path=Path(r"\\?\C:\held\tool.exe")),
            replace(
                source,
                final_path=Path(r"\\?\UNC\server\share\tool.exe"),
            ),
            replace(source, final_path=Path(r"\\.\C:\held\tool.exe")),
            replace(source, final_path=Path(r"\??\C:\held\tool.exe")),
            replace(source, final_path=Path(r"\\??\C:\held\tool.exe")),
            replace(
                source,
                final_path=Path(r"\Device\HarddiskVolume1\tool.exe"),
            ),
            replace(
                source,
                final_path=Path(r"\\Device\HarddiskVolume1\tool.exe"),
            ),
            replace(source, final_path=Path("//CON/share/tool.exe")),
            replace(source, final_path=Path("//server/AUX/tool.exe")),
            replace(source, device=True),
            replace(source, device=-1),
            replace(source, inode=False),
            replace(source, inode=-1),
            replace(source, size=True),
            replace(source, size=-1),
            replace(source, sha256="a" * 63),
            replace(source, sha256="a" * 65),
            replace(source, sha256="A" * 64),
            replace(source, sha256="g" * 64),
        )
        for descriptor in malformed:
            with self.subTest(descriptor=descriptor):
                with self.assertRaisesRegex(RuntimeError, "invalid held source"):
                    layout_key(
                        "tool", ((Path("tool.exe"), descriptor),), {}, ()
                    )

        drive_upper = replace(source, final_path=Path("C:/Held/Tool.exe"))
        drive_lower = replace(source, final_path=Path("c:/held/tool.exe"))
        drive_upper_key, drive_upper_input = layout_key(
            "tool", ((Path("tool.exe"), drive_upper),), {}, ()
        )
        drive_lower_key, drive_lower_input = layout_key(
            "tool", ((Path("tool.exe"), drive_lower),), {}, ()
        )
        self.assertEqual(drive_upper_key, drive_lower_key)
        self.assertEqual(drive_upper_input, drive_lower_input)

        unc_upper = replace(
            source, final_path=Path("//Server/Share$/Held/Tool.exe")
        )
        unc_lower = replace(
            source, final_path=Path("//server/share$/held/tool.exe")
        )
        unc_upper_key, unc_upper_input = layout_key(
            "tool", ((Path("tool.exe"), unc_upper),), {}, ()
        )
        unc_lower_key, unc_lower_input = layout_key(
            "tool", ((Path("tool.exe"), unc_lower),), {}, ()
        )
        self.assertEqual(unc_upper_key, unc_lower_key)
        self.assertEqual(unc_upper_input, unc_lower_input)

    def test_layout_key_rejects_invalid_generated_policy(self) -> None:
        for policy in ("", 7, object()):
            with self.subTest(policy=policy):
                entry = GeneratedLayoutEntry(policy, {}, lambda _root: b"x")
                with self.assertRaisesRegex(RuntimeError, "invalid generated policy"):
                    layout_key("tool", ((Path("value"), entry),), {}, ())

    def test_generated_entry_key_binds_policy_inputs_but_not_callable_identity(
        self,
    ) -> None:
        one = GeneratedLayoutEntry(
            "qt-conf-v1",
            {"plugins": "p", "qml": "q"},
            lambda _root: b"one",
        )
        two = GeneratedLayoutEntry(
            "qt-conf-v1",
            {"qml": "q", "plugins": "p"},
            lambda _root: b"two",
        )
        self.assertEqual(
            layout_key("app", ((Path("qt.conf"), one),), {}, ())[0],
            layout_key("app", ((Path("qt.conf"), two),), {}, ())[0],
        )
        changed = GeneratedLayoutEntry(
            "qt-conf-v2",
            {"plugins": "p", "qml": "q"},
            lambda _root: b"one",
        )
        self.assertNotEqual(
            layout_key("app", ((Path("qt.conf"), changed),), {}, ())[0],
            layout_key("app", ((Path("qt.conf"), one),), {}, ())[0],
        )

    def test_public_type_aliases_import_and_contain_declared_types(self) -> None:
        self.assertIn(type(None), get_args(JsonScalar))
        self.assertIn(HeldSource, get_args(LayoutEntry))
        self.assertIn(GeneratedLayoutEntry, get_args(LayoutEntry))
        self.assertTrue(get_args(LayoutEntries))


@unittest.skipUnless(os.name == "nt", "requires Windows known folders and ACLs")
class WindowsFilesystemTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sid = _current_user_sid()

    def tearDown(self) -> None:
        if self.root.exists():
            _set_tree_acl(self.root, self.sid, "DELETE")
            shutil.rmtree(self.root, ignore_errors=True)
        self.temporary.cleanup()

    def make_cache(self, root: Path) -> WindowsRuntimeCache:
        return WindowsRuntimeCache(
            root,
            self.sid,
            lambda path: held(path.name, path.read_bytes()),
            lambda path: (object(), path),
        )

    def test_known_folder_root_ignores_environment_and_rejects_reparse_parent(
        self,
    ) -> None:
        expected = local_app_data_cache_root()
        with mock.patch.dict(
            os.environ,
            {"LOCALAPPDATA": str(self.root / "poison")},
            clear=False,
        ):
            self.assertEqual(local_app_data_cache_root(), expected)

        real = self.root / "real"
        real.mkdir()
        alias = self.root / "alias"
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(alias), str(real)],
            check=True,
            capture_output=True,
        )
        try:
            with self.assertRaisesRegex(RuntimeError, "reparse"):
                self.make_cache(alias / "cache")
        finally:
            if alias.exists():
                os.rmdir(alias)

    def test_rx_tree_denies_file_and_directory_mutation_access(self) -> None:
        cache = self.make_cache(self.root / "rx-cache")
        tree = cache.objects / "tree"
        tree.mkdir()
        payload = tree / "payload.dll"
        payload.write_bytes(b"payload")

        _set_tree_acl(tree, self.sid, "RX")

        _assert_mutation_denied(tree, directory=True)
        _assert_mutation_denied(payload, directory=False)

    def test_mutation_probe_rejects_each_individually_permitted_right(self) -> None:
        cache = self.make_cache(self.root / "individual-rights-cache")
        tree = cache.objects / "individual-rights"
        tree.mkdir()
        payload = tree / "payload.dll"
        payload.write_bytes(b"payload")
        read_execute = "GRGX"

        directory_rights = (
            runtime_cache._FILE_ADD_FILE,
            runtime_cache._FILE_ADD_SUBDIRECTORY,
            runtime_cache._FILE_DELETE_CHILD,
            runtime_cache._DELETE,
        )
        for right in directory_rights:
            with self.subTest(kind="directory", right=right):
                owner = _hold_path_and_set_acl_sddl(
                    tree,
                    f"D:P(A;;{read_execute};;;{self.sid})(A;;0x{right:x};;;{self.sid})",
                )
                try:
                    with self.assertRaisesRegex(RuntimeError, "mutation access"):
                        _assert_mutation_denied(tree, directory=True)
                finally:
                    runtime_cache._set_handle_acl(owner.value, self.sid, "BUILD")
                    owner.close()

        file_rights = (
            runtime_cache._FILE_WRITE_DATA,
            runtime_cache._FILE_APPEND_DATA,
            runtime_cache._FILE_WRITE_EA,
            runtime_cache._FILE_WRITE_ATTRIBUTES,
            runtime_cache._DELETE,
        )
        for right in file_rights:
            with self.subTest(kind="file", right=right):
                owner = _hold_path_and_set_acl_sddl(
                    payload,
                    f"D:P(A;;{read_execute};;;{self.sid})(A;;0x{right:x};;;{self.sid})",
                )
                try:
                    with self.assertRaisesRegex(RuntimeError, "mutation access"):
                        _assert_mutation_denied(payload, directory=False)
                finally:
                    runtime_cache._set_handle_acl(owner.value, self.sid, "BUILD")
                    owner.close()

    def test_build_acl_allows_nested_payload_and_manifest_then_recursive_rx_denies_writes(
        self,
    ) -> None:
        tree = self.root / "staging"
        tree.mkdir()
        _set_tree_acl(tree, self.sid, "BUILD")
        nested = tree / "layout" / "plugins" / "imageformats"
        nested.mkdir(parents=True)
        payload = nested / "qjpeg.dll"
        manifest = tree / "manifest.json"
        payload.write_bytes(b"plugin")
        manifest.write_text('{"complete":true}\n', encoding="utf-8")

        _set_tree_acl(tree, self.sid, "RX")

        self.assertEqual(payload.read_bytes(), b"plugin")
        self.assertEqual(
            manifest.read_text(encoding="utf-8"), '{"complete":true}\n'
        )
        for target in (payload, manifest):
            _assert_mutation_denied(target, directory=False)
            with self.assertRaises(OSError):
                target.write_bytes(b"changed")
        _assert_mutation_denied(nested, directory=True)
        with self.assertRaises(OSError):
            (nested / "late.dll").write_bytes(b"late")

    def test_atomic_move_never_replaces_an_existing_destination(self) -> None:
        cache = self.make_cache(self.root / "atomic-no-replace")
        source = cache.staging / "source"
        source.mkdir()
        (source / "payload").write_bytes(b"source")
        sealed = runtime_cache._seal_tree_acl(source, self.sid, "RX")
        destination = cache.objects / ("8" * 64)
        destination.mkdir()
        (destination / "payload").write_bytes(b"destination")
        try:
            with self.assertRaisesRegex(RuntimeError, "retained sealed tree"):
                _atomic_move_no_replace(source, destination)
            with self.assertRaises(FileExistsError):
                _atomic_move_no_replace(source, destination, sealed=sealed)
            self.assertEqual((source / "payload").read_bytes(), b"source")
            self.assertEqual((destination / "payload").read_bytes(), b"destination")

            shutil.rmtree(destination)
            _atomic_move_no_replace(source, destination, sealed=sealed)
            self.assertFalse(source.exists())
            self.assertEqual((destination / "payload").read_bytes(), b"source")
        finally:
            sealed.close()

    def test_rx_conversion_denies_late_insertion_before_descendant_walk(self) -> None:
        tree = self.root / "race"
        nested = tree / "nested"
        nested.mkdir(parents=True)
        (nested / "payload.dll").write_bytes(b"payload")
        _set_tree_acl(tree, self.sid, "BUILD")
        real_set_handle_acl = runtime_cache._set_handle_acl
        attempted = False

        def attempt_late_insert(handle: int, sid: str, mode: str) -> None:
            nonlocal attempted
            real_set_handle_acl(handle, sid, mode)
            final = runtime_cache._final_path_from_handle(
                runtime_cache._kernel32(), handle
            )
            if mode == "RX" and final == tree:
                attempted = True
                with self.assertRaises(OSError):
                    (tree / "late").mkdir()

        with mock.patch.object(
            runtime_cache, "_set_handle_acl", side_effect=attempt_late_insert
        ):
            _set_tree_acl(tree, self.sid, "RX")

        self.assertTrue(attempted)
        self.assertFalse((tree / "late").exists())

    def test_objects_parent_cannot_authorize_published_child_rename(self) -> None:
        cache = self.make_cache(self.root / "cache")
        published = cache.objects / ("a" * 64)
        published.mkdir()
        (published / "payload.dll").write_bytes(b"payload")
        _set_tree_acl(published, self.sid, "RX")

        _assert_mutation_denied(published, directory=True)
        with self.assertRaises(OSError):
            published.rename(cache.objects / ("b" * 64))
        self.assertTrue(published.exists())

        _set_tree_acl(published, self.sid, "DELETE")
        quarantined = cache.quarantine / published.name
        published.rename(quarantined)
        self.assertTrue(quarantined.exists())

    def test_immutable_staging_tree_can_publish_into_objects_namespace(self) -> None:
        cache = self.make_cache(self.root / "publish")
        source = cache.staging / "creator"
        source.mkdir()
        (source / "payload.dll").write_bytes(b"payload")
        sealed = runtime_cache._seal_tree_acl(source, self.sid, "RX")
        destination = cache.objects / ("c" * 64)
        try:
            _atomic_move_no_replace(source, destination, sealed=sealed)

            self.assertFalse(source.exists())
            self.assertEqual((destination / "payload.dll").read_bytes(), b"payload")
            with self.assertRaises(OSError):
                destination.rename(cache.objects / ("d" * 64))
        finally:
            sealed.close()

        loser = cache.staging / "loser"
        loser.mkdir()
        (loser / "payload.dll").write_bytes(b"loser")
        loser_sealed = runtime_cache._seal_tree_acl(loser, self.sid, "RX")
        try:
            with self.assertRaises(FileExistsError):
                _atomic_move_no_replace(loser, destination, sealed=loser_sealed)
            self.assertTrue(loser.exists())
        finally:
            loser_sealed.close()

    def test_staging_publication_cannot_publish_a_path_replacement(self) -> None:
        cache = self.make_cache(self.root / "identity-bound-publish")
        source = cache.staging / "creator"
        source.mkdir()
        (source / "payload.dll").write_bytes(b"immutable")
        sealed = runtime_cache._seal_tree_acl(source, self.sid, "RX")
        destination = cache.objects / ("e" * 64)
        escaped = cache.staging / "escaped"
        try:
            with self.assertRaises(OSError):
                source.rename(escaped)

            replacement = cache.staging / "replacement"
            replacement.mkdir()
            (replacement / "payload.dll").write_bytes(b"writable replacement")
            with self.assertRaisesRegex(RuntimeError, "does not match publication source"):
                _atomic_move_no_replace(replacement, destination, sealed=sealed)
            self.assertFalse(destination.exists())

            _atomic_move_no_replace(source, destination, sealed=sealed)
            self.assertFalse(source.exists())
            self.assertEqual((destination / "payload.dll").read_bytes(), b"immutable")
            with self.assertRaises(OSError):
                (destination / "payload.dll").write_bytes(b"changed")
        finally:
            sealed.close()

    def test_rx_conversion_holds_protected_descendants_against_mutation(self) -> None:
        tree = self.root / "protected-descendants"
        tree.mkdir()
        _set_tree_acl(tree, self.sid, "BUILD")
        rename_target = tree / "rename.dll"
        mutate_target = tree / "mutate.dll"
        rename_target.write_bytes(b"rename-original")
        mutate_target.write_bytes(b"mutate-original")
        runtime_cache._set_path_acl(rename_target, self.sid, "BUILD")
        runtime_cache._set_path_acl(mutate_target, self.sid, "BUILD")
        escaped = self.root / "escaped.dll"
        real_set_handle_acl = runtime_cache._set_handle_acl
        attempted = False

        def attack_after_root_rx(handle: int, sid: str, mode: str) -> None:
            nonlocal attempted
            real_set_handle_acl(handle, sid, mode)
            final = runtime_cache._final_path_from_handle(
                runtime_cache._kernel32(), handle
            )
            if mode == "RX" and final == tree:
                attempted = True
                with self.assertRaises(OSError):
                    rename_target.rename(escaped)
                with self.assertRaises(OSError):
                    mutate_target.write_bytes(b"mutated")

        with mock.patch.object(
            runtime_cache, "_set_handle_acl", side_effect=attack_after_root_rx
        ):
            _set_tree_acl(tree, self.sid, "RX")

        self.assertTrue(attempted)
        self.assertFalse(escaped.exists())
        self.assertEqual(rename_target.read_bytes(), b"rename-original")
        self.assertEqual(mutate_target.read_bytes(), b"mutate-original")
        _assert_mutation_denied(rename_target, directory=False)
        _assert_mutation_denied(mutate_target, directory=False)

    def test_rx_conversion_holds_descendants_before_transitional_root_freeze(self) -> None:
        tree = self.root / "pre-freeze-descendants"
        tree.mkdir()
        _set_tree_acl(tree, self.sid, "BUILD")
        payload = tree / "protected.dll"
        payload.write_bytes(b"original")
        runtime_cache._set_path_acl(payload, self.sid, "BUILD")
        escaped = self.root / "escaped-before-freeze.dll"
        real_set_handle_acl_sddl = runtime_cache._set_handle_acl_sddl
        attempted = False

        def attack_at_root_freeze(handle: int, sddl: str) -> None:
            nonlocal attempted
            final = runtime_cache._final_path_from_handle(
                runtime_cache._kernel32(), handle
            )
            if final == tree and "0x1f01b9" in sddl:
                attempted = True
                with self.assertRaises(OSError):
                    payload.rename(escaped)
                with self.assertRaises(OSError):
                    payload.write_bytes(b"mutated")
            real_set_handle_acl_sddl(handle, sddl)

        with mock.patch.object(
            runtime_cache,
            "_set_handle_acl_sddl",
            side_effect=attack_at_root_freeze,
        ):
            _set_tree_acl(tree, self.sid, "RX")

        self.assertTrue(attempted)
        self.assertFalse(escaped.exists())
        self.assertEqual(payload.read_bytes(), b"original")
        _assert_mutation_denied(payload, directory=False)

    def test_publication_uses_bound_write_through_move_and_revalidates_identity(self) -> None:
        cache = self.make_cache(self.root / "write-through-publish")
        source = cache.staging / "creator"
        source.mkdir()
        (source / "payload.dll").write_bytes(b"payload")
        sealed = runtime_cache._seal_tree_acl(source, self.sid, "RX")
        destination = cache.objects / ("f" * 64)
        try:
            real_move = runtime_cache._move_file_no_replace_write_through
            with mock.patch.object(
                runtime_cache,
                "_move_file_no_replace_write_through",
                wraps=real_move,
            ) as move:
                _atomic_move_no_replace(source, destination, sealed=sealed)

            move.assert_called_once_with(source, destination)
            self.assertEqual(
                runtime_cache._final_path_from_handle(
                    runtime_cache._kernel32(), sealed.handle
                ),
                destination,
            )
            self.assertEqual((destination / "payload.dll").read_bytes(), b"payload")
            with self.assertRaises(OSError):
                (destination / "payload.dll").write_bytes(b"changed")
        finally:
            sealed.close()

    def test_publication_blocks_rename_and_replacement_at_move(self) -> None:
        cache = self.make_cache(self.root / "move-substitution")
        source = cache.staging / "creator"
        source.mkdir()
        (source / "payload.dll").write_bytes(b"immutable")
        sealed = runtime_cache._seal_tree_acl(source, self.sid, "RX")
        destination = cache.objects / ("9" * 64)
        escaped = cache.staging / "escaped"
        real_move = runtime_cache._move_file_no_replace_write_through
        attack_ran = False

        def attack_then_move(move_source: Path, move_destination: Path) -> None:
            nonlocal attack_ran
            attack_ran = True
            with self.assertRaises(OSError):
                move_source.rename(escaped)
            with self.assertRaises(OSError):
                (cache.staging / "replacement").mkdir()
            real_move(move_source, move_destination)

        try:
            with mock.patch.object(
                runtime_cache,
                "_move_file_no_replace_write_through",
                side_effect=attack_then_move,
            ):
                _atomic_move_no_replace(source, destination, sealed=sealed)

            self.assertTrue(attack_ran)
            self.assertTrue(destination.exists())
            self.assertFalse(source.exists())
            self.assertFalse(escaped.exists())
            self.assertEqual((destination / "payload.dll").read_bytes(), b"immutable")
            with self.assertRaises(OSError):
                (destination / "payload.dll").write_bytes(b"changed")
            (cache.staging / "after-publication").mkdir()
        finally:
            sealed.close()

    def test_existing_cache_root_swap_is_detected_during_handle_bound_acl_update(
        self,
    ) -> None:
        cache_root = self.root / "existing"
        cache_root.mkdir()
        swapped = self.root / "swapped"
        real_set_handle_acl = runtime_cache._set_handle_acl
        attempted = False

        def attempt_swap(handle: int, sid: str, mode: str) -> None:
            nonlocal attempted
            final = runtime_cache._final_path_from_handle(
                runtime_cache._kernel32(), handle
            )
            if final == cache_root:
                attempted = True
                cache_root.rename(swapped)
            real_set_handle_acl(handle, sid, mode)

        with mock.patch.object(
            runtime_cache, "_set_handle_acl", side_effect=attempt_swap
        ):
            with self.assertRaisesRegex(RuntimeError, "moved during protected update"):
                self.make_cache(cache_root)

        self.assertTrue(attempted)
        self.assertFalse(cache_root.exists())
        self.assertTrue(swapped.exists())

    def test_missing_managed_directories_receive_acl_at_creation(self) -> None:
        cache_root = self.root / "atomic"
        real_create = runtime_cache._create_directory_with_acl
        created: list[Path] = []

        def record_create(path: Path, sid: str, sddl: str) -> bool:
            created.append(path)
            self.assertEqual(sid, self.sid)
            self.assertTrue(sddl.startswith("D:P"))
            return real_create(path, sid, sddl)

        with mock.patch.object(
            runtime_cache,
            "_create_directory_with_acl",
            side_effect=record_create,
        ):
            cache = self.make_cache(cache_root)

        self.assertIn(cache_root, created)
        self.assertIn(cache.objects, created)


@unittest.skipUnless(os.name == "nt", "requires Windows publication semantics")
class PublicationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.cache_root = self.root / "cache"
        self.sources = self.root / "sources"
        self.sources.mkdir()
        self.sid = _current_user_sid()
        self.leases: list[CachedLayoutLease] = []
        self.descriptors: list[HeldSource] = []
        self.cache = self.make_cache()

    def tearDown(self) -> None:
        for lease in reversed(self.leases):
            lease.close()
        for descriptor in reversed(self.descriptors):
            descriptor.stream.close()
        for owner in self.cache._management_handles:
            owner.close()
        if self.root.exists():
            _set_tree_acl(self.root, self.sid, "DELETE")
            shutil.rmtree(self.root, ignore_errors=True)
        self.temporary.cleanup()

    def make_cache(self) -> WindowsRuntimeCache:
        return WindowsRuntimeCache(
            self.cache_root,
            self.sid,
            _resolved_source,
            lambda path: runtime_cache._open_canonical_non_reparse(
                path, path.parent, directory=True
            ),
        )

    def held_file(self, name: str, payload: bytes) -> HeldSource:
        path = self.sources / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        descriptor = _resolved_source(path)
        self.descriptors.append(descriptor)
        return descriptor

    def remember(self, lease: CachedLayoutLease) -> CachedLayoutLease:
        self.leases.append(lease)
        return lease

    def close(self, lease: CachedLayoutLease) -> None:
        lease.close()
        self.leases.remove(lease)

    def force_mutable(self, object_root: Path) -> None:
        _set_tree_acl(object_root, self.sid, "DELETE")

    def assert_warm_zero(self, lease: CachedLayoutLease) -> None:
        self.assertTrue(lease.cache_hit)
        self.assertEqual((lease.payload_files, lease.payload_bytes), (0, 0))

    def publish_and_close(
        self,
        name: str,
        entries: LayoutEntries,
        *,
        context=None,
    ) -> CachedLayoutLease:
        lease = self.cache.materialize(name, entries, context=context or {})
        self.close(self.remember(lease))
        return lease

    def test_cold_publish_then_exact_warm_hit_writes_no_payload(self) -> None:
        entries = ((Path("bin/tool.exe"), self.held_file("tool.exe", b"tool")),)
        first = self.remember(
            self.cache.materialize("tool", entries, context={"purpose": "ctest"})
        )
        self.assertFalse(first.cache_hit)
        self.assertEqual((first.logical_files, first.logical_bytes), (1, 4))
        self.assertEqual((first.payload_files, first.payload_bytes), (1, 4))
        self.assertEqual((first.layout / "bin/tool.exe").read_bytes(), b"tool")
        self.assertTrue(first._owners)
        first_root = first.object_root
        self.close(first)

        second = self.remember(
            self.cache.materialize("tool", entries, context={"purpose": "ctest"})
        )
        self.assert_warm_zero(second)
        self.assertEqual(second.object_root, first_root)
        self.assertEqual((second.logical_files, second.logical_bytes), (1, 4))

    def test_case_only_destination_and_layout_spelling_reuses_stored_spelling(self) -> None:
        source = self.held_file("tool.exe", b"tool")
        first = self.publish_and_close(
            "Tool", ((Path("Bin/Tool.exe"), source),), context={}
        )
        second = self.remember(
            self.cache.materialize(
                "tool", ((Path("bin/tool.exe"), source),), context={}
            )
        )
        self.assert_warm_zero(second)
        self.assertEqual(second.key, first.key)
        self.assertEqual(second.layout.name, "Tool")
        self.assertTrue((second.layout / "Bin/Tool.exe").is_file())
        manifest = json.loads((second.object_root / "manifest.json").read_text("utf-8"))
        self.assertEqual(manifest["layout_name"], "Tool")
        self.assertEqual(manifest["files"][0]["destination"], "Bin/Tool.exe")

    def test_case_only_generated_hit_renders_and_returns_stored_layout(self) -> None:
        rendered_at: list[Path] = []

        def render(final_layout: Path) -> bytes:
            rendered_at.append(final_layout)
            return final_layout.as_posix().encode("utf-8")

        generated = GeneratedLayoutEntry("case-layout-v1", {}, render)
        first = self.publish_and_close(
            "Tool", ((Path("Bin/generated.txt"), generated),), context={}
        )
        rendered_at.clear()

        second = self.remember(
            self.cache.materialize(
                "tool", ((Path("bin/GENERATED.txt"), generated),), context={}
            )
        )

        self.assert_warm_zero(second)
        self.assertEqual(second.key, first.key)
        self.assertEqual(second.layout, first.layout)
        self.assertEqual(second.layout.name, "Tool")
        self.assertEqual(rendered_at, [first.layout])
        self.assertEqual(
            (second.layout / "Bin/generated.txt").read_bytes(),
            first.layout.as_posix().encode("utf-8"),
        )

    def test_warm_hit_holds_object_before_manifest_read_or_generated_render(self) -> None:
        rendered_at: list[Path] = []

        def render(final_layout: Path) -> bytes:
            rendered_at.append(final_layout)
            return b"generated"

        generated = GeneratedLayoutEntry("held-hit-v1", {}, render)
        entries = ((Path("generated.txt"), generated),)
        published = self.publish_and_close("held-hit", entries)
        rendered_at.clear()

        real_object = self.cache.quarantine / "held-hit-real"
        self.force_mutable(published.object_root)
        runtime_cache._set_path_acl(self.cache.objects, self.sid, "BUILD")
        published.object_root.rename(real_object)
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(published.object_root), str(real_object)],
            check=True,
            capture_output=True,
        )
        try:
            with mock.patch.object(
                runtime_cache,
                "_read_manifest",
                wraps=runtime_cache._read_manifest,
            ) as read_manifest:
                with self.assertRaisesRegex(RuntimeError, "reparse"):
                    self.cache.materialize("held-hit", entries, context={})
            self.assertNotIn(
                published.object_root,
                [call.args[0] for call in read_manifest.call_args_list],
            )
            self.assertEqual(rendered_at, [])
        finally:
            if os.path.lexists(published.object_root):
                os.rmdir(published.object_root)

    def test_race_loser_holds_winner_before_manifest_read_or_generated_render(self) -> None:
        rendered_at: list[Path] = []

        def render(final_layout: Path) -> bytes:
            rendered_at.append(final_layout)
            return b"generated"

        generated = GeneratedLayoutEntry("held-race-v1", {}, render)
        entries = ((Path("generated.txt"), generated),)
        published = self.publish_and_close("held-race", entries)
        rendered_at.clear()

        real_object = self.cache.quarantine / "held-race-real"
        self.force_mutable(published.object_root)
        runtime_cache._set_path_acl(self.cache.objects, self.sid, "BUILD")
        published.object_root.rename(real_object)

        def publish_alias_then_lose(
            _source: Path, destination: Path, *, sealed=None
        ) -> None:
            self.assertIsNotNone(sealed)
            subprocess.run(
                ["cmd", "/c", "mklink", "/J", str(destination), str(real_object)],
                check=True,
                capture_output=True,
            )
            raise FileExistsError("simulated concurrent winner")

        try:
            with mock.patch.object(
                runtime_cache,
                "_atomic_move_no_replace",
                side_effect=publish_alias_then_lose,
            ), mock.patch.object(
                runtime_cache,
                "_read_manifest",
                wraps=runtime_cache._read_manifest,
            ) as read_manifest:
                with self.assertRaisesRegex(RuntimeError, "reparse"):
                    self.cache.materialize("held-race", entries, context={})
            self.assertNotIn(
                published.object_root,
                [call.args[0] for call in read_manifest.call_args_list],
            )
            # One render is required to build the losing staging tree. The
            # winner alias must be rejected before any second render.
            self.assertEqual(len(rendered_at), 1)
        finally:
            if os.path.lexists(published.object_root):
                os.rmdir(published.object_root)

    def test_manifest_is_written_last_and_only_complete_tree_is_published(self) -> None:
        observed = {"render_before_manifest": False, "sealed_complete": False}

        def render(final_layout: Path) -> bytes:
            if not observed["render_before_manifest"]:
                staging_roots = tuple(self.cache.staging.iterdir())
                self.assertEqual(staging_roots, ())
            self.assertEqual(final_layout.parent, self.cache.objects / expected_key)
            observed["render_before_manifest"] = True
            return final_layout.as_posix().encode("utf-8")

        source = self.held_file("payload.bin", b"payload")
        generated = GeneratedLayoutEntry("generated-v1", {}, render)
        entries = ((Path("payload.bin"), source), (Path("generated.txt"), generated))
        expected_key = layout_key("complete", entries, {}, ())[0]
        real_seal = runtime_cache._seal_tree_acl

        def inspect_seal(path: Path, sid: str, mode: str = "RX"):
            self.assertTrue((path / "manifest.json").is_file())
            self.assertEqual((path / "complete/payload.bin").read_bytes(), b"payload")
            self.assertTrue((path / "complete/generated.txt").is_file())
            observed["sealed_complete"] = True
            return real_seal(path, sid, mode)

        with mock.patch.object(runtime_cache, "_seal_tree_acl", side_effect=inspect_seal):
            lease = self.remember(self.cache.materialize("complete", entries, context={}))

        self.assertEqual(observed, {"render_before_manifest": True, "sealed_complete": True})
        self.assertFalse((self.cache.staging / expected_key).exists())
        self.assertTrue((self.cache.objects / expected_key / "manifest.json").is_file())
        manifest = json.loads((lease.object_root / "manifest.json").read_text("utf-8"))
        self.assertEqual(manifest["object_key"], lease.key)
        self.assertEqual(manifest["key_input"], layout_key("complete", entries, {}, ())[1])
        self.assertEqual({item["kind"] for item in manifest["files"]}, {"held-source", "generated"})

    def test_generated_output_is_rendered_once_and_retained_for_publication(self) -> None:
        rendered_at: list[Path] = []

        def render(final_layout: Path) -> bytes:
            rendered_at.append(final_layout)
            return b"generated once"

        generated = GeneratedLayoutEntry("generated-v1", {}, render)
        lease = self.remember(
            self.cache.materialize(
                "render-once",
                ((Path("generated.bin"), generated),),
                context={},
            )
        )
        self.assertEqual(rendered_at, [lease.layout])
        self.assertEqual(
            (lease.layout / "generated.bin").read_bytes(),
            b"generated once",
        )

    def test_invalid_input_source_mismatch_and_limits_leave_cache_unmodified(self) -> None:
        source = self.held_file("value.bin", b"value")
        cases = (
            lambda: self.cache.materialize("bad/name", ((Path("value"), source),), context={}),
            lambda: self.cache.materialize("safe", ((Path("../value"), source),), context={}),
            lambda: self.cache.materialize("safe", ((Path("value"), replace(source, inode=source.inode + 1)),), context={}),
            lambda: self.cache.materialize("safe", ((Path("value"), source),), context={}, entry_limit=0),
            lambda: self.cache.materialize("safe", ((Path("value"), source),), context={}, byte_limit=4),
        )
        for invoke in cases:
            with self.subTest(invoke=invoke):
                with self.assertRaises(RuntimeError):
                    invoke()
                self.assertEqual(tuple(self.cache.objects.iterdir()), ())
                self.assertEqual(tuple(self.cache.staging.iterdir()), ())

    def test_entry_limit_bounds_iteration_before_key_sort_or_encoding(self) -> None:
        yielded = 0

        def oversized_entries():
            nonlocal yielded
            for index in range(4):
                yielded += 1
                yield Path(f"value-{index}.bin"), b"x"
            raise AssertionError("entry iterator consumed past the configured bound")

        with mock.patch.object(
            runtime_cache, "layout_key", wraps=runtime_cache.layout_key
        ) as key_builder, mock.patch.object(
            runtime_cache, "canonical_json", wraps=runtime_cache.canonical_json
        ) as encoder:
            with self.assertRaisesRegex(RuntimeError, "entry limit exceeded"):
                self.cache.materialize(
                    "bounded-count",
                    oversized_entries(),
                    context={},
                    entry_limit=3,
                )

        self.assertEqual(yielded, 4)
        key_builder.assert_not_called()
        encoder.assert_not_called()

    def test_cheap_entry_validation_and_byte_limits_precede_key_work(self) -> None:
        cases = (
            ("destination", (("not-a-path", b"x"),), 1024, "destination"),
            ("entry-type", ((Path("value.bin"), object()),), 1024, "entry"),
            ("literal-bytes", ((Path("value.bin"), b"oversized"),), 4, "byte limit"),
            (
                "held-bytes",
                ((Path("value.bin"), self.held_file("oversized.bin", b"oversized")),),
                4,
                "byte limit",
            ),
        )
        for name, entries, byte_limit, message in cases:
            with self.subTest(name=name), mock.patch.object(
                runtime_cache, "layout_key", wraps=runtime_cache.layout_key
            ) as key_builder, mock.patch.object(
                runtime_cache, "canonical_json", wraps=runtime_cache.canonical_json
            ) as encoder:
                with self.assertRaisesRegex((RuntimeError, TypeError), message):
                    self.cache.materialize(
                        "bounded-entry",
                        entries,
                        context={},
                        byte_limit=byte_limit,
                    )
                key_builder.assert_not_called()
                encoder.assert_not_called()

    def test_source_is_revalidated_and_copied_from_the_resolved_held_stream(self) -> None:
        source = self.held_file("held.bin", b"before")
        source.stream.close()
        (self.sources / "held.bin").write_bytes(b"after")
        with self.assertRaisesRegex(RuntimeError, "held source"):
            self.cache.materialize("held", ((Path("held.bin"), source),), context={})
        self.assertEqual(tuple(self.cache.objects.iterdir()), ())
        self.assertEqual(tuple(self.cache.staging.iterdir()), ())

    def test_source_validation_never_reopens_the_held_path(self) -> None:
        source = self.held_file("held-direct.bin", b"held bytes")
        resolver = self.cache.source_resolver
        with mock.patch.object(
            self.cache,
            "source_resolver",
            wraps=resolver,
        ) as reopened:
            lease = self.remember(
                self.cache.materialize(
                    "held-direct",
                    ((Path("held.bin"), source),),
                    context={},
                )
            )
        reopened.assert_not_called()
        self.assertEqual((lease.layout / "held.bin").read_bytes(), b"held bytes")

    def test_closed_or_mismatched_held_stream_is_rejected_without_cache_mutation(self) -> None:
        closed = self.held_file("closed.bin", b"closed")
        closed.stream.close()
        other = self.held_file("other.bin", b"closed")
        mismatched = replace(closed, stream=other.stream)
        for name, descriptor in (("closed", closed), ("mismatched", mismatched)):
            with self.subTest(name=name):
                with self.assertRaisesRegex(RuntimeError, "held source"):
                    self.cache.materialize(
                        name,
                        ((Path("value.bin"), descriptor),),
                        context={},
                    )
                self.assertEqual(tuple(self.cache.objects.iterdir()), ())
                self.assertEqual(tuple(self.cache.staging.iterdir()), ())

    def test_generated_validation_and_limits_precede_staging_creation(self) -> None:
        cases = (
            (
                "invalid-type",
                GeneratedLayoutEntry("generated-v1", {}, lambda _root: "not bytes"),
                1024,
                "did not render bytes",
            ),
            (
                "oversized",
                GeneratedLayoutEntry("generated-v1", {}, lambda _root: b"too large"),
                4,
                "byte limit exceeded",
            ),
        )
        for name, generated, byte_limit, message in cases:
            with self.subTest(name=name):
                with mock.patch.object(
                    runtime_cache,
                    "_create_directory_with_acl",
                    wraps=runtime_cache._create_directory_with_acl,
                ) as create_staging:
                    with self.assertRaisesRegex(RuntimeError, message):
                        self.cache.materialize(
                            name,
                            ((Path("generated.bin"), generated),),
                            context={},
                            byte_limit=byte_limit,
                        )
                create_staging.assert_not_called()
                self.assertEqual(tuple(self.cache.objects.iterdir()), ())
                self.assertEqual(tuple(self.cache.staging.iterdir()), ())

    def test_source_identity_and_content_changes_publish_distinct_misses(self) -> None:
        first_source = self.held_file("one.bin", b"same")
        first = self.publish_and_close("source", ((Path("value.bin"), first_source),))
        identity_source = self.held_file("two.bin", b"same")
        identity = self.remember(
            self.cache.materialize("source", ((Path("value.bin"), identity_source),), context={})
        )
        self.assertFalse(identity.cache_hit)
        self.assertNotEqual(identity.key, first.key)
        self.close(identity)
        content_source = self.held_file("three.bin", b"changed")
        content = self.remember(
            self.cache.materialize("source", ((Path("value.bin"), content_source),), context={})
        )
        self.assertFalse(content.cache_hit)
        self.assertNotIn(content.key, (first.key, identity.key))
        self.assertEqual(len(tuple(self.cache.objects.iterdir())), 3)

    def test_hit_repairs_extra_and_missing_files(self) -> None:
        entries = ((Path("value.bin"), self.held_file("value.bin", b"value")),)
        for mutation in ("extra", "missing"):
            with self.subTest(mutation=mutation):
                cache = self.make_cache()
                lease = cache.materialize("exact-" + mutation, entries, context={})
                lease.close()
                self.force_mutable(lease.object_root)
                if mutation == "extra":
                    (lease.layout / "extra.bin").write_bytes(b"extra")
                else:
                    (lease.layout / "value.bin").unlink()
                repaired = self.remember(
                    cache.materialize("exact-" + mutation, entries, context={})
                )
                self.assertFalse(repaired.cache_hit)
                self.assertEqual((repaired.layout / "value.bin").read_bytes(), b"value")
                self.assertFalse((repaired.layout / "extra.bin").exists())

    def test_hit_repairs_payload_and_manifest_tampering(self) -> None:
        entries = ((Path("value.bin"), self.held_file("value.bin", b"value")),)
        for mutation in ("payload", "manifest"):
            with self.subTest(mutation=mutation):
                cache = self.make_cache()
                lease = cache.materialize("tamper-" + mutation, entries, context={})
                lease.close()
                self.force_mutable(lease.object_root)
                if mutation == "payload":
                    (lease.layout / "value.bin").write_bytes(b"other")
                else:
                    (lease.object_root / "manifest.json").write_text("{}", encoding="utf-8")
                repaired = self.remember(
                    cache.materialize("tamper-" + mutation, entries, context={})
                )
                self.assertFalse(repaired.cache_hit)
                self.assertEqual((repaired.layout / "value.bin").read_bytes(), b"value")

    def test_hit_repairs_coordinated_payload_and_manifest_tampering(self) -> None:
        held_entry = self.held_file("coordinated-held.bin", b"original")
        cases: tuple[tuple[str, LayoutEntry], ...] = (
            ("held", held_entry),
            ("literal", b"original"),
        )
        for name, entry in cases:
            with self.subTest(name=name):
                cache = self.make_cache()
                entries = ((Path("value.bin"), entry),)
                lease = cache.materialize("coordinated-" + name, entries, context={})
                lease.close()
                self.force_mutable(lease.object_root)
                replacement = b"rewritten payload"
                (lease.layout / "value.bin").write_bytes(replacement)
                manifest_path = lease.object_root / "manifest.json"
                manifest = json.loads(manifest_path.read_text("utf-8"))
                manifest["files"][0]["size"] = len(replacement)
                manifest["files"][0]["sha256"] = hashlib.sha256(
                    replacement
                ).hexdigest()
                manifest_path.write_bytes(canonical_json(manifest))
                _set_tree_acl(lease.object_root, self.sid, "RX")
                repaired = self.remember(
                    cache.materialize(
                        "coordinated-" + name,
                        entries,
                        context={},
                    )
                )
                self.assertFalse(repaired.cache_hit)
                self.assertEqual(
                    (repaired.layout / "value.bin").read_bytes(), b"original"
                )

    def test_hit_repairs_file_alias_replacement_with_the_same_bytes(self) -> None:
        source = self.held_file("alias.bin", b"same")
        entries = ((Path("one.bin"), source), (Path("two.bin"), source))
        lease = self.publish_and_close("alias", entries)
        self.force_mutable(lease.object_root)
        one = lease.layout / "one.bin"
        two = lease.layout / "two.bin"
        two.unlink()
        os.link(one, two)
        repaired = self.remember(self.cache.materialize("alias", entries, context={}))
        self.assertFalse(repaired.cache_hit)
        self.assertEqual(
            os.stat(repaired.layout / "one.bin").st_ino
            == os.stat(repaired.layout / "two.bin").st_ino,
            False,
        )

    def test_hit_repairs_changed_generated_bytes(self) -> None:
        rendered = [b"one"]
        generated = GeneratedLayoutEntry("generated-v1", {"stable": True}, lambda _root: rendered[0])
        entries = ((Path("value.txt"), generated),)
        self.publish_and_close("generated", entries)
        rendered[0] = b"two"
        repaired = self.remember(
            self.cache.materialize("generated", entries, context={})
        )
        self.assertFalse(repaired.cache_hit)
        self.assertEqual((repaired.layout / "value.txt").read_bytes(), b"two")

    def test_hit_repairs_loosened_file_and_directory_acl(self) -> None:
        entries = ((Path("nested/value.bin"), self.held_file("acl.bin", b"value")),)
        for target_kind in ("file", "directory"):
            with self.subTest(target_kind=target_kind):
                cache = self.make_cache()
                lease = cache.materialize("acl-" + target_kind, entries, context={})
                lease.close()
                target = lease.layout / ("nested/value.bin" if target_kind == "file" else "nested")
                runtime_cache._set_path_acl(target, self.sid, "BUILD")
                repaired = self.remember(
                    cache.materialize("acl-" + target_kind, entries, context={})
                )
                self.assertFalse(repaired.cache_hit)
                repaired_target = repaired.layout / (
                    "nested/value.bin" if target_kind == "file" else "nested"
                )
                _assert_mutation_denied(
                    repaired_target,
                    directory=target_kind == "directory",
                )

    def test_hit_rejects_directory_acl_permitting_only_file_insertion(self) -> None:
        entries = ((Path("nested/value.bin"), self.held_file("add-file.bin", b"value")),)
        lease = self.publish_and_close("add-file", entries)
        nested = lease.layout / "nested"
        owner = _hold_path_and_set_acl_sddl(
            nested,
            f"D:P(A;;GRGX;;;{self.sid})(A;;0x{runtime_cache._FILE_ADD_FILE:x};;;{self.sid})",
        )
        try:
            with self.assertRaisesRegex(RuntimeError, "active"):
                self.cache.materialize("add-file", entries, context={})
        finally:
            runtime_cache._set_handle_acl(owner.value, self.sid, "RX")
            owner.close()
        repaired = self.remember(
            self.cache.materialize("add-file", entries, context={})
        )
        self.assertTrue(repaired.cache_hit)

    def test_hit_rejects_object_retargeted_between_validation_and_hold(self) -> None:
        entries = ((Path("value.bin"), self.held_file("retarget.bin", b"value")),)
        published = self.publish_and_close("retarget", entries)
        replacement = self.cache.staging / "replacement"
        self.force_mutable(published.object_root)
        shutil.copytree(published.object_root, replacement)
        _set_tree_acl(published.object_root, self.sid, "RX")
        _set_tree_acl(replacement, self.sid, "RX")
        displaced = self.cache.quarantine / "displaced"
        real_validate = runtime_cache._validate_object_contents
        retargeted = False

        def retarget_after_validation(*args, **kwargs):
            nonlocal retargeted
            result = real_validate(*args, **kwargs)
            if kwargs.get("require_acl") and not retargeted:
                retargeted = True
                runtime_cache._set_path_acl(self.cache.objects, self.sid, "BUILD")
                runtime_cache._set_path_acl(published.object_root, self.sid, "DELETE")
                published.object_root.rename(displaced)
                runtime_cache._set_path_acl(replacement, self.sid, "DELETE")
                replacement.rename(published.object_root)
                _set_tree_acl(published.object_root, self.sid, "RX")
            return result

        with mock.patch.object(
            runtime_cache,
            "_validate_object_contents",
            side_effect=retarget_after_validation,
        ):
            with self.assertRaisesRegex(RuntimeError, "changed|identity|moved"):
                self.cache.materialize("retarget", entries, context={})
        self.assertTrue(retargeted)

    def test_interrupted_staging_is_ignored_and_never_treated_as_an_object(self) -> None:
        interrupted = self.cache.staging / "interrupted"
        interrupted.mkdir()
        (interrupted / "manifest.json").write_text("{}", encoding="utf-8")
        entries = ((Path("value.bin"), self.held_file("interrupted.bin", b"value")),)
        cold = self.publish_and_close("interrupted", entries)
        self.assertFalse(cold.cache_hit)
        self.assertTrue(interrupted.is_dir())
        warm = self.remember(self.cache.materialize("interrupted", entries, context={}))
        self.assert_warm_zero(warm)
        self.assertNotEqual(interrupted, warm.object_root)

    def test_concurrent_creators_publish_one_complete_object(self) -> None:
        source = self.sources / "race.bin"
        source.write_bytes(b"concurrent publication")
        start = self.root / "start"
        ready = (self.root / "worker-one", self.root / "worker-two")
        processes = []
        for index in range(2):
            code = (
                "from tests.scripts.test_windows_runtime_cache import _publication_worker; "
                f"_publication_worker({str(self.cache_root)!r}, {str(source)!r}, "
                f"{str(ready[index])!r}, {str(ready[1 - index])!r}, {str(start)!r})"
            )
            processes.append(
                subprocess.Popen(
                    [sys.executable, "-c", code],
                    cwd=str(Path(__file__).parents[2]),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
            )
        deadline = time.monotonic() + 30
        while not all(path.exists() for path in ready):
            self.assertLess(time.monotonic(), deadline, "workers did not reach start barrier")
            time.sleep(0.01)
        start.write_text("start", encoding="utf-8")
        results = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 0, stderr)
            results.append(json.loads(stdout.strip()))
        self.assertEqual(results[0]["key"], results[1]["key"])
        self.assertEqual(sorted(result["cache_hit"] for result in results), [False, True])
        self.assertEqual(
            [(result["payload_files"], result["payload_bytes"]) for result in results],
            [(1, len(b"concurrent publication"))] * 2,
        )
        objects = tuple(self.cache.objects.iterdir())
        self.assertEqual(len(objects), 1)
        self.assertEqual(tuple(path for path in self.cache.staging.iterdir() if path.name != "interrupted"), ())
        descriptor = self.held_file("race-parent.bin", b"concurrent publication")
        # The worker source identity is part of the key, so validate the winner
        # using the same source path rather than the parent helper's new file.
        descriptor.stream.close()
        actual = _resolved_source(source)
        self.descriptors.append(actual)
        warm = self.remember(
            self.cache.materialize(
                "race",
                ((Path("bin/tool.exe"), actual),),
                context={"purpose": "concurrent-test"},
            )
        )
        self.assert_warm_zero(warm)

    def test_concurrent_case_only_generated_creators_use_winner_spelling(self) -> None:
        start = self.root / "case-start"
        ready = (self.root / "case-worker-one", self.root / "case-worker-two")
        processes = []
        for index, layout_name in enumerate(("Tool", "tool")):
            code = (
                "from tests.scripts.test_windows_runtime_cache import "
                "_generated_case_publication_worker; "
                f"_generated_case_publication_worker({str(self.cache_root)!r}, "
                f"{layout_name!r}, {str(ready[index])!r}, "
                f"{str(ready[1 - index])!r}, {str(start)!r})"
            )
            processes.append(
                subprocess.Popen(
                    [sys.executable, "-c", code],
                    cwd=str(Path(__file__).parents[2]),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
            )
        deadline = time.monotonic() + 30
        while not all(path.exists() for path in ready):
            self.assertLess(time.monotonic(), deadline, "workers did not reach start barrier")
            time.sleep(0.01)
        start.write_text("start", encoding="utf-8")
        results = []
        for process in processes:
            stdout, stderr = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 0, stderr)
            results.append(json.loads(stdout.strip()))

        self.assertEqual(results[0]["key"], results[1]["key"])
        self.assertEqual(sorted(result["cache_hit"] for result in results), [False, True])
        self.assertEqual(results[0]["layout"], results[1]["layout"])
        self.assertEqual([result["payload_files"] for result in results], [1, 1])
        self.assertTrue(all(result["payload_bytes"] > 0 for result in results))


@unittest.skipUnless(os.name == "nt", "requires Windows lifecycle semantics")
class LifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.cache_root = self.root / "cache"
        self.sources = self.root / "sources"
        self.sources.mkdir()
        self.sid = _current_user_sid()
        self.now = 1_700_000_000
        self.caches: list[WindowsRuntimeCache] = []
        self.leases: list[CachedLayoutLease] = []
        self.descriptors: list[HeldSource] = []
        self.source_index = 0
        self.cache = self.make_cache()

    def tearDown(self) -> None:
        for lease in reversed(self.leases):
            lease.close()
        for descriptor in reversed(self.descriptors):
            descriptor.stream.close()
        for cache in self.caches:
            for owner in cache._management_handles:
                owner.close()
        if self.root.exists():
            _set_tree_acl(self.root, self.sid, "DELETE")
            shutil.rmtree(self.root, ignore_errors=True)
        self.temporary.cleanup()

    def make_cache(
        self, *, limits: CacheLimits = CacheLimits()
    ) -> WindowsRuntimeCache:
        cache = WindowsRuntimeCache(
            self.cache_root,
            self.sid,
            _resolved_source,
            lambda path: runtime_cache._open_canonical_non_reparse(
                path, path.parent, directory=True
            ),
            limits=limits,
            clock=lambda: self.now,
        )
        self.caches.append(cache)
        return cache

    def held_file(self, name: str, payload: bytes) -> HeldSource:
        self.source_index += 1
        path = self.sources / f"{self.source_index}-{name}"
        path.write_bytes(payload)
        descriptor = _resolved_source(path)
        self.descriptors.append(descriptor)
        return descriptor

    def remember(self, lease: CachedLayoutLease) -> CachedLayoutLease:
        self.leases.append(lease)
        return lease

    def close(self, lease: CachedLayoutLease) -> None:
        lease.close()
        self.leases.remove(lease)

    def publish_and_close(
        self,
        name: str,
        payload: bytes,
        *,
        cache: WindowsRuntimeCache | None = None,
    ) -> CachedLayoutLease:
        selected = self.cache if cache is None else cache
        lease = selected.materialize(
            name,
            ((Path("value.bin"), self.held_file(name, payload)),),
            context={},
        )
        lease.close()
        return lease

    def corrupt_manifest(self, object_root: Path) -> None:
        _set_tree_acl(object_root, self.sid, "DELETE")
        manifest = object_root / "manifest.json"
        decoded = json.loads(manifest.read_text(encoding="utf-8"))
        decoded["object_key"] = "0" * 64
        manifest.write_bytes(canonical_json(decoded))
        _set_tree_acl(object_root, self.sid, "RX")

    def backdate(self, path: Path, seconds: int) -> None:
        timestamp = self.now - seconds
        os.utime(path, (timestamp, timestamp))

    def test_access_stamp_is_written_at_most_once_per_hour(self) -> None:
        source = self.held_file("stamp", b"stamp")
        entries = ((Path("value.bin"), source),)
        first = self.remember(self.cache.materialize("stamp", entries, context={}))
        stamp = self.cache.access / f"{first.key}.stamp"
        self.assertTrue(stamp.is_file())
        first_value = stamp.read_bytes()
        first_mtime = stamp.stat().st_mtime_ns
        self.close(first)

        self.now += 3599
        second = self.remember(self.cache.materialize("stamp", entries, context={}))
        self.assertEqual(stamp.read_bytes(), first_value)
        self.assertEqual(stamp.stat().st_mtime_ns, first_mtime)
        self.close(second)

        self.now += 1
        third = self.remember(self.cache.materialize("stamp", entries, context={}))
        self.assertNotEqual(stamp.read_bytes(), first_value)
        self.assertGreater(stamp.stat().st_mtime_ns, first_mtime)
        self.close(third)

    def test_stale_access_lock_is_recovered_without_losing_future_touches(self) -> None:
        source = self.held_file("access-lock", b"stamp")
        entries = ((Path("value.bin"), source),)
        first = self.cache.materialize("access-lock", entries, context={})
        key = first.key
        first.close()
        stamp = self.cache.access / f"{key}.stamp"
        stamp.unlink()
        lock = self.cache.locks / f"access-{key}"
        runtime_cache._create_directory_with_acl(
            lock, self.sid, runtime_cache._acl_sddl(self.sid, "PRIVATE")
        )
        self.backdate(lock, self.cache.limits.stamp_seconds)

        warm = self.remember(
            self.cache.materialize("access-lock", entries, context={})
        )

        self.assertTrue(warm.cache_hit)
        self.assertTrue(stamp.is_file())
        self.assertFalse(lock.exists())

    def test_wall_clock_rollback_does_not_suppress_access_forever(self) -> None:
        source = self.held_file("clock-rollback", b"stamp")
        entries = ((Path("value.bin"), source),)
        first = self.cache.materialize("clock-rollback", entries, context={})
        stamp = self.cache.access / f"{first.key}.stamp"
        before = stamp.read_bytes()
        first.close()
        self.now -= 1

        warm = self.remember(
            self.cache.materialize("clock-rollback", entries, context={})
        )

        self.assertTrue(warm.cache_hit)
        self.assertNotEqual(stamp.read_bytes(), before)

    def test_cleanup_is_throttled_for_one_day_without_enumerating_objects(self) -> None:
        stale = self.cache.staging / "stale-first"
        stale.mkdir()
        self.backdate(stale, self.cache.limits.staging_seconds + 1)
        self.cache.cleanup()
        self.assertFalse(stale.exists())

        throttled = self.cache.staging / "stale-second"
        throttled.mkdir()
        self.backdate(throttled, self.cache.limits.staging_seconds + 1)
        with mock.patch.object(
            runtime_cache,
            "_cleanup_cache_contents",
            wraps=runtime_cache._cleanup_cache_contents,
        ) as cleanup_contents:
            self.cache.cleanup()
        cleanup_contents.assert_not_called()
        self.assertTrue(throttled.exists())

        self.now += self.cache.limits.cleanup_seconds
        self.cache.cleanup()
        self.assertFalse(throttled.exists())

    def test_stale_cleanup_lock_is_recovered(self) -> None:
        stale = self.cache.staging / "cleanup-stale"
        stale.mkdir()
        self.backdate(stale, self.cache.limits.staging_seconds + 1)
        lock = self.cache.locks / "cleanup"
        runtime_cache._create_directory_with_acl(
            lock, self.sid, runtime_cache._acl_sddl(self.sid, "PRIVATE")
        )
        self.backdate(lock, self.cache.limits.cleanup_seconds)

        self.cache.cleanup(force=True)

        self.assertFalse(stale.exists())
        self.assertFalse(lock.exists())

    def test_cleanup_removes_old_staging_and_quarantine_entries(self) -> None:
        for parent, name in (
            (self.cache.staging, "old-stage"),
            (self.cache.quarantine, "old-quarantine"),
        ):
            candidate = parent / name
            candidate.mkdir()
            (candidate / "payload").write_bytes(b"x")
            self.backdate(candidate, self.cache.limits.staging_seconds + 1)

        self.cache.cleanup(force=True)

        self.assertEqual(tuple(self.cache.staging.iterdir()), ())
        self.assertEqual(tuple(self.cache.quarantine.iterdir()), ())

    def test_cleanup_removes_objects_unused_for_fourteen_days(self) -> None:
        lease = self.publish_and_close("unused", b"old")
        stamp = self.cache.access / f"{lease.key}.stamp"
        self.backdate(stamp, self.cache.limits.max_unused_seconds + 1)

        self.cache.cleanup(force=True)

        self.assertFalse(lease.object_root.exists())
        self.assertFalse(stamp.exists())

    def test_cleanup_uses_deterministic_lru_for_byte_and_object_bounds(self) -> None:
        cache = self.make_cache(limits=CacheLimits(max_bytes=8, max_objects=2))
        first = self.publish_and_close("one", b"1111", cache=cache)
        self.now += 1
        second = self.publish_and_close("two", b"2222", cache=cache)
        self.now += 1
        third = self.publish_and_close("three", b"3333", cache=cache)

        self.assertFalse(first.object_root.exists())
        self.assertTrue(second.object_root.exists())
        self.assertTrue(third.object_root.exists())
        self.assertEqual(len(tuple(cache.objects.iterdir())), 2)

    def test_cleanup_skips_active_object_and_publication_fails_at_capacity(self) -> None:
        cache = self.make_cache(limits=CacheLimits(max_bytes=8, max_objects=1))
        active = self.remember(
            cache.materialize(
                "one",
                ((Path("one.bin"), self.held_file("one", b"12345678")),),
                context={},
            )
        )
        second_source = self.held_file("two", b"abcdefgh")
        second_key = layout_key(
            "two", ((Path("two.bin"), second_source),), {}, ()
        )[0]

        with self.assertRaisesRegex(RuntimeError, "cache capacity"):
            cache.materialize(
                "two", ((Path("two.bin"), second_source),), context={}
            )

        self.assertTrue(active.layout.is_dir())
        self.assertFalse((cache.objects / second_key).exists())

    def test_cleanup_skips_active_unused_object(self) -> None:
        active = self.remember(
            self.cache.materialize(
                "active",
                ((Path("value.bin"), self.held_file("active", b"active")),),
                context={},
            )
        )
        stamp = self.cache.access / f"{active.key}.stamp"
        self.backdate(stamp, self.cache.limits.max_unused_seconds + 1)

        self.cache.cleanup(force=True)

        self.assertTrue(active.object_root.exists())
        self.assertTrue(stamp.exists())

    def test_repair_lock_times_out_after_ten_monotonic_seconds(self) -> None:
        source = self.held_file("timeout", b"value")
        entries = ((Path("value.bin"), source),)
        published = self.cache.materialize("timeout", entries, context={})
        published.close()
        self.corrupt_manifest(published.object_root)
        key = layout_key("timeout", entries, {}, ())[0]
        lock = self.cache.locks / f"repair-{key}"
        runtime_cache._create_directory_with_acl(
            lock, self.sid, runtime_cache._acl_sddl(self.sid, "PRIVATE")
        )
        os.utime(lock, (self.now, self.now))
        ticks = iter((0.0, 9.99, 10.0, 10.0))

        with mock.patch.object(time, "monotonic", side_effect=lambda: next(ticks)), mock.patch.object(
            time, "sleep", return_value=None
        ):
            with self.assertRaisesRegex(RuntimeError, "repair lock timeout"):
                self.cache.materialize("timeout", entries, context={})

    def test_stale_repair_lock_is_recovered_after_ten_minutes(self) -> None:
        source = self.held_file("stale", b"value")
        entries = ((Path("value.bin"), source),)
        published = self.cache.materialize("stale", entries, context={})
        published.close()
        self.corrupt_manifest(published.object_root)
        key = layout_key("stale", entries, {}, ())[0]
        lock = self.cache.locks / f"repair-{key}"
        runtime_cache._create_directory_with_acl(
            lock, self.sid, runtime_cache._acl_sddl(self.sid, "PRIVATE")
        )
        self.backdate(lock, 600)

        lease = self.remember(self.cache.materialize("stale", entries, context={}))

        self.assertFalse(lock.exists())
        self.assertEqual((lease.layout / "value.bin").read_bytes(), b"value")

    def test_corrupt_object_is_quarantined_and_rebuilt_under_repair_lock(self) -> None:
        source = self.held_file("repair", b"before")
        entries = ((Path("value.bin"), source),)
        lease = self.cache.materialize("repair", entries, context={})
        object_root = lease.object_root
        lease.close()
        self.corrupt_manifest(object_root)

        rebuilt = self.remember(
            self.cache.materialize("repair", entries, context={})
        )

        self.assertFalse(rebuilt.cache_hit)
        self.assertEqual((rebuilt.layout / "value.bin").read_bytes(), b"before")
        quarantined = tuple(self.cache.quarantine.iterdir())
        self.assertEqual(len(quarantined), 1)
        self.assertTrue(quarantined[0].name.startswith(f"{rebuilt.key}-"))

    def test_repair_starts_quarantine_retention_at_the_move_time(self) -> None:
        source = self.held_file("old-repair", b"before")
        entries = ((Path("value.bin"), source),)
        lease = self.cache.materialize("old-repair", entries, context={})
        object_root = lease.object_root
        lease.close()
        self.now += self.cache.limits.staging_seconds + 1
        self.corrupt_manifest(object_root)
        runtime_cache._set_path_acl(object_root, self.sid, "DELETE")
        try:
            self.backdate(object_root, self.cache.limits.staging_seconds + 1)
        finally:
            runtime_cache._set_path_acl(object_root, self.sid, "RX")

        rebuilt = self.remember(
            self.cache.materialize("old-repair", entries, context={})
        )

        quarantined = tuple(self.cache.quarantine.iterdir())
        self.assertEqual(len(quarantined), 1)
        self.assertGreaterEqual(
            quarantined[0].stat().st_mtime,
            self.now,
        )
        self.assertFalse(rebuilt.cache_hit)

    def test_cleanup_failure_does_not_remove_or_modify_active_object(self) -> None:
        active = self.remember(
            self.cache.materialize(
                "held",
                ((Path("value.bin"), self.held_file("held", b"held")),),
                context={},
            )
        )
        before = (active.object_root / "manifest.json").read_bytes()
        self.backdate(
            self.cache.access / f"{active.key}.stamp",
            self.cache.limits.max_unused_seconds + 1,
        )

        self.cache.cleanup(force=True)

        self.assertEqual((active.object_root / "manifest.json").read_bytes(), before)
        _assert_mutation_denied(active.object_root, directory=True)


if __name__ == "__main__":
    unittest.main()
