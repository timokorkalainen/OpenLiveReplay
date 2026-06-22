pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

Item {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== null && root.ui !== undefined
    readonly property real durMax: root.hasUi
                                   ? Math.max(0, Number(root.ui.recordedDurationMs) - Number(root.ui.liveBufferMs))
                                   : 0
    property bool dragging: false
    property real dragMs: 0
    readonly property real shownMs: root.dragging
                                    ? root.dragMs
                                    : (root.hasUi ? Number(root.ui.scrubPosition) : 0)

    implicitHeight: Theme.hControl
    Layout.minimumWidth: 0

    function clampMs(ms) {
        return Math.max(0, Math.min(root.durMax, Number(ms)))
    }

    function msToX(ms) {
        return root.durMax > 0 && track.width > 0
            ? (root.clampMs(ms) / root.durMax) * track.width
            : 0
    }

    function xToMs(x) {
        return root.durMax > 0 && track.width > 0
            ? root.clampMs((Math.max(0, Math.min(track.width, Number(x))) / track.width) * root.durMax)
            : 0
    }

    function seekTo(ms) {
        if (root.hasUi) root.ui.seekPlayback(Math.round(root.clampMs(ms)))
    }

    function beginScrubAtX(x) {
        root.dragging = true
        root.dragMs = root.xToMs(x)
        root.seekTo(root.dragMs)
    }

    function updateScrubAtX(x) {
        if (root.dragging) {
            root.dragMs = root.xToMs(x)
            root.seekTo(root.dragMs)
        }
    }

    function endScrubAtX(x) {
        root.dragMs = root.xToMs(x)
        root.seekTo(root.dragMs)
        root.finishScrub()
    }

    function finishScrub() {
        root.dragging = false
        if (root.hasUi) root.ui.endScrubGesture()
    }

    Rectangle {
        id: track

        anchors.fill: parent
        anchors.topMargin: Theme.s2
        anchors.bottomMargin: Theme.s2
        radius: Theme.r1
        color: Theme.panelPressed
        border.width: Theme.borderW
        border.color: Theme.line
        clip: true

        Repeater {
            model: root.hasUi && root.ui.playlistModel !== undefined && root.ui.playlistModel !== null
                   ? root.ui.playlistModel
                   : 0

            delegate: Rectangle {
                id: region

                required property var inMs
                required property var outMs

                readonly property real startX: root.msToX(Number(region.inMs))
                readonly property real endX: Number(region.outMs) < 0
                                             ? region.startX + 2
                                             : root.msToX(Number(region.outMs))

                x: region.startX
                y: 0
                width: Math.max(2, region.endX - region.startX)
                height: track.height
                color: Number(region.outMs) < 0 ? Theme.armed : Theme.accent
                opacity: Number(region.outMs) < 0 ? 1.0 : 0.25
                radius: Theme.r1
            }
        }

        Rectangle {
            width: root.msToX(root.shownMs)
            height: parent.height
            color: Theme.accent
            opacity: 0.35
            radius: Theme.r1
        }

        Rectangle {
            x: Math.max(0, parent.width - width)
            width: 2
            height: parent.height
            color: Theme.recordOnAir
            opacity: 0.7
        }

        Rectangle {
            x: Math.max(0, Math.min(parent.width - width, root.msToX(root.shownMs) - width / 2))
            width: 3
            height: parent.height
            color: Theme.textHi
        }

        MouseArea {
            id: scrubMouse

            anchors.fill: parent
            hoverEnabled: true
            enabled: root.hasUi
            onPressed: (mouse) => {
                root.beginScrubAtX(mouse.x)
                mouse.accepted = true
            }
            onPositionChanged: (mouse) => {
                root.updateScrubAtX(mouse.x)
            }
            onReleased: (mouse) => {
                root.endScrubAtX(mouse.x)
            }
            onCanceled: root.finishScrub()

            ToolTip.visible: scrubMouse.containsMouse && root.hasUi
            ToolTip.text: root.hasUi ? root.ui.recordTimecode(root.xToMs(scrubMouse.mouseX)) : ""
        }
    }
}
