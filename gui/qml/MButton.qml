import QtQuick
import QtQuick.Controls.Basic

// A Material 3 button: filled, tonal, outlined, text or error (tonal red). It rounds off while it is pressed, as Expressive buttons do.
Button {
    id: b
    property string kind: "filled"
    property string iconName: ""
    readonly property color fg: !enabled ? Theme.withAlpha(Theme.fgSurface, 0.38)
        : kind === "filled" ? Theme.fgPrimary : kind === "tonal" ? Theme.fgSecondaryContainer : kind === "error" ? Theme.fgErrorContainer : Theme.primary
    readonly property color bg: !enabled ? (kind === "outlined" || kind === "text" ? "transparent" : Theme.withAlpha(Theme.fgSurface, 0.12))
        : kind === "filled" ? Theme.primary : kind === "tonal" ? Theme.secondaryContainer : kind === "error" ? Theme.errorContainer : "transparent"

    implicitHeight: 40
    leftPadding: iconName !== "" && Theme.iconFont !== "" ? 16 : 24
    rightPadding: 24
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: b.down ? Theme.radiusM : height / 2
        color: b.bg
        border.width: b.kind === "outlined" ? 1 : 0
        border.color: b.enabled ? Theme.outline : Theme.withAlpha(Theme.fgSurface, 0.12)
        Behavior on radius { NumberAnimation { duration: Theme.durShort; easing.type: Easing.OutCubic } }
        Rectangle { // the state layer
            anchors.fill: parent
            radius: parent.radius
            color: b.fg
            opacity: !b.enabled ? 0 : b.down ? 0.12 : (b.hovered || b.visualFocus) ? 0.08 : 0
            Behavior on opacity { NumberAnimation { duration: 100 } }
        }
    }
    contentItem: Row {
        spacing: 8
        MIcon { name: b.iconName; size: 18; color: b.fg; anchors.verticalCenter: parent.verticalCenter }
        MText { text: b.text; kind: "label"; scale: "l"; color: b.fg; anchors.verticalCenter: parent.verticalCenter; wrapMode: Text.NoWrap }
    }
}
