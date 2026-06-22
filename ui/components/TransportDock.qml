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
    readonly property int primaryKeyHeight: root.compact ? Theme.hControl : Theme.hPrimary
    readonly property int liveKeyHeight: root.compact ? Theme.hControl : Theme.hTransport
    readonly property int actionKeyHeight: root.compact ? Theme.hControl : Theme.hAction
    readonly property int shuttleKeyHeight: Theme.hControl
    readonly property int shuttleFramePadding: root.compact ? 0 : Theme.s1
    readonly property int shuttleFrameHeight: root.shuttleKeyHeight + root.shuttleFramePadding * 2
    readonly property int deckRowHeight: Math.max(root.primaryKeyHeight,
                                                  root.liveKeyHeight,
                                                  root.actionKeyHeight,
                                                  root.shuttleFrameHeight)

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.preferredHeight: implicitHeight
    implicitHeight: scrubBar.implicitHeight + root.deckRowHeight + root.spacing
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
        Layout.preferredHeight: root.deckRowHeight
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
            objectName: "transportPlayButton"
            text: root.hasUi && root.ui.transport.isPlaying ? "PAUSE" : "PLAY"
            implicitHeight: root.primaryKeyHeight
            Layout.preferredHeight: root.primaryKeyHeight
            font.pixelSize: root.compact ? Theme.fsBody : Theme.fsHeading
            enabled: root.hasUi
            onClicked: root.ui.playPause()
            highlighted: root.hasUi && root.ui.transport.isPlaying
        }

        Frame {
            id: shuttleFrame

            objectName: "transportShuttleFrame"
            implicitHeight: root.shuttleFrameHeight
            padding: root.shuttleFramePadding
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: root.shuttleFrameHeight

            background: Rectangle {
                color: Theme.panel
                border.color: Theme.line
                border.width: Theme.borderW
                radius: Theme.r1
            }

            contentItem: RowLayout {
                spacing: root.compact ? 1 : (root.tight ? Theme.s1 : 12)

                Button {
                    objectName: "transportRewindButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
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
                    objectName: "transportStepBackButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
                    Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                    leftPadding: root.compact ? Theme.s1 : Theme.s3
                    rightPadding: root.compact ? Theme.s1 : Theme.s3
                    font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                    text: "<"
                    enabled: root.hasUi
                    onClicked: root.ui.stepFrameBack()
                }

                Button {
                    objectName: "transportStepForwardButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
                    Layout.preferredWidth: root.compact ? root.compactShuttleWidth : implicitWidth
                    leftPadding: root.compact ? Theme.s1 : Theme.s3
                    rightPadding: root.compact ? Theme.s1 : Theme.s3
                    font.pixelSize: root.compact ? Theme.fsMicro : Theme.fsBody
                    text: ">"
                    enabled: root.hasUi
                    onClicked: root.ui.stepFrame()
                }

                Button {
                    objectName: "transportQuarterButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
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
                    objectName: "transportHalfButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
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
                    objectName: "transportDoubleButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
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
                    objectName: "transportForwardButton"
                    implicitHeight: root.shuttleKeyHeight
                    Layout.preferredHeight: root.shuttleKeyHeight
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
        }

        Button {
            objectName: "transportLiveButton"
            text: root.compact ? "Live" : "GO LIVE"
            implicitHeight: root.liveKeyHeight
            Layout.preferredHeight: root.liveKeyHeight
            enabled: root.hasUi
            onClicked: {
                root.ui.goLive()
                root.ui.transport.setSpeed(1.0)
                root.ui.transport.setPlaying(true)
            }
        }

        Button {
            objectName: "transportCaptureButton"
            implicitHeight: root.actionKeyHeight
            Layout.preferredHeight: root.actionKeyHeight
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
