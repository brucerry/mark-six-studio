#!/usr/bin/env python3
"""Check a version tag and the complete unsigned installer set before publication."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent

def source_version():
    source = (ROOT / "src/desktop/CMakeLists.txt").read_text()
    match = re.search(r"project\(MarkSixStatisticsDesktop VERSION (\d+\.\d+\.\d+)\b", source)
    if not match:
        raise ValueError("Application version is missing")
    return match.group(1)

def validate_tag(tag, version):
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag) or tag != "v" + version:
        raise ValueError("Release tag must match application version v" + version)

def validate_artifacts(directory, version):
    windows = "MarkSixStudio-" + version + "-windows-x64-unsigned-setup.exe"
    mac = "MarkSixStudio-" + version + "-macos-universal-unsigned"
    expected = {windows, windows + ".sha256", mac + ".pkg", mac + ".sha256", mac + ".manifest.json"}
    actual = {path.name for path in directory.iterdir()}
    if actual != expected:
        raise ValueError("Unexpected release inventory; missing={}, extra={}".format(sorted(expected - actual), sorted(actual - expected)))
    for name in expected:
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError("Release asset must be a nonempty regular file: " + name)
    for checksum, installer in ((windows + ".sha256", windows), (mac + ".sha256", mac + ".pkg")):
        fields = (directory / checksum).read_text().strip().split("  ")
        if len(fields) != 2 or fields[1] != installer or not re.fullmatch(r"[0-9a-fA-F]{64}", fields[0]):
            raise ValueError("Invalid installer checksum record: " + checksum)
        digest = hashlib.sha256()
        with (directory / installer).open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != fields[0].lower():
            raise ValueError("Installer checksum mismatch: " + installer)
    manifest = json.loads((directory / (mac + ".manifest.json")).read_text())
    if (manifest.get("format") != "marksix-macos-package-v1" or manifest.get("version") != version or
            manifest.get("architectures") != ["arm64", "x86_64"] or manifest.get("minimumMacOS") != "13.0" or
            not isinstance(manifest.get("files"), dict) or not manifest["files"]):
        raise ValueError("Invalid or mismatched macOS release manifest")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    version = source_version()
    validate_tag(args.tag, version)
    if args.artifacts:
        validate_artifacts(args.artifacts, version)
        print("PASS complete unsigned release inventory, versions and installer checksums")
    else:
        print("PASS release tag matches application version")

if __name__ == "__main__":
    main()
