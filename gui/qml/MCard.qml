import QtQuick
import QtQuick.Layouts

// A rounded tonal container. `tone` picks the surface (low, normal, high, primary, error); children go inside, with padding.
Rectangle {
    id: card
    default property alias content: body.data
    property string tone: "normal"
    property int padding: Theme.s4
    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + padding * 2
    radius: Theme.radiusL
    color: tone === "low" ? Theme.surfaceContainerLow : tone === "high" ? Theme.surfaceContainerHigh : tone === "primary" ? Theme.primaryContainer
        : tone === "error" ? Theme.errorContainer : tone === "tertiary" ? Theme.tertiaryContainer : Theme.surfaceContainer
    ColumnLayout {
        id: body
        anchors { fill: parent; margins: card.padding }
        spacing: Theme.s3
    }
}
