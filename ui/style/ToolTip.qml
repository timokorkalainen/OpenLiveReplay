import QtQuick
import QtQuick.Templates as T
import OlrTheme

// Dark tooltip. QtQuick Controls do NOT inherit QGuiApplication's palette, so the Basic
// fallback would render white-on-dark; this delegate paints from Theme tokens instead.
T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - 4

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    margins: 6
    leftPadding: Theme.s2
    rightPadding: Theme.s2
    topPadding: Theme.s1
    bottomPadding: Theme.s1

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent
                 | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsMicro
        color: Theme.textHi
        wrapMode: Text.WordWrap
    }

    background: Rectangle {
        color: Theme.panelRaised
        border.width: Theme.borderW
        border.color: Theme.lineStrong
        radius: Theme.r1
    }
}
