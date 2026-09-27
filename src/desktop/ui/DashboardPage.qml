import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Column {
    property var appWindow
    width: parent.width
    visible: appWindow.pageIndex === 0
    spacing: 16
    Flow {
        width: parent.width
        spacing: 8
        ActionButton { objectName: "updateButton"; text: statisticsLoader.updateBusy ? "Updating…" : "Update history"; primary: true; enabled: statisticsLoader.updateAvailable && !statisticsLoader.updateBusy; onClicked: statisticsLoader.startUpdate() }
        ActionButton { objectName: "reloadButton"; text: "Reload cache"; enabled: !statisticsLoader.busy; onClicked: statisticsLoader.reload() }
    }
    Text { text: "Six main numbers · Forecast Lab"; font.pixelSize: 16; color: StudioTheme.muted }
    GridLayout {
        objectName: "predictionTiles"
        width: parent.width
        columns: appWindow.compact ? 3 : 6
        columnSpacing: 8
        rowSpacing: 8
        Repeater {
            model: 6
            Rectangle {
                required property int index
                objectName: "predictionTile"
                Layout.fillWidth: true
                Layout.preferredHeight: 108
                color: StudioTheme.surface
                border.color: StudioTheme.border
                radius: StudioTheme.radius
                Column {
                    anchors.centerIn: parent
                    spacing: 4
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "MAIN " + (index + 1)
                        color: StudioTheme.muted
                        font.pixelSize: 12
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: statisticsLoader.selectionAvailable ?
                              String(statisticsLoader.suggestedMain[index]).padStart(2, "0") : "—"
                        color: StudioTheme.gold
                        font.pixelSize: 39
                        font.weight: Font.DemiBold
                    }
                }
                Accessible.role: Accessible.StaticText
                Accessible.name: "Suggested main number " + (index + 1) + ": " +
                    (statisticsLoader.selectionAvailable ? statisticsLoader.suggestedMain[index] : "unavailable")
            }
        }
    }
    Rectangle {
        objectName: "forecastStatus"
        width: parent.width
        height: forecastBody.implicitHeight + 32
        radius: StudioTheme.radius
        color: StudioTheme.surface
        border.color: StudioTheme.border
        Column {
            id: forecastBody
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 6
            Text { text: "MODEL AND EVIDENCE"; color: StudioTheme.accent; font.pixelSize: 12; font.weight: Font.Bold }
            Text { width: parent.width; text: statisticsLoader.selectionState; color: StudioTheme.text; font.pixelSize: 17; wrapMode: Text.Wrap }
            Text { width: parent.width; text: statisticsLoader.selectedModel.length ? statisticsLoader.selectedModel + " · Cutoff " + statisticsLoader.cutoff : "No compatible forecast available"; color: StudioTheme.muted; wrapMode: Text.Wrap }
            Text { width: parent.width; text: statisticsLoader.evidence.length ? statisticsLoader.evidence : "Model evidence is not loaded yet."; color: StudioTheme.muted; wrapMode: Text.Wrap }
        }
    }
    Rectangle {
        width: parent.width
        height: sourceBody.implicitHeight + 32
        radius: StudioTheme.radius
        color: StudioTheme.surface
        border.color: StudioTheme.border
        Column {
            id: sourceBody
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 6
            Text { text: "SOURCE STATUS"; color: StudioTheme.accent; font.pixelSize: 12; font.weight: Font.Bold }
            Text { width: parent.width; text: statisticsLoader.sourceStatus.length ? statisticsLoader.sourceStatus : "Loading local history…"; color: StudioTheme.text; wrapMode: Text.Wrap }
            Text { width: parent.width; visible: statisticsLoader.error.length > 0; text: statisticsLoader.error; color: StudioTheme.error; wrapMode: Text.Wrap }
            Text { width: parent.width; visible: statisticsLoader.updateStatus.length > 0; text: statisticsLoader.updateStatus; color: StudioTheme.muted; wrapMode: Text.Wrap }
        }
    }
    Rectangle {
        width: parent.width
        height: 222
        radius: StudioTheme.radius
        color: StudioTheme.surface
        border.color: StudioTheme.border
        Column {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 5
            Text { text: "LEARNING PROGRESS"; color: StudioTheme.accent; font.pixelSize: 12; font.weight: Font.Bold }
            Text {
                width: parent.width
                text: statisticsLoader.progressPoints.length ?
                    "Recent " + statisticsLoader.progressPoints.length + " scored draws · zoomed axis · latest mean gain " +
                    Number(statisticsLoader.progressPoints[statisticsLoader.progressPoints.length - 1]).toFixed(6) :
                    "No scored learning history yet"
                color: StudioTheme.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            ProgressChart {
                objectName: "dashboardProgressChart"
                width: parent.width
                height: Math.max(90, parent.height - 65)
                primaryValues: statisticsLoader.progressPoints
                showZero: false
                firstLabel: "recent 100"
                primaryLabel: "Historical replay cumulative mean log gain"
            }
        }
    }
}
