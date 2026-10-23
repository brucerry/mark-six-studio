#!/usr/bin/env python3
"""Exercise PKG installation only on a disposable CI runner."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pkg", type=Path)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true":
        parser.error("System installation test requires a disposable GitHub Actions runner")
    app = Path("/Applications/Mark Six Studio.app")
    if app.exists():
        parser.error("Refusing to replace an existing installed application")
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "QML", "DYLD_"))}
    environment["QT_QPA_PLATFORM"] = "cocoa"
    with tempfile.TemporaryDirectory(prefix="marksix-installer-") as temporary:
        data = Path(temporary) / "user-data"
        data.mkdir()
        (data / "preserve.txt").write_text("Preserve user data")
        def hashes():
            return {p.relative_to(data).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in data.rglob("*") if p.is_file()}
        def install():
            subprocess.run(["sudo", "installer", "-pkg", str(args.pkg.resolve()), "-target", "/"], check=True)
        def smoke():
            subprocess.run([str(app / "Contents/MacOS/mark-six-studio"), "--smoke-load", "--data-dir", str(data)], check=True, timeout=60, env=environment)
        try:
            install()
            smoke()
            before = hashes()
            install()
            if hashes() != before:
                raise RuntimeError("Upgrade changed user data")
            smoke()
            before = hashes()
            # Matches the documented macOS removal operation; no data paths are removed.
            subprocess.run(["sudo", "rm", "-rf", str(app)], check=True)
            if hashes() != before:
                raise RuntimeError("Removal changed user data")
            install()
            if hashes() != before:
                raise RuntimeError("Reinstall changed user data")
            smoke()
        finally:
            if app.exists():
                subprocess.run(["sudo", "rm", "-rf", str(app)], check=True)
    print("PASS macOS PKG install, upgrade, removal, retained data and offline launch")

if __name__ == "__main__":
    main()
