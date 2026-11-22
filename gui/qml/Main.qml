import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as Platform

ApplicationWindow {
    id: win
    width: 780
    height: 620
    minimumWidth: 520
    minimumHeight: 460
    visible: !startHidden
    title: "pdisp"

    readonly property var session: backend.state.session ? backend.state.session : null
    readonly property bool trayOk: tray.available

    function summary() {
        if (!backend.connected) return "pdisp: the daemon is not running"
        if (session) return "pdisp: " + session.device + " (" + session.fps.toFixed(0) + " fps)"
        return "pdisp: waiting for a device"
    }
    function present() { show(); raise(); requestActivate() }

    header: ToolBar {
        RowLayout {
            anchors { fill: parent; leftMargin: 16; rightMargin: 16 }
            Label { text: "pdisp"; font.bold: true; font.pixelSize: 18 }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: 10; height: 10; radius: 5
                color: !backend.connected ? "#c0392b" : win.session ? "#27ae60" : "#f1c40f"
            }
            Label {
                text: !backend.connected ? "daemon not running" : win.session ? "connected" : "waiting"
                opacity: 0.8
            }
        }
    }

    TabBar {
        id: tabs
        anchors { top: parent.top; left: parent.left; right: parent.right }
        currentIndex: initialPage
        TabButton { text: "Status" }
        TabButton { text: "Devices" }
        TabButton { text: "Settings" }
    }
    StackLayout {
        anchors { top: tabs.bottom; bottom: parent.bottom; left: parent.left; right: parent.right; margins: 14 }
        currentIndex: tabs.currentIndex
        StatusPage {}
        DevicesPage {}
        SettingsPage {}
    }

    PairingDialog { id: pairing }

    // a request while the window is hidden: bring it up, and say so in the tray
    Connections {
        target: backend
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
            Platform.MenuItem { text: "Disconnect"; visible: win.session !== null; onTriggered: backend.disconnectDevice() }
            Platform.MenuSeparator {}
            Platform.MenuItem { text: "Quit"; onTriggered: Qt.quit() }
        }
    }
}
