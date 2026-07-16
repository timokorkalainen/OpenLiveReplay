import hashlib
import io
import unittest
from pathlib import Path
from typing import get_args

from tools.windows_runtime_cache import (
    GeneratedLayoutEntry,
    HeldSource,
    JsonScalar,
    LayoutEntry,
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
        first = {
            Path("bin/tool.exe"): held("tool.exe", b"tool"),
            Path("literal"): b"x",
        }
        second = dict(reversed(tuple(first.items())))
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
            {
                Path("bin/tool.exe"): held("tool.exe", b"tool", inode=2),
                Path("literal"): b"x",
            },
            {
                Path("bin/tool.exe"): held("tool.exe", b"changed"),
                Path("literal"): b"x",
            },
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

    def test_layout_key_rejects_unsafe_and_case_colliding_destinations(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unsafe destination"):
            layout_key(
                "tool", {Path("../tool.exe"): held("tool.exe", b"x")}, {}, ()
            )
        with self.assertRaisesRegex(RuntimeError, "duplicate destination"):
            layout_key(
                "tool",
                {
                    Path("Bin/stra\u00dfe.exe"): held("one", b"1"),
                    Path("bin/STRASSE.exe"): held("two", b"2"),
                },
                {},
                (),
            )

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
            layout_key("app", {Path("qt.conf"): one}, {}, ())[0],
            layout_key("app", {Path("qt.conf"): two}, {}, ())[0],
        )
        changed = GeneratedLayoutEntry(
            "qt-conf-v2",
            {"plugins": "p", "qml": "q"},
            lambda _root: b"one",
        )
        self.assertNotEqual(
            layout_key("app", {Path("qt.conf"): changed}, {}, ())[0],
            layout_key("app", {Path("qt.conf"): one}, {}, ())[0],
        )

    def test_public_type_aliases_import_and_contain_declared_types(self) -> None:
        self.assertIn(type(None), get_args(JsonScalar))
        self.assertIn(HeldSource, get_args(LayoutEntry))
        self.assertIn(GeneratedLayoutEntry, get_args(LayoutEntry))


if __name__ == "__main__":
    unittest.main()
