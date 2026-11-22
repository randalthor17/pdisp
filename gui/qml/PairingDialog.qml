import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A device that is not trusted yet asks to connect. The same code is on its screen.
Dialog {
    id: dlg
    readonly property var req: backend.pairing
    readonly property bool open_: req.id !== undefined
    property int left: 90
    title: "A device wants to connect"
    modal: true
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, Overlay.overlay ? Overlay.overlay.width - 40 : 460)

    onOpen_Changed: { if (open_) { left = 90; open() } else close() }
    Timer { running: dlg.visible; interval: 1000; repeat: true; onTriggered: dlg.left = Math.max(0, dlg.left - 1) }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: "“" + (dlg.req.name || "") + "” would like to use this PC as a second monitor. Allow it only if the code below is the same as on its screen."
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: dlg.req.code ? dlg.req.code.substring(0, 3) + " " + dlg.req.code.substring(3) : ""
            font { family: "monospace"; pixelSize: 44 }
            color: palette.highlight
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: dlg.req.fingerprint || ""
            font { family: "monospace"; pixelSize: 11 }
            opacity: 0.55
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "No answer in " + dlg.left + " s refuses it."
            font.pixelSize: 12
            opacity: 0.65
        }
    }

    footer: DialogButtonBox {
        Button { text: "Deny"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: backend.answerPairing(dlg.req.id, "deny") }
        Button { text: "Allow this time"; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole; onClicked: backend.answerPairing(dlg.req.id, "once") }
        Button { text: "Allow always"; highlighted: true; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole; onClicked: backend.answerPairing(dlg.req.id, "always") }
    }
}
