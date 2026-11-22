import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One number with its name.
ColumnLayout {
    property string label
    property string value
    property bool warn: false
    spacing: 0
    Label { text: parent.value; font.pixelSize: 22; font.weight: Font.DemiBold; color: parent.warn ? "#c0392b" : palette.text }
    Label { text: parent.label; font.pixelSize: 12; opacity: 0.65 }
}
