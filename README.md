# Mark Six Studio

[![Windows build](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml)
[![macOS build](https://github.com/brucerry/mark-six-studio/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/brucerry/mark-six-studio/actions/workflows/macos-ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

Desktop application for historical draw analysis and experimental forecasts, using C++20, Qt 6.10.2 Quick/QML and SQLite 3.53.4.

## Installation

The delivery targets are Windows 10 (1809+)/11 x64 and macOS 13+ on Apple Silicon or Intel. Windows uses a setup EXE; macOS uses a native PKG installation wizard. Both carry application dependencies for offline installation. Windows setup installs the official Microsoft C++ runtime when needed and can request administrator permission. The macOS package installs **Mark Six Studio** in Applications.

GitHub releases use **unsigned installers**, with `unsigned` in each filename and a SHA-256 checksum for each download. No signing certificates, Apple Developer membership or signing service are required. The manual [release workflow](.github/workflows/release.yml) publishes only after both platforms pass their build, package and installation checks.

Unsigned downloads may show Windows publisher/SmartScreen prompts or require macOS approval in System Settings > Privacy & Security. Some security policies block unsigned apps. See [installation guidance and verification limits](installer/README.md).

## Build on Windows

Install Visual Studio C++ Build Tools and the Windows SDK. In PowerShell:

```powershell
./scripts/fetch-msvc.ps1
./scripts/fetch-sqlite.ps1 -Fetch
./scripts/fetch-statistics-qt.ps1 -Fetch
./scripts/build-statistics-desktop.ps1 -Tests
./scripts/package-statistics-desktop.ps1
./scripts/build-windows-installer.ps1 -PackageDirectory ./release/mark-six-studio
```

Run `build/statistics-desktop/mark-six-studio.exe`, or the copied application in `release/mark-six-studio`. The installer is under `release/windows-installer`. To verify a copied folder, run `./scripts/verify-statistics-package.ps1 -PackageDirectory ./release/mark-six-studio`. Build dependencies are pinned and checked; package output directories are never overwritten.

## Build on macOS

Install Xcode 26+ Command Line Tools, Python 3 and CMake 3.25 or newer. Then:

```sh
bash scripts/build-macos.sh
python3 scripts/package-macos.py
```

The build script fetches the pinned universal Qt kit and SQLite into `deps`, builds both architectures, and runs backend/Qt tests and a GUI smoke check. The installer and manifest are under `release/macos`. Use `--output-dir PATH` for another package candidate. The app runs from `build/macos/mark-six-studio.app` during development.

## Data and operation

The app opens to a six-number dashboard and provides model comparison, learning progress, draw history and data sources. Empty history shows unavailable forecasts and charts. Downloads occur only after selecting **Update history**; cached analysis works offline. Forecasts are experimental and do not establish future predictive advantage.

Windows retains `%LOCALAPPDATA%\MarkSixEmulator`; macOS uses `~/Library/Application Support/MarkSixStudio`. Databases are inside the `statistics` subdirectory. `--data-dir PATH` selects an isolated directory. Installers never bundle user databases; application upgrades and removal preserve these data directories.

Use Windows Installed apps to uninstall. On macOS, move `/Applications/Mark Six Studio.app` to Trash; the PKG does not create an uninstaller. Retained data remains available to a compatible reinstall.
