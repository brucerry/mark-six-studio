# Mark Six Studio

Unofficial local statistics and experimental forecasts. Not affiliated with HKJC; no betting or guaranteed predictions.

Run `mark-six-studio.exe` on a supported Windows x64 system. The package carries its Qt 6.10.2 shared runtime; the offline setup wizard installs the official Microsoft Visual C++ Redistributable when needed. A standalone copied folder may require that runtime separately. The app starts from the local cache and never downloads automatically. Use **Update history** to request public HKJC history; imported CSV remains unverified until independently corroborated.

The local archive lives at `%LOCALAPPDATA%\MarkSixEmulator\statistics` unless `--data-dir <directory>` is supplied for an isolated fixture. Keep a backup before replacing an earlier installation. No results database or user recording is included in this package.

See `licenses/Qt-Notice.txt`, the accompanying Qt license texts and SBOMs, and `licenses/SQLite.txt` for redistribution information. The previous folder package passed development-PC and Windows Server 2022 CI checks. The new installation wizard requires its own native CI and clean Windows 10/11 acceptance; older results do not qualify the installer. Files named unsigned are development artifacts.
