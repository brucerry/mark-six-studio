# Mark Six Studio

Unofficial local statistics and experimental forecasts. Not affiliated with HKJC; no betting or guaranteed predictions.

Run `mark-six-studio.exe` on Windows 10 version 1809 or newer, or Windows 11 x64. The package carries its Qt 6.10.2 shared runtime; the offline setup wizard installs the official Microsoft Visual C++ Redistributable when needed. A standalone copied folder may require that runtime separately. The app starts from the local cache and never downloads automatically. Use **Update history** to request public HKJC history; imported CSV remains unverified until independently corroborated.

The local archive lives at `%LOCALAPPDATA%\MarkSixEmulator\statistics` unless `--data-dir <directory>` is supplied for an isolated fixture. Keep a backup before replacing an earlier installation. No results database or user recording is included in this package.

See `licenses/Qt-Notice.txt`, the accompanying Qt license texts and SBOMs, and `licenses/SQLite.txt` for redistribution information. This project publishes unsigned installers on GitHub with SHA-256 checksums. No signing account is required. Windows can show publisher or SmartScreen prompts; device policies may block unsigned apps. Download from the project's GitHub Releases and compare its checksum. Automated Windows Server 2022 build/package/install checks pass; separate Windows 10/11 and manual screen-reader acceptance remain unverified.
