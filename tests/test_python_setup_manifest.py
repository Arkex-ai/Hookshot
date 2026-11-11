"""Validate the Python Builder manifest before any install step."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder/py"))
from hookshot_builder import setup_parts

TAG = "v2.1.0-rc.1"
DIGEST = "a" * 64
URL = "https://example.com/part.zip"
PART = f"part {DIGEST} 120 . - - {URL}"
MANIFEST = f"tag {TAG}\n{PART}\n"


class SetupManifestTests(unittest.TestCase):
    def test_release_generator_output(self):
        with tempfile.TemporaryDirectory(prefix="setup-manifest-") as temp:
            root = Path(temp)
            package, output = root / "package", root / "release"
            entries = ("HookshotBuilder.exe", "runtime.dll", "tools/nx2/make_game.py",
                       *(f"tools/{name}/sample" for name in setup_parts.OWN))
            for name in entries:
                file = package / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(name, encoding="utf-8")
            subprocess.run([sys.executable, str(ROOT / "scripts/make_setup_parts.py"),
                            str(package), TAG, str(output)], check=True, capture_output=True)
            manifest = (output / "hookshot-setup.txt").read_bytes()
            with patch.object(setup_parts.Http, "get", return_value=manifest):
                tag, parts = setup_parts.read_manifest(lambda _: None, tag=TAG)
        self.assertEqual(tag, TAG)
        self.assertEqual(len(parts), 8)
        self.assertTrue(all(part.dest == "." for part in parts[:-1]))
        self.assertEqual(parts[-1].dest, "HookshotBuilder/tools")
        self.assertEqual(parts[-1].to, "llvm-mingw")

    def test_reject_invalid_metadata_before_install_paths(self):
        cases = {
            "empty tag": MANIFEST.replace(f"tag {TAG}", "tag"),
            "repeated tag": MANIFEST + f"tag {TAG}\n",
            "bad digest": MANIFEST.replace(DIGEST, "wrong-digest"),
            "noninteger size": MANIFEST.replace(" 120 ", " none "),
            "zero size": MANIFEST.replace(" 120 ", " 0 "),
            "empty destination component": MANIFEST.replace(" . - - ", " tools//python - - "),
            "incomplete rename": MANIFEST.replace(" . - - ", " . old - "),
            "non-HTTPS URL": MANIFEST.replace(URL, "http://example.com/part.zip"),
            "missing part field": MANIFEST.replace(URL, ""),
        }
        for name, text in cases.items():
            with self.subTest(name=name), patch.object(setup_parts.Http, "get", return_value=text.encode()):
                with patch.object(setup_parts.paths, "versions") as versions:
                    with self.assertRaises(setup_parts.SetupError):
                        setup_parts.install(lambda _: None)
                    versions.assert_not_called()

    def test_reuse_requires_whole_digest(self):
        part = setup_parts.Part(DIGEST, "120", "tools", "old", "new", URL)
        with tempfile.TemporaryDirectory(prefix="setup-reuse-") as temp:
            versions = Path(temp) / "versions"
            old = versions / "v2.0.0"
            original = old / "tools" / "new"
            original.mkdir(parents=True)
            (versions / "current.txt").write_text("v2.0.0\n", encoding="ascii")
            checksums = old / "parts.txt"
            new = versions / "next"
            with patch.object(setup_parts.paths, "versions", return_value=str(versions)):
                checksums.write_text(DIGEST + "0\n", encoding="ascii")
                self.assertFalse(setup_parts.reuse_part(TAG, part, str(new)))
                self.assertTrue(original.is_dir())
                checksums.write_text(DIGEST + "\n", encoding="ascii")
                self.assertTrue(setup_parts.reuse_part(TAG, part, str(new)))
                self.assertTrue((new / "tools" / "new").is_dir())


if __name__ == "__main__":
    unittest.main()
