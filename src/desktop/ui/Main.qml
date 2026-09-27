import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "statisticsWindow"
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: "Mark Six Studio"
    color: StudioTheme.background
    font.family: StudioTheme.fontFamily
    property int pageIndex: 0
    property int comparisonIndex: 0
    property bool reducedMotion: false
    property string pendingWebsite: ""
    readonly property bool compact: width < 1000
    readonly property bool taskBlocking: statisticsLoader.busy || statisticsLoader.updateBusy ||
        statisticsLoader.sourceWorkBusy || statisticsLoader.controlBusy
    readonly property var destinations: ["Dashboard", "Model comparison", "Learning history", "Draw history", "Data sources"]
    function openWebsite(address) {
        if (!address.toString().startsWith("https://")) return
        pendingWebsite = address.toString()
        licenseDialog.close()
        websiteDialog.open()
    }

    Rectangle {
        id: sidebar
        objectName: "sidebar"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.compact ? 76 : 230
        color: StudioTheme.sidebar
        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            Repeater {
                model: root.destinations
                ActionButton {
                    required property int index
                    required property string modelData
                    width: parent.width
                    text: root.compact ? String(index + 1).padStart(2, "0") : modelData
                    selected: root.pageIndex === index
                    Accessible.name: modelData
                    ToolTip.visible: hovered && root.compact
                    ToolTip.text: modelData
                    onClicked: root.pageIndex = index
                }
            }
            Item {
                width: 1
                height: 10
            }
            Rectangle {
                width: parent.width
                height: 1
                color: StudioTheme.border
            }
            ActionButton {
                id: licenseButton
                objectName: "licenseButton"
                width: parent.width
                text: root.compact ? "L" : "License"
                accessibleLabel: "License"
                ToolTip.visible: hovered && root.compact
                ToolTip.text: "License"
                onClicked: licenseDialog.open()
            }
        }
    }

    Flickable {
        id: content
        objectName: "content"
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
        clip: true
        contentWidth: width
        contentHeight: page.height + 48
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        Column {
            id: page
            x: 24
            y: 24
            width: content.width - 48
            spacing: 16
            RowLayout {
                width: parent.width
                Text {
                    Layout.fillWidth: true
                    text: root.destinations[root.pageIndex]
                    font.pixelSize: 28
                    font.weight: Font.DemiBold
                    color: StudioTheme.text
                }
            }
            DashboardPage {
                width: parent.width
                appWindow: root
            }
            ModelComparisonPage {
                id: comparisonPage
                width: parent.width
                appWindow: root
            }
            LearningPage {
                id: learningPage
                width: parent.width
                appWindow: root
            }
            DrawHistoryPage {
                id: drawPage
                width: parent.width
                appWindow: root
            }
            DataSourcesPage {
                width: parent.width
                appWindow: root
            }
        }
    }
    Rectangle {
        id: busyCover
        objectName: "busyCover"
        anchors.fill: parent
        z: 90
        visible: root.taskBlocking
        color: "#dd0a1322"
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (event) => { event.accepted = true }
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 440)
            height: coverBody.implicitHeight + 52
            radius: 20
            color: StudioTheme.surface
            border.color: StudioTheme.accent
            border.width: 2
            Column {
                id: coverBody
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 26
                spacing: 14
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 68
                    height: 68
                    running: root.taskBlocking && !root.reducedMotion
                    visible: !root.reducedMotion
                }
                Text {
                    width: parent.width
                    text: "Please wait"
                    color: StudioTheme.text
                    font.pixelSize: 25
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                }
                Text {
                    width: parent.width
                    text: statisticsLoader.updateBusy ? "Updating source history" :
                          statisticsLoader.sourceWorkBusy ? "Checking or importing source records" :
                          statisticsLoader.controlBusy ? "Evaluating fair-history controls" : "Loading local analysis"
                    color: StudioTheme.muted
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }
                ActionButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: statisticsLoader.updateBusy || statisticsLoader.sourceWorkBusy || statisticsLoader.controlBusy
                    text: "Cancel operation"
                    onClicked: {
                        if (statisticsLoader.updateBusy) statisticsLoader.cancelUpdate()
                        else if (statisticsLoader.sourceWorkBusy) statisticsLoader.cancelSourceWork()
                        else if (statisticsLoader.controlBusy) statisticsLoader.cancelFairControls()
                    }
                }
            }
        }
    }
    Dialog {
        id: licenseDialog
        objectName: "licenseDialog"
        title: "License"
        modal: true
        width: Math.min(root.width - 32, 840)
        height: Math.min(root.height - 32, 650)
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.NoButton
        padding: 18
        background: Rectangle {
            color: StudioTheme.surface
            border.color: StudioTheme.accent
            border.width: 1
            radius: 18
        }
        header: Rectangle {
            implicitHeight: 54
            color: StudioTheme.surface
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "License"
                color: StudioTheme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
        }
        footer: ActionButton {
            text: "Close"
            onClicked: licenseDialog.close()
        }
        ScrollView {
            anchors.fill: parent
            clip: true
            ScrollBar.vertical.policy: ScrollBar.AlwaysOn
            Column {
                width: Math.max(300, licenseDialog.width - 74)
                spacing: 18
                Text {
                    width: parent.width
                    text: "SOFTWARE LICENSES"
                    color: StudioTheme.gold
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                SelectableText {
                    width: parent.width
                    text: "This interface uses Qt 6.10.2 shared libraries. In a packaged copy, see licenses/Qt-Notice.txt, LGPL-3.0-only.txt and GPL-3.0-only.txt for Qt attribution and terms. Qt is a trademark of The Qt Company."
                }
                Text {
                    width: parent.width
                    text: "UNOFFICIAL LOCAL STATISTICS"
                    color: StudioTheme.gold
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                SelectableText {
                    width: parent.width
                    tone: StudioTheme.text
                    text: "This app is not affiliated with HKJC. It does not place bets, guarantee predictions, or establish a next-draw advantage. A fair independent six-main draw gives each number 6/49 inclusion probability and a specified six-number set 1/13,983,816 probability. Historical rates do not change those fair odds."
                }
                Text {
                    text: "MODEL AND LEARNING"
                    color: StudioTheme.accent
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }
                SelectableText {
                    width: parent.width
                    tone: StudioTheme.text
                    text: "Forecast Lab uses a frozen candidate grid and chronological inner validation. Adaptive and fixed models are shown for comparison. All displayed scores were computed on previously inspected historical results; model rankings and suggested numbers are experimental estimates, not winning confidence. The next draw is unobserved."
                }
                SelectableText {
                    width: parent.width
                    text: "Current comparison: " + (root.comparisonIndex === 0 ?
                        statisticsModels.forecastLab.explanation : comparisonPage.chosenModel.explanation)
                }
                Text {
                    text: "DATA AND SOURCES"
                    color: StudioTheme.accent
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }
                SelectableText {
                    width: parent.width
                    text: "History is retained locally with source status and audit records. Imported CSV is unverified; earlier-era records and unresolved conflicts remain outside current-49 analysis. Completeness and freshness after the last recorded retrieval remain unknown. HKJC update is user-initiated only; cached analysis works offline."
                }
                SelectableText {
                    width: parent.width
                    text: statisticsModels.sources.explanation
                }
                SelectableText {
                    width: parent.width
                    text: statisticsModels.progress.explanation
                }
                SelectableText {
                    width: parent.width
                    text: drawPage.chosenModel.explanation
                }
                SelectableText {
                    width: parent.width
                    text: "Date filters narrow the Draws table and descriptive analysis. Window and scope filters affect analysis only. These filters never tune Forecast Lab, Adaptive or Fixed forecasts. Exact 6/49 and 7/49 baselines are theoretical, unchanged by past draws."
                }
                SelectableText {
                    width: parent.width
                    text: "Learning curves separate historical replay from locally saved pre-fetch evidence. A positive cumulative mean log gain favors Adaptive over uniform on those scored records; a negative gain favors uniform. This is descriptive, not proof of improving future accuracy."
                }
                SelectableText {
                    width: parent.width
                    text: statisticsLoader.progressSummary
                }
                Text {
                    text: "APP AND CONTROLS"
                    color: StudioTheme.accent
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }
                CheckBox {
                    text: "Reduced motion (replace activity spinner with text)"
                    checked: root.reducedMotion
                    onToggled: root.reducedMotion = checked
                    Accessible.name: text
                }
                SelectableText {
                    width: parent.width
                    text: "Keyboard: Tab and Shift+Tab move through controls; Space activates a focused button. Tables can be sorted by selecting a header and resized by dragging its right edge. Horizontal and vertical scrolling expose wide data; select a row for a text equivalent. Text can be highlighted and copied."
                }
                SelectableText {
                    width: parent.width
                    richText: true
                    text: "<a href='https://bet.hkjc.com/ch/marksix/home' style='color:#55d6c1'>Official HKJC Mark Six</a> · <a href='https://doc.qt.io/qt-6/licensing.html' style='color:#55d6c1'>Qt licensing</a>"
                    onLinkActivated: (link) => root.openWebsite(link)
                }
            }
        }
    }
    Dialog {
        id: websiteDialog
        objectName: "websiteDialog"
        title: "Open website"
        modal: true
        width: Math.min(root.width - 40, 540)
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Open | Dialog.Cancel
        background: Rectangle {
            color: StudioTheme.surface
            border.color: StudioTheme.accent
            radius: 14
        }
        onAccepted: Qt.openUrlExternally(root.pendingWebsite)
        Column {
            width: parent.width
            spacing: 10
            SelectableText {
                width: parent.width
                tone: StudioTheme.text
                text: "This link opens in your default web browser."
            }
            SelectableText {
                width: parent.width
                text: root.pendingWebsite
            }
        }
    }
}
