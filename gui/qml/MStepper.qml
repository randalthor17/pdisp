import QtQuick
import QtQuick.Layouts

// A value with minus and plus buttons, in a tonal pill. `moved(v)` says what the user asked for; the owner sets `value`.
Rectangle {
    id: st
    property int value
    property int from: 0
    property int to: 100
    property int step: 1
    property var format: function(v) { return String(v) }
    signal moved(int v)
    implicitWidth: 176
    implicitHeight: 40
    radius: 20
    color: Theme.surfaceContainerHighest
    RowLayout {
        anchors.fill: parent
        spacing: 0
        MIconButton { iconName: "remove"; fallback: "−"; enabled: st.value > st.from; implicitWidth: 44; implicitHeight: 40; onClicked: st.moved(Math.max(st.from, st.value - st.step)) }
        MText { Layout.fillWidth: true; text: st.format(st.value); kind: "label"; scale: "l"; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.NoWrap }
        MIconButton { iconName: "add"; fallback: "+"; enabled: st.value < st.to; implicitWidth: 44; implicitHeight: 40; onClicked: st.moved(Math.min(st.to, st.value + st.step)) }
    }
}
