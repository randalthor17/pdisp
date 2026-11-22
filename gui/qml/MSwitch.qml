import QtQuick
import QtQuick.Controls.Basic

// The Material 3 switch: a 52 x 32 track, a thumb that grows when it is on or pressed, and a check mark.
Switch {
    id: s
    implicitWidth: 52
    implicitHeight: 32
    hoverEnabled: true
    indicator: Rectangle {
        width: 52; height: 32; radius: 16
        color: s.checked ? Theme.primary : Theme.surfaceContainerHighest
        border.width: s.checked ? 0 : 2
        border.color: Theme.outline
        Behavior on color { ColorAnimation { duration: Theme.durShort } }
        Rectangle {
            id: thumb
            property real d: s.down ? 28 : s.checked ? 24 : 16
            width: d; height: d; radius: d / 2
            x: (s.checked ? 36 : 16) - d / 2
            y: 16 - d / 2
            color: s.checked ? Theme.fgPrimary : Theme.outline
            Behavior on x { NumberAnimation { duration: Theme.durShort; easing.type: Easing.OutCubic } }
            Behavior on d { NumberAnimation { duration: Theme.durShort; easing.type: Easing.OutCubic } }
            MIcon { anchors.centerIn: parent; name: "check"; size: 16; color: Theme.primary; opacity: s.checked ? 1 : 0; Behavior on opacity { NumberAnimation { duration: 100 } } }
        }
    }
    contentItem: Item {}
}
