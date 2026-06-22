import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.Switch {
    id: control

    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding,
                            implicitIndicatorWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding, Theme.hControl)
    padding: Theme.s1
    spacing: Theme.s2
    opacity: control.enabled ? 1.0 : 0.5

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 20
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked ? Theme.ready : Theme.panelPressed
        border.width: Theme.borderW
        border.color: control.visualFocus ? Theme.focusRing
                                          : (control.checked ? Theme.ready : Theme.lineStrong)

        Rectangle {
            x: control.checked ? parent.width - width - 2 : 2
            y: 2
            width: 16
            height: 16
            radius: 8
            color: Theme.textHi
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: Theme.textBody
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
