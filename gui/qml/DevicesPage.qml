import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    property string removing: ""
    property string removingName: ""

    ColumnLayout {
        anchors { fill: parent; margins: 20 }
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { Layout.fillWidth: true; text: "Devices that may use this PC as a monitor"; font.bold: true; font.pixelSize: 15 }
            Button { text: "Refresh"; enabled: backend.connected; onClicked: backend.refreshDevices() }
        }
        Label {
            visible: !backend.connected
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "The list comes from the daemon, which is not running."
        }
        Label {
            visible: backend.connected && list.count === 0
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            text: "No device has been approved yet. The first time a phone or tablet connects, a window asks whether to allow it."
        }
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: backend.devices
            delegate: Rectangle {
                width: ListView.view.width
                height: 64
                radius: 8
                color: Qt.tint(palette.window, Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.07))
                border.color: Qt.rgba(palette.text.r, palette.text.g, palette.text.b, 0.15)
                RowLayout {
                    anchors { fill: parent; leftMargin: 16; rightMargin: 12 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { Layout.fillWidth: true; text: modelData.name; font.pixelSize: 16; font.weight: Font.DemiBold; elide: Text.ElideRight }
                        Label { Layout.fillWidth: true; text: modelData.fingerprint; font.family: "monospace"; font.pixelSize: 12; opacity: 0.6 }
                    }
                    Button {
                        text: "Remove"
                        onClicked: { page.removing = modelData.fingerprint; page.removingName = modelData.name; confirm.open() }
                    }
                }
            }
        }
    }

    Dialog {
        id: confirm
        title: "Remove this device?"
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: backend.removeDevice(page.removing)
        Label {
            width: 360
            wrapMode: Text.Wrap
            text: "“" + page.removingName + "” will have to be approved again the next time it connects."
        }
    }
}
