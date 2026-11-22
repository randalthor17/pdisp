import QtQuick
import QtQuick.Layouts

// A small read-only pill with an optional icon: "USB cable", "2304×1440".
Rectangle {
    property string text
    property string iconName: ""
    implicitHeight: 32
    implicitWidth: row.implicitWidth + 24
    radius: Theme.radiusS
    color: "transparent"
    border.width: 1
    border.color: Theme.outlineVariant
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        MIcon { name: iconName; size: 18; color: Theme.primary }
        MText { text: parent.parent.text; kind: "label"; scale: "l"; color: Theme.fgSurfaceVariant; wrapMode: Text.NoWrap }
    }
}
