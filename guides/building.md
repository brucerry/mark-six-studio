# Building and packaging

Mark Six Studio uses C++20, Qt 6.10.2 Quick/QML and SQLite 3.53.4. Dependency scripts fetch pinned, hash-checked downloads into `deps/`. Build and release output stays outside tracked source files.

## Get the source

```sh
git clone https://github.com/brucerry/mark-six-studio.git
cd mark-six-studio
```

## Windows

Install Visual Studio C++ Build Tools and the Windows SDK. In PowerShell:

```powershell
./scripts/fetch-msvc.ps1
./scripts/fetch-sqlite.ps1 -Fetch
./scripts/fetch-statistics-qt.ps1 -Fetch
./scripts/build-statistics-desktop.ps1 -Tests
```

Run `build/statistics-desktop/mark-six-studio.exe`. To create a copied application folder and offline setup wizard:

```powershell
./scripts/package-statistics-desktop.ps1
./scripts/verify-statistics-package.ps1 -PackageDirectory ./release/mark-six-studio
./scripts/build-windows-installer.ps1 -PackageDirectory ./release/mark-six-studio
```

The copied application is in `release/mark-six-studio`; the installer is in `release/windows-installer`.

## macOS

Install Xcode 26+ Command Line Tools, Python 3 and CMake 3.25 or newer:

```sh
bash scripts/build-macos.sh
python3 scripts/package-macos.py
```

The build script fetches the universal Qt kit and SQLite, builds Apple Silicon and Intel binaries, and runs backend/Qt tests and a GUI smoke check. The development app is `build/macos/mark-six-studio.app`; the installer and manifest are in `release/macos`.

## Packaging and releases

Package output directories are never overwritten. Use Windows `-OutputDirectory PATH` or macOS `--output-dir PATH` to create another candidate.

See [installer maintenance](../installer/README.md) for release tags, publication checks, package verification, unsigned installation approval and tested-platform limits.
