# Mark Six Studio

[![Windows build](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/brucerry/mark-six-studio/actions/workflows/windows-ci.yml)

Windows desktop application for historical draw analysis and experimental forecasts.

---

## Tech stack

- C++20 domain and application code in `src/statistics/` and `src/desktop/`
- Qt 6.10.2 Quick/QML interface, built with MSVC 14.44 and CMake/Ninja
- SQLite 3.53.4 for local history and model ledgers
- Windows 10/11 x64 target; Qt's GPU renderer is used when available

---

## Build and run

Install Visual Studio C++ Build Tools and the Windows SDK. In PowerShell:

```powershell
./scripts/fetch-msvc.ps1
./scripts/fetch-sqlite.ps1 -Fetch
./scripts/fetch-statistics-qt.ps1 -Fetch
./scripts/build-statistics-desktop.ps1 -Tests
./build/statistics-desktop/mark-six-studio.exe
```

The dependency fetches are only needed for a new checkout. The build script verifies pinned dependencies and runs the backend/Qt tests when `-Tests` is supplied. The application is a Windows GUI executable; opening it does not create a console window.

To run the verified copied package, open `release/mark-six-studio/mark-six-studio.exe`. To create a new package, run `./scripts/package-statistics-desktop.ps1 -PackageDirectory ./release/mark-six-studio-candidate`, then verify it with `./scripts/verify-statistics-package.ps1 -PackageDirectory ./release/mark-six-studio-candidate`. The package script will not overwrite an existing directory.

The [Windows build workflow](.github/workflows/windows-ci.yml) runs on pushes to `main`, pull requests, and manual dispatch. It builds, tests, packages, verifies, and uploads a downloadable Windows x64 artifact. It does not publish a GitHub Release or include local draw data.

---

## Expected result

The app opens to a six-number dashboard and can switch among model comparison, learning progress, draw history, and data sources. Without local history, forecasts and charts show an unavailable state. History and model data are stored under `%LOCALAPPDATA%\MarkSixEmulator` by default; `--data-dir PATH` selects a separate directory. Data downloads occur only after the user selects **Update history**.
