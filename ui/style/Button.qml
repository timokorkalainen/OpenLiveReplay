import QtQuick
import QtQuick.Templates as T
import OlrTheme

// Bespoke OlrStyle button: flat, dark, sharp. Default = subtle raised panel; set
// `highlighted: true` for an accent-filled primary action. Compact by default; the app
// sets a larger implicitHeight (Theme.hPrimary/hTransport) on transport keys.
T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Theme.hControl)

    leftPadding: Theme.s3
    rightPadding: Theme.s3
    topPadding: Theme.s1
    bottomPadding: Theme.s1
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody
    font.weight: control.highlighted ? Font.DemiBold : Font.Medium

    contentItem: Text {
        text: control.text
        font: control.font
        opacity: control.enabled ? 1.0 : 0.5
        color: control.highlighted ? Theme.textOnTally : Theme.textHi
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 64
        implicitHeight: Theme.hControl
        radius: Theme.r1
        color: {
            if (control.highlighted)
                return control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accent)
            if (control.down) return Theme.panelPressed
            if (control.hovered) return Theme.panelHover
            return Theme.panelRaised
        }
        border.width: Theme.borderW
        border.color: control.highlighted ? "transparent"
                                          : (control.visualFocus ? Theme.focusRing : Theme.line)
    }
}
