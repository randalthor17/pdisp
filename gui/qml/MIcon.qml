import QtQuick

// A Material Symbols glyph, by its ligature name ("usb", "wifi", "devices", ...). Nothing is drawn if no icon font was found.
Text {
    property string name
    property real size: 24
    property bool filled: false
    visible: Theme.iconFont !== "" && name !== ""
    text: name
    color: Theme.fgSurface
    font.family: Theme.iconFont
    font.pixelSize: size
    font.variableAxes: ({ "FILL": filled ? 1 : 0, "wght": 400, "GRAD": 0, "opsz": 24 })
    width: visible ? size : 0
    height: size
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    renderType: Text.NativeRendering
}
