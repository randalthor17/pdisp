import QtQuick
import QtQuick.Layouts

// Material 3 segmented buttons: pick one of a few values. model: [{label, value}]; `picked(value)` says what the user chose.
Rectangle {
    id: seg
    property var model: []
    property var current
    signal picked(var value)
    implicitHeight: 40
    implicitWidth: row.implicitWidth
    radius: 20
    color: "transparent"
    border.width: 1
    border.color: Theme.outline
    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 0
        Repeater {
            model: seg.model
            delegate: Rectangle {
                readonly property bool on: modelData.value === seg.current
                Layout.fillHeight: true
                Layout.fillWidth: true
                implicitWidth: lab.implicitWidth + 32 + (on ? 22 : 0)
                color: on ? Theme.secondaryContainer : "transparent"
                radius: 20
                Rectangle { anchors.fill: parent; radius: 20; color: Theme.fgSurface; opacity: ma.pressed ? 0.12 : ma.containsMouse ? 0.08 : 0 }
                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    MIcon { visible: on; name: "check"; size: 16; color: Theme.fgSecondaryContainer; anchors.verticalCenter: parent.verticalCenter }
                    MText { id: lab; text: modelData.label; kind: "label"; scale: "l"; color: on ? Theme.fgSecondaryContainer : Theme.fgSurface; wrapMode: Text.NoWrap }
                }
                MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: seg.picked(modelData.value) }
            }
        }
    }
}
