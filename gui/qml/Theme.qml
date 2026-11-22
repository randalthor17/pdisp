pragma Singleton
import QtQuick
import Pdisp 1.0

// Material 3 colour roles derived from the Qt palette, so the app follows whatever Qt theme is active (qtengine, KDE colour schemes, Fusion, ...), light or dark.
// The window colour is the surface, the highlight colour is the accent: the tonal roles (containers, outlines, the secondary and tertiary accents) are built from those two
// the way a Material 3 scheme is built from a seed colour. Shape, spacing and type are Material 3 Expressive and do not depend on the theme.
QtObject {
    id: t

    property SystemPalette pal: SystemPalette { colorGroup: SystemPalette.Active }

    function mix(a, b, f) { return Qt.rgba(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1) }
    function tone(base, hueShift, sat, l) { // the base colour's hue at a given lightness: a tone of its tonal palette
        var h = (base.hslHue < 0 ? 0 : base.hslHue + hueShift + 1) % 1
        return Qt.hsla(h, Math.min(1, base.hslSaturation * sat), l, 1)
    }
    function withAlpha(color, a) { return Qt.rgba(color.r, color.g, color.b, a) }
    function layer(base, over, a) { return Qt.tint(base, Qt.rgba(over.r, over.g, over.b, a)) } // a state layer: `over` drawn on `base` at opacity a

    readonly property color surface: pal.window
    readonly property bool dark: surface.hslLightness < 0.5
    readonly property color fgSurface: pal.windowText
    readonly property color seed: pal.highlight

    // the accent, at a tone that reads on the surface: light on dark, dark on light
    readonly property color primary: tone(seed, 0, 1, dark ? Math.max(seed.hslLightness, 0.72) : Math.min(seed.hslLightness, 0.38))
    readonly property color fgPrimary: dark ? tone(seed, 0, 0.8, 0.16) : "#ffffff"
    readonly property color primaryContainer: tone(seed, 0, 0.9, dark ? 0.27 : 0.86)
    readonly property color fgPrimaryContainer: tone(seed, 0, 0.8, dark ? 0.88 : 0.14)
    readonly property color secondary: tone(seed, 0, 0.35, dark ? 0.76 : 0.40)
    readonly property color secondaryContainer: tone(seed, 0, 0.32, dark ? 0.24 : 0.88)
    readonly property color fgSecondaryContainer: tone(seed, 0, 0.32, dark ? 0.88 : 0.14)
    readonly property color tertiary: tone(seed, 0.12, 0.55, dark ? 0.76 : 0.40)
    readonly property color tertiaryContainer: tone(seed, 0.12, 0.5, dark ? 0.25 : 0.88)
    readonly property color fgTertiaryContainer: tone(seed, 0.12, 0.5, dark ? 0.88 : 0.14)

    // errors are red whatever the theme
    readonly property color error: dark ? "#ffb4ab" : "#ba1a1a"
    readonly property color fgError: dark ? "#690005" : "#ffffff"
    readonly property color errorContainer: dark ? "#93000a" : "#ffdad6"
    readonly property color fgErrorContainer: dark ? "#ffdad6" : "#410002"

    // surfaces step away from the window colour towards the text colour
    readonly property color surfaceContainerLow: mix(surface, fgSurface, 0.03)
    readonly property color surfaceContainer: mix(surface, fgSurface, 0.06)
    readonly property color surfaceContainerHigh: mix(surface, fgSurface, 0.09)
    readonly property color surfaceContainerHighest: mix(surface, fgSurface, 0.12)
    readonly property color fgSurfaceVariant: mix(surface, fgSurface, 0.74)
    readonly property color outline: mix(surface, fgSurface, 0.46)
    readonly property color outlineVariant: mix(surface, fgSurface, 0.18)
    readonly property color scrim: "#000000"

    // shape: Expressive containers are round
    readonly property int radiusS: 8
    readonly property int radiusM: 12
    readonly property int radiusL: 20
    readonly property int radiusXL: 28

    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 24
    readonly property int s6: 32

    // the theme's own font, whatever it is
    readonly property string fontFamily: Application.font.family
    readonly property string iconFont: Backend.iconFont
    readonly property int durShort: 150
    readonly property int durMedium: 280
}
