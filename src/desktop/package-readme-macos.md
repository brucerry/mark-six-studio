# Mark Six Studio for macOS

Open the PKG to install Mark Six Studio in Applications. The universal application supports Apple Silicon and Intel Macs running macOS 13 or later. Qt frameworks and QML resources are bundled; no developer SDK is needed.

The app starts from local storage and downloads history only when Update history is selected. User data is stored in `~/Library/Application Support/MarkSixStudio/statistics`; `--data-dir PATH` selects an isolated location. Installing or replacing the app never includes or removes user databases.

To remove the application, move `/Applications/Mark Six Studio.app` to Trash. Keep the Application Support directory to retain history for a later reinstall. Native PKG installation does not provide a separate uninstall wizard.

Files named `-unsigned.pkg` are development artifacts, not signed/notarized public releases. See the included licenses and SBOMs. Forecasts remain experimental and do not establish future predictive advantage.
