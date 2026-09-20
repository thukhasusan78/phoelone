import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "build_default_assets", ROOT / "scripts" / "build_default_assets.py"
)
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildDefaultAssetsTest(unittest.TestCase):
    def test_text_font_metadata_uses_bundle_charset_size_and_bpp(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = Path(directory)
            BUILD.generate_index_json(
                str(assets),
                None,
                "font_noto_sans_common_20_4.bin",
                None,
                font_bundle_id="noto-v1",
            )
            index = json.loads((assets / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(
                index["text_font_meta"],
                {"charset": "common", "size": 20, "bpp": 4, "bundle": "noto-v1"},
            )

    def test_text_font_requires_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                BUILD.generate_index_json(
                    directory, None, "font_noto_sans_common_20_4.bin", None
                )

    def test_split_custom_wake_word_phrases(self):
        self.assertEqual(
            BUILD.split_custom_wake_word_phrases(
                "mickey, hey mickey, hi mickey"
            ),
            ["mickey", "hey mickey", "hi mickey"],
        )
        self.assertEqual(
            BUILD.split_custom_wake_word_phrases("xiao tu dou"),
            ["xiao tu dou"],
        )
        self.assertEqual(BUILD.split_custom_wake_word_phrases(" , , "), [])

    def test_emoji_overlay_replaces_same_name_and_adds_new(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / "base"
            overlay = Path(directory) / "overlay"
            assets = Path(directory) / "assets"
            base.mkdir()
            overlay.mkdir()
            assets.mkdir()
            (base / "happy.gif").write_bytes(b"base-happy")
            (base / "blink.gif").write_bytes(b"base-blink")
            (overlay / "blink.gif").write_bytes(b"overlay-blink")
            (overlay / "focus.gif").write_bytes(b"overlay-focus")

            emoji_list = BUILD.process_emoji_collection(str(base), str(assets))
            merged = BUILD.merge_emoji_overlay(str(overlay), str(assets), emoji_list)
            by_name = {entry["name"]: entry["file"] for entry in merged}

            self.assertEqual(set(by_name), {"happy", "blink", "focus"})
            self.assertEqual((assets / "blink.gif").read_bytes(), b"overlay-blink")
            self.assertEqual((assets / "focus.gif").read_bytes(), b"overlay-focus")
            self.assertEqual((assets / "happy.gif").read_bytes(), b"base-happy")


if __name__ == "__main__":
    unittest.main()
