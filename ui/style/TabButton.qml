import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.TabButton {
    id: control

    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, 72)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Theme.hControl)
    leftPadding: Theme.s3
    rightPadding: Theme.s3
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody
    font.weight: control.checked ? Font.DemiBold : Font.Medium

    contentItem: Text {
        text: control.text
        font: control.font
        opacity: control.enabled ? 1.0 : 0.5
        color: control.checked ? Theme.textHi : Theme.textDim
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitHeight: Theme.hControl
        color: control.checked ? Theme.panelRaised : (control.hovered ? Theme.panelHover : "transparent")
        Rectangle { // active underline
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 2
            color: control.checked ? Theme.accent : "transparent"
        }
    }
}
