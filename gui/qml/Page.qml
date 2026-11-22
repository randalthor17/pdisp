import QtQuick
import QtQuick.Layouts

// A scrolling page with a centred column no wider than 760: the same layout serves a phone-sized window and a wide one.
Flickable {
    id: page
    default property alias content: col.data
    property string title
    property alias headerRight: headRight.data
    contentHeight: col.implicitHeight + 56
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ColumnLayout {
        id: col
        width: Math.min(page.width - 2 * Theme.s5, 760)
        x: (page.width - width) / 2
        y: Theme.s5
        spacing: Theme.s4
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.bottomMargin: Theme.s2
            MText { text: page.title; kind: "headline"; scale: "m"; Layout.fillWidth: true; wrapMode: Text.NoWrap }
            RowLayout { id: headRight; spacing: Theme.s2 }
        }
    }
}
