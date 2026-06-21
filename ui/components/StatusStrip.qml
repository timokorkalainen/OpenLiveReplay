pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

RowLayout {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    property bool configOpen: false
    property string recordingError: ""
    readonly property int avail: root.parent ? root.parent.width : root.width
    readonly property int tallyCount: root.hasUi ? Math.max(0, Math.min(16, root.ui.multiviewCount)) : 4
    readonly property int sourceConnectionVersion: root.hasUi ? root.ui.sourceConnectionVersion : 0
    readonly property int sourceStatsVersion: root.hasUi ? root.ui.sourceStatsVersion : 0
    readonly property bool tight: root.avail <= Theme.bpMD
    readonly property bool compact: root.avail < Theme.bpMD
    readonly property bool tiny: root.avail <= Theme.bpSM
    readonly property bool tallyDots: root.compact
                                      || root.tallyCount > 8
                                      || (root.avail < Theme.bpLG && root.tallyCount > 4)
    readonly property int tallyDotSize: Math.max(6, Theme.dotSize - 4)

    signal toggleConfig()
    signal fullscreenMultiviewRequested(real x, real y)

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    implicitHeight: Theme.hControl
    height: Theme.hControl
    spacing: root.compact || root.tight ? Theme.s1 : Theme.s3

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

    function tallyColor(index) {
        if (!root.hasUi || !root.ui.isRecording) return Theme.idle

        var connected = root.sourceConnectionVersion >= 0 && root.ui.isSourceConnected(index)
        if (!connected) return Theme.error

        var linkHealth = root.sourceStatsVersion >= 0 ? root.ui.sourceLinkHealth(index) : 0
        if (linkHealth === 3) return Theme.error
        if (linkHealth === 2) return Theme.armed
        return Theme.ready
    }

    Button {
        id: recordButton
        Layout.preferredHeight: Theme.hControl
        Layout.minimumWidth: root.tiny ? 64 : (root.compact ? 76 : 108)
        text: root.tiny && !(root.hasUi && root.ui.isRecording)
              ? "REC"
              : (root.hasUi && root.ui.isRecording ? "STOP" : "START")
        enabled: root.hasUi
        highlighted: root.hasUi && root.ui.isRecording
        onClicked: root.ui.isRecording ? root.ui.stopRecording() : root.ui.startRecording()

        ToolTip.visible: hovered
        ToolTip.text: root.hasUi && root.ui.isRecording ? "Stop recording" : "Start recording"
    }

    Text {
        Layout.alignment: Qt.AlignVCenter
        Layout.minimumWidth: root.tiny ? 76 : (root.compact ? 104 : 118)
        text: (root.tiny ? "" : "REC ") + root.formatTimecode(root.hasUi ? root.ui.recordedDurationMs : 0)
        color: root.hasUi && root.ui.isRecording ? Theme.recordOnAir : Theme.textDim
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsTcInline
        elide: Text.ElideRight
    }

    Text {
        id: masterClock
        Layout.alignment: Qt.AlignVCenter
        Layout.minimumWidth: root.tiny ? 70 : (root.compact ? 76 : 110)
        text: root.hasUi ? root.ui.playbackTimecode : "00:00:00"
        color: Theme.textHi
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsTcInline
        elide: Text.ElideRight

        MouseArea {
            anchors.fill: parent
            enabled: root.hasUi
            onClicked: root.ui.timeOfDayMode = !root.ui.timeOfDayMode
        }

        ToolTip.visible: clockHover.hovered
        ToolTip.text: root.hasUi && root.ui.timeOfDayMode ? "Clock time" : "Master timecode"
        HoverHandler { id: clockHover }
    }

    RowLayout {
        Layout.alignment: Qt.AlignVCenter
        spacing: root.tallyDots ? 1 : Theme.s1

        Repeater {
            model: root.tallyCount

            delegate: Item {
                id: tallyChip
                required property int index

                Layout.preferredWidth: root.tallyDots ? root.tallyDotSize : 44
                Layout.preferredHeight: Theme.hCompact

                Rectangle {
                    id: tallyFill

                    anchors.centerIn: parent
                    width: root.tallyDots ? root.tallyDotSize : parent.width
                    height: root.tallyDots ? root.tallyDotSize : parent.height
                    radius: root.tallyDots ? root.tallyDotSize / 2 : Theme.r1
                    color: root.tallyColor(tallyChip.index)
                    border.color: root.hasUi && root.ui.isRecording ? Theme.lineStrong : Theme.line
                    border.width: Theme.borderW

                    Text {
                        anchors.centerIn: parent
                        visible: !root.tallyDots
                        text: "CAM" + (tallyChip.index + 1)
                        color: tallyFill.color === Theme.armed
                               || tallyFill.color === Theme.error
                               || tallyFill.color === Theme.ready
                               ? Theme.textOnTally
                               : Theme.textHi
                        font.family: Theme.fontMono
                        font.pixelSize: 10
                    }
                }
            }
        }
    }

    Text {
        Layout.alignment: Qt.AlignVCenter
        Layout.fillWidth: true
        visible: root.recordingError !== ""
        text: root.recordingError
        color: Theme.armed
        font.pixelSize: Theme.fsBody
        elide: Text.ElideRight
        maximumLineCount: 1
    }

    Item {
        Layout.fillWidth: root.recordingError === ""
        Layout.preferredWidth: root.recordingError === "" ? 1 : 0
    }

    Button {
        id: fullscreenButton
        Layout.preferredHeight: Theme.hControl
        Layout.preferredWidth: root.tiny ? Theme.hControl : implicitWidth
        Layout.minimumWidth: root.tiny ? Theme.hControl : 64
        leftPadding: root.tiny ? Theme.s1 : Theme.s3
        rightPadding: root.tiny ? Theme.s1 : Theme.s3
        text: root.tiny ? "▾" : "Fullscreen Multiview"
        onClicked: root.fullscreenMultiviewRequested(fullscreenButton.x,
                                                     fullscreenButton.y + fullscreenButton.height + Theme.s1)

        ToolTip.visible: hovered
        ToolTip.text: "Open fullscreen multiview"
    }

    Button {
        Layout.preferredWidth: root.tiny ? 44 : implicitWidth
        Layout.minimumWidth: root.tiny ? 44 : 64
        Layout.preferredHeight: Theme.hControl
        leftPadding: root.tiny ? Theme.s1 : Theme.s3
        rightPadding: root.tiny ? Theme.s1 : Theme.s3
        text: root.tiny ? "Cfg" : "Config"
        highlighted: root.configOpen
        onClicked: root.toggleConfig()

        ToolTip.visible: hovered
        ToolTip.text: root.configOpen ? "Hide configuration" : "Show configuration"
    }
}
