import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.Slider {
    id: control

    implicitWidth: control.horizontal ? 120 : implicitHandleWidth + leftPadding + rightPadding
    implicitHeight: control.horizontal ? implicitHandleHeight + topPadding + bottomPadding : 120
    padding: control.horizontal ? 0 : Theme.s1

    background: Rectangle {
        x: control.leftPadding + (control.horizontal ? 0 : (control.availableWidth - width) / 2)
        y: control.topPadding + (control.horizontal ? (control.availableHeight - height) / 2 : 0)
        width: control.horizontal ? control.availableWidth : 4
        height: control.horizontal ? 4 : control.availableHeight
        radius: 2
        color: Theme.line

        Rectangle { // filled (progress)
            width: control.horizontal ? control.position * parent.width : parent.width
            height: control.horizontal ? parent.height : control.position * parent.height
            y: control.horizontal ? 0 : parent.height - height
            radius: 2
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + (control.horizontal ? control.visualPosition * (control.availableWidth - width)
                                                     : (control.availableWidth - width) / 2)
        y: control.topPadding + (control.horizontal ? (control.availableHeight - height) / 2
                                                    : control.visualPosition * (control.availableHeight - height))
        implicitWidth: 14
        implicitHeight: 14
        radius: 7
        color: control.pressed ? Theme.accentPressed : Theme.textHi
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focusRing
    }
}
