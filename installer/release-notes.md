Mark Six Studio provides local draw analysis and experimental forecasts. Source code is available under GPLv3.

Download the Windows x64 setup EXE or the macOS universal PKG. These are **unsigned releases**: they have no verified publisher certificate, and the macOS package is not notarized. Signing accounts and paid certificates are not required by this project.

Windows may show an unknown-publisher or SmartScreen prompt. macOS may require approval in System Settings > Privacy & Security after opening a trusted downloaded package or app. Organization policies and Smart App Control can block unsigned applications. See [Apple's instructions](https://support.apple.com/102445) and [Microsoft's explanation](https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation).

Each installer has a SHA-256 checksum asset. Download from this repository's GitHub Releases and compare that checksum before installation. The macOS manifest lists its packaged files. Checksums detect mismatched downloads; they do not authenticate a publisher.

Installers preserve local user databases during application upgrades and removal. Cached analysis works offline; history downloads start only when Update history is selected. Forecasts are experimental and do not establish future predictive advantage.

Automated checks cover builds, backend/Qt tests, package verification, installation, reinstall, removal and retained data hashes on Windows Server 2022 and macOS 15 CI. Separate minimum-OS, real-display and manual screen-reader acceptance remain unverified.
