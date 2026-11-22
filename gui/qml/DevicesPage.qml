import QtQuick
import QtQuick.Layouts
import Pdisp 1.0

Page {
    id: page
    title: "Devices"
    property string removing: ""
    property string removingName: ""

    headerRight: MIconButton { iconName: "refresh"; fallback: "↻"; tip: "Refresh"; enabled: Backend.connected; onClicked: Backend.refreshDevices() }

    MText {
        visible: !Backend.connected || Backend.devices.length > 0
        Layout.fillWidth: true
        color: Theme.fgSurfaceVariant
        text: Backend.connected ? "These devices may use this PC as a monitor without asking." : "The list comes from the daemon, which is not running."
    }

    MCard {
        visible: Backend.connected && Backend.devices.length === 0
        tone: "high"
        padding: Theme.s5
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s3
            Rectangle { Layout.alignment: Qt.AlignHCenter; implicitWidth: 72; implicitHeight: 72; radius: 24; color: Theme.secondaryContainer
                MIcon { anchors.centerIn: parent; name: "tablet_android"; size: 36; color: Theme.fgSecondaryContainer } }
            MText { Layout.fillWidth: true; text: "No device yet"; kind: "title"; scale: "l"; horizontalAlignment: Text.AlignHCenter }
            MText { Layout.fillWidth: true; color: Theme.fgSurfaceVariant; horizontalAlignment: Text.AlignHCenter
                text: "The first time a phone or tablet connects, a window asks whether to allow it. Devices you allow always show up here." }
        }
    }

    Repeater {
        model: Backend.devices
        delegate: MCard {
            tone: "normal"
            padding: Theme.s4
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s4
                Rectangle { implicitWidth: 48; implicitHeight: 48; radius: 16; color: Theme.primaryContainer
                    MIcon { anchors.centerIn: parent; name: "tablet_android"; size: 26; color: Theme.fgPrimaryContainer } }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    MText { Layout.fillWidth: true; text: modelData.name; kind: "title"; scale: "m"; elide: Text.ElideRight; wrapMode: Text.NoWrap }
                    MText { Layout.fillWidth: true; text: modelData.fingerprint; font.family: "monospace"; kind: "label"; scale: "m"; color: Theme.fgSurfaceVariant; elide: Text.ElideRight; wrapMode: Text.NoWrap }
                }
                MIconButton { iconName: "delete"; fallback: "✕"; kind: "error"; tip: "Remove"
                    onClicked: { page.removing = modelData.fingerprint; page.removingName = modelData.name; confirm.open() } }
            }
        }
    }

    MDialog {
        id: confirm
        headline: "Remove this device?"
        iconName: "delete"
        MText { Layout.fillWidth: true; color: Theme.fgSurfaceVariant; horizontalAlignment: Text.AlignHCenter
            text: "“" + page.removingName + "” will have to be approved again the next time it connects." }
        buttons: [
            MButton { text: "Cancel"; kind: "text"; onClicked: confirm.close() },
            MButton { text: "Remove"; kind: "error"; onClicked: { Backend.removeDevice(page.removing); confirm.close() } }
        ]
    }
}
