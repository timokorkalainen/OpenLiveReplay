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

    ListModel {
        id: malformedRegionModel

        ListElement {
            inMs: "bad"
            outMs: 20000
        }
    }

    ListModel {
        id: reversedRegionModel

        ListElement {
            inMs: 20000
            outMs: 10000
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

    QtObject {
        id: uiWithoutPlaylistModel

        property int recordedDurationMs: 61000
        property int liveBufferMs: 1000
        property int scrubPosition: 15000

        function seekPlayback(ms) {}
        function endScrubGesture() {}
        function recordTimecode(ms) { return String(ms) }
    }

    QtObject {
        id: malformedUi

        property int recordedDurationMs: 61000
        property int liveBufferMs: 1000
        property int scrubPosition: 15000
        property var playlistModel: malformedRegionModel

        function seekPlayback(ms) {}
        function endScrubGesture() {}
        function recordTimecode(ms) { return String(ms) }
    }

    QtObject {
        id: reversedUi

        property int recordedDurationMs: 61000
        property int liveBufferMs: 1000
        property int scrubPosition: 15000
        property var playlistModel: reversedRegionModel

        function seekPlayback(ms) {}
        function endScrubGesture() {}
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
        scrubPlaylistModel.clear()
        scrubPlaylistModel.append({ "inMs": 10000, "outMs": 20000 })
        mockUi.recordedDurationMs = 61000
        mockUi.liveBufferMs = 1000
        mockUi.scrubPosition = 15000
        mockUi.lastSeekMs = -1
        mockUi.endCount = 0
        timeline.ui = mockUi
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

    function test_mouseAreaCoversTheFullControlHeight() {
        var mouseLayer = findChild(timeline, "scrubMouse")
        verify(mouseLayer !== null)
        compare(mouseLayer.width, timeline.width)
        compare(mouseLayer.height, timeline.height)
        tryCompare(mouseLayer, "enabled", true)
    }

    function test_openRegionAtRightEdgeStaysVisibleInsideTrack() {
        scrubPlaylistModel.clear()
        scrubPlaylistModel.append({ "inMs": 60000, "outMs": -1 })
        wait(0)

        var track = findChild(timeline, "track")
        var region = findChild(timeline, "openRegion")
        verify(track !== null)
        verify(region !== null)
        verify(region.durationAvailable)
        compare(region.width, 2)
        verify(region.x >= 0)
        verify(region.x + region.width <= track.width)
    }

    function test_zeroDurationHidesLiveEdgeAndRegions() {
        mockUi.recordedDurationMs = 1000
        mockUi.liveBufferMs = 1000
        scrubPlaylistModel.clear()
        scrubPlaylistModel.append({ "inMs": 0, "outMs": -1 })
        wait(0)

        var liveEdge = findChild(timeline, "liveEdge")
        var region = findChild(timeline, "openRegion")
        verify(liveEdge !== null)
        verify(region !== null)
        compare(timeline.durMax, 0)
        verify(!liveEdge.durationAvailable)
        verify(!region.durationAvailable)
        compare(timeline.xToMs(timeline.width), 0)
    }

    function test_zeroDurationMouseInputIsInert() {
        mockUi.recordedDurationMs = 1000
        mockUi.liveBufferMs = 1000
        wait(0)

        var mouseLayer = findChild(timeline, "scrubMouse")
        verify(mouseLayer !== null)
        tryCompare(mouseLayer, "enabled", false)

        mousePress(mouseLayer, timeline.width / 2, timeline.height / 2, Qt.LeftButton)
        mouseRelease(mouseLayer, timeline.width / 2, timeline.height / 2, Qt.LeftButton)
        compare(mockUi.lastSeekMs, -1)
        compare(mockUi.endCount, 0)
        verify(!timeline.dragging)
    }

    function test_nonFiniteValuesClampToZero() {
        verify(isNaN(Number.NaN))
        compare(timeline.clampMs(Number.NaN), 0)
        compare(timeline.clampMs(Number.POSITIVE_INFINITY), 0)

        mockUi.scrubPosition = Number.NaN
        timeline.beginScrubAtX(Number.NaN)
        compare(mockUi.lastSeekMs, 0)
    }

    function test_malformedRegionsAreHidden() {
        timeline.ui = malformedUi
        wait(0)

        var badRegion = findChild(timeline, "closedRegion")
        verify(badRegion !== null)
        verify(!badRegion.rangeValid)
        compare(badRegion.width, 0)
        compare(badRegion.opacity, 0)

        timeline.ui = reversedUi
        wait(0)

        var reversedRegion = findChild(timeline, "closedRegion")
        verify(reversedRegion !== null)
        verify(!reversedRegion.rangeValid)
        compare(reversedRegion.width, 0)
        compare(reversedRegion.opacity, 0)
    }

    function test_closedRegionAtRightEdgeStaysVisibleInsideTrack() {
        scrubPlaylistModel.clear()
        scrubPlaylistModel.append({ "inMs": 60000, "outMs": 60000 })
        wait(0)

        var track = findChild(timeline, "track")
        var region = findChild(timeline, "closedRegion")
        verify(track !== null)
        verify(region !== null)
        verify(region.rangeValid)
        compare(region.width, 2)
        compare(region.height, track.height)
        verify(region.opacity > 0)
        verify(region.x >= 0)
        verify(region.x + region.width <= track.width)
    }

    function test_nullUiAndMissingPlaylistModelAreSafe() {
        timeline.ui = null
        timeline.beginScrubAtX(timeline.width)
        verify(!timeline.dragging)
        timeline.finishScrub()
        compare(mockUi.endCount, 0)

        timeline.ui = uiWithoutPlaylistModel
        wait(0)
        compare(Math.round(timeline.xToMs(timeline.width)), 60000)
        compare(findChild(timeline, "closedRegion"), null)
    }
}
