import QtQuick
import QtQuick.Layouts
import Pdisp 1.0

Page {
    id: page
    title: "Settings"
    readonly property var cfg: Backend.state.config ? Backend.state.config : ({})
    readonly property bool online: Backend.connected

    // one row of a settings card: what it is, what it does, and its control on the right
    component Setting: RowLayout {
        property string headline
        property string supporting
        default property alias control: slot.data
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 4
        spacing: Theme.s4
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            MText { text: headline; kind: "body"; scale: "l" }
            MText { Layout.fillWidth: true; text: supporting; kind: "body"; scale: "s"; color: Theme.fgSurfaceVariant }
        }
        RowLayout { id: slot }
    }
    component Divider: Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.outlineVariant }

    function rates() {
        var base = [30, 60, 90, 120, 144], cur = cfg.refresh_cap_hz
        if (cur !== undefined && base.indexOf(cur) < 0) { base.push(cur); base.sort(function(a, b) { return a - b }) }
        return base.map(function(v) { return { "label": v + " Hz", "value": v } })
    }

    MText { text: "Picture"; kind: "title"; scale: "m"; color: Theme.primary; Layout.topMargin: Theme.s2 }
    MCard {
        tone: "normal"
        enabled: page.online
        opacity: page.online ? 1 : 0.5
        Setting {
            headline: "Highest refresh rate"
            supporting: "The PC uses the best rate the device offers that is not above this. 60 Hz is the smoothest on every link tried."
            MSegmented { model: page.rates(); current: page.cfg.refresh_cap_hz; onPicked: function(v) { Backend.setConfig("refresh_cap_hz", v) } }
        }
        Divider {}
        Setting {
            headline: "Starting bitrate"
            supporting: "Where the picture quality starts. It adapts to the link from there."
            MStepper { from: 2; to: 100; step: 2; value: (page.cfg.bitrate_kbps || 12000) / 1000
                format: function(v) { return v + " Mbit/s" }; onMoved: function(v) { Backend.setConfig("bitrate_kbps", v * 1000) } }
        }
        Divider {}
        Setting {
            headline: "Highest bitrate"
            supporting: "The ceiling the rate control may climb to."
            MStepper { from: 4; to: 200; step: 4; value: (page.cfg.max_bitrate_kbps || 40000) / 1000
                format: function(v) { return v + " Mbit/s" }; onMoved: function(v) { Backend.setConfig("max_bitrate_kbps", v * 1000) } }
        }
        Divider {}
        Setting {
            headline: "Sharpen still pictures"
            supporting: "When nothing changes for a moment, send better versions of the same screen. Text gets sharper."
            MSwitch { checked: (page.cfg.refine_frames || 0) > 0; onToggled: Backend.setConfig("refine_frames", checked ? 40 : 0) }
        }
    }

    MText { text: "Connection"; kind: "title"; scale: "m"; color: Theme.primary; Layout.topMargin: Theme.s3 }
    MCard {
        tone: "normal"
        enabled: page.online
        opacity: page.online ? 1 : 0.5
        Setting {
            headline: "Keep the monitor after a lost link"
            supporting: "If a device drops out (cable, Wi-Fi), the monitor and its windows stay this long so it can come back. Off removes the monitor at once."
            MStepper { from: 0; to: 600; step: 5; value: page.cfg.grace_secs !== undefined ? page.cfg.grace_secs : 30
                format: function(v) { return v === 0 ? "Off" : v + " s" }; onMoved: function(v) { Backend.setConfig("grace_secs", v) } }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        spacing: Theme.s3
        MIcon { name: "info"; size: 20; color: Theme.fgSurfaceVariant; Layout.alignment: Qt.AlignTop }
        MText { Layout.fillWidth: true; kind: "body"; scale: "s"; color: Theme.fgSurfaceVariant
            text: "Changes apply from the next connection and are saved. The port, the USB option and how new devices are approved are set when the daemon starts (on NixOS in services.pdisp). Control socket: " + Backend.socketPath }
    }
}
