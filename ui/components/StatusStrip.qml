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
    readonly property int tallyCount: root.hasUi ? Math.max(0, Math.min(16, root.ui.multiviewCount)) : 4
    readonly property int sourceConnectionVersion: root.hasUi ? root.ui.sourceConnectionVersion : 0
    readonly property int sourceStatsVersion: root.hasUi ? root.ui.sourceStatsVersion : 0

    signal toggleConfig()
    signal fullscreenMultiviewRequested(real x, real y)

    Layout.fillWidth: true
    implicitHeight: Theme.hControl
    height: Theme.hControl
    spacing: Theme.s3

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
        Layout.minimumWidth: 108
        text: root.hasUi && root.ui.isRecording ? "STOP" : "START"
        enabled: root.hasUi
        highlighted: root.hasUi && root.ui.isRecording
        onClicked: root.ui.isRecording ? root.ui.stopRecording() : root.ui.startRecording()

        ToolTip.visible: hovered
        ToolTip.text: root.hasUi && root.ui.isRecording ? "Stop recording" : "Start recording"
    }

    Text {
        Layout.alignment: Qt.AlignVCenter
        Layout.minimumWidth: 118
        text: "REC " + root.formatTimecode(root.hasUi ? root.ui.recordedDurationMs : 0)
        color: root.hasUi && root.ui.isRecording ? Theme.recordOnAir : Theme.textDim
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsTcInline
        elide: Text.ElideRight
    }

    Text {
        id: masterClock
        Layout.alignment: Qt.AlignVCenter
        Layout.minimumWidth: 110
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
        spacing: Theme.s1

        Repeater {
            model: root.tallyCount

            delegate: Rectangle {
                id: tallyChip
                required property int index

                Layout.preferredWidth: 22
                Layout.preferredHeight: Theme.hCompact
                radius: Theme.r1
                color: root.tallyColor(tallyChip.index)
                border.color: root.hasUi && root.ui.isRecording ? Theme.lineStrong : Theme.line
                border.width: Theme.borderW

                Text {
                    anchors.centerIn: parent
                    text: tallyChip.index + 1
                    color: tallyChip.color === Theme.armed
                           || tallyChip.color === Theme.error
                           || tallyChip.color === Theme.ready
                           ? Theme.textOnTally
                           : Theme.textHi
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fsMicro
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
        text: "Fullscreen Multiview"
        onClicked: root.fullscreenMultiviewRequested(fullscreenButton.x,
                                                     fullscreenButton.y + fullscreenButton.height + Theme.s1)

        ToolTip.visible: hovered
        ToolTip.text: "Open fullscreen multiview"
    }

    Button {
        Layout.preferredHeight: Theme.hControl
        text: "Config"
        highlighted: root.configOpen
        onClicked: root.toggleConfig()

        ToolTip.visible: hovered
        ToolTip.text: root.configOpen ? "Hide configuration" : "Show configuration"
    }
}
