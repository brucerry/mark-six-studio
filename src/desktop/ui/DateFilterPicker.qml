import QtQuick
import QtQuick.Controls

Item {
    id: picker

    property string label: "Date"
    property string minimumDate: ""
    property string maximumDate: ""
    property string selectedDate: ""
    readonly property bool hasDates: minimumDate !== "" && maximumDate !== ""

    width: 190
    height: 44

    function parseIso(value) {
        const parts = value.split("-")
        return new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]))
    }

    function monthKey(year, month) {
        return year * 12 + month
    }

    function chooseDate(date) {
        return chooseIso(Qt.formatDate(date, "yyyy-MM-dd"))
    }

    function chooseIso(iso) {
        if (!/^\d{4}-\d{2}-\d{2}$/.test(iso) ||
                Qt.formatDate(parseIso(iso), "yyyy-MM-dd") !== iso ||
                !hasDates || iso < minimumDate || iso > maximumDate) {
            return false
        }

        selectedDate = iso
        calendar.close()
        return true
    }

    function clearDate() {
        selectedDate = ""
        calendar.close()
    }

    onMinimumDateChanged: {
        if (selectedDate !== "" && selectedDate < minimumDate) {
            clearDate()
        }
    }

    onMaximumDateChanged: {
        if (selectedDate !== "" && selectedDate > maximumDate) {
            clearDate()
        }
    }

    ActionButton {
        id: dateButton

        anchors.fill: parent
        text: picker.label + ": " + (picker.selectedDate || "Any date") + "  ▾"
        accessibleLabel: picker.label + " date filter, " +
            (picker.selectedDate || "any date")
        enabled: picker.hasDates

        onClicked: calendar.open()
    }

    Popup {
        id: calendar
        objectName: "dateCalendar"

        property int viewYear: 2000
        property int viewMonth: 0
        readonly property int earliestMonth: picker.hasDates ?
            picker.monthKey(picker.parseIso(picker.minimumDate).getFullYear(),
                            picker.parseIso(picker.minimumDate).getMonth()) : 0
        readonly property int latestMonth: picker.hasDates ?
            picker.monthKey(picker.parseIso(picker.maximumDate).getFullYear(),
                            picker.parseIso(picker.maximumDate).getMonth()) : 0

        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        modal: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 16
        width: 340

        onAboutToShow: {
            const initial = picker.parseIso(picker.selectedDate || picker.maximumDate)
            viewYear = initial.getFullYear()
            viewMonth = initial.getMonth()
        }

        background: Rectangle {
            color: StudioTheme.surface
            border.color: StudioTheme.accent
            radius: StudioTheme.radius
        }

        contentItem: Column {
            spacing: 8

            Row {
                width: parent.width
                spacing: 8

                ActionButton {
                    text: "‹"
                    width: 44
                    accessibleLabel: "Previous month"
                    enabled: calendar.monthKey(calendar.viewYear, calendar.viewMonth) >
                        calendar.earliestMonth

                    onClicked: {
                        const previous = new Date(calendar.viewYear,
                                                  calendar.viewMonth - 1, 1)
                        calendar.viewYear = previous.getFullYear()
                        calendar.viewMonth = previous.getMonth()
                    }
                }

                Text {
                    width: parent.width - 104
                    height: 44
                    text: Qt.formatDate(new Date(calendar.viewYear,
                                                 calendar.viewMonth, 1), "MMMM yyyy")
                    color: StudioTheme.text
                    font.pixelSize: 17
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                ActionButton {
                    text: "›"
                    width: 44
                    accessibleLabel: "Next month"
                    enabled: calendar.monthKey(calendar.viewYear, calendar.viewMonth) <
                        calendar.latestMonth

                    onClicked: {
                        const next = new Date(calendar.viewYear,
                                              calendar.viewMonth + 1, 1)
                        calendar.viewYear = next.getFullYear()
                        calendar.viewMonth = next.getMonth()
                    }
                }
            }

            Row {
                width: parent.width
                spacing: 10

                Text {
                    width: 44
                    height: 40
                    text: "Year"
                    color: StudioTheme.muted
                    verticalAlignment: Text.AlignVCenter
                }

                ComboBox {
                    width: parent.width - 54
                    model: {
                        if (!picker.hasDates) {
                            return []
                        }

                        const first = picker.parseIso(picker.minimumDate).getFullYear()
                        const last = picker.parseIso(picker.maximumDate).getFullYear()
                        return Array.from({ length: last - first + 1 },
                                          (_, index) => first + index)
                    }
                    currentIndex: picker.hasDates ?
                        calendar.viewYear - picker.parseIso(picker.minimumDate).getFullYear() : -1
                    Accessible.name: "Calendar year"

                    onActivated: {
                        calendar.viewYear = Number(currentText)
                        const current = calendar.monthKey(calendar.viewYear,
                                                          calendar.viewMonth)
                        if (current < calendar.earliestMonth) {
                            calendar.viewMonth = picker.parseIso(picker.minimumDate).getMonth()
                        } else if (current > calendar.latestMonth) {
                            calendar.viewMonth = picker.parseIso(picker.maximumDate).getMonth()
                        }
                    }
                }
            }

            DayOfWeekRow {
                width: parent.width
                locale: monthGrid.locale
            }

            MonthGrid {
                id: monthGrid

                width: parent.width
                month: calendar.viewMonth
                year: calendar.viewYear
                locale: Qt.locale()

                delegate: Rectangle {
                    required property var model

                    readonly property string isoDate:
                        Qt.formatDate(model.date, "yyyy-MM-dd")
                    readonly property bool allowed: picker.hasDates &&
                        isoDate >= picker.minimumDate &&
                        isoDate <= picker.maximumDate

                    implicitWidth: 40
                    implicitHeight: 36
                    radius: 7
                    color: picker.selectedDate === isoDate ? StudioTheme.accent :
                        dayMouse.containsMouse && allowed ? StudioTheme.hover : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: parent.model.day
                        color: !parent.allowed ? StudioTheme.border :
                            picker.selectedDate === parent.isoDate ?
                                StudioTheme.background : StudioTheme.text
                    }

                    MouseArea {
                        id: dayMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: parent.allowed
                        onClicked: picker.chooseDate(parent.model.date)
                    }
                }
            }

            ActionButton {
                width: parent.width
                text: "Clear date filter"
                enabled: picker.selectedDate !== ""
                onClicked: picker.clearDate()
            }

            Text {
                width: parent.width
                text: picker.minimumDate + " — " + picker.maximumDate
                color: StudioTheme.muted
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
