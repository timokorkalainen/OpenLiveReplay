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
    readonly property int avail: root.parent ? root.parent.width : root.width
    readonly property bool tight: root.avail <= Theme.bpMD
    readonly property bool compact: root.avail < Theme.bpMD
    readonly property int compactTimeWidth: 54
    readonly property int compactShuttleWidth: 26
    readonly property int compactCaptureWidth: 40

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.preferredHeight: implicitHeight
    implicitHeight: scrubBar.implicitHeight + transportRow.implicitHeight + root.spacing
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

    ScrubTimeline {
        id: scrubBar

        Layout.fillWidth: true
        ui: root.ui
    }

    RowLayout {
        id: transportRow

        Layout.alignment: Qt.AlignHCenter
        Layout.fillWidth: true
        spacing: root.compact || root.tight ? Theme.s1 : 12

        Text {
            // Single source of truth (UIManager): the same string the
            // Stream Deck shows. Do not reformat here.
            text: root.hasUi ? root.ui.playbackTimecode : "00:00:00"
            color: Theme.textHi
            font.family: Theme.fontMono
            font.pixelSize: root.compact || root.tight ? Theme.fsMicro : 14
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.compact || root.tight ? root.compactTimeWidth : implicitWidth
            elide: Text.ElideRight
            MouseArea {
                anchors.fill: parent
                enabled: root.hasUi
                onClicked: root.ui.timeOfDayMode = !root.ui.timeOfDayMode
            }
        }

        Item {
            Layout.fillWidth: true
            visible: !root.compact
        }

        Button {
            text: root.hasUi && root.ui.transport.isPlaying ? "PAUSE" : "PLAY"
            enabled: root.hasUi
            onClicked: root.ui.playPause()
            highlighted: root.hasUi && root.ui.transport.isPlaying
        }

        RowLayout {
            spacing: root.compact ? 1 : (root.tight ? Theme.s1 : 12)

            Button {
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: root.compact ? "-5" : "-5.0x"
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
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: "<"
                enabled: root.hasUi
                onClicked: root.ui.stepFrameBack()
            }

            Button {
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: ">"
                enabled: root.hasUi
                onClicked: root.ui.stepFrame()
            }

            Button {
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: root.compact ? "¼" : "0.25x"
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
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: root.compact ? "½" : "0.5x"
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
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: root.compact ? "2" : "2.0x"
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
                Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                leftPadding: root.compact ? Theme.s1 : Theme.s3
                rightPadding: root.compact ? Theme.s1 : Theme.s3
                font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                text: root.compact ? "5" : "5.0x"
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
            Layout.preferredWidth: root.compact ? root.compactCaptureWidth : implicitWidth
            leftPadding: root.compact ? Theme.s1 : Theme.s3
            rightPadding: root.compact ? Theme.s1 : Theme.s3
            font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
            text: root.compact ? "Cap" : "Capture"
            enabled: root.hasUi
            onClicked: root.ui.captureCurrent()
        }

        Item {
            Layout.fillWidth: true
            visible: !root.compact
        }

        Text {
            text: root.showTimeOfDay
                ? (root.clockTick >= 0
                   ? root.formatTimeOfDay(Date.now())
                   : root.formatTimeOfDay(Date.now()))
                : root.formatTimecode(root.hasUi ? root.ui.recordedDurationMs : 0)
            color: Theme.textHi
            font.family: Theme.fontMono
            font.pixelSize: root.compact || root.tight ? Theme.fsMicro : 14
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: root.compact || root.tight ? root.compactTimeWidth : implicitWidth
            elide: Text.ElideRight
            MouseArea {
                anchors.fill: parent
                enabled: root.hasUi
                onClicked: root.ui.timeOfDayMode = !root.ui.timeOfDayMode
            }
            onVisibleChanged: clockTimer.restart()
        }
    }
}
