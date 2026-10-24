# Maintaining application updates

The app checks the public latest stable release at `api.github.com/repos/brucerry/mark-six-studio/releases/latest` only after a user selects **Check for updates**. It compares three numeric version components and never offers a downgrade, draft or prerelease. This is independent of **Update history**.

## Release contract

Keep `src/desktop/CMakeLists.txt`, the stable `vMAJOR.MINOR.PATCH` tag and installer filenames aligned. The updater selects the exact unsigned Windows x64 setup EXE or universal macOS PKG, and its matching SHA-256 asset. All downloads use normal OS certificate verification. Only official release URLs and HTTPS redirects to GitHub's `release-assets.githubusercontent.com` / `objects.githubusercontent.com` asset hosts are accepted.

Metadata is limited to 1 MiB, checksum text to 4 KiB and installers to 512 MiB. Checks/checksums have a 30-second deadline; installer downloads have a ten-minute deadline and a 15-second idle timeout. Transfers are asynchronous and cancellable. Private partial downloads are discarded on failure or cancellation.

The controller checks the downloaded length, checksum filename, SHA-256 and GitHub asset digest when present, then rechecks the file before native handoff. Checksums detect mismatches; they do not authenticate a publisher. Verified installers stay in the app's update cache after handoff because the wizard can need the file after the app exits. They contain no user databases.

## Installation behavior

The user selects **Install and close app** separately. The OS opens the downloaded installer with its usual approval/elevation flow, and the app exits after successful handoff. Users finish the wizard and reopen manually. Failed handoff keeps the app open for retry. Updates use the normal native wizard, with no silent installation, automatic restart or signing bypass.

If an older app has no update controls, install the latest release manually.

See the [installer guide](../installer/README.md) for publishing and the [user guide](usage.md) for the update steps.
