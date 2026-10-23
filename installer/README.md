# Installer builds and GitHub releases

Mark Six Studio is maintained as a GPLv3 open-source project with **unsigned Windows and macOS releases**. No paid signing service, developer membership or signing credentials are required. Installer filenames retain `unsigned` and releases include SHA-256 checksums. Application/model protocols and SQLite formats are unchanged.

Users can download installers from the [latest release](https://github.com/brucerry/mark-six-studio/releases/latest). From v0.1.1, **Check for updates** can download and verify the newer installer inside the app; users finish in the native wizard. Existing v0.1.0 users first need a manual installer upgrade. See [using updates](../guides/usage.md#updates) and [update maintenance](../guides/updates.md).

## Build installers

Windows: run the README's build/package commands. The setup wizard enforces Windows 10 build 17763 (1809) or newer, matching the [Qt 6.10 platform matrix](https://doc.qt.io/qt-6.10/supported-platforms.html). Hash-pinned Inno Setup 7.1.0 and an official VC redistributable are fetched automatically. The redistributable must have a valid Microsoft signature; this is a third-party prerequisite check, not a publisher certificate for our app. The wizard uses stable AppId `3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8`, installs under Program Files and retains user data during removal. Setup carries the runtime for offline installation and retains a newer compatible runtime. Administrator approval can be required.

macOS: run `bash scripts/build-macos.sh`, then `python3 scripts/package-macos.py`. CMake 3.25+ and Xcode 26+ Command Line Tools are build prerequisites. Dependencies are hash-pinned Qt 6.10.2 universal qtbase/qtdeclarative/qtsvg and SQLite 3.53.4. The PKG installs a universal app under `/Applications`, with minimum macOS 13 and bundle relocation disabled. Remove the app bundle to uninstall; Application Support remains intact. Installer receipts are OS metadata, not a second application.

The macOS app uses free ad-hoc code signatures for binary integrity and Apple Silicon execution. These do not identify a trusted developer or provide notarization. The PKG is unsigned. Packaging does not need an Apple account.

Output directories are never overwritten. Windows accepts `-OutputDirectory PATH`; macOS accepts `--output-dir PATH`. Manifests record packaged files and hashes. macOS manifests sit outside the app bundle so they do not invalidate its integrity signature.

## Publish a GitHub release

1. Merge the installer changes into the default branch so the **Release unsigned installers** workflow is available in Actions.
2. Add `installer/releases/vMAJOR.MINOR.PATCH.md` with **📦 Version description**, **✨ Changes** and **🧾 Commits** sections. Put `<!-- RELEASE_COMMITS -->` in the commits section to generate a short linked list automatically.
3. Create and push a `vMAJOR.MINOR.PATCH` tag matching `src/desktop/CMakeLists.txt` (currently `v0.1.1`).
4. Run **Release unsigned installers** from Actions and enter that tag.

The workflow checks out the exact tag, builds/tests each platform, creates unsigned installers and exercises installation, reinstall, removal and retained data. Publication requires both jobs to pass. Before publishing, it checks the complete two-platform asset set, versions, manifest and installer checksums. There are no signing secrets to configure. Ordinary CI uploads test artifacts and does not create a release.

Release names are just the version tag. The workflow renders that version's notes from [releases](releases/) and links up to ten commits since the prior stable tag plus a full changelog. Keep descriptions short, changes in bullets and unsigned installation guidance linked. The [original v0.1.0 summary](release-notes.md) is retained for reference. A checksum verifies that a download matches the repository's recorded file; it does not authenticate a publisher.

## Installation approval

Download from this repository's GitHub Releases and compare the supplied SHA-256 checksum. On Windows, an unsigned installer can show an unknown-publisher or SmartScreen prompt. Smart App Control or organization policies may block unsigned apps; see [Microsoft's explanation](https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation).

For a trusted macOS download that is blocked as unidentified or unnotarized, try opening it, then use System Settings > Privacy & Security > Open Anyway when available. Review the package/app name before approving. See [Apple's instructions](https://support.apple.com/102445). No global disabling of security features is required by our build or installer scripts.

## Verification and known limits

`verify-statistics-package.ps1` checks the exact Windows folder inventory and hashes. `verify-macos-package.py APP --manifest MANIFEST` checks universal binaries, minimum OS, bundle identity, resources, internal dependencies, symlinks, runtime search paths, integrity signatures and hashes. Negative fixtures reject missing resources, user databases, escaping links and external runtime search paths.

`test-windows-installer.ps1` and `test-macos-installer.py` install, reinstall, remove, compare retained data hashes and reinstall again on disposable CI runners. They refuse to run these system changes outside GitHub Actions. `verify-release-artifacts.py --tag v0.1.1 --artifacts DIRECTORY` checks the files to be published; its unit fixtures cover wrong tags, missing/extra assets, modified installers, bad checksum paths, mismatched manifests and symlinks.

On 2026-10-23, native Windows Server 2022 and macOS 15 CI passed builds, backend/Qt suites, package checks and installer lifecycle/data-preservation checks at commit 56ba685. Local macOS 27 tests also passed natively and under Rosetta x86_64, with copied-app launch, screenshot review and a native worker-thread HTTPS probe. Separate minimum-OS installs, native Intel hardware, real-display scaling and manual VoiceOver/Narrator acceptance remain unverified. The user's unsigned-release decision retires signed release qualification; it does not turn these unperformed checks into passes.

The unsigned v0.1.0 PKG was additionally installed through the native wizard on an Apple Silicon Mac running macOS 27.0.1. The installed copy launched from `/Applications/Mark Six Studio.app`, rendered its dashboard, and the user confirmed it runs. This verifies that installation and launch on this machine; it does not complete the other target-machine or accessibility checks above.

Local opt-in signing switches remain available for a future maintainer who already has credentials. They are optional and are not used by the GitHub release workflow.
