pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

ColumnLayout {
    id: root
    property var ui
    readonly property bool hasUi: ui !== null && ui !== undefined
    property bool showTimeOfDay: ui ? ui.timeOfDayMode : false
    property int clockTick: 0
    property bool holdWasPlaying: false

    Layout.fillWidth: true
    spacing: 8

    function formatTimecode(ms) {
        if (root.hasUi) return root.ui.recordTimecode(ms)

        var totalSeconds = Math.max(0, Math.floor(ms / 1000))
        var hh = Math.floor(totalSeconds / 3600)
        var mm = Math.floor((totalSeconds % 3600) / 60)
        var ss = totalSeconds % 60
        return (hh < 10 ? "0" + hh : "" + hh)
            + ":" + (mm < 10 ? "0" + mm : "" + mm)
            + ":" + (ss < 10 ? "0" + ss : "" + ss)
    }

    function formatTimeOfDay(epochMs) {
        if (epochMs <= 0) return "--:--:--"
        var d = new Date(epochMs)
        var hh = d.getHours() < 10 ? "0" + d.getHours() : "" + d.getHours()
        var mm = d.getMinutes() < 10 ? "0" + d.getMinutes() : "" + d.getMinutes()
        var ss = d.getSeconds() < 10 ? "0" + d.getSeconds() : "" + d.getSeconds()
        return hh + ":" + mm + ":" + ss
    }

    Timer {
        id: clockTimer
        interval: 500
        running: root.showTimeOfDay
        repeat: true
        onTriggered: root.clockTick = root.clockTick + 1
    }

    Slider {
        id: scrubBar
        Layout.fillWidth: true
        from: 0
        to: root.hasUi ? Math.max(0, root.ui.recordedDurationMs - root.ui.liveBufferMs) : 100
        // While the user is dragging, hold the handle at their drag position;
        // otherwise follow the playhead. Without the pressed guard,
        // scrubPositionChanged (~30/s while recording) yanks the handle back mid-drag.
        value: scrubBar.pressed ? scrubBar.value : (root.hasUi ? root.ui.scrubPosition : 0)

        onMoved: {
            if (root.hasUi) {
                root.ui.seekPlayback(value)
            }
        }
        onPressedChanged: {
            // On release (pressed -> false) flush the final scrub target and end
            // the coalesce gesture.
            if (!scrubBar.pressed && root.hasUi) {
                root.ui.endScrubGesture()
            }
        }

        background: Rectangle {
            height: 6
            radius: 3
            color: Theme.line
            Rectangle {
                width: scrubBar.visualPosition * parent.width
                height: parent.height
                color: Theme.accent
                radius: 3
            }
        }

        handle: Rectangle {
            implicitWidth: 0
            implicitHeight: 0
            width: 0
            height: 0
            visible: false
        }
    }

    // Tier3 replay cue list: mark in/out at the playhead, recall as a
    // frame-perfect armed cut (pre-rolled, no flash). "Play Playlist"
    // runs the rundown -- it auto-advances across each entry boundary with
    // a frame-perfect cut, honoring per-entry speed; a manual scrub or
    // recall exits playout.
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        spacing: 8
        Button { text: "Mark In"; enabled: root.hasUi; onClicked: root.ui.markIn() }
        Button { text: "Mark Out"; enabled: root.hasUi; onClicked: root.ui.markOut() }
        Button { text: "Recall 0"; enabled: root.hasUi; onClicked: root.ui.recallEntry(0) }
        Button {
            text: "Play Playlist"
            enabled: root.hasUi
            onClicked: root.ui.playPlaylist(0)
        }
        Button {
            text: "Stop Playout"
            enabled: root.hasUi
            onClicked: root.ui.stopPlaylistPlayout()
        }
    }

    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        Layout.fillWidth: true
        spacing: 12

        Text {
            // Single source of truth (UIManager): the same string the
            // Stream Deck shows. Do not reformat here.
            text: root.hasUi ? root.ui.playbackTimecode : "00:00:00"
            color: Theme.textHi
            font.family: Theme.fontMono
            font.pixelSize: 14
            Layout.alignment: Qt.AlignVCenter
            MouseArea {
                anchors.fill: parent
                enabled: root.hasUi
                onClicked: root.ui.timeOfDayMode = !root.ui.timeOfDayMode
            }
        }

        Item { Layout.fillWidth: true }

        Button {
            text: root.hasUi && root.ui.transport.isPlaying ? "PAUSE" : "PLAY"
            enabled: root.hasUi
            onClicked: root.ui.playPause()
            highlighted: root.hasUi && root.ui.transport.isPlaying
        }

        Button {
            text: "-5.0x"
            enabled: root.hasUi
            onPressed: {
                root.ui.cancelFollowLive()
                root.holdWasPlaying = root.ui.transport.isPlaying
                root.ui.transport.setSpeed(-5.0)
                root.ui.transport.setPlaying(true)
            }
            onReleased: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
            onCanceled: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
        }

        Button {
            text: "<"
            enabled: root.hasUi
            onClicked: root.ui.stepFrameBack()
        }

        Button {
            text: ">"
            enabled: root.hasUi
            onClicked: root.ui.stepFrame()
        }

        Button {
            text: "0.25x"
            enabled: root.hasUi
            onPressed: {
                root.ui.cancelFollowLive()
                root.holdWasPlaying = root.ui.transport.isPlaying
                root.ui.transport.setSpeed(0.25)
                root.ui.transport.setPlaying(true)
            }
            onReleased: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
            onCanceled: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
        }

        Button {
            text: "0.5x"
            enabled: root.hasUi
            onPressed: {
                root.ui.cancelFollowLive()
                root.holdWasPlaying = root.ui.transport.isPlaying
                root.ui.transport.setSpeed(0.5)
                root.ui.transport.setPlaying(true)
            }
            onReleased: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
            onCanceled: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
        }

        Button {
            text: "2.0x"
            enabled: root.hasUi
            onPressed: {
                root.ui.cancelFollowLive()
                root.holdWasPlaying = root.ui.transport.isPlaying
                root.ui.transport.setSpeed(2.0)
                root.ui.transport.setPlaying(true)
            }
            onReleased: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
            onCanceled: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
        }

        Button {
            text: "5.0x"
            enabled: root.hasUi
            onPressed: {
                root.ui.cancelFollowLive()
                root.holdWasPlaying = root.ui.transport.isPlaying
                root.ui.transport.setSpeed(5.0)
                root.ui.transport.setPlaying(true)
            }
            onReleased: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
            onCanceled: {
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(root.holdWasPlaying)
            }
        }

        Button {
            text: "Live"
            enabled: root.hasUi
            onClicked: {
                root.ui.goLive()
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(true)
            }
        }

        Button {
            text: "Capture"
            enabled: root.hasUi
            onClicked: root.ui.captureCurrent()
        }

        Item { Layout.fillWidth: true }

        Text {
            text: root.showTimeOfDay
                ? (root.clockTick >= 0
                   ? root.formatTimeOfDay(Date.now())
                   : root.formatTimeOfDay(Date.now()))
                : root.formatTimecode(root.hasUi ? root.ui.recordedDurationMs : 0)
            color: Theme.textHi
            font.family: Theme.fontMono
            font.pixelSize: 14
            Layout.alignment: Qt.AlignVCenter
            MouseArea {
                anchors.fill: parent
                enabled: root.hasUi
                onClicked: root.ui.timeOfDayMode = !root.ui.timeOfDayMode
            }
            onVisibleChanged: clockTimer.restart()
        }
    }
}
