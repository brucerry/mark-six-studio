#!/usr/bin/env python3
"""Exercise PKG installation only on a disposable CI runner."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import uuid

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pkg", type=Path)
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true" or not os.environ.get("RUNNER_TEMP"):
        parser.error("System installation test requires a disposable GitHub Actions runner")
    app = Path("/Applications/Mark Six Studio.app")
    if app.exists():
        parser.error("Refusing to replace an existing installed application")
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("QT_", "QML", "DYLD_"))}
    environment["QT_QPA_PLATFORM"] = "cocoa"
    with tempfile.TemporaryDirectory(prefix="marksix-installer-", dir=os.environ["RUNNER_TEMP"]) as temporary:
        data = Path(temporary) / "user-data"
        data.mkdir()
        (data / "preserve.txt").write_text("Preserve user data")
        def hashes():
            return {p.relative_to(data).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in data.rglob("*") if p.is_file()}
        def install():
            subprocess.run(["sudo", "installer", "-pkg", str(args.pkg.resolve()), "-target", "/"], check=True)
        def smoke():
            subprocess.run([str(app / "Contents/MacOS/mark-six-studio"), "--smoke-load", "--data-dir", str(data)], check=True, timeout=60, env=environment)
            subprocess.run([str(app / "Contents/Helpers/mark-six-maintenance"), "--verify-installation-state"], check=True, timeout=30)
        account = "marksixfixture" + uuid.uuid4().hex[:10]
        created_account = False
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
            # All-user discovery uses a real disposable Directory Services
            # account, with its home and all sentinel bytes inside RUNNER_TEMP.
            home = Path(temporary) / "second-profile"
            second = home / "Library/Application Support/MarkSixStudio"
            (second / "statistics").mkdir(parents=True)
            for db in (data / "statistics").glob("*.sqlite3"):
                shutil.copyfile(db, second / "statistics" / db.name)
            (home / "Downloads").mkdir()
            older = home / "Downloads/MarkSixStudio-0.1.0-macos-universal-unsigned.pkg"
            urllib.request.urlretrieve("https://github.com/brucerry/mark-six-studio/releases/download/v0.1.0/" + older.name, older)
            legacy = data / "legacy-保留.MSX"
            legacy.write_bytes(b"Opaque legacy fixture; never parsed")
            other_legacy = second / "other-account.msx"
            other_legacy.write_bytes(b"Other account legacy fixture")
            foreign = second / "personal.txt"
            foreign.write_bytes(b"Unrelated private document fixture")
            installed_legacy = app / "retained-recording.MSX"
            subprocess.run(["sudo", "tee", str(installed_legacy)], input=b"Opaque installed-copy legacy fixture", stdout=subprocess.DEVNULL, check=True)
            sentinels = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in (legacy, other_legacy, installed_legacy, foreign)}
            used = {int(line.split()[-1]) for line in subprocess.check_output(["dscl", ".", "-list", "/Users", "UniqueID"], text=True).splitlines()}
            uid = next(value for value in range(60000, 65000) if value not in used)
            subprocess.run(["sudo", "dscl", ".", "-create", "/Users/" + account], check=True)
            created_account = True
            for key, value in {"UniqueID": str(uid), "PrimaryGroupID": "20", "NFSHomeDirectory": str(home),
                               "UserShell": "/usr/bin/false", "RealName": "Mark Six disposable cleanup fixture",
                               "AuthenticationAuthority": ";DisabledUser;"}.items():
                subprocess.run(["sudo", "dscl", ".", "-create", "/Users/" + account, key, value], check=True)
            # A registered copy outside ~/Applications must be discovered in
            # this account's own LaunchServices context, not root's context.
            copy = home / "NonstandardCopies/Other Studio.app"
            subprocess.run(["ditto", str(app), str(copy)], check=True)
            os.chmod(temporary, 0o755)
            subprocess.run(["sudo", "chown", "-R", f"{uid}:20", str(home)], check=True)
            lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
            subprocess.run(["sudo", "-H", "-u", f"#{uid}", lsregister, "-f", str(copy)], check=True)
            registered = subprocess.check_output(["sudo", "-H", "-u", f"#{uid}", str(app / "Contents/Helpers/mark-six-maintenance"), "--list-registered-applications"])
            if str(copy).encode() not in registered:
                raise RuntimeError("Other-account native registration fixture was not observable")
            copy_legacy = copy / "retained-recording.MSX"
            sentinels[copy_legacy] = hashlib.sha256(copy_legacy.read_bytes()).hexdigest()
            fixture = Path("build/macos/maintenance/MarkSixUninstallFixture").resolve()
            subprocess.run([str(fixture), str(app), str(data), str(legacy)], check=True, timeout=600)
            if older.exists() or list((second / "statistics").glob("*.sqlite3")):
                raise RuntimeError("Other-account data or identified older browser installer remains")
            if (copy / "Contents/MacOS/mark-six-studio").exists():
                raise RuntimeError("Other-account registered nonstandard installed copy remains")
            if any(hashlib.sha256(p.read_bytes()).hexdigest() != digest for p, digest in sentinels.items()):
                raise RuntimeError("Native full uninstall changed preserved legacy or foreign bytes")
        finally:
            if created_account:
                subprocess.run(["sudo", "dscl", ".", "-delete", "/Users/" + account], check=True)
            fixture_home = Path(temporary) / "second-profile"
            if fixture_home.exists():
                # Restore ownership of this private CI tree before Python's
                # TemporaryDirectory removes its preserved test sentinels.
                subprocess.run(["sudo", "chown", "-R", f"{os.getuid()}:{os.getgid()}", str(fixture_home)], check=True)
            if app.exists():
                subprocess.run(["sudo", "rm", "-rf", str(app)], check=True)
    print("PASS macOS ordinary retention plus native full removal, second-account/download cleanup, preserved legacy/foreign hashes and standalone retry")

if __name__ == "__main__":
    main()
