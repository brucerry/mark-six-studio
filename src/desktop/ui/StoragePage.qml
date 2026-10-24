import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Column {
    id: storagePage
    property var appWindow
    visible: appWindow.pageIndex === 5
    spacing: 14
    onVisibleChanged: if (visible) appStorage.refresh(false)
    FolderDialog {
        id: additionalFolder
        property bool downloads: false
        property string account: ""
        title: downloads ? "Review another downloads folder" : "Review another saved-data folder"
        onAccepted: appStorage.addLocation(selectedFolder, downloads, account)
    }
    Dialog {
        id: cleanupPrompt
        objectName: "cleanupReviewDialog"
        property int operation: 0
        title: operation === 0 ? "Close app to review cache cleanup" : operation === 1 ? "Close app to review saved-data reset" : "Close app to review complete uninstall"
        modal: true
        focus: true
        enter: Transition {}
        exit: Transition {}
        anchors.centerIn: Overlay.overlay
        width: Math.min(appWindow.width - 32, 620)
        height: Math.min(appWindow.height - 32, 540)
        contentItem: ScrollView {
            id: cleanupScopeScroll
            clip: true
            contentWidth: availableWidth
            SelectableText {
                objectName: "cleanupScopeText"
                width: cleanupScopeScroll.availableWidth
                text: (cleanupPrompt.operation === 0
                ? "Clear regenerable caches for this OS account. Saved records, learning history, legacy recordings and browser downloads are preserved."
                : cleanupPrompt.operation === 1 ? "Permanently remove saved records and learning history from the active root, including migration recovery copies. This cannot be undone. Other data roots and legacy recordings are preserved.\n\nActive root: " + appStorage.activeRoot
                : "Permanently uninstall Mark Six Studio, including validated app storage for ALL reviewed OS accounts and identified downloaded installers. This cannot be undone. Legacy recordings, unrelated files and development checkouts are preserved.\n\nNative administrator authorization is required for inspection. The final review lists inaccessible accounts and any remaining work. Additional data and downloads folders selected on this page are included. Release identity verification contacts GitHub; no installer is downloaded or run.")
                + "\n\nThe app will close its workers and files. A native review will then list the exact files to remove and the full paths, accounts and reasons for preserved legacy files. You can cancel there before deletion starts."
                + (cleanupPrompt.operation === 2 ? " The native result reports completed or partial removal and provides a standalone retry helper if needed." : " The final result can reopen the app.")
            }
        }
        footer: DialogButtonBox {
            Button { text: "Close app and review"; Accessible.name: text; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { id: cancelCleanup; objectName: "cancelCleanupButton"; text: "Cancel"; Accessible.name: text; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onAccepted: cleanupPrompt.accept()
            onRejected: cleanupPrompt.reject()
        }
        onOpened: cancelCleanup.forceActiveFocus()
        onAccepted: appMaintenance.start(operation)
    }
    SelectableText { width: parent.width; tone: StudioTheme.text; text: "Active saved-data root: " + appStorage.activeRoot }
    SelectableText { objectName: "storageStatus"; width: parent.width; text: appStorage.status }
    SelectableText { width: parent.width; visible: appStorage.registrationNotice.length > 0; text: appStorage.registrationNotice }
    RowLayout {
        width: parent.width
        Text { text: "Account for additional folders"; color: StudioTheme.text }
        ComboBox {
            id: locationAccount
            objectName: "additionalLocationAccount"
            activeFocusOnTab: true
            Layout.fillWidth: true
            model: appStorage.accounts
            textRole: "label"
            valueRole: "account"
            Accessible.name: "OS account for additional folders. Inspect all accounts to select another profile."
        }
    }
    Flow {
        width: parent.width
        spacing: 8
        ActionButton { objectName: "refreshStorageButton"; text: "Refresh my storage"; enabled: !appStorage.busy; onClicked: appStorage.refresh(false) }
        ActionButton { text: "Inspect all OS accounts"; enabled: !appStorage.busy; onClicked: appStorage.refresh(true) }
        ActionButton { text: "Review another data folder"; enabled: !appStorage.busy; onClicked: { additionalFolder.downloads = false; additionalFolder.account = locationAccount.currentValue || ""; additionalFolder.open() } }
        ActionButton { text: "Review another downloads folder"; enabled: !appStorage.busy; onClicked: { additionalFolder.downloads = true; additionalFolder.account = locationAccount.currentValue || ""; additionalFolder.open() } }
    }
    Repeater {
        model: appStorage.categories
        delegate: SelectableText {
            required property var modelData
            width: storagePage.width
            text: modelData.name + ": " + modelData.size + " · " + modelData.files + " validated files" + (modelData.complete ? "" : " · incomplete inspection")
        }
    }
    Text { text: "Storage locations"; font.pixelSize: 20; font.weight: Font.DemiBold; color: StudioTheme.text }
    SelectableText { width: parent.width; text: appStorage.downloadStatus }
    ActionButton {
        text: "Verify downloaded installers with GitHub"
        enabled: !appStorage.busy
        onClicked: appStorage.verifyDownloads()
    }
    Repeater {
        model: appStorage.locations
        delegate: Column {
            required property var modelData
            width: storagePage.width
            spacing: 4
            SelectableText { width: parent.width; text: modelData.kind + " · account " + (modelData.account || "shared installation") + "\n" + modelData.path + (modelData.available ? "" : "\nUnavailable or absent — not counted as empty") }
            ActionButton { text: modelData.openLabel; accessibleLabel: text + " " + modelData.openPath; enabled: modelData.available; onClicked: appStorage.openFolder(modelData.path) }
        }
    }
    Text { text: "Preserved legacy files"; font.pixelSize: 20; font.weight: Font.DemiBold; color: StudioTheme.gold }
    SelectableText {
        objectName: "preservedLegacyFiles"
        width: parent.width
        tone: StudioTheme.text
        text: appStorage.preservedReport.length ? appStorage.preservedReport : "Inspect storage to list preserved files."
        Accessible.name: "Preserved legacy files. " + text
    }
    Text { text: "Inspection coverage"; font.pixelSize: 20; font.weight: Font.DemiBold; color: StudioTheme.text }
    SelectableText { width: parent.width; text: appStorage.coverage }
    Text { text: "Cleanup actions"; font.pixelSize: 20; font.weight: Font.DemiBold; color: StudioTheme.text }
    SelectableText { width: parent.width; text: "Cleanup closes the app for a final native review. Complete uninstall also requires normal OS administrator authorization and covers all reviewed accounts. Legacy recordings are always preserved and listed in the result." }
    SelectableText { objectName: "maintenanceStatus"; width: parent.width; text: appMaintenance.status }
    Flow {
        width: parent.width
        spacing: 8
        ActionButton {
            objectName: "clearCacheButton"
            text: "Clear cache"
            enabled: appMaintenance.available && !appMaintenance.busy && !appStorage.busy && !appWindow.taskBlocking && !appUpdater.busy
            onClicked: { cleanupPrompt.operation = 0; cleanupPrompt.open() }
        }
        ActionButton {
            objectName: "resetSavedRecordsButton"
            text: "Reset saved records and learning history"
            enabled: appMaintenance.available && !appMaintenance.busy && !appStorage.busy && !appWindow.taskBlocking && !appUpdater.busy
            onClicked: { cleanupPrompt.operation = 1; cleanupPrompt.open() }
        }
        ActionButton {
            objectName: "uninstallCompletelyButton"
            text: "Uninstall completely"
            enabled: appMaintenance.available && !appMaintenance.busy && !appStorage.busy && !appWindow.taskBlocking && !appUpdater.busy
            onClicked: { cleanupPrompt.operation = 2; cleanupPrompt.open() }
        }
    }
}
