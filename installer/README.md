# Installer builds and release qualification

The application remains C++/Qt. No model protocol or SQLite schema changes are introduced by these installers.

## Development installers

Windows: run the README's build/package commands. The wizard enforces Windows 10 build 17763 (1809) or newer, matching the [Qt 6.10 platform matrix](https://doc.qt.io/qt-6.10/supported-platforms.html). Inno Setup 7.1.0 and an official Microsoft VC redistributable are fetched with pinned SHA-256 hashes; the redistributable must also have a valid Microsoft Authenticode signature. The wizard uses stable AppId `3B52BF71-65D1-4C1F-A25E-0A3B16A7B3C8`, installs under Program Files and retains user data during removal. The VC prerequisite is bundled, so setup does not need internet. Standard-user installation can prompt for administrator credentials.

macOS: `bash scripts/build-macos.sh`, then `python3 scripts/package-macos.py`. CMake 3.25+ and Xcode 26+ Command Line Tools are prerequisites. Dependencies are hash-pinned Qt 6.10.2 universal qtbase/qtdeclarative/qtsvg and SQLite 3.53.4. The PKG installs a universal app under `/Applications`, with minimum macOS 13. No database, account, network download or developer SDK is required at installation. Remove the app bundle to uninstall; keep Application Support to retain data. Installer receipts are OS metadata, not a second application.

The package scripts refuse an existing output directory. Windows accepts `-OutputDirectory PATH`; macOS accepts `--output-dir PATH`. Unsigned development artifacts use `unsigned` in filenames and are not qualified public releases. Package signing changes payload hashes, so manifests are generated after signing. macOS manifests are outside the app bundle to preserve its code signature. The verifier permits only bundle-contained framework symlinks and resolves every Mach-O dependency against the bundle or system libraries.

## Signing and publication

The manual **Signed installers** workflow accepts an existing `vMAJOR.MINOR.PATCH` tag matching CMake's application version. It checks out that exact tag, builds/tests both platforms, signs, verifies and runs installation/data-preservation checks. Publication depends on both jobs passing; missing credentials fail the workflow. No release is published by ordinary CI.

Configure these repository secrets before invoking the workflow:

| Secret | Value |
| --- | --- |
| `WINDOWS_PFX_BASE64` | Base64 PKCS#12 containing a Windows code-signing certificate and private key |
| `WINDOWS_PFX_PASSWORD` | Password protecting that PKCS#12 |
| `MACOS_CERTIFICATES_BASE64` | Base64 PKCS#12 containing Developer ID Application and Developer ID Installer certificates and private keys |
| `MACOS_CERTIFICATE_PASSWORD` | Password protecting the Apple PKCS#12 |
| `MACOS_APP_IDENTITY` | Exact Developer ID Application signing identity |
| `MACOS_INSTALLER_IDENTITY` | Exact Developer ID Installer signing identity |
| `APPLE_NOTARY_KEY_BASE64` | Base64 App Store Connect API private key authorized for notarization |
| `APPLE_NOTARY_KEY_ID` | API key identifier |
| `APPLE_NOTARY_ISSUER` | API issuer identifier |

For local Windows signing, import the certificate into the current user's certificate store and set `WINDOWS_SIGN_CERT_THUMBPRINT`; optionally set `WINDOWS_SIGNTOOL`. Run package and installer scripts with `-Signed`. Signatures use SHA-256 and a timestamp, then undergo Authenticode verification. A hardware-backed signing provider can supply the imported certificate/key through the Windows certificate store.

For local macOS signing, set `MACOS_APP_IDENTITY`, `MACOS_INSTALLER_IDENTITY` and `MACOS_NOTARY_PROFILE` (an existing `notarytool` keychain profile), then run `python3 scripts/package-macos.py --signed`. `MACOS_NOTARY_KEYCHAIN` optionally identifies a non-default keychain. The app uses hardened runtime and the QML JIT entitlement. Both app and PKG are notarized, stapled and assessed. Temporary CI credentials are removed at job completion.

## Verification and limits

`verify-statistics-package.ps1` checks the Windows folder's exact inventory and hashes. `verify-macos-package.py APP --manifest MANIFEST` checks universal binaries, minimum OS, bundle identity, resources, dependencies, symlink containment, signatures and hashes. With `--require-signed`, it also requires a Developer ID hardened-runtime signature. Negative fixtures reject missing resources, user databases and escaping links.

`test-windows-installer.ps1` and `test-macos-installer.py` install, reinstall, remove, compare retained data hashes and reinstall again. They deliberately run only on disposable GitHub Actions runners. Do not run a system installation test against a personal installation or live data root.

Implementation-machine evidence (2026-10-23): macOS 27 Apple Silicon, universal build; backend and Qt adapter suites pass natively and under Rosetta x86_64. A copied development app launches with developer Qt paths absent; package/extracted-payload checks and negative fixtures pass. This is not native Intel hardware or minimum-macOS acceptance. The current Windows installer has not been executed on this Mac. Workflow lint and PowerShell syntax checks are separate from native execution.

Public release qualification remains open until actual Developer ID/Windows credentials, clean target-machine runs, offline installation, real-display scaling and keyboard/VoiceOver/Narrator checks are available. Prior manual waivers and old folder-package checks do not count as passes for these new installers.
