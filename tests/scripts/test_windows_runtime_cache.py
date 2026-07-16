import hashlib
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from types import MappingProxyType
from typing import get_args
from unittest import mock

import tools.windows_runtime_cache as runtime_cache
from tools.windows_runtime_cache import (
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
        tree = self.root / "tree"
        tree.mkdir()
        payload = tree / "payload.dll"
        payload.write_bytes(b"payload")

        _set_tree_acl(tree, self.sid, "RX")

        _assert_mutation_denied(tree, directory=True)
        _assert_mutation_denied(payload, directory=False)

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


if __name__ == "__main__":
    unittest.main()
