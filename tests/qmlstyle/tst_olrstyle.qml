import QtQuick
import QtQuick.Controls
import QtTest
import OlrTheme

// Verifies the bespoke OlrStyle custom QQC2 style actually LOADS and applies the
// design tokens (rather than silently falling back to Basic), and that the Theme
// token singleton is coherent. Runs offscreen.
TestCase {
    id: tc
    name: "OlrStyle"
    when: windowShown
    width: 240
    height: 120

    Button {
        id: btn
        text: "X"
    }
    Button {
        id: primary
        text: "GO"
        highlighted: true
    }
    TextField {
        id: field
        text: "input"
    }
    ComboBox {
        id: combo
        model: ["A", "B"]
    }

    function channelToLinear(channel) {
        return channel <= 0.03928
            ? channel / 12.92
            : Math.pow((channel + 0.055) / 1.055, 2.4)
    }

    function relativeLuminance(color) {
        return 0.2126 * channelToLinear(color.r)
            + 0.7152 * channelToLinear(color.g)
            + 0.0722 * channelToLinear(color.b)
    }

    function contrastRatio(foreground, background) {
        var fg = relativeLuminance(foreground)
        var bg = relativeLuminance(background)
        var high = Math.max(fg, bg)
        var low = Math.min(fg, bg)
        return (high + 0.05) / (low + 0.05)
    }

    function verifyContrast(name, foreground, background, minimum) {
        var ratio = contrastRatio(foreground, background)
        verify(ratio >= minimum,
               name + " contrast " + ratio.toFixed(2) + " below " + minimum.toFixed(1))
    }

    // The OlrStyle Button default background is Theme.panelRaised; a highlighted
    // button is Theme.accent. If the style failed to load and fell back to Basic,
    // these would not match the tokens.
    function test_buttonUsesOlrStyleTokens() {
        compare(btn.background.color, Theme.panelRaised)
        compare(primary.background.color, Theme.accent)
    }

    // Token coherence: semantic tally colors are distinct, the timecode size is the
    // largest, and the window floor is sane.
    function test_designTokensCoherent() {
        verify(Theme.recordOnAir !== Theme.ready)
        verify(Theme.recordOnAir !== Theme.armed)
        verify(Theme.fsTc > Theme.fsBody)
        verify(Theme.windowMinW >= 480 && Theme.windowMinH >= 480)
        verify(Theme.minWUrl > Theme.minWField)
        verify(Theme.warnSurface !== Theme.panel)
        verify(Theme.warnBorder !== Theme.line)
    }

    function test_contrastTokensMeetThresholds() {
        var surfaces = [Theme.canvas, Theme.panel, Theme.panelRaised, Theme.panelPressed]
        for (var i = 0; i < surfaces.length; ++i) {
            verifyContrast("textHi surface " + i, Theme.textHi, surfaces[i], 4.5)
            verifyContrast("textBody surface " + i, Theme.textBody, surfaces[i], 4.5)
            verifyContrast("textDim surface " + i, Theme.textDim, surfaces[i], 4.5)
        }

        verifyContrast("warning text", Theme.armed, Theme.warnSurface, 4.5)
        verifyContrast("textOnTally record", Theme.textOnTally, Theme.recordOnAir, 4.5)
        verifyContrast("textOnTally armed", Theme.textOnTally, Theme.armed, 4.5)
        verifyContrast("textOnTally ready", Theme.textOnTally, Theme.ready, 4.5)
        verifyContrast("textOnTally accent", Theme.textOnTally, Theme.accent, 4.5)
    }

    function test_focusAndHitTargetsUseTokens() {
        btn.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(btn.background.border, "color", Theme.focusRing)
        verify(btn.implicitHeight >= Theme.hControl)

        field.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(field.background.border, "color", Theme.focusRing)
        verify(field.implicitHeight >= Theme.hField)

        combo.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(combo.background.border, "color", Theme.focusRing)
        verify(combo.implicitHeight >= Theme.hControl)
        verify(Theme.hCompact >= 24)
    }
}
