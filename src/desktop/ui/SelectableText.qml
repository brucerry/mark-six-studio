import QtQuick

TextEdit {
    id: control
    property color tone: StudioTheme.muted
    property bool richText: false
    readOnly: true
    selectByMouse: true
    selectByKeyboard: true
    activeFocusOnTab: true
    textFormat: richText ? TextEdit.RichText : TextEdit.PlainText
    wrapMode: TextEdit.Wrap
    color: hover.hovered ? StudioTheme.text : tone
    selectionColor: StudioTheme.accent
    selectedTextColor: StudioTheme.background
    font.family: StudioTheme.fontFamily
    visible: text.length > 0
    height: visible ? contentHeight : 0
    font.pixelSize: 15
    Accessible.name: text
    HoverHandler { id: hover }
}
