import QtQuick
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 520
    height: 640
    color: Theme.canvas

    ConfigDrawer {
        id: drawer
        parent: parent
        enter: Transition { }
        exit: Transition { }
        visible: true
        Component.onCompleted: drawer.open()
    }
}
