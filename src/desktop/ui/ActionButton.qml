import QtQuick
import QtQuick.Controls

Button {
    id: control
    property bool selected: false
    property bool primary: false
    property string accessibleLabel: ""
    activeFocusOnTab: true
    Accessible.name: accessibleLabel.length ? accessibleLabel : text
    implicitHeight: 44
    leftPadding: 16
    rightPadding: 16
    font.family: StudioTheme.fontFamily
    font.pixelSize: 14
    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? StudioTheme.muted : control.primary ? StudioTheme.background : StudioTheme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: StudioTheme.radius / 2
        color: !control.enabled ? StudioTheme.surface : control.primary ? StudioTheme.accent :
               control.selected ? StudioTheme.raised : control.hovered ? StudioTheme.hover : StudioTheme.surface
        border.color: control.activeFocus ? StudioTheme.gold : control.selected ? StudioTheme.accent : StudioTheme.border
        border.width: control.activeFocus ? 2 : 1
    }
}
