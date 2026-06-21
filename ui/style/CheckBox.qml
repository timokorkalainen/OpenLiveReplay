import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.CheckBox {
    id: control

    implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding,
                            implicitIndicatorWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding, Theme.hControl)
    padding: Theme.s1
    spacing: Theme.s2

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fsBody

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.r1
        color: control.checked ? Theme.accent : Theme.panelPressed
        border.width: Theme.borderW
        border.color: control.checked ? Theme.accent
                     : (control.visualFocus ? Theme.focusRing : Theme.lineStrong)

        Text {
            text: "✓"
            visible: control.checkState === Qt.Checked
            color: Theme.textOnTally
            font.pixelSize: 13
            anchors.centerIn: parent
        }
        Rectangle { // partially-checked dash
            visible: control.checkState === Qt.PartiallyChecked
            width: 10; height: 2; radius: 1
            color: Theme.textOnTally
            anchors.centerIn: parent
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
