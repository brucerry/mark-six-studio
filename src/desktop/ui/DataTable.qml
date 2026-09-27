import QtQuick
import QtQuick.Controls

Item {
    id: root
    property var tableModel
    property int sortedColumn: -1
    property bool descending: false
    property var selectedId: -1
    property var columnWidths: ({})
    property int wheelEvents: 0
    implicitHeight: 360
    clip: true
    function columnWidth(column) {
        return columnWidths[column] || (column === 0 ? 120 : 172)
    }
    function resizeColumn(column, width) {
        const next = Object.assign({}, columnWidths)
        next[column] = Math.max(82, Math.min(650, Math.round(width)))
        columnWidths = next
        table.forceLayout()
    }

    Item {
        id: header
        anchors.left: table.left
        anchors.right: table.right
        anchors.top: parent.top
        height: 38
        clip: true
        Row {
            x: -table.contentX
            Repeater {
                model: root.tableModel ? root.tableModel.headers : []
                Rectangle {
                    required property int index
                    required property string modelData
                    width: root.columnWidth(index) + 1
                    height: 38
                    color: StudioTheme.raised
                    border.color: StudioTheme.border
                    Text {
                        anchors.fill: parent
                        anchors.margins: 8
                        text: modelData
                        color: StudioTheme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.rightMargin: 12
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.descending = root.sortedColumn === index ? !root.descending : false
                            root.sortedColumn = index
                            root.tableModel.sortBy(index, root.descending)
                        }
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 1
                        height: 20
                        color: StudioTheme.muted
                    }
                    MouseArea {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 12
                        cursorShape: Qt.SplitHCursor
                        preventStealing: true
                        property real startX: 0
                        property real startWidth: 0
                        onPressed: (mouse) => {
                            startX = mouse.x
                            startWidth = root.columnWidth(index)
                        }
                        onPositionChanged: (mouse) => {
                            if (pressed) root.resizeColumn(index, startWidth + mouse.x - startX)
                        }
                    }
                }
            }
        }
    }
    TableView {
        id: table
        objectName: "innerTable"
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.tableModel
        columnSpacing: 1
        rowSpacing: 1
        columnWidthProvider: function(column) { return root.columnWidth(column) }
        rowHeightProvider: function() { return 36 }
        WheelHandler {
            target: null
            blocking: true
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                root.wheelEvents += 1
                const vertical = event.pixelDelta.y || event.angleDelta.y / 120 * 54
                const horizontal = event.pixelDelta.x || event.angleDelta.x / 120 * 54
                if (Math.abs(horizontal) > Math.abs(vertical)) {
                    table.contentX = Math.max(0, Math.min(Math.max(0, table.contentWidth - table.width), table.contentX - horizontal))
                } else {
                    table.contentY = Math.max(0, Math.min(Math.max(0, table.contentHeight - table.height), table.contentY - vertical))
                }
                event.accepted = true
            }
        }
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        delegate: Rectangle {
            required property int row
            required property int column
            required property string display
            required property var stableId
            implicitWidth: root.columnWidth(column)
            implicitHeight: 36
            color: root.selectedId === stableId ? StudioTheme.raised : cellHover.hovered ? StudioTheme.hover : row % 2 ? StudioTheme.sidebar : StudioTheme.surface
            HoverHandler { id: cellHover }
            Text {
                anchors.fill: parent
                anchors.margins: 8
                text: display
                color: StudioTheme.text
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            MouseArea { anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.selectedId = stableId }
        }
    }
}
