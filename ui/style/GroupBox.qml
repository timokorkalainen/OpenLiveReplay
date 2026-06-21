import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.GroupBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    leftPadding: Theme.s3
    rightPadding: Theme.s3
    topPadding: (implicitLabelHeight > 0 ? implicitLabelHeight + Theme.s2 : 0) + Theme.s3
    bottomPadding: Theme.s3
    spacing: Theme.s2

    label: Text {
        x: control.leftPadding
        width: control.availableWidth
        text: control.title
        color: Theme.textDim
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fsMicro
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        elide: Text.ElideRight
    }

    background: Rectangle {
        y: control.topPadding - control.bottomPadding
        width: parent.width
        height: parent.height - control.topPadding + control.bottomPadding
        radius: Theme.r1
        color: Theme.panel
        border.width: Theme.borderW
        border.color: Theme.line
    }
}
