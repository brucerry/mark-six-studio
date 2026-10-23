import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "appUpdateDialog"
    required property var updater
    property bool installationBlocked: false
    title: "App updates"
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape
    onClosed: updater.cancel()
    onOpened: notes.forceActiveFocus()
    background: Rectangle {
        color: StudioTheme.surface
        border.color: StudioTheme.accent
        radius: StudioTheme.radius
    }
    header: Item {
        implicitHeight: 56
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: "App updates"
            color: StudioTheme.text
            font.pixelSize: 22
            font.weight: Font.DemiBold
        }
    }
    contentItem: ColumnLayout {
        spacing: 12
        Text {
            Layout.fillWidth: true
            text: "Installed: v" + dialog.updater.currentVersion +
                  (dialog.updater.latestVersion.length ? "  ·  Latest: v" + dialog.updater.latestVersion : "")
            color: StudioTheme.muted
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
        }
        Text {
            objectName: "appUpdateStatus"
            Layout.fillWidth: true
            text: dialog.updater.status
            color: dialog.updater.state === "error" ? StudioTheme.error : StudioTheme.accent
            font.pixelSize: 16
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            Accessible.name: text
        }
        ProgressBar {
            objectName: "appUpdateProgress"
            Layout.fillWidth: true
            visible: dialog.updater.busy && dialog.updater.state !== "opening"
            indeterminate: dialog.updater.progress < 0
            value: Math.max(0, dialog.updater.progress)
            Accessible.name: "Update progress"
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.updater.state === "downloading" && dialog.updater.progress >= 0
            text: (dialog.updater.receivedBytes / 1048576).toFixed(1) + " / " +
                  (dialog.updater.totalBytes / 1048576).toFixed(1) + " MB"
            color: StudioTheme.muted
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            TextArea {
                id: notes
                objectName: "appUpdateNotes"
                text: dialog.updater.releaseNotes.length ? dialog.updater.releaseNotes :
                      "Release notes appear here after checking for updates."
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                activeFocusOnTab: true
                Keys.priority: Keys.BeforeItem
                Keys.onTabPressed: (event) => {
                    if (dialog.updater.busy) cancelButton.forceActiveFocus()
                    else checkAgainButton.forceActiveFocus()
                    event.accepted = true
                }
                Keys.onBacktabPressed: (event) => {
                    closeButton.forceActiveFocus()
                    event.accepted = true
                }
                wrapMode: TextEdit.Wrap
                color: StudioTheme.text
                font.family: StudioTheme.fontFamily
                font.pixelSize: 14
                background: Rectangle {
                    color: StudioTheme.background
                    radius: 8
                    border.color: notes.activeFocus ? StudioTheme.gold : "transparent"
                    border.width: notes.activeFocus ? 2 : 0
                }
                Accessible.name: "Release notes"
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Installers are unsigned. Your OS may ask for approval. Your local history is kept."
            color: StudioTheme.muted
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.updater.canInstall
            text: dialog.installationBlocked ? "Wait for the current history operation to finish before installing." :
                  "Installing closes this app. Finish the installation wizard, then reopen Mark Six Studio."
            color: StudioTheme.gold
            wrapMode: Text.Wrap
        }
    }
    footer: Item {
        implicitHeight: 68
        RowLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            ActionButton {
                id: checkAgainButton
                objectName: "checkAppUpdatesAgainButton"
                text: "Check again"
                visible: !dialog.updater.busy
                onClicked: dialog.updater.checkForUpdates()
            }
            ActionButton {
                objectName: "viewAppReleaseButton"
                text: "View release"
                onClicked: dialog.updater.openReleasePage()
            }
            Item { Layout.fillWidth: true }
            ActionButton {
                objectName: "downloadAppUpdateButton"
                text: "Download update"
                primary: true
                visible: dialog.updater.canDownload
                onClicked: dialog.updater.downloadUpdate()
            }
            ActionButton {
                objectName: "installAppUpdateButton"
                text: "Install and close app"
                primary: true
                visible: dialog.updater.canInstall
                enabled: !dialog.installationBlocked
                onClicked: dialog.updater.openInstaller()
            }
            ActionButton {
                id: cancelButton
                text: "Cancel"
                visible: dialog.updater.state === "checking" || dialog.updater.state === "downloading"
                onClicked: dialog.updater.cancel()
            }
            ActionButton {
                id: closeButton
                text: "Close"
                onClicked: dialog.close()
            }
        }
    }
}
