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
}
