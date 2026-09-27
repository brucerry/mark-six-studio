import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Column {
    property var appWindow
    objectName: "learningPage"
    id: learningPage
    width: parent.width
    visible: appWindow.pageIndex === 2
    spacing: 16
    property bool progressView: false
    Flow {
        width: parent.width
        spacing: 8
        ActionButton { text: "Learning records"; selected: !learningPage.progressView; onClicked: learningPage.progressView = false }
        ActionButton { text: "Learning progress"; selected: learningPage.progressView; onClicked: learningPage.progressView = true }
    }
    Text {
        width: parent.width
        text: learningPage.progressView ? statisticsModels.progress.summary : statisticsModels.learning.summary
        color: StudioTheme.text
        wrapMode: Text.Wrap
    }
    Column {
        width: parent.width
        visible: !learningPage.progressView
        spacing: 12
        Text { text: "History version / retained lineage"; color: StudioTheme.muted }
        ComboBox {
            width: Math.min(parent.width, 580)
            model: statisticsLoader.learningLineages
            currentIndex: statisticsLoader.learningLineageIndex
            enabled: !statisticsLoader.busy
            onActivated: statisticsLoader.chooseLearningLineage(currentIndex)
            Accessible.name: "Learning history lineage"
        }
        Flow {
            width: parent.width
            spacing: 8
            ActionButton { text: "First 100"; enabled: !statisticsLoader.busy && statisticsLoader.learningOffset > 0; onClicked: statisticsLoader.pageLearning(0) }
            ActionButton { text: "Previous 100"; enabled: !statisticsLoader.busy && statisticsLoader.learningOffset > 0; onClicked: statisticsLoader.pageLearning(1) }
            ActionButton { text: "Next 100"; enabled: !statisticsLoader.busy && statisticsLoader.learningOffset + 100 < statisticsLoader.learningCount; onClicked: statisticsLoader.pageLearning(2) }
            ActionButton { text: "Last 100"; enabled: !statisticsLoader.busy && statisticsLoader.learningOffset < Math.max(0, statisticsLoader.learningCount - 100); onClicked: statisticsLoader.pageLearning(3) }
        }
        Text {
            text: statisticsLoader.learningCount ? "Rows " + (statisticsLoader.learningOffset + 1) + "–" + Math.min(statisticsLoader.learningCount, statisticsLoader.learningOffset + 100) + " of " + statisticsLoader.learningCount : "No learning records"
            color: StudioTheme.muted
        }
        DataTable { id: learningTable; width: parent.width; height: 380; tableModel: statisticsModels.learning }
        SelectableText {
            width: parent.width
            text: learningTable.selectedId >= 0 ? statisticsModels.learning.detailForId(learningTable.selectedId) : "Select a row to inspect exact scores and evidence type."
        }
    }
    Column {
        width: parent.width
        visible: learningPage.progressView
        spacing: 12
        Flow {
            width: parent.width
            spacing: 8
            ComboBox {
                id: progressEvidenceChoice
                width: 240
                model: ["Historical replay", "Saved before local fetch"]
                currentIndex: statisticsLoader.progressEvidence
                onActivated: statisticsLoader.setProgressView(currentIndex, progressModeChoice.currentIndex)
                Accessible.name: "Learning progress evidence type"
            }
            ComboBox {
                id: progressModeChoice
                width: 260
                model: ["Rolling log error", "Cumulative log gain", "Rolling Brier error", "Cumulative Brier gain"]
                currentIndex: statisticsLoader.progressMode
                onActivated: statisticsLoader.setProgressView(progressEvidenceChoice.currentIndex, currentIndex)
                Accessible.name: "Learning progress metric"
            }
        }
        Rectangle {
            width: parent.width
            height: 220
            color: StudioTheme.surface
            border.color: StudioTheme.border
            radius: StudioTheme.radius
            ProgressChart {
                anchors.fill: parent
                anchors.margins: 16
                primaryValues: statisticsModels.progress.chartValues
                comparisonValues: statisticsModels.progress.chartOther
                showZero: statisticsModels.progress.chartZero
                primaryLabel: statisticsModels.progress.chartTitle
            }
        }
        Text {
            width: parent.width
            visible: statisticsModels.progress.chartOther.length > 1
            text: "Teal: selected model  ·  Gold dashed: comparison  ·  Exact values below"
            color: StudioTheme.muted
            font.pixelSize: 12
        }
        Text { width: parent.width; text: statisticsModels.progress.chartTitle; color: StudioTheme.muted; wrapMode: Text.Wrap }
        DataTable { id: progressTable; width: parent.width; height: 360; tableModel: statisticsModels.progress }
        SelectableText { width: parent.width; text: progressTable.selectedId >= 0 ? statisticsModels.progress.detailForId(progressTable.selectedId) : "Select a row to inspect its exact values." }
    }
}
