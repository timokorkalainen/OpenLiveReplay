import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, Theme.hControl)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Theme.hControl)
    padding: Theme.s1
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    contentItem: Text {
        text: control.text
        font: control.font
        opacity: control.enabled ? 1.0 : 0.5
        color: control.checked ? Theme.accent : Theme.textBody
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: Theme.hControl
        implicitHeight: Theme.hControl
        radius: Theme.r1
        color: control.down ? Theme.panelPressed
                            : (control.checked || control.hovered ? Theme.panelHover : "transparent")
        border.width: control.visualFocus ? Theme.borderW : 0
        border.color: Theme.focusRing
    }
}
