import QtQuick
import QtQuick.Controls.Basic

// A destination of the navigation rail (icon over label) or of the bottom bar.
AbstractButton {
    id: item
    property string iconName
    property bool selected: false
    implicitWidth: 80
    implicitHeight: 72
    hoverEnabled: true
    background: null
    contentItem: Column {
        spacing: 4
        topPadding: 8
        Rectangle { // the indicator pill, 56 x 32
            anchors.horizontalCenter: parent.horizontalCenter
            width: item.selected ? 56 : 40
            height: 32
            radius: 16
            color: item.selected ? Theme.secondaryContainer : "transparent"
            Behavior on width { NumberAnimation { duration: Theme.durShort; easing.type: Easing.OutCubic } }
            Rectangle { anchors.fill: parent; radius: 16; color: Theme.fgSurface; opacity: item.down ? 0.12 : item.hovered ? 0.08 : 0 }
            MIcon { anchors.centerIn: parent; name: item.iconName; filled: item.selected; size: 24; color: item.selected ? Theme.fgSecondaryContainer : Theme.fgSurfaceVariant }
        }
        MText {
            anchors.horizontalCenter: parent.horizontalCenter
            text: item.text
            kind: "label"; scale: "m"
            font.weight: item.selected ? Font.Bold : Font.Medium
            color: item.selected ? Theme.fgSurface : Theme.fgSurfaceVariant
            wrapMode: Text.NoWrap
        }
    }
}
