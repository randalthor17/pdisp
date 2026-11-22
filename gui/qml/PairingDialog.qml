import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Pdisp 1.0

// A device that is not trusted yet asks to connect. The same code is on its screen.
MDialog {
    id: dlg
    readonly property var req: Backend.pairing
    readonly property bool wanted: req.id !== undefined
    property int left: 90
    headline: "Allow “" + (req.name || "") + "”?"
    iconName: "devices"
    closePolicy: Popup.NoAutoClose

    onWantedChanged: { if (wanted) { left = 90; open() } else close() }
    Timer { running: dlg.visible; interval: 1000; repeat: true; onTriggered: dlg.left = Math.max(0, dlg.left - 1) }

    MText {
        Layout.fillWidth: true
        text: "It wants to use this PC as a second monitor. Allow it only if the code below is the same as on its screen."
        color: Theme.fgSurfaceVariant
        horizontalAlignment: Text.AlignHCenter
    }
    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        implicitWidth: codeText.implicitWidth + 48
        implicitHeight: 72
        radius: Theme.radiusL
        color: Theme.primaryContainer
        MText {
            id: codeText
            anchors.centerIn: parent
            text: dlg.req.code ? dlg.req.code.substring(0, 3) + " " + dlg.req.code.substring(3) : ""
            font.family: "monospace"
            font.pixelSize: 40
            font.letterSpacing: 4
            color: Theme.fgPrimaryContainer
            wrapMode: Text.NoWrap
        }
    }
    MText { Layout.alignment: Qt.AlignHCenter; text: dlg.req.fingerprint || ""; font.family: "monospace"; kind: "label"; scale: "m"; color: Theme.fgSurfaceVariant; wrapMode: Text.NoWrap }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 4
        Rectangle { // a linear progress bar: the time left to answer
            Layout.fillWidth: true; implicitHeight: 4; radius: 2; color: Theme.surfaceContainerHighest
            Rectangle { width: parent.width * dlg.left / 90; height: 4; radius: 2; color: Theme.primary; Behavior on width { NumberAnimation { duration: 900 } } }
        }
        MText { text: "No answer in " + dlg.left + " s refuses it."; kind: "label"; scale: "m"; color: Theme.fgSurfaceVariant }
    }

    buttons: [
        MButton { text: "Deny"; kind: "text"; onClicked: Backend.answerPairing(dlg.req.id, "deny") },
        MButton { text: "Allow this time"; kind: "tonal"; onClicked: Backend.answerPairing(dlg.req.id, "once") },
        MButton { text: "Allow always"; kind: "filled"; iconName: "check"; onClicked: Backend.answerPairing(dlg.req.id, "always") }
    ]
}
