import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys
import subprocess
import shutil
import plistlib
import hashlib
import struct
from urllib.parse import unquote

module_path = Path(__file__).with_name("write-maintenance-payload.py")
specification = importlib.util.spec_from_file_location("maintenance_payload_writer", module_path)
writer = importlib.util.module_from_spec(specification)
specification.loader.exec_module(writer)

verifier_spec = importlib.util.spec_from_file_location("maintenance_macos_verifier", Path(__file__).with_name("verify-macos-package.py"))
verifier = importlib.util.module_from_spec(verifier_spec)
verifier_spec.loader.exec_module(verifier)


class MaintenancePayloadTests(unittest.TestCase):
    def test_universal_helper_dependency_headers_are_not_libraries(self):
        linked = "helper (architecture x86_64):\n\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\nhelper (architecture arm64):\n\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 1.0.0)\n"
        verifier.verify_helper_dependencies(linked)
        with self.assertRaises(ValueError):
            verifier.verify_helper_dependencies(linked + "\t@rpath/QtCore.framework/Versions/A/QtCore (compatibility version 6.0.0)\n")
        with self.assertRaises(ValueError):
            verifier.verify_helper_dependencies("helper (architecture arm64):\n")

    @unittest.skipUnless(sys.platform == "darwin", "Native bundle-seal check requires macOS")
    def test_native_seal_accepts_resource_checksum_and_keeps_helper_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            source = base / "fixture.c"
            source.write_text("int main(void) { return 0; }\n")
            app = base / "Fixture.app"
            main = app / "Contents/MacOS/mark-six-studio"
            main.parent.mkdir(parents=True)
            subprocess.run(["xcrun", "clang", "-arch", "arm64", "-arch", "x86_64", "-mmacosx-version-min=13.0", str(source), "-o", str(main)], check=True, capture_output=True)
            helper = app / "Contents/Helpers/mark-six-maintenance"
            helper.parent.mkdir()
            shutil.copy2(main, helper)
            verifier.verify_helper_dependencies(subprocess.check_output(["otool", "-L", str(helper)], text=True))
            subprocess.run(["codesign", "--force", "--identifier", "com.brucerry.marksixstudio.maintenance", "--sign", "-", str(helper)], check=True, capture_output=True)
            subprocess.run(["codesign", "--verify", "--strict", "--all-architectures", "-R", '=identifier "com.brucerry.marksixstudio.maintenance"', str(helper)], check=True, capture_output=True)
            resources = app / "Contents/Resources"
            resources.mkdir()
            digest = hashlib.sha256(helper.read_bytes()).hexdigest()
            (resources / "maintenance-helper.sha256").write_text(digest + "\n")
            with (app / "Contents/Info.plist").open("wb") as output:
                plistlib.dump({"CFBundleIdentifier": "com.brucerry.marksixstudio", "CFBundleExecutable": main.name,
                              "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.1.1"}, output)
            writer.write_payload(app, True)
            subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True, capture_output=True)
            subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True, capture_output=True)
            self.assertEqual(digest, hashlib.sha256(helper.read_bytes()).hexdigest())
            native = Path(__file__).resolve().parents[1] / "build/macos/maintenance/mark-six-maintenance"
            if native.is_file():
                subprocess.run([str(native), "--verify-application-code", str(app)], check=True, capture_output=True)
                changed = bytearray(main.read_bytes())
                first_slice = struct.unpack_from(">I", changed, 16)[0]
                changed[first_slice + 0x300] ^= 1
                main.write_bytes(changed)
                self.assertNotEqual(subprocess.run([str(native), "--verify-application-code", str(app)], capture_output=True).returncode, 0)
            # Only signing is exercised; neither fixture binary is executed.

    def test_final_macos_seal_does_not_create_a_hash_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "Contents/MacOS/mark-six-studio"
            envelope = root / "Contents/_CodeSignature/CodeResources"
            resource = root / "Contents/Resources/a unicode 測試.txt"
            for path in (executable, envelope, resource):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"before signing")
            manifest = writer.write_payload(root, True)
            before = manifest.read_bytes()
            executable.write_bytes(b"new code signature slots")
            envelope.write_bytes(b"new resource envelope")
            writer.write_payload(root, True)
            self.assertEqual(before, manifest.read_bytes())
            rows = [line.split("\t") for line in before.decode().splitlines()[1:]]
            paths = {unquote(row[2]) for row in rows}
            self.assertIn(resource.relative_to(root).as_posix(), paths)
            self.assertNotIn(executable.relative_to(root).as_posix(), paths)
            self.assertNotIn(envelope.relative_to(root).as_posix(), paths)

    def test_foreign_recording_and_external_link_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "payload"
            root.mkdir()
            legacy = root / "personal.MsX"
            legacy.write_bytes(b"opaque recording")
            with self.assertRaises(ValueError):
                writer.write_payload(root, False)
            self.assertEqual(legacy.read_bytes(), b"opaque recording")
            legacy.unlink()
            (root / "outside").symlink_to(Path(temporary))
            with self.assertRaises(ValueError):
                writer.write_payload(root, True)

    def test_windows_metadata_has_no_mutual_hash_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "mark-six-studio.exe").write_bytes(b"fixture executable")
            manifest = writer.write_payload(root, False)
            before = manifest.read_bytes()
            (root / "manifest.json").write_text('{"format":"marksix-statistics-package-v1"}')
            writer.write_payload(root, False)
            self.assertEqual(before, manifest.read_bytes())


if __name__ == "__main__":
    unittest.main()
