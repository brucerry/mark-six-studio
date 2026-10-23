#!/usr/bin/env python3
"""Retain negative fixtures for incomplete, contaminated or escaping app payloads."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile

spec = importlib.util.spec_from_file_location("package_verifier", Path(__file__).with_name("verify-macos-package.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def main():
    source = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="marksix-verifier-") as temporary:
        app = Path(temporary) / "fixture.app"
        shutil.copytree(source, app, symlinks=True, copy_function=os.link)
        def rejects(label):
            try:
                module.verify(app)
            except (ValueError, FileNotFoundError):
                print("PASS rejected", label)
            else:
                raise RuntimeError("Accepted invalid payload: " + label)
        relative = "Contents/Resources/licenses/SQLite.txt"
        missing = app / relative
        missing.unlink()
        rejects("missing required resource")
        os.link(source / relative, missing)
        database = app / "Contents/Resources/contamination.sqlite3"
        database.write_text("Never bundle user history")
        rejects("bundled user database")
        database.unlink()
        escaping = app / "Contents/Resources/escape"
        escaping.symlink_to(Path(temporary))
        rejects("escaping framework/resource link")
        escaping.unlink()
        print("PASS package verifier negative fixtures")

if __name__ == "__main__":
    main()
