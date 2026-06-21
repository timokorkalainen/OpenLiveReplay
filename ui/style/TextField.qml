import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.TextField {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, 100)
    implicitHeight: Math.max(contentHeight + topPadding + bottomPadding,
                             implicitBackgroundHeight, Theme.hField)

    leftPadding: Theme.s2
    rightPadding: Theme.s2
    topPadding: Theme.s1
    bottomPadding: Theme.s1

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody
    color: Theme.textHi
    selectionColor: Theme.accent
    selectedTextColor: Theme.textOnTally
    placeholderTextColor: Theme.textDim
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        implicitWidth: Theme.minWField
        implicitHeight: Theme.hField
        radius: Theme.r1
        color: control.enabled ? Theme.panelPressed : Theme.panel
        border.width: Theme.borderW
        border.color: control.activeFocus ? Theme.focusRing
                                          : (control.hovered ? Theme.lineStrong : Theme.line)
    }
}
