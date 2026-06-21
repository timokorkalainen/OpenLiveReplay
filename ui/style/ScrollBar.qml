import QtQuick
import QtQuick.Templates as T
import OlrTheme

T.ScrollBar {
    id: control

    implicitWidth: control.interactive ? 10 : 8
    implicitHeight: control.interactive ? 10 : 8

    padding: 2
    minimumSize: 0.08
    visible: control.policy !== T.ScrollBar.AlwaysOff

    contentItem: Rectangle {
        implicitWidth: control.interactive ? 8 : 6
        implicitHeight: control.interactive ? 8 : 6
        radius: width / 2
        color: control.pressed ? Theme.lineStrong : (control.hovered ? Theme.line : Qt.darker(Theme.line, 1.15))
        opacity: control.active ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }
}
