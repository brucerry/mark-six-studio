#!/usr/bin/env python3
"""Validate a relocatable universal Qt app, including framework link containment."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import posixpath
import re
import subprocess

MAGIC = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf"}

def verify(app):
    app = app.resolve(strict=True)
    if app.suffix != ".app":
        raise ValueError("Expected an application bundle")
    with (app / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    if info.get("CFBundleIdentifier") != "com.brucerry.marksixstudio" or info.get("LSMinimumSystemVersion") != "13.0":
        raise ValueError("Unexpected bundle identity or deployment target")
    required = ["Contents/MacOS/mark-six-studio", "Contents/Resources/MarkSixStudio.icns",
                "Contents/Frameworks/QtCore.framework/Versions/A/QtCore",
                "Contents/PlugIns/platforms/libqcocoa.dylib", "Contents/PlugIns/tls/libqsecuretransportbackend.dylib", "Contents/Resources/qml/QtQuick/Controls/qmldir",
                "Contents/Resources/licenses/GPL-3.0.txt", "Contents/Resources/licenses/Qt-Notice.txt",
                "Contents/Resources/licenses/LGPL-3.0-only.txt", "Contents/Resources/licenses/GPL-3.0-only.txt",
                "Contents/Resources/licenses/SQLite.txt", "Contents/Resources/sbom/qtbase-6.10.2.cdx.json",
                "Contents/Resources/sbom/qtdeclarative-6.10.2.cdx.json"]
    for relative in required:
        if not (app / relative).is_file():
            raise ValueError("Missing package resource: " + relative)
    inventory = {}
    binaries = 0
    for path in sorted(app.rglob("*")):
        relative = path.relative_to(app).as_posix()
        if path.is_symlink():
            target = path.resolve(strict=True)
            if app not in target.parents:
                raise ValueError("Escaping package symlink: " + relative)
            inventory[relative] = {"link": path.readlink().as_posix()}
            continue
        if path.is_dir():
            continue
        if path.suffix.lower() in {".sqlite3", ".sqlite", ".db", ".msx", ".mp4", ".pfx", ".p12"}:
            raise ValueError("User data or credentials in package: " + relative)
        with path.open("rb") as source:
            magic = source.read(4)
        if magic in MAGIC:
            binaries += 1
            arches = subprocess.check_output(["lipo", "-archs", str(path)], text=True).split()
            if set(arches) != {"arm64", "x86_64"}:
                raise ValueError("Non-universal binary: " + relative)
            commands = subprocess.check_output(["otool", "-l", str(path)], text=True)
            for rpath in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", commands):
                for prefix, base in (("@loader_path/", path.parent), ("@executable_path/", app / "Contents/MacOS")):
                    if rpath.startswith(prefix):
                        target = (base / rpath[len(prefix):]).resolve()
                        if target != app and app not in target.parents:
                            raise ValueError("Escaping runtime search path: " + rpath)
                        break
                else:
                    raise ValueError("External runtime search path: " + rpath)
            linked = subprocess.check_output(["otool", "-L", str(path)], text=True)
            for line in linked.splitlines():
                if not line.startswith("\t"):
                    continue
                dependency = line.strip().split(" (", 1)[0]
                if posixpath.normpath(dependency).startswith(("/System/Library/", "/usr/lib/")):
                    continue
                mappings = {"@rpath/": app / "Contents/Frameworks", "@executable_path/": app / "Contents/MacOS", "@loader_path/": path.parent}
                for prefix, base in mappings.items():
                    if dependency.startswith(prefix):
                        target = (base / dependency[len(prefix):]).resolve()
                        if not target.is_file() or app not in target.parents:
                            raise ValueError("Unresolved package dependency: " + dependency)
                        break
                else:
                    raise ValueError("External package dependency: " + dependency)
        inventory[relative] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    if not binaries:
        raise ValueError("No Mach-O binaries found")
    return {"format": "marksix-macos-package-v1", "version": info["CFBundleShortVersionString"],
            "architectures": ["arm64", "x86_64"], "minimumMacOS": "13.0", "files": inventory}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--write-manifest", type=Path)
    parser.add_argument("--require-signed", action="store_true")
    args = parser.parse_args()
    result = verify(args.app)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(args.app)], check=True)
    if args.require_signed:
        signature = subprocess.run(["codesign", "-dv", "--verbose=4", str(args.app)], capture_output=True, text=True, check=True).stderr
        if "Authority=Developer ID Application:" not in signature or "runtime" not in signature:
            raise ValueError("Developer ID hardened-runtime signature required")
    if args.manifest and json.loads(args.manifest.read_text()) != result:
        raise ValueError("Package manifest changed")
    if args.write_manifest:
        args.write_manifest.write_text(json.dumps(result, indent=2) + "\n")
    print("PASS universal macOS package:", len(result["files"]), "files/links; internal dependencies and required resources")

if __name__ == "__main__":
    main()
