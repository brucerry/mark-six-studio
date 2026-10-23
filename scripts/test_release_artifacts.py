"""Publication gates must reject incomplete, mismatched or modified assets."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release_artifacts", Path(__file__).with_name("verify-release-artifacts.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

class ReleaseArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.windows = "MarkSixStudio-1.2.3-windows-x64-unsigned-setup.exe"
        self.mac = "MarkSixStudio-1.2.3-macos-universal-unsigned"
        for name, body, checksum in ((self.windows, b"windows-fixture", self.windows + ".sha256"),
                                     (self.mac + ".pkg", b"macos-fixture", self.mac + ".sha256")):
            (self.root / name).write_bytes(body)
            (self.root / checksum).write_text(hashlib.sha256(body).hexdigest() + "  " + name + "\n")
        (self.root / (self.mac + ".manifest.json")).write_text(json.dumps({
            "format": "marksix-macos-package-v1", "version": "1.2.3", "architectures": ["arm64", "x86_64"],
            "minimumMacOS": "13.0", "files": {"Contents/Info.plist": {"sha256": "fixture"}}}))

    def test_complete_unsigned_release(self):
        release.validate_tag("v1.2.3", "1.2.3")
        release.validate_artifacts(self.root, "1.2.3")

    def test_wrong_or_unsafe_tag(self):
        for tag in ("v1.2.4", "main", "v1.2.3;echo", "v1.2.3\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.validate_tag(tag, "1.2.3")

    def test_missing_platform(self):
        (self.root / self.windows).unlink()
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

    def test_changed_installer(self):
        (self.root / self.windows).write_bytes(b"modified")
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

    def test_wrong_checksum_filename(self):
        (self.root / (self.mac + ".sha256")).write_text("a" * 64 + "  ../outside.pkg\n")
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

    def test_mismatched_manifest(self):
        (self.root / (self.mac + ".manifest.json")).write_text('{"version":"0.0.0"}')
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

    def test_unexpected_asset(self):
        (self.root / "unexpected.exe").write_bytes(b"extra")
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

    def test_linked_installer(self):
        original = self.root / self.windows
        original.unlink()
        target = self.root / (self.mac + ".pkg")
        target.write_bytes(b"windows-fixture")
        (self.root / (self.mac + ".sha256")).write_text(hashlib.sha256(target.read_bytes()).hexdigest() + "  " + target.name + "\n")
        original.symlink_to(target.name)
        with self.assertRaises(ValueError):
            release.validate_artifacts(self.root, "1.2.3")

if __name__ == "__main__":
    unittest.main()
