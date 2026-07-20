import QtQuick

// Theme is the single source of design tokens for Creator Studio's UI: the dark
// premium palette, the Pretendard typographic scale, the spacing/radius rhythm
// and a few reusable gradients. Every QML page instantiates one `Theme { id: theme }`
// and reads its colours, sizes and fonts from it, so the whole product stays
// visually consistent from one design vocabulary. It holds no application state
// and never touches a controller — it is pure presentation vocabulary. (It is a
// plain component rather than a pragma-singleton so it also resolves when a page
// is loaded standalone by file path, e.g. from the QML smoke tests.)
QtObject {
    id: theme

    // ---- Base surfaces (near-black, layered) ---------------------------------
    readonly property color bg: "#0B0B0F"            // window backdrop
    readonly property color bgDeep: "#070709"        // deepest wells (previews)
    readonly property color surface: "#15151C"       // cards / panels
    readonly property color surfaceElevated: "#1E1E28" // raised cards, inputs
    readonly property color surfaceHover: "#26263340" // hover wash (alpha)
    readonly property color surfaceHigh: "#2A2A38"    // pressed / selected rows

    // ---- Borders / dividers --------------------------------------------------
    readonly property color border: "#2A2A36"
    readonly property color borderStrong: "#3A3A48"
    readonly property color borderFocus: "#6D5EF6"

    // ---- Text ----------------------------------------------------------------
    readonly property color textPrimary: "#F2F2F7"
    readonly property color textSecondary: "#9A9AA8"
    readonly property color textMuted: "#6B6B78"
    readonly property color textOnAccent: "#FFFFFF"

    // ---- Accent (indigo → violet) -------------------------------------------
    readonly property color accent: "#6D5EF6"
    readonly property color accentBright: "#8B7CFF"
    readonly property color accentDim: "#5648C9"
    // Translucent washes use Qt's #AARRGGBB byte order (alpha first).
    readonly property color accentSoft: "#266D5EF6"   // ~15% accent wash

    // ---- Semantic ------------------------------------------------------------
    readonly property color danger: "#FF5C6C"
    readonly property color dangerSoft: "#26FF5C6C"
    readonly property color success: "#35C48B"
    readonly property color warning: "#F5B84A"
    readonly property color info: "#4CB4E8"

    // ---- Accent gradient (primary actions, hero glows) -----------------------
    readonly property color gradientStart: "#6D5EF6"
    readonly property color gradientEnd: "#8B7CFF"

    // ---- Typography ----------------------------------------------------------
    // Pretendard is loaded at startup (src/main.cpp) and set as the application
    // default; naming it explicitly keeps rendering stable if a control resets
    // its font.
    readonly property string fontFamily: "Pretendard"
    readonly property string monoFamily: "JetBrains Mono, Consolas, monospace"

    readonly property int weightRegular: Font.Normal      // 400
    readonly property int weightMedium: Font.Medium       // 500
    readonly property int weightSemiBold: Font.DemiBold   // 600
    readonly property int weightBold: Font.Bold           // 700

    // Type scale (pixel sizes tuned for a 1440×900 desktop canvas).
    readonly property int sizeDisplay: 34
    readonly property int sizeTitle: 24
    readonly property int sizeHeading: 19
    readonly property int sizeSubtitle: 16
    readonly property int sizeBody: 14
    readonly property int sizeLabel: 13
    readonly property int sizeCaption: 12
    readonly property int sizeMicro: 11

    // ---- Spacing scale (4 / 8 / 12 / 16 / 24 / 32) --------------------------
    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 16
    readonly property int spaceXl: 24
    readonly property int spaceXxl: 32

    // ---- Radius --------------------------------------------------------------
    readonly property int radiusSm: 8
    readonly property int radiusMd: 12
    readonly property int radiusLg: 16
    readonly property int radiusPill: 999

    // ---- Motion --------------------------------------------------------------
    readonly property int animFast: 120
    readonly property int animBase: 160
    readonly property int animSlow: 220
}
