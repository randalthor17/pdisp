import QtQuick
import QtQuick.Layouts

// One number with its name, in a small tonal tile.
Rectangle {
    property string label
    property string value
    property string iconName: ""
    property bool warn: false
    Layout.fillWidth: true
    implicitHeight: 88
    radius: Theme.radiusL
    color: warn ? Theme.errorContainer : Theme.surfaceContainerHigh
    ColumnLayout {
        anchors { fill: parent; margins: Theme.s4 }
        spacing: 2
        RowLayout {
            spacing: 6
            MIcon { name: iconName; size: 16; color: warn ? Theme.fgErrorContainer : Theme.primary }
            MText { text: label; kind: "label"; scale: "m"; color: warn ? Theme.fgErrorContainer : Theme.fgSurfaceVariant; elide: Text.ElideRight; Layout.fillWidth: true; wrapMode: Text.NoWrap }
        }
        Item { Layout.fillHeight: true }
        MText { text: value; kind: "headline"; scale: "s"; color: warn ? Theme.fgErrorContainer : Theme.fgSurface; wrapMode: Text.NoWrap }
    }
}
