import QtQuick

// Text in the Material 3 type scale: kind is one of display, headline, title, body, label (with size s, m or l in `scale`).
Text {
    property string kind: "body"
    property string scale: "m"
    readonly property var sizes: ({
        "display": { "s": 36, "m": 45, "l": 57 }, "headline": { "s": 24, "m": 28, "l": 32 }, "title": { "s": 14, "m": 16, "l": 22 },
        "body": { "s": 12, "m": 14, "l": 16 }, "label": { "s": 11, "m": 12, "l": 14 }
    })
    color: Theme.fgSurface
    font.family: Theme.fontFamily
    font.pixelSize: sizes[kind][scale]
    font.weight: kind === "label" || (kind === "title" && scale !== "l") ? Font.Medium : Font.Normal
    wrapMode: Text.Wrap
}
