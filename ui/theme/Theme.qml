pragma Singleton
import QtQuick

// OpenLiveReplay design tokens — the single source of truth for the broadcast-console
// look. Replaces ~65 inline hex literals and ~9 ad-hoc font sizes. Used by the OlrStyle
// custom QQC2 style and directly by the app's bespoke components.
//
// Dark, low-glare, dense. Semantic tally colors mean exactly ONE thing each, so the one
// color an operator scans for (record/on-air red) is never ambiguous.
QtObject {
    id: theme

    // --- Surfaces (back-to-front) ---
    readonly property color canvas: "#0C0D10"      // app background / video letterbox
    readonly property color panel: "#16191D"        // status bar, transport, rails
    readonly property color panelRaised: "#21262C"  // cards, rows, fields, menus
    readonly property color panelHover: "#2A3037"   // hover surface
    readonly property color panelPressed: "#121417" // pressed/sunken
    readonly property color line: "#323840"          // 1px dividers / control borders
    readonly property color lineStrong: "#444B54"    // emphasized borders / focus ring base
    readonly property color scrim: "#8C000000"        // 55% black — UMD bars, drawer dim
    readonly property color warnSurface: "#2B2615"   // soft-warning banner, distinct from error
    readonly property color warnBorder: "#5B4818"

    // --- Text (all >= 4.5:1 on panel/panelRaised) ---
    readonly property color textHi: "#ECEEF0"   // primary
    readonly property color textBody: "#C2C7CC" // body
    readonly property color textDim: "#8A9099"  // secondary (fixes the old ~2.6:1 #666)
    readonly property color textDisabled: "#5A6068"
    readonly property color textOnTally: "#0E0F11" // text on a saturated tally fill

    // --- Semantic tally (one token per MEANING) ---
    readonly property color recordOnAir: "#E53935" // record active AND the single PGM/on-air tally
    readonly property color armed: "#FFB300"        // cued / preview / armed cut
    readonly property color warning: "#FFD166"      // generic warnings / destructive previews
    readonly property color ready: "#43A047"        // healthy / ready / connected
    readonly property color idle: "#6E6E6E"         // no source / inactive
    readonly property color error: "#FF7043"        // no-signal / link-error / NDI drop (render as hatch/blink, never on-air)

    // --- Accent (focus / selection / scrub progress) ---
    readonly property color accent: "#2E7BFF"
    readonly property color accentHover: "#4A8DFF"
    readonly property color accentPressed: "#2C76F4"
    readonly property color focusRing: "#2E7BFF"

    // --- Type --- QML's font.family takes a SINGLE family; a comma list (and font.families,
    // which is not a QML font property) silently fail. So use Qt generic families, which
    // resolve to the platform UI sans and a real monospace on every OS.
    readonly property string fontFamily: "sans-serif" // generic UI sans
    readonly property string fontMono: "monospace"    // generic monospace (tabular) for timecode
    readonly property int fsMicro: 11
    readonly property int fsBody: 13
    readonly property int fsHeading: 15
    readonly property int fsDisplay: 18
    readonly property int fsTcInline: 14 // inline timecode
    readonly property int fsTc: 22       // the always-on read-across-the-gallery timecode

    // --- Spacing (8px base) ---
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 16 // only between major region groups
    readonly property int pad: 8 // default region/control inset (was ~48px stacked chrome)

    // --- Radius ---
    readonly property int r0: 0 // canvas / timeline / table cells
    readonly property int r1: 4 // compact controls
    readonly property int r2: 6 // large primary transport keys

    // --- Control sizes (two tiers; inverts "RECORD is biggest") ---
    readonly property int hPrimary: 48   // PLAY/PAUSE, GO LIVE
    readonly property int hTransport: 44 // step / shuttle
    readonly property int hAction: 40    // TAKE / NEXT
    readonly property int hControl: 30   // standard buttons / combos / fields
    readonly property int hCompact: 26   // rundown rows, segmented chips, toggles
    readonly property int hField: 30     // fields aligned to buttons for even rows
    readonly property int dotSize: 12
    readonly property int tallyBorder: 3
    readonly property int borderW: 1

    // --- Responsive breakpoints + min widths (so narrow windows scroll, not squash) ---
    readonly property int bpXS: 480
    readonly property int bpSM: 520
    readonly property int bpMD: 840
    readonly property int bpLG: 1200
    readonly property int minWUrl: 220
    readonly property int minWField: 120
    readonly property int minWCombo: 110

    // --- Window floors ---
    readonly property int windowMinW: 480
    readonly property int windowMinH: 480
}
