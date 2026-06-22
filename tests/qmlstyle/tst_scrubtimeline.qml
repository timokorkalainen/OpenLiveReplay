import QtQuick
import QtTest
import QtQml
import "../../ui/components"

TestCase {
    id: tc
    name: "ScrubTimeline"
    when: windowShown
    width: 600
    height: 80

    ListModel {
        id: scrubPlaylistModel

        ListElement {
            inMs: 10000
            outMs: 20000
        }
    }

    QtObject {
        id: mockUi

        property int recordedDurationMs: 61000
        property int liveBufferMs: 1000
        property int scrubPosition: 15000
        property var playlistModel: scrubPlaylistModel
        property int lastSeekMs: -1
        property int endCount: 0

        function seekPlayback(ms) { lastSeekMs = ms }
        function endScrubGesture() { endCount += 1 }
        function recordTimecode(ms) { return String(ms) }
    }

    ScrubTimeline {
        id: timeline
        width: tc.width
        height: 40
        anchors.verticalCenter: parent.verticalCenter
        ui: mockUi
    }

    function init() {
        mockUi.lastSeekMs = -1
        mockUi.endCount = 0
        timeline.dragging = false
        timeline.dragMs = 0
    }

    function test_mapsXToDurationMinusLiveBuffer() {
        compare(Math.round(timeline.xToMs(timeline.width / 2)), 30000)
        compare(Math.round(timeline.xToMs(timeline.width)), 60000)
    }

    function test_pressDragReleaseSeeksAndEndsGesture() {
        timeline.beginScrubAtX(timeline.width / 4)
        compare(mockUi.lastSeekMs, 15000)
        verify(timeline.dragging)

        timeline.updateScrubAtX(timeline.width / 2)
        compare(mockUi.lastSeekMs, 30000)

        timeline.endScrubAtX(timeline.width / 2)
        compare(mockUi.lastSeekMs, 30000)
        compare(mockUi.endCount, 1)
        verify(!timeline.dragging)
    }
}
