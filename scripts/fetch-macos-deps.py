#!/usr/bin/env python3
"""Fetch the pinned universal Qt kit and SQLite; never overwrite an existing kit."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DEPS = ROOT / "deps"
QT = DEPS / "Qt-6.10.2-macos"
BASE = "https://download.qt.io/online/qtsdkrepository/mac_x64/desktop/qt6_6102/qt6_6102/qt.qt6.6102.clang_64/"
ARCHIVES = {
    "qtsvg": "5659dc2a0fec1652bf9d264beeee63d02ddf4599ea0655350f499f86be1e4e7c",
    "qtbase": "29f8ae84c74e5f3b48f136d22971eec321e79645a7087804b8fdf9976a99e01b",
    "qtdeclarative": "81bfcbe1d1244ff8c7347d5aa309a68fdbe8db71ba696721eebd4935970b9cc7",
}
LICENSES = {
    "LGPL-3.0-only.txt": "da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768",
    "GPL-3.0-only.txt": "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
}

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def fetch(url, path, expected):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        print("Downloading", path.name, flush=True)
        partial = path.with_name(path.name + ".part")
        with urllib.request.urlopen(url, timeout=60) as response, partial.open("wb") as target:
            shutil.copyfileobj(response, target)
        if digest(partial) != expected:
            raise RuntimeError("Downloaded hash mismatch: " + str(partial))
        partial.rename(path)
    if digest(path) != expected:
        raise RuntimeError("Pinned dependency changed: " + str(path))
    return path

def main():
    DEPS.mkdir(exist_ok=True)
    sqlite = DEPS / "sqlite-amalgamation-3530400"
    if not sqlite.exists():
        archive = fetch("https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip",
                        DEPS / "sqlite-amalgamation-3530400.zip",
                        "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d")
        with zipfile.ZipFile(archive) as source:
            allowed = {"sqlite-amalgamation-3530400/" + name for name in ("", "sqlite3.c", "sqlite3.h", "sqlite3ext.h", "shell.c")}
            if set(source.namelist()) != allowed:
                raise RuntimeError("Unexpected SQLite archive inventory")
            source.extractall(DEPS)
    for name, expected in (("sqlite3.c", "b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189"),
                           ("sqlite3.h", "919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d")):
        if digest(sqlite / name) != expected:
            raise RuntimeError("Pinned SQLite changed: " + name)
    if not QT.exists():
        archives = []
        for module, expected in ARCHIVES.items():
            name = "6.10.2-0-202601261210" + module + "-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z"
            archives.append(fetch(BASE + name, DEPS / "qt-downloads/macos" / name, expected))
        with tempfile.TemporaryDirectory(prefix="qt-stage-", dir=DEPS) as temporary:
            for archive in archives:
                subprocess.run(["tar", "-xf", str(archive), "-C", temporary], check=True)
            if not (Path(temporary) / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
                raise RuntimeError("Incomplete Qt archive")
            shutil.copytree(temporary, QT, symlinks=True)
    for required in ("lib/cmake/Qt6/Qt6Config.cmake", "bin/macdeployqt", "qml/QtQuick/Controls/qmldir", "lib/QtSvg.framework/Versions/A/QtSvg"):
        if not (QT / required).exists():
            raise RuntimeError("Incomplete Qt kit: " + required)
    for name, expected in LICENSES.items():
        fetch("https://raw.githubusercontent.com/qt/qtbase/v6.10.2/LICENSES/" + name,
              QT / "redistribution-licenses" / name, expected)
    print("Verified Qt 6.10.2 universal kit and SQLite 3.53.4:", QT)

if __name__ == "__main__":
    main()
