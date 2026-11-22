import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A Material 3 dialog: extra-large rounded, on the highest tonal surface, over a scrim. Children go in the body; `buttons` is a row of MButton.
Dialog {
    id: dlg
    default property alias body: bodyCol.data
    property alias buttons: buttonRow.data
    property string headline
    property string iconName: ""
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, (Overlay.overlay ? Overlay.overlay.width : 440) - 48)
    padding: 24
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durShort } NumberAnimation { property: "scale"; from: 0.92; to: 1; duration: Theme.durMedium; easing.type: Easing.OutBack } }
    exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: 100 } }
    Overlay.modal: Rectangle { color: Theme.withAlpha(Theme.scrim, 0.32) }
    background: Rectangle { radius: Theme.radiusXL; color: Theme.surfaceContainerHigh }
    header: null
    footer: null
    contentItem: ColumnLayout {
        spacing: 16
        MIcon { visible: dlg.iconName !== ""; name: dlg.iconName; size: 28; color: Theme.primary; Layout.alignment: Qt.AlignHCenter }
        MText { text: dlg.headline; kind: "headline"; scale: "s"; Layout.fillWidth: true; horizontalAlignment: dlg.iconName !== "" ? Text.AlignHCenter : Text.AlignLeft }
        ColumnLayout { id: bodyCol; Layout.fillWidth: true; spacing: 12 }
        RowLayout { id: buttonRow; Layout.alignment: Qt.AlignRight; spacing: 8 }
    }
}
