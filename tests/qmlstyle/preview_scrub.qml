import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 900
    height: 80
    color: Theme.canvas

    ListModel {
        id: mockPlaylistModel

        ListElement {
            inMs: 5000
            outMs: 12000
        }
        ListElement {
            inMs: 20000
            outMs: -1
        }
    }

    QtObject {
        id: mockUi

        property int recordedDurationMs: 60000
        property int liveBufferMs: 1000
        property int scrubPosition: 18000
        property var playlistModel: mockPlaylistModel
        property int lastSeekMs: -1
        property int endCount: 0

        function seekPlayback(ms) { lastSeekMs = ms }
        function endScrubGesture() { endCount += 1 }
        function recordTimecode(ms) {
            var totalSeconds = Math.max(0, Math.floor(ms / 1000))
            var mm = Math.floor(totalSeconds / 60)
            var ss = totalSeconds % 60
            return (mm < 10 ? "0" + mm : "" + mm)
                + ":" + (ss < 10 ? "0" + ss : "" + ss)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s3
        spacing: Theme.s2

        ScrubTimeline {
            Layout.fillWidth: true
            ui: mockUi
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: mockUi.recordTimecode(0)
                color: Theme.textDim
                font.family: Theme.fontMono
            }
            Item { Layout.fillWidth: true }
            Label {
                text: mockUi.recordTimecode(mockUi.recordedDurationMs - mockUi.liveBufferMs)
                color: Theme.textDim
                font.family: Theme.fontMono
            }
        }
    }
}
