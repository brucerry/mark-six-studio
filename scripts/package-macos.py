#!/usr/bin/env python3
"""Create an offline macOS PKG wizard; --signed requires Developer ID and notarization."""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/macos")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release/macos")
    parser.add_argument("--signed", action="store_true")
    args = parser.parse_args()
    identities = [os.environ.get(key) for key in ("MACOS_APP_IDENTITY", "MACOS_INSTALLER_IDENTITY", "MACOS_NOTARY_PROFILE")]
    notary_keychain = ["--keychain", os.environ["MACOS_NOTARY_KEYCHAIN"]] if os.environ.get("MACOS_NOTARY_KEYCHAIN") else []
    if args.signed and not all(identities):
        parser.error("Signed release requires MACOS_APP_IDENTITY, MACOS_INSTALLER_IDENTITY and MACOS_NOTARY_PROFILE")
    if args.output_dir.exists():
        parser.error("Output already exists; choose a new --output-dir")
    source = args.build_dir / "mark-six-studio.app"
    with (source / "Contents/Info.plist").open("rb") as handle:
        version = plistlib.load(handle)["CFBundleShortVersionString"]
    args.output_dir.mkdir(parents=True)
    app = args.output_dir.resolve() / "Mark Six Studio.app"
    shutil.copytree(source, app, symlinks=True)
    resources = app / "Contents/Resources"
    resources.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="marksix-icon-") as temporary:
        iconset = Path(temporary) / "MarkSixStudio.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = "icon_{}x{}{}.png".format(size, size, "@2x" if scale == 2 else "")
                run("sips", "-z", size * scale, size * scale, ROOT / "assets/icon.png", "--out", iconset / name, stdout=subprocess.DEVNULL)
        run("iconutil", "-c", "icns", iconset, "-o", resources / "MarkSixStudio.icns")
    shutil.copy2(ROOT / "installer/macos.entitlements", resources / "marksix.entitlements")
    qt = ROOT / "deps/Qt-6.10.2-macos"
    (resources / "licenses").mkdir()
    (resources / "sbom").mkdir()
    for name in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt"):
        shutil.copy2(qt / "redistribution-licenses" / name, resources / "licenses" / name)
    shutil.copy2(ROOT / "LICENSE", resources / "licenses/GPL-3.0.txt")
    shutil.copy2(ROOT / "src/desktop/qt-redistribution-notice.txt", resources / "licenses/Qt-Notice.txt")
    shutil.copy2(ROOT / "src/resources/sqlite-notice.txt", resources / "licenses/SQLite.txt")
    shutil.copy2(ROOT / "src/desktop/package-readme-macos.md", resources / "README.md")
    for name in ("qtbase-6.10.2.cdx.json", "qtdeclarative-6.10.2.cdx.json", "qtsvg-6.10.2.cdx.json"):
        shutil.copy2(qt / "sbom" / name, resources / "sbom" / name)
    # Deploy only the native plugins this application uses. Qt's default
    # plugin sweep includes SQL drivers for external database installations.
    plugins = ["platforms/libqcocoa.dylib", "tls/libqsecuretransportbackend.dylib",
               "tls/libqcertonlybackend.dylib", "networkinformation/libqapplenetworkinformation.dylib",
               "imageformats/libqgif.dylib",
               "imageformats/libqico.dylib", "imageformats/libqjpeg.dylib", "imageformats/libqsvg.dylib"]
    extra_executables = []
    for relative in plugins:
        target = app / "Contents/PlugIns" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(qt / "plugins" / relative, target)
        extra_executables.append("-executable=" + str(target))
    signing = "-sign-for-notarization=" + identities[0] if args.signed else "-codesign=-"
    run(qt / "bin/macdeployqt", app, "-qmldir=" + str(ROOT / "src/desktop"),
        "-no-plugins", *extra_executables, signing)
    # Re-sign the root after deployment to include all resources and nested code.
    if args.signed:
        run("codesign", "--force", "--options", "runtime", "--timestamp", "--entitlements", resources / "marksix.entitlements", "--sign", identities[0], app)
    else:
        run("codesign", "--force", "--deep", "--sign", "-", app)
    suffix = "" if args.signed else "-unsigned"
    stem = "MarkSixStudio-{}-macos-universal{}".format(version, suffix)
    verify = [sys.executable, str(ROOT / "scripts/verify-macos-package.py"), str(app)]
    if args.signed:
        verify.append("--require-signed")
    run(*verify)
    if args.signed:
        archive = args.output_dir / "notarization-app.zip"
        run("ditto", "-c", "-k", "--keepParent", app, archive)
        run("xcrun", "notarytool", "submit", archive, "--keychain-profile", identities[2], *notary_keychain, "--wait")
        run("xcrun", "stapler", "staple", app)
        run("xcrun", "stapler", "validate", app)
        run("spctl", "--assess", "--type", "execute", "--verbose=2", app)
        archive.unlink()
    clean_env = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "QML", "DYLD_"))}
    clean_env["QT_QPA_PLATFORM"] = "cocoa"
    with tempfile.TemporaryDirectory(prefix="marksix-package-test-") as temporary:
        smoke = run(app / "Contents/MacOS/mark-six-studio", "--smoke-load", "--data-dir", temporary, env=clean_env, timeout=60,
            capture_output=True, text=True)
        if any(error in smoke.stderr for error in ("TypeError:", "ReferenceError:", "failed to load component")):
            raise RuntimeError("QML runtime error: " + smoke.stderr)
    pkg = args.output_dir / (stem + ".pkg")
    command = ["productbuild", "--component", app, "/Applications"]
    if args.signed:
        command.extend(["--sign", identities[1]])
    run(*command, pkg)
    if args.signed:
        run("xcrun", "notarytool", "submit", pkg, "--keychain-profile", identities[2], *notary_keychain, "--wait")
        run("xcrun", "stapler", "staple", pkg)
        run("xcrun", "stapler", "validate", pkg)
        run("pkgutil", "--check-signature", pkg)
        run("spctl", "--assess", "--type", "install", "--verbose=2", pkg)
    run(*verify, "--write-manifest", args.output_dir / (stem + ".manifest.json"))
    (args.output_dir / (stem + ".sha256")).write_text(hashlib.sha256(pkg.read_bytes()).hexdigest() + "  " + pkg.name + "\n")
    print("Installer:", pkg)

if __name__ == "__main__":
    main()
