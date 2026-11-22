import QtQuick
import QtQuick.Controls.Basic

// A round icon-only button (48 dp touch target, 40 dp circle).
Button {
    id: b
    property string iconName
    property string kind: "standard" // standard | tonal | error
    property string tip: ""
    property string fallback: "" // drawn when there is no icon font
    readonly property color fg: !enabled ? Theme.withAlpha(Theme.fgSurface, 0.38) : kind === "error" ? Theme.error : kind === "tonal" ? Theme.fgSecondaryContainer : Theme.fgSurfaceVariant
    implicitWidth: 48
    implicitHeight: 48
    hoverEnabled: true
    background: Item {
        Rectangle {
            anchors.centerIn: parent
            width: 40; height: 40; radius: 20
            color: b.kind === "tonal" ? Theme.secondaryContainer : "transparent"
            Rectangle { anchors.fill: parent; radius: 20; color: b.fg; opacity: b.down ? 0.12 : b.hovered ? 0.08 : 0 }
        }
    }
    contentItem: Item {
        MIcon { name: b.iconName; size: 24; color: b.fg; anchors.centerIn: parent }
        MText { visible: Theme.iconFont === ""; text: b.fallback; kind: "title"; scale: "l"; color: b.fg; anchors.centerIn: parent; wrapMode: Text.NoWrap }
    }
    ToolTip.visible: tip !== "" && hovered
    ToolTip.text: tip
    ToolTip.delay: 600
}
