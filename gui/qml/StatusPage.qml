import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property var st: backend.state
    readonly property var session: st.session ? st.session : null
    readonly property var monitor: st.monitor ? st.monitor : null

    function clock(secs) {
        var s = Math.floor(secs), m = Math.floor(s / 60), h = Math.floor(m / 60)
        return h > 0 ? h + " h " + (m % 60) + " min" : m > 0 ? m + " min " + (s % 60) + " s" : s + " s"
    }

    ColumnLayout {
        width: page.availableWidth
        anchors.margins: 20
        spacing: 14

        // the daemon is not there
        Card {
            visible: !backend.connected
            title: "pdisp is not running"
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "The daemon (pdispd) has to run for a phone or tablet to connect. It normally starts with your session."
                opacity: 0.8
            }
            RowLayout {
                spacing: 12
                Button { text: "Start it"; highlighted: true; onClicked: backend.startDaemon() }
                Label { text: "or: systemctl --user start pdispd"; font.family: "monospace"; opacity: 0.6 }
            }
        }

        // a device is connected
        Card {
            visible: backend.connected && page.session !== null
            title: "Connected"
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label { Layout.fillWidth: true; text: page.session ? page.session.device : ""; font.pixelSize: 26; font.weight: Font.DemiBold; elide: Text.ElideRight }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        opacity: 0.7
                        text: page.session ? ((page.session.transport === "usb" ? "USB cable" : "Wi-Fi") + "  ·  " + page.session.width + "×" + page.session.height
                              + " at " + Math.round(page.session.refresh_hz) + " Hz  ·  " + (page.monitor ? page.monitor.connector : "")) : ""
                    }
                }
                Button { text: "Disconnect"; onClicked: backend.disconnectDevice() }
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                columns: page.availableWidth > 620 ? 4 : 2
                rowSpacing: 14
                columnSpacing: 20
                uniformCellWidths: true
                Stat { label: "pictures per second"; value: page.session ? page.session.fps.toFixed(0) : "" }
                Stat { label: "bitrate"; value: page.session ? page.session.bitrate_mbps.toFixed(1) + " Mbit/s" : "" }
                Stat { label: "lost frames"; value: page.session ? String(page.session.frames_lost) : ""; warn: page.session ? page.session.frames_lost > 50 : false }
                Stat { label: "connected for"; value: page.session ? page.clock(page.session.seconds) : "" }
                Stat { label: "send queue"; value: page.session ? page.session.queue_ms.toFixed(0) + " ms" : ""; warn: page.session ? page.session.queue_ms > 100 : false }
                Stat { label: "device decode time"; value: page.session ? page.session.phone_decode_ms.toFixed(0) + " ms" : ""; warn: page.session ? page.session.phone_decode_ms > 50 : false }
                Stat { label: "keyframes"; value: page.session ? String(page.session.idr_frames) : "" }
                Stat { label: "repair packets"; value: page.session ? String(page.session.repair_packets) : "" }
            }
        }

        // nobody is connected
        Card {
            visible: backend.connected && page.session === null
            title: "Waiting for a device"
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.8
                text: "Plug a phone or tablet in, or open the pdisp app on it and connect to this PC."
            }
            Label {
                visible: page.monitor !== null && page.monitor.grace
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "The monitor " + (page.monitor ? page.monitor.connector : "") + " is kept for a short while, so the device can come back to the same windows."
                color: "#b9770e"
            }
            Button {
                visible: page.monitor !== null
                text: "Remove the monitor now"
                onClicked: backend.disconnectDevice()
            }
        }

        Card {
            visible: backend.connected
            title: "Connections"
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 6
                Label { text: "USB cable"; opacity: 0.7 }
                Label {
                    text: !page.st.usb ? "off (the daemon was started without the USB option)"
                          : page.st.usb_state === "accessory" ? "a device is in accessory mode"
                          : "looking for a cable"
                }
                Label { text: "Wi-Fi"; opacity: 0.7 }
                Label { text: page.st.net ? "listening on port " + page.st.port : "off" }
            }
        }
    }
}
