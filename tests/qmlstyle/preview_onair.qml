import QtQuick
import QtQml
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 960
    height: 96
    color: Theme.canvas

    QtObject {
        id: mockUi

        property bool isRecording: true
        property bool playbackSingleView: true
        property int playbackSelectedIndex: 1
        property int playbackViewStateVersion: 0
        property int sourceConnectionVersion: 0
        property int sourceStatsVersion: 0
        property int multiviewCount: 4
        property int recordedDurationMs: 74231
        property string playbackTimecode: "12:34:56"
        property bool timeOfDayMode: false

        function recordTimecode(ms) {
            var totalSeconds = Math.max(0, Math.floor(ms / 1000))
            var hh = Math.floor(totalSeconds / 3600)
            var mm = Math.floor((totalSeconds % 3600) / 60)
            var ss = totalSeconds % 60
            return (hh < 10 ? "0" + hh : "" + hh)
                + ":" + (mm < 10 ? "0" + mm : "" + mm)
                + ":" + (ss < 10 ? "0" + ss : "" + ss)
        }

        function isSourceConnected(index) {
            return index >= 0 && index < 4
        }

        function sourceLinkHealth(index) {
            return index >= 0 && index < 4 ? 1 : 3
        }

        function startRecording() {}
        function stopRecording() {}
    }

    StatusStrip {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: Theme.s3
        ui: mockUi
    }
}
