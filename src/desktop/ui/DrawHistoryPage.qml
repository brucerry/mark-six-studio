import QtQuick
import QtQuick.Controls

Column {
    id: drawPage
    objectName: "drawHistoryPage"

    property var appWindow
    property int viewIndex: 0
    readonly property string eraFirstDate:
        statisticsLoader.eraFirstDates[eraChoice.currentIndex] || ""
    readonly property string eraLastDate:
        statisticsLoader.eraLastDates[eraChoice.currentIndex] || ""
    readonly property var chosenModel: [
        statisticsModels.archive,
        statisticsModels.analysis,
        statisticsModels.pairs,
        statisticsModels.rolling
    ][viewIndex]

    function applyFilters() {
        return statisticsLoader.setAnalysisSettings(
            scopeChoice.currentIndex,
            [0, 50, 100, 500][windowChoice.currentIndex],
            fromDate.selectedDate,
            throughDate.selectedDate,
            numberChoice.currentIndex + 1,
            eraChoice.currentIndex)
    }

    function clearDateFilters() {
        fromDate.clearDate()
        throughDate.clearDate()
        applyFilters()
    }

    width: parent.width
    visible: appWindow.pageIndex === 3
    spacing: 14

    Flow {
        width: parent.width
        spacing: 8
        Repeater {
            model: ["Draws", "Numbers", "Pairs", "Rolling"]
            ActionButton {
                required property int index
                required property string modelData
                text: modelData
                selected: drawPage.viewIndex === index
                onClicked: drawPage.viewIndex = index
            }
        }
    }
    Flow {
        width: parent.width
        spacing: 8
        ActionButton {
            objectName: "drawUpdateButton"
            text: "Update history"
            primary: true
            enabled: statisticsLoader.updateAvailable && !statisticsLoader.updateBusy &&
                !statisticsLoader.sourceWorkBusy && !statisticsLoader.importPreviewReady
            onClicked: statisticsLoader.startUpdate()
        }
        ActionButton {
            text: "Reload cache"
            enabled: !statisticsLoader.busy
            onClicked: statisticsLoader.reload()
        }
    }
    Flow {
        width: parent.width
        spacing: 8

        ComboBox {
            id: scopeChoice
            width: 150
            model: ["Six main", "Extra only", "All seven"]
            Accessible.name: "Analysis scope"
        }

        ComboBox {
            id: windowChoice
            width: 150
            model: ["All eligible", "Last 50", "Last 100", "Last 500"]
            Accessible.name: "Analysis window"
        }

        ComboBox {
            id: numberChoice
            width: 95
            model: 49
            displayText: "No. " + (currentIndex + 1)
            Accessible.name: "Rolling number"
        }

        ComboBox {
            id: eraChoice
            width: 205
            model: ["Current 49", "Earlier / unreviewed"]
            Accessible.name: "Browsing era"

            onActivated: {
                fromDate.clearDate()
                throughDate.clearDate()
            }
        }
    }

    Flow {
        width: parent.width
        spacing: 8

        DateFilterPicker {
            id: fromDate
            objectName: "fromDatePicker"
            label: "From"
            minimumDate: drawPage.eraFirstDate
            maximumDate: throughDate.selectedDate || drawPage.eraLastDate
        }

        DateFilterPicker {
            id: throughDate
            objectName: "throughDatePicker"
            label: "Through"
            minimumDate: fromDate.selectedDate || drawPage.eraFirstDate
            maximumDate: drawPage.eraLastDate
        }

        ActionButton {
            text: "Apply filters"
            enabled: !statisticsLoader.busy && drawPage.eraFirstDate !== ""
            onClicked: drawPage.applyFilters()
        }

        ActionButton {
            objectName: "clearDateFiltersButton"
            text: "Clear filters"
            accessibleLabel: "Clear date filters and apply"
            enabled: !statisticsLoader.busy &&
                (fromDate.selectedDate !== "" || throughDate.selectedDate !== "")
            onClicked: drawPage.clearDateFilters()
        }
    }

    Text {
        width: parent.width
        text: statisticsLoader.analysisStatus
        color: StudioTheme.muted
        wrapMode: Text.Wrap
    }

    Text {
        width: parent.width
        text: drawPage.chosenModel.summary
        color: StudioTheme.text
        wrapMode: Text.Wrap
    }

    DataTable {
        id: drawTable
        objectName: "drawTable"
        width: parent.width
        height: 440
        tableModel: drawPage.chosenModel
    }

    SelectableText {
        width: parent.width
        text: drawTable.selectedId >= 0 ?
            drawPage.chosenModel.detailForId(drawTable.selectedId) :
            "Select a row to inspect its exact values and source."
    }
}
