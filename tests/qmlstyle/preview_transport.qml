import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 1120
    height: 280
    color: Theme.canvas

    QtObject {
        id: mockTransport

        property bool isPlaying: false

        function setSpeed(speed) {}
        function setPlaying(playing) { isPlaying = playing }
    }

    ListModel {
        id: transportPlaylistModel

        ListElement {
            inMs: 5000
            outMs: 12000
        }
        ListElement {
            inMs: 24000
            outMs: -1
        }
    }

    QtObject {
        id: mockUi

        property var transport: mockTransport
        property string playbackTimecode: "00:00:18"
        property bool timeOfDayMode: false
        property int recordedDurationMs: 60000
        property int liveBufferMs: 1000
        property int scrubPosition: 18000
        property var playlistModel: transportPlaylistModel

        function recordTimecode(ms) {
            var totalSeconds = Math.max(0, Math.floor(ms / 1000))
            var mm = Math.floor(totalSeconds / 60)
            var ss = totalSeconds % 60
            return (mm < 10 ? "0" + mm : "" + mm)
                + ":" + (ss < 10 ? "0" + ss : "" + ss)
        }

        function playPause() { mockTransport.isPlaying = !mockTransport.isPlaying }
        function cancelFollowLive() {}
        function stepFrameBack() {}
        function stepFrame() {}
        function goLive() {}
        function captureCurrent() {}
        function seekPlayback(ms) {}
        function endScrubGesture() {}
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s3
        spacing: Theme.s3

        Label {
            text: "Wide"
            color: Theme.textDim
        }

        TransportDock {
            Layout.fillWidth: true
            ui: mockUi
        }

        Label {
            text: "Compact"
            color: Theme.textDim
        }

        Item {
            Layout.preferredWidth: Theme.bpSM
            Layout.preferredHeight: compactDock.implicitHeight

            TransportDock {
                id: compactDock

                anchors.fill: parent
                ui: mockUi
            }
        }
    }
}
