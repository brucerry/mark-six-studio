# Using Mark Six Studio

## First launch

Open the app and select **Update history** to fetch source records. The dashboard starts empty until compatible history is available; unavailable forecasts are shown explicitly. Cached analysis works offline.

- **Dashboard:** six-number experimental forecast and learning progress.
- **Draw history:** inspect records and narrow the table with date filters.
- **Model comparison:** compare Forecast Lab, Adaptive and Fixed models.
- **Learning history:** review recorded model scores over time.
- **Data sources:** inspect provenance, coverage and imports.

Forecasts are experimental and do not establish future predictive advantage. This app does not place bets or guarantee results.

## Updates

Starting with v0.1.1, select **Check for updates** in the sidebar:

1. Read the available version and release notes, then select **Download update**.
2. Wait for the download and checksum verification. You can cancel and try again later.
3. Select **Install and close app**, finish the native installation wizard, then reopen Mark Six Studio.

The app checks only when you ask. Installers remain unsigned and normal OS approval may be required. Local history is retained. A failed or cancelled download never starts an installer.

**Still using v0.1.0?** Install the first release containing in-app updates manually from the [latest release](https://github.com/brucerry/mark-six-studio/releases/latest). You can also use this manual route any time.

## Your data

Databases are kept in the `statistics` subdirectory of:

- Windows: `%LOCALAPPDATA%\MarkSixEmulator`
- macOS: `~/Library/Application Support/MarkSixStudio`

Use `--data-dir PATH` to select an isolated directory. Installers do not bundle or replace user databases. Keep backups of important local records.

## Remove the app

Use Windows **Installed apps**, or move **Mark Six Studio.app** from macOS Applications to Trash. The macOS PKG does not install a separate uninstaller. Local data remains available to a compatible reinstall.

For OS approval prompts and checksum verification, see the [installation guide](../installer/README.md).
