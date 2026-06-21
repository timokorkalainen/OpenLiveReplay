import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, 80)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Theme.hCompact)

    leftPadding: Theme.s2
    rightPadding: Theme.s2
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.textHi
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: control.highlighted || control.down ? Theme.accent
              : (control.hovered ? Theme.panelHover : "transparent")
        radius: Theme.r0
    }
}
