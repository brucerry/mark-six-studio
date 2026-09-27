import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Column {
    property var appWindow
    id: comparisonPage
    width: parent.width
    visible: appWindow.pageIndex === 1
    spacing: 16
    readonly property var chosenModel: [statisticsModels.labProbabilities, statisticsModels.adaptive,
        statisticsModels.fixed, statisticsModels.exact, statisticsModels.calibration][appWindow.comparisonIndex]
    Flow {
        width: parent.width
        spacing: 8
        Repeater {
            model: ["Forecast Lab", "Adaptive", "Fixed", "Fair odds", "Calibration"]
            ActionButton {
                required property int index
                required property string modelData
                text: modelData
                selected: appWindow.comparisonIndex === index
                onClicked: appWindow.comparisonIndex = index
            }
        }
    }
    Rectangle {
        width: parent.width
        height: comparisonBody.implicitHeight + 32
        color: StudioTheme.surface
        radius: StudioTheme.radius
        border.color: StudioTheme.border
        Column {
            id: comparisonBody
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            spacing: 8
            Text { text: appWindow.comparisonIndex === 3 ? "EXACT FAIR BASELINE" : "EXPERIMENTAL · NOT WINNING CONFIDENCE"; color: StudioTheme.accent; font.pixelSize: 12; font.weight: Font.Bold }
            Text {
                width: parent.width
                text: appWindow.comparisonIndex === 0 ? statisticsLoader.comparisonSummary :
                      comparisonPage.chosenModel.summary
                color: StudioTheme.text
                wrapMode: Text.Wrap
                font.pixelSize: 15
            }
            Text {
                width: parent.width
                visible: appWindow.comparisonIndex === 0
                text: statisticsLoader.selectedModel.length ? statisticsLoader.selectedModel + " · Training cutoff " + statisticsLoader.cutoff : "No compatible selection loaded"
                color: StudioTheme.muted
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                visible: appWindow.comparisonIndex === 1 || appWindow.comparisonIndex === 2
                text: appWindow.comparisonIndex === 1 ?
                      (statisticsLoader.adaptiveExtra ? "Suggested main: " + statisticsLoader.adaptiveMain.join("  ") + " · distinct Extra: " + statisticsLoader.adaptiveExtra + " · cutoff " + statisticsLoader.adaptiveCutoff : "Adaptive selection unavailable") :
                      (statisticsLoader.fixedExtra ? "Suggested main: " + statisticsLoader.fixedMain.join("  ") + " · distinct Extra: " + statisticsLoader.fixedExtra + " · cutoff " + statisticsLoader.fixedCutoff : "Fixed selection unavailable")
                color: StudioTheme.muted
                wrapMode: Text.Wrap
            }
        }
    }
    RowLayout {
        width: parent.width
        visible: appWindow.comparisonIndex === 0
        spacing: 8
        ActionButton {
            text: "Run fair-history controls"
            enabled: statisticsLoader.controlEligible && !statisticsLoader.controlBusy
            onClicked: statisticsLoader.startFairControls()
        }
    }
    Text {
        width: parent.width
        visible: appWindow.comparisonIndex === 0
        text: statisticsLoader.controlStatus
        color: StudioTheme.muted
        wrapMode: Text.Wrap
    }
    DataTable {
        objectName: "comparisonTable"
        width: parent.width
        height: 430
        tableModel: comparisonPage.chosenModel
    }
}
