# Mark Six Studio

Unofficial local statistics and experimental forecasts. Not affiliated with HKJC; no betting or guaranteed predictions.

Run `mark-six-studio.exe` on a supported Windows x64 system. The package carries its Qt 6.10.2 shared runtime; the official Microsoft Visual C++ Redistributable may be needed separately. The app starts from the local cache and never downloads automatically. Use **Update history** to request public HKJC history; imported CSV remains unverified until independently corroborated.

The local archive lives at `%LOCALAPPDATA%\MarkSixEmulator\statistics` unless `--data-dir <directory>` is supplied for an isolated fixture. Keep a backup before replacing an earlier installation. No results database or user recording is included in this package.

See `licenses/Qt-Notice.txt`, the accompanying Qt license texts and SBOMs, and `licenses/SQLite.txt` for redistribution information. The package has passed development-PC and Windows Server 2022 CI checks; separate Windows 10 and Windows 11 acceptance remains pending.
