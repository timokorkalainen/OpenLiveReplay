import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.SpinBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, 96)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitBackgroundHeight, Theme.hControl)

    leftPadding: Theme.s2
    rightPadding: Theme.s2 + (up.indicator ? up.indicator.width : 0)
    opacity: control.enabled ? 1.0 : 0.5

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    validator: IntValidator {
        locale: control.locale.name
        bottom: Math.min(control.from, control.to)
        top: Math.max(control.from, control.to)
    }

    contentItem: TextInput {
        text: control.displayText
        font: control.font
        color: Theme.textHi
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnTally
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 0 : parent.width - width
        height: parent.height / 2
        implicitWidth: 26
        implicitHeight: Theme.hControl / 2
        color: control.up.pressed ? Theme.panelPressed : (control.up.hovered ? Theme.panelHover : Theme.panelRaised)
        Text {
            text: "+"
            font.pixelSize: Theme.fsHeading
            color: Theme.textBody
            anchors.fill: parent
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    down.indicator: Rectangle {
        x: control.mirrored ? 0 : parent.width - width
        y: parent.height / 2
        height: parent.height / 2
        implicitWidth: 26
        implicitHeight: Theme.hControl / 2
        color: control.down.pressed ? Theme.panelPressed : (control.down.hovered ? Theme.panelHover : Theme.panelRaised)
        Text {
            text: "−"
            font.pixelSize: Theme.fsHeading
            color: Theme.textBody
            anchors.fill: parent
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    background: Rectangle {
        implicitWidth: 96
        implicitHeight: Theme.hControl
        radius: Theme.r1
        color: Theme.panelPressed
        border.width: Theme.borderW
        border.color: control.activeFocus ? Theme.focusRing : Theme.line
    }
}
