# Storage and cleanup

Open **Storage and cleanup** to inspect saved-data locations, caches, downloaded installers and preserved files.

## Choose an action

| Action | What it removes | What stays |
| --- | --- | --- |
| **Clear cache** | Regenerable caches and unused in-app update downloads for your OS account | Saved records, learning history, browser downloads and legacy recordings |
| **Reset saved records and learning history** | The active root's draw archive, learning databases, sidecars and validated migration copies | Other data roots, documents, browser downloads and legacy recordings |
| **Uninstall completely** | Validated installed copies, app storage for reviewed OS accounts, identified installers and matching OS registrations | Unrelated files, legacy recordings, developer checkouts and shared prerequisites |

Reset and complete uninstall are permanent. **Close app and review** closes workers and databases before a native dialog lists the exact files. Cancel before final approval to leave those targets untouched. Complete uninstall also requires normal OS administrator authorization; authorization alone does not approve deletion.

After reset, reopening starts with empty saved records and learning history. Select **Update history** when you want to fetch records again.

## Find your files

Saved databases are inside the `statistics` subdirectory of the data root:

| Location | Windows | macOS |
| --- | --- | --- |
| Default data root | `%LOCALAPPDATA%\MarkSixEmulator` | `~/Library/Application Support/MarkSixStudio` |
| Cache | `%LOCALAPPDATA%\Mark Six Studio\cache` | `~/Library/Caches/Mark Six Studio` |
| Browser downloads | Your resolved Downloads folder | `~/Downloads` |

`--data-dir PATH` selects a custom root. The page shows inspected locations and lets you open available folders. Use the additional-folder controls to review older custom roots or nonstandard download locations; choose the correct OS account first.

**Verify downloaded installers with GitHub** reads public release metadata to identify installers and their matching checksum/manifest files. This inspection does not download or run an installer. A matching filename alone is insufficient; uncertain files stay untouched. Packages still needed by an active or unconfirmed installation remain protected.

## Preserved files and coverage

**Preserved legacy files** lists each discovered `.msx` recording's full path, OS account and preservation reason. The native review and final result keep the list selectable and copyable. Parent folders remain when they contain preserved files. The application does not read the retired recording format.

Unrelated files and unknown payloads stay untouched. Inaccessible accounts, privacy restrictions, offline homes, unsupported metadata and unregistered locations are reported as coverage limits. An empty list refers only to inspected locations. Administrator authorization does not guarantee access to every account's files.

Ordinary Windows uninstall or removal of the macOS app bundle retains saved data. Use **Uninstall completely** for the broader reviewed cleanup. Complete uninstall removes approved files permanently; it does not move app bundles to Trash.

## Interrupted cleanup

Close other app instances and active native installers before retrying blocked work. If files change after review, inspect them again and approve the refreshed list.

An incomplete reset gates its data root and offers **Recover reset** on startup. Recovery restores the old generation before irreversible purge, or finishes the approved purge once that phase has begun. Conflicting files keep the root blocked.

A partial uninstall lists remaining work and the location of a standalone retry helper with `remaining.txt`. That helper works after the main app is gone and requests a fresh review. Close any process holding a remaining file, or restart the OS when required, then retry. A scheduled deletion or restart alone is not reported as completed removal.

Successful cleanup removes its temporary helper and recovery state. Intentionally preserved files do not count as deletion failures.
