import hashlib
import io
import unittest
from dataclasses import replace
from pathlib import Path
from types import MappingProxyType
from typing import get_args

from tools.windows_runtime_cache import (
    GeneratedLayoutEntry,
    HeldSource,
    JsonScalar,
    LayoutEntry,
    LayoutEntries,
    canonical_json,
    layout_key,
)


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


if __name__ == "__main__":
    unittest.main()
