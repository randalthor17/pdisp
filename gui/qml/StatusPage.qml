import QtQuick
import QtQuick.Layouts
import Pdisp 1.0

Page {
    id: page
    title: "Status"
    readonly property var st: Backend.state
    readonly property var session: st.session ? st.session : null
    readonly property var monitor: st.monitor ? st.monitor : null
    readonly property bool online: Backend.connected

    function clock(secs) {
        var s = Math.floor(secs), m = Math.floor(s / 60), h = Math.floor(m / 60)
        return h > 0 ? h + " h " + (m % 60) + " min" : m > 0 ? m + " min " + (s % 60) + " s" : s + " s"
    }

    headerRight: Chip {
        iconName: !page.online ? "power_off" : page.session ? "link" : "hourglass_empty"
        text: !page.online ? "Daemon not running" : page.session ? "Connected" : "Waiting"
    }

    // the one big card: what is going on
    MCard {
        tone: !page.online ? "error" : page.session ? "primary" : "high"
        padding: Theme.s5
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s5
            Rectangle { // an Expressive container for the icon
                implicitWidth: 80; implicitHeight: 80
                radius: 28
                rotation: 0
                color: !page.online ? Theme.error : page.session ? Theme.primary : Theme.secondaryContainer
                MIcon {
                    anchors.centerIn: parent
                    size: 40
                    filled: true
                    name: !page.online ? "power_off" : page.session ? (page.session.transport === "usb" ? "usb" : "wifi") : "devices"
                    color: !page.online ? Theme.fgError : page.session ? Theme.fgPrimary : Theme.fgSecondaryContainer
                }
                SequentialAnimation on scale { running: page.online && !page.session; loops: Animation.Infinite
                    NumberAnimation { to: 1.06; duration: 1400; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0; duration: 1400; easing.type: Easing.InOutSine } }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                MText {
                    Layout.fillWidth: true
                    text: !page.online ? "pdisp isn't running" : page.session ? page.session.device : "Waiting for a device"
                    kind: "headline"; scale: "s"
                    color: !page.online ? Theme.fgErrorContainer : page.session ? Theme.fgPrimaryContainer : Theme.fgSurface
                    elide: Text.ElideRight
                    maximumLineCount: 2
                }
                MText {
                    Layout.fillWidth: true
                    color: !page.online ? Theme.fgErrorContainer : page.session ? Theme.fgPrimaryContainer : Theme.fgSurfaceVariant
                    text: !page.online ? "The daemon has to run for a phone or tablet to connect. It normally starts with your session."
                        : page.session ? (page.session.transport === "usb" ? "USB cable" : "Wi-Fi") + "  ·  " + page.session.width + "×" + page.session.height + " at "
                                         + Math.round(page.session.refresh_hz) + " Hz" + (page.session.codec ? "  ·  " + (page.session.codec === "h264" ? "H.264" : "HEVC") : "") + (page.monitor ? "  ·  " + page.monitor.connector : "")
                        : "Plug a phone or tablet in, or open the pdisp app on it and connect to this PC."
                }
            }
            MButton {
                visible: !page.online
                text: "Start"
                kind: "filled"
                iconName: "play_arrow"
                onClicked: Backend.startDaemon()
            }
            MButton {
                visible: page.session !== null
                text: "Disconnect"
                kind: "tonal"
                iconName: "link_off"
                onClicked: Backend.disconnectDevice()
            }
        }
        MText {
            visible: !page.online
            text: "or run  systemctl --user start pdispd"
            font.family: "monospace"; kind: "label"; scale: "l"
            color: Theme.fgErrorContainer
        }
    }

    // a monitor kept for a lost device
    MCard {
        visible: page.online && page.session === null && page.monitor !== null && page.monitor.grace
        tone: "tertiary"
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s3
            MIcon { name: "schedule"; size: 24; color: Theme.fgTertiaryContainer }
            MText { Layout.fillWidth: true; color: Theme.fgTertiaryContainer
                text: "The monitor " + (page.monitor ? page.monitor.connector : "") + " is kept for a short while, so the device can come back to the same windows." }
            MButton { text: "Remove now"; kind: "text"; onClicked: Backend.disconnectDevice() }
        }
    }

    // the numbers
    GridLayout {
        visible: page.online && page.session !== null
        Layout.fillWidth: true
        columns: page.width > 700 ? 4 : 2
        rowSpacing: Theme.s3
        columnSpacing: Theme.s3
        Tile { iconName: "speed"; label: "Pictures per second"; value: page.session ? page.session.fps.toFixed(0) : "" }
        Tile { iconName: "network_check"; label: "Bitrate"; value: page.session ? page.session.bitrate_mbps.toFixed(1) + " Mbit/s" : "" }
        Tile { iconName: "warning"; label: "Lost frames"; value: page.session ? String(page.session.frames_lost) : ""; warn: page.session ? page.session.frames_lost > 50 : false }
        Tile { iconName: "schedule"; label: "Connected for"; value: page.session ? page.clock(page.session.seconds) : "" }
        Tile { iconName: "hourglass_empty"; label: "Send queue"; value: page.session ? page.session.queue_ms.toFixed(0) + " ms" : ""; warn: page.session ? page.session.queue_ms > 100 : false }
        Tile { iconName: "memory"; label: "Device decode"; value: page.session ? page.session.phone_decode_ms.toFixed(0) + " ms" : ""; warn: page.session ? page.session.phone_decode_ms > 50 : false }
        Tile { iconName: "key"; label: "Keyframes"; value: page.session ? String(page.session.idr_frames) : "" }
        Tile { iconName: "build"; label: "Repair packets"; value: page.session ? String(page.session.repair_packets) : "" }
    }

    // how devices can reach the PC
    MCard {
        visible: page.online
        tone: "low"
        MText { text: "Connections"; kind: "title"; scale: "m"; color: Theme.fgSurfaceVariant }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s3
            MIcon { name: "usb"; size: 24; color: Theme.primary }
            ColumnLayout { Layout.fillWidth: true; spacing: 0
                MText { text: "USB cable"; kind: "title"; scale: "s" }
                MText { Layout.fillWidth: true; kind: "body"; scale: "s"; color: Theme.fgSurfaceVariant
                    text: !page.st.usb ? "Off: the daemon was started without the USB option" : page.st.usb_state === "accessory" ? "A device is in accessory mode" : "Looking for a cable" } }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s3
            MIcon { name: "wifi"; size: 24; color: Theme.primary }
            ColumnLayout { Layout.fillWidth: true; spacing: 0
                MText { text: "Wi-Fi"; kind: "title"; scale: "s" }
                MText { Layout.fillWidth: true; kind: "body"; scale: "s"; color: Theme.fgSurfaceVariant; text: page.st.net ? "Listening on port " + page.st.port : "Off" } }
        }
    }
}
