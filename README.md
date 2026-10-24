# Mark Six Studio

[![Windows build](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml)
[![macOS build](https://github.com/brucerry/mark-six-studio/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/brucerry/mark-six-studio/actions/workflows/macos-ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

Explore historical Mark Six draws, compare experimental models, and follow learning history on Windows and macOS.

---

## 🚀 Quick start

1. Download an installer from the **[latest release](https://github.com/brucerry/mark-six-studio/releases/latest)**:
   - **Windows 10 (1809+) / 11 x64:** choose the setup `.exe`.
   - **macOS 13+ · Apple Silicon or Intel:** choose the universal `.pkg`.
2. Run the installation wizard, then open **Mark Six Studio** from the Start menu or Applications.
3. Click **Update history**, then explore **Draw history**, **Model comparison**, and **Learning history**. Cached analysis works offline.

⚠️  Installers are **unsigned**, so your OS may ask for approval. See the [installation guide](installer/README.md) for help and checksums. Forecasts are experimental and do not establish a future predictive advantage.

### In-app updates

Use **Check for updates** in the app to download a newer installer. See [storage and cleanup](guides/storage-maintenance.md) for saved files, caches and removal options.

---

## 🛠️ Build

Clone this repository first. The app uses C++20, Qt and SQLite; scripts fetch the pinned dependencies.

### Windows

Install Visual Studio C++ Build Tools and the Windows SDK, then run in PowerShell:

```powershell
./scripts/fetch-msvc.ps1
./scripts/fetch-sqlite.ps1 -Fetch
./scripts/fetch-statistics-qt.ps1 -Fetch
./scripts/build-statistics-desktop.ps1 -Tests
```

Run `build/statistics-desktop/mark-six-studio.exe`.

### macOS

Install Xcode 26+ Command Line Tools, Python 3 and CMake 3.25+, then run:

```sh
bash scripts/build-macos.sh
```

Open `build/macos/mark-six-studio.app`.

---

## 📚 Docs

See [build and packaging details](guides/building.md), the [user guide](guides/usage.md), and [installer/release maintenance](installer/README.md).
