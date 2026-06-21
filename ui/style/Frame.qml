import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.Frame {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    padding: Theme.s2

    background: Rectangle {
        radius: Theme.r1
        color: Theme.panel
        border.width: Theme.borderW
        border.color: Theme.line
    }
}
