import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Column {
    property var appWindow
    id: sourcesPage
    width: parent.width
    visible: appWindow.pageIndex === 4
    spacing: 14
    property url selectedCsv: ""
    property url selectedManifest: ""
    FileDialog {
        id: csvDialog
        title: "Choose results CSV"
        nameFilters: ["CSV files (*.csv)", "All files (*)"]
        onAccepted: sourcesPage.selectedCsv = selectedFile
    }
    FileDialog {
        id: manifestDialog
        title: "Choose import manifest JSON"
        nameFilters: ["JSON files (*.json)", "All files (*)"]
        onAccepted: sourcesPage.selectedManifest = selectedFile
    }
    Dialog {
        id: confirmResolution
        title: "Resolve conflicting result"
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay
        onAccepted: statisticsLoader.resolveConflict(sourceTable.selectedId,
            reasonField.text, evidenceField.text)
        Text {
            text: "Use this revision? Prior revisions and your reason/evidence remain stored. Unverified rows stay ineligible."
            width: 380
            wrapMode: Text.Wrap
            color: StudioTheme.text
        }
    }
    Flow {
        width: parent.width
        spacing: 8
        ActionButton { text: "Update history"; primary: true; enabled: !statisticsLoader.updateBusy && !statisticsLoader.sourceWorkBusy && !statisticsLoader.importPreviewReady; onClicked: statisticsLoader.startUpdate() }
        ActionButton { text: "Cancel update"; enabled: statisticsLoader.updateBusy; onClicked: statisticsLoader.cancelUpdate() }
        ActionButton { text: "Reload cache"; enabled: !statisticsLoader.busy; onClicked: statisticsLoader.reload() }
    }
    Text { width: parent.width; text: statisticsLoader.updateStatus; color: StudioTheme.muted; wrapMode: Text.Wrap }
    Text { width: parent.width; text: statisticsModels.sources.summary; color: StudioTheme.text; wrapMode: Text.Wrap }
    Text { text: "CSV IMPORT · unverified until separately corroborated"; color: StudioTheme.accent; font.weight: Font.Bold }
    Flow {
        width: parent.width
        spacing: 8
        ActionButton { text: "Choose CSV"; enabled: !statisticsLoader.sourceWorkBusy; onClicked: csvDialog.open() }
        ActionButton { text: "Choose manifest"; enabled: !statisticsLoader.sourceWorkBusy; onClicked: manifestDialog.open() }
        ActionButton { text: "Preview import"; enabled: sourcesPage.selectedCsv.toString().length > 0 && sourcesPage.selectedManifest.toString().length > 0 && !statisticsLoader.sourceWorkBusy && !statisticsLoader.updateBusy; onClicked: statisticsLoader.previewCsv(sourcesPage.selectedCsv, sourcesPage.selectedManifest) }
        ActionButton { text: "Accept import"; primary: true; enabled: statisticsLoader.importPreviewReady && !statisticsLoader.sourceWorkBusy; onClicked: statisticsLoader.acceptCsv() }
        ActionButton { text: "Cancel work"; enabled: statisticsLoader.sourceWorkBusy || statisticsLoader.importPreviewReady; onClicked: statisticsLoader.cancelSourceWork() }
    }
    Text { width: parent.width; text: "CSV: " + (sourcesPage.selectedCsv.toString().length ? sourcesPage.selectedCsv.toString() : "none") + " · Manifest: " + (sourcesPage.selectedManifest.toString().length ? sourcesPage.selectedManifest.toString() : "none"); color: StudioTheme.muted; wrapMode: Text.Wrap }
    Text { width: parent.width; text: statisticsLoader.sourceWorkStatus; color: StudioTheme.muted; wrapMode: Text.Wrap }
    DataTable { id: sourceTable; width: parent.width; height: 350; tableModel: statisticsModels.sources }
    SelectableText { width: parent.width; text: sourceTable.selectedId >= 0 ? statisticsModels.sources.detailForId(sourceTable.selectedId) : "Select a revision to inspect provenance and conflict status." }
    Text { text: "CONFLICT RESOLUTION"; color: StudioTheme.accent; font.weight: Font.Bold }
    Flow {
        width: parent.width
        spacing: 8
        TextField { id: reasonField; width: 280; placeholderText: "Reason for selection"; Accessible.name: "Resolution reason" }
        TextField { id: evidenceField; width: 350; placeholderText: "Source evidence"; Accessible.name: "Source evidence" }
        ActionButton { text: "Resolve selected"; enabled: sourceTable.selectedId >= 0 && reasonField.text.trim().length > 0 && evidenceField.text.trim().length > 0 && !statisticsLoader.sourceWorkBusy && !statisticsLoader.updateBusy; onClicked: confirmResolution.open() }
    }
}
