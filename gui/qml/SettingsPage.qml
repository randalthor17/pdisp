import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: page
    contentWidth: availableWidth
    readonly property var cfg: backend.state.config ? backend.state.config : ({})

    // a field that follows the daemon's value, except while the user is typing in it
    component Row2: RowLayout {
        property string title
        property string hint
        default property alias control: slot.data
        Layout.fillWidth: true
        spacing: 16
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Label { text: title }
            Label { text: hint; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12; opacity: 0.6 }
        }
        Item { id: slot; implicitWidth: childrenRect.width; implicitHeight: childrenRect.height }
    }

    ColumnLayout {
        width: page.availableWidth
        spacing: 14

        Card {
            title: "Picture"
            enabled: backend.connected
            Row2 {
                title: "Highest refresh rate"
                hint: "The PC uses the best rate the device offers that is not above this. 60 Hz is the smoothest on every link tried; higher needs a good cable or Wi-Fi."
                ComboBox {
                    id: hz
                    model: [30, 45, 60, 75, 90, 100, 120, 144]
                    displayText: currentText + " Hz"
                    currentIndex: Math.max(0, model.indexOf(page.cfg.refresh_cap_hz))
                    onActivated: backend.setConfig("refresh_cap_hz", model[currentIndex])
                }
            }
            Row2 {
                title: "Starting bitrate"
                hint: "Where the picture quality starts; it adapts to the link from there."
                SpinBox {
                    id: br
                    from: 2; to: 100; stepSize: 1
                    editable: true
                    textFromValue: function(v) { return v + " Mbit/s" }
                    valueFromText: function(t) { return parseInt(t) }
                    onValueModified: backend.setConfig("bitrate_kbps", value * 1000)
                    Binding on value { value: (page.cfg.bitrate_kbps || 12000) / 1000; when: !br.activeFocus }
                }
            }
            Row2 {
                title: "Highest bitrate"
                hint: "The ceiling the rate control may climb to."
                SpinBox {
                    id: bm
                    from: 4; to: 200; stepSize: 2
                    editable: true
                    textFromValue: function(v) { return v + " Mbit/s" }
                    valueFromText: function(t) { return parseInt(t) }
                    onValueModified: backend.setConfig("max_bitrate_kbps", value * 1000)
                    Binding on value { value: (page.cfg.max_bitrate_kbps || 40000) / 1000; when: !bm.activeFocus }
                }
            }
            Row2 {
                title: "Sharpen still pictures"
                hint: "When nothing changes for a moment, send better versions of the same screen. Text gets sharper; 0 turns it off."
                SpinBox {
                    id: rf
                    from: 0; to: 200; stepSize: 10
                    editable: true
                    textFromValue: function(v) { return v === 0 ? "off" : v + " pictures" }
                    valueFromText: function(t) { return t === "off" ? 0 : parseInt(t) }
                    onValueModified: backend.setConfig("refine_frames", value)
                    Binding on value { value: page.cfg.refine_frames !== undefined ? page.cfg.refine_frames : 40; when: !rf.activeFocus }
                }
            }
        }

        Card {
            title: "Connection"
            enabled: backend.connected
            Row2 {
                title: "Keep the monitor after a lost link"
                hint: "If a device drops out (cable, Wi-Fi), the monitor and its windows stay this long so it can come back. 0 removes the monitor at once."
                SpinBox {
                    id: gr
                    from: 0; to: 600; stepSize: 5
                    editable: true
                    textFromValue: function(v) { return v === 0 ? "no" : v + " s" }
                    valueFromText: function(t) { return t === "no" ? 0 : parseInt(t) }
                    onValueModified: backend.setConfig("grace_secs", value)
                    Binding on value { value: page.cfg.grace_secs !== undefined ? page.cfg.grace_secs : 30; when: !gr.activeFocus }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 12
            opacity: 0.65
            text: "Changes apply from the next connection and are saved. The port, the USB option and how new devices are approved are set when the daemon starts "
                  + "(on NixOS in services.pdisp). Control socket: " + backend.socketPath
        }
    }
}
