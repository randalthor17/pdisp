import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt.labs.platform as Platform
import Pdisp 1.0

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 380
    minimumHeight: 520
    visible: !startHidden
    title: "pdisp"
    color: Theme.surface
    font.family: Theme.fontFamily

    property int pageIndex: initialPage
    readonly property bool compact: width < 640
    readonly property var session: Backend.state.session ? Backend.state.session : null
    readonly property bool trayOk: tray.available
    readonly property var pages: [
        { "label": "Status", "icon": "cast_connected" },
        { "label": "Devices", "icon": "devices" },
        { "label": "Settings", "icon": "tune" }
    ]

    function summary() {
        if (!Backend.connected) return "pdisp: the daemon is not running"
        if (session) return "pdisp: " + session.device + " (" + session.fps.toFixed(0) + " fps)"
        return "pdisp: waiting for a device"
    }
    function present() { show(); raise(); requestActivate() }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // the navigation rail on a wide window
        Rectangle {
            visible: !win.compact
            Layout.fillHeight: true
            Layout.preferredWidth: 96
            color: Theme.surface
            ColumnLayout {
                anchors { fill: parent; topMargin: Theme.s5 }
                spacing: Theme.s3
                Repeater {
                    model: win.pages
                    delegate: NavItem {
                        Layout.alignment: Qt.AlignHCenter
                        text: modelData.label
                        iconName: modelData.icon
                        selected: win.pageIndex === index
                        onClicked: win.pageIndex = index
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: win.pageIndex
            StatusPage {}
            DevicesPage {}
            SettingsPage {}
        }
    }

    // the bottom bar on a narrow one
    footer: Rectangle {
        visible: win.compact
        height: win.compact ? 80 : 0
        color: Theme.surfaceContainer
        RowLayout {
            anchors.fill: parent
            spacing: 0
            Repeater {
                model: win.pages
                delegate: NavItem {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: modelData.label
                    iconName: modelData.icon
                    selected: win.pageIndex === index
                    onClicked: win.pageIndex = index
                }
            }
        }
    }

    PairingDialog { id: pairing }

    // a request while the window is hidden: bring it up, and say so in the tray
    Connections {
        target: Backend
        function onPairingRequested(request) {
            win.present()
            if (win.trayOk) tray.showMessage("A device wants to connect", "“" + request.name + "” asks to use this PC as a monitor. Code " + request.code, Platform.SystemTrayIcon.Information, 15000)
        }
    }

    onClosing: function(close) {
        if (trayOk) { close.accepted = false; hide() } // the tray keeps it running
        else Qt.quit()
    }

    Platform.SystemTrayIcon {
        id: tray
        icon.source: "qrc:/pdisp.svg"
        visible: true
        tooltip: win.summary()
        onActivated: function(reason) { if (win.visible) win.hide(); else win.present() }
        menu: Platform.Menu {
            Platform.MenuItem { text: win.summary(); enabled: false }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: "Open pdisp"; onTriggered: win.present() }
            Platform.MenuItem { text: "Disconnect"; visible: win.session !== null; onTriggered: Backend.disconnectDevice() }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: "Quit"; onTriggered: Qt.quit() }
        }
    }
}
