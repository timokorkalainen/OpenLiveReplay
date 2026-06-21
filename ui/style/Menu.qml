import QtQuick
import QtQuick.Window
import QtQuick.Templates as T
import OlrTheme

// Dark menu surface (the status-bar "Fullscreen Multiview" menu and any future menus).
T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 0
    overlap: 1
    padding: Theme.s1

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window
                     ? contentHeight + control.topPadding + control.bottomPadding > control.height
                     : false
        clip: true
        currentIndex: control.currentIndex
        boundsBehavior: Flickable.StopAtBounds
    }

    background: Rectangle {
        implicitWidth: 180
        color: Theme.panelRaised
        border.width: Theme.borderW
        border.color: Theme.lineStrong
        radius: Theme.r1
    }
}
