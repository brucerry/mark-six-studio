#!/usr/bin/env python3
"""Import release-only credentials into an isolated, temporary CI keychain."""
import base64
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import tempfile

def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)

def main():
    if os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("Signing setup is restricted to disposable CI runners")
    required = ("RELEASE_TAG", "MACOS_CERTIFICATES_BASE64", "MACOS_CERTIFICATE_PASSWORD", "APPLE_NOTARY_KEY_BASE64",
                "APPLE_NOTARY_KEY_ID", "APPLE_NOTARY_ISSUER", "MACOS_APP_IDENTITY", "MACOS_INSTALLER_IDENTITY")
    if any(not os.environ.get(key) for key in required):
        raise RuntimeError("All Apple signing and notarization credentials are required")
    source = (Path(__file__).resolve().parent.parent / "src/desktop/CMakeLists.txt").read_text()
    version = re.search(r"project\(MarkSixStatisticsDesktop VERSION ([0-9.]+)", source).group(1)
    if os.environ["RELEASE_TAG"] != "v" + version:
        raise RuntimeError("Release tag must match the application version")
    keychain = Path(os.environ["RUNNER_TEMP"]) / "marksix-release.keychain-db"
    if keychain.exists():
        raise RuntimeError("Signing keychain already exists")
    password = secrets.token_urlsafe(32)
    run("security", "create-keychain", "-p", password, keychain)
    run("security", "set-keychain-settings", "-lut", "21600", keychain)
    run("security", "unlock-keychain", "-p", password, keychain)
    current = shlex.split(subprocess.check_output(["security", "list-keychains", "-d", "user"], text=True))
    run("security", "list-keychains", "-d", "user", "-s", keychain, *current)
    with tempfile.TemporaryDirectory(dir=os.environ["RUNNER_TEMP"]) as temporary:
        certificate = Path(temporary) / "signing.p12"
        api_key = Path(temporary) / "notary.p8"
        certificate.write_bytes(base64.b64decode(os.environ["MACOS_CERTIFICATES_BASE64"], validate=True))
        api_key.write_bytes(base64.b64decode(os.environ["APPLE_NOTARY_KEY_BASE64"], validate=True))
        certificate.chmod(0o600); api_key.chmod(0o600)
        run("security", "import", certificate, "-k", keychain, "-P", os.environ["MACOS_CERTIFICATE_PASSWORD"],
            "-T", "/usr/bin/codesign", "-T", "/usr/bin/productsign", "-T", "/usr/bin/productbuild")
        run("security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:", "-s", "-k", password, keychain,
            stdout=subprocess.DEVNULL)
        run("xcrun", "notarytool", "store-credentials", "marksix-release", "--key", api_key,
            "--key-id", os.environ["APPLE_NOTARY_KEY_ID"], "--issuer", os.environ["APPLE_NOTARY_ISSUER"], "--keychain", keychain)
    with open(os.environ["GITHUB_ENV"], "a") as environment:
        environment.write("MACOS_NOTARY_KEYCHAIN=" + str(keychain) + "\n")

if __name__ == "__main__":
    main()
