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
                                   ? Math.max(0, root.finiteMs(root.ui.recordedDurationMs)
                                              - root.finiteMs(root.ui.liveBufferMs))
                                   : 0
    property bool dragging: false
    property real dragMs: 0
    readonly property real shownMs: root.dragging
                                    ? root.dragMs
                                    : (root.hasUi ? Number(root.ui.scrubPosition) : 0)

    implicitHeight: Theme.hControl
    Layout.minimumWidth: 0

    function finiteMs(value) {
        var number = Number(value)
        return Number.isFinite(number) ? number : 0
    }

    function clampMs(ms) {
        var number = Number(ms)
        if (!Number.isFinite(number)) return 0
        return Math.max(0, Math.min(root.durMax, number))
    }

    function msToX(ms) {
        return root.durMax > 0 && track.width > 0
            ? (root.clampMs(ms) / root.durMax) * track.width
            : 0
    }

    function xToMs(x) {
        var number = Number(x)
        if (!Number.isFinite(number)) return 0
        return root.durMax > 0 && track.width > 0
            ? root.clampMs((Math.max(0, Math.min(track.width, number)) / track.width) * root.durMax)
            : 0
    }

    function seekTo(ms) {
        if (root.hasUi) root.ui.seekPlayback(Math.round(root.clampMs(ms)))
    }

    function beginScrubAtX(x) {
        if (!root.hasUi || root.durMax <= 0) return
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
        if (!root.dragging) return
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

        objectName: "track"
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

                objectName: region.openEnded ? "openRegion" : "closedRegion"
                readonly property real inValue: Number(region.inMs)
                readonly property real outValue: Number(region.outMs)
                readonly property bool openEnded: Number.isFinite(region.outValue) && region.outValue < 0
                readonly property bool durationAvailable: root.durMax > 0
                readonly property real markerWidth: 2
                readonly property bool startValid: Number.isFinite(region.inValue) && region.inValue >= 0
                readonly property bool rangeValid: region.openEnded
                                                   ? region.startValid
                                                   : (region.startValid
                                                      && Number.isFinite(region.outValue)
                                                      && region.outValue >= region.inValue)
                readonly property real startX: region.startValid ? root.msToX(region.inValue) : 0
                readonly property real endX: region.openEnded ? region.startX + region.markerWidth
                                                              : root.msToX(region.outValue)
                readonly property real regionWidth: !region.durationAvailable || !region.rangeValid ? 0
                                                    : region.openEnded
                                                    ? region.markerWidth
                                                    : Math.min(track.width,
                                                               Math.max(region.markerWidth,
                                                                        region.endX - region.startX))

                x: region.openEnded
                   ? Math.max(0, Math.min(track.width - region.markerWidth, region.startX - region.markerWidth / 2))
                   : Math.max(0, Math.min(track.width - region.regionWidth, region.startX))
                y: 0
                width: region.regionWidth
                height: region.durationAvailable && region.rangeValid ? track.height : 0
                color: region.openEnded ? Theme.armed : Theme.accent
                opacity: !region.durationAvailable || !region.rangeValid ? 0.0 : (region.openEnded ? 1.0 : 0.25)
                radius: Theme.r1
            }
        }

        Rectangle {
            visible: root.durMax > 0
            width: root.msToX(root.shownMs)
            height: parent.height
            color: Theme.accent
            opacity: 0.35
            radius: Theme.r1
        }

        Rectangle {
            objectName: "liveEdge"
            readonly property bool durationAvailable: root.durMax > 0

            visible: durationAvailable
            x: Math.max(0, parent.width - width)
            width: 2
            height: parent.height
            color: Theme.recordOnAir
            opacity: 0.7
        }

        Rectangle {
            visible: root.durMax > 0
            x: Math.max(0, Math.min(parent.width - width, root.msToX(root.shownMs) - width / 2))
            width: 3
            height: parent.height
            color: Theme.textHi
        }
    }

    MouseArea {
        id: scrubMouse

        objectName: "scrubMouse"
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.hasUi && root.durMax > 0
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
