import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             Theme.hCompact)

    leftPadding: Theme.s2
    rightPadding: Theme.s2
    topPadding: Theme.s1
    bottomPadding: Theme.s1
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    contentItem: Text {
        leftPadding: control.indicator && control.checkable ? control.indicator.width + control.spacing : 0
        rightPadding: control.arrow && control.subMenu ? control.arrow.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.highlighted ? Theme.textHi : Theme.textBody
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.checkable && control.checked
        text: "✓"
        font.pixelSize: Theme.fsBody
        color: control.highlighted ? Theme.textHi : Theme.accent
    }

    arrow: Text {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.subMenu
        text: "›"
        font.pixelSize: Theme.fsHeading
        color: control.highlighted ? Theme.textHi : Theme.textDim
    }

    background: Rectangle {
        implicitWidth: 180
        implicitHeight: Theme.hCompact
        color: control.down || control.highlighted ? Theme.accent
              : (control.hovered ? Theme.panelHover : "transparent")
        radius: Theme.r0
    }
}
