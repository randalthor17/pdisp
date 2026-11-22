import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A panel with a title line, for grouping what belongs together.
Rectangle {
    id: card
    default property alias content: body.data
    property string title: ""
    Layout.fillWidth: true
    implicitHeight: col.implicitHeight + 32
    radius: 8
    color: Qt.tint(palette.window, Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07))
    border.color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.15)

    ColumnLayout {
        id: col
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 16 }
        spacing: 10
        Label { visible: card.title !== ""; text: card.title; font.bold: true; font.pixelSize: 15 }
        ColumnLayout { id: body; Layout.fillWidth: true; spacing: 8 }
    }
}
