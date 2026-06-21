import QtQuick
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 1000
    height: 80
    color: Theme.canvas

    StatusStrip {
        width: parent.width
        anchors.top: parent.top
    }
}
