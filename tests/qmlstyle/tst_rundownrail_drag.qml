import QtQuick
import QtTest
import QtQml
import "../../ui/components"

TestCase {
    id: tc
    name: "RundownRailDrag"
    when: windowShown
    width: 420
    height: 640

    ListModel {
        id: rundownModel
        ListElement {
            index: 0
            label: "a.mkv"
            inMs: 0
            outMs: 1000
            durationMs: 1000
            speed: 1.0
            hasOut: true
            boundaryReady: true
        }
        ListElement {
            index: 1
            label: "b.mkv"
            inMs: 2000
            outMs: 3000
            durationMs: 1000
            speed: 1.0
            hasOut: true
            boundaryReady: true
        }
        ListElement {
            index: 2
            label: "c.mkv"
            inMs: 4000
            outMs: 5000
            durationMs: 1000
            speed: 1.0
            hasOut: true
            boundaryReady: true
        }
    }

    QtObject {
        id: mockUi

        property var playlistModel: rundownModel
        property int playlistCount: rundownModel.count
        property int currentPlaylistEntryIndex: -1
        property int nextPlaylistEntryIndex: -1
        property bool playlistPlayoutActive: false
        property bool playlistDirty: false
        property string playlistOperationError: ""
        property int moveCount: 0
        property int lastFrom: -1
        property int lastTo: -1

        function recordTimecode(ms) { return String(ms) }
        function markIn() {}
        function markOut() {}
        function playPlaylist(index) {}
        function stopPlaylistPlayout() {}
        function clearPlaylist() {}
        function insertPlaylistEntryAt(index) {}
        function recallEntry(index) {}
        function removePlaylistEntry(index) {}
        function setPlaylistEntrySpeed(index, speed) {}
        function setPlaylistEntryInFromPlayhead(index) {}
        function setPlaylistEntryOutFromPlayhead(index) {}
        function movePlaylistEntry(fromIndex, toIndex) {
            moveCount += 1
            lastFrom = fromIndex
            lastTo = toIndex
        }
    }

    RundownRail {
        id: rail
        anchors.fill: parent
        ui: mockUi
        expanded: true
    }

    function init() {
        mockUi.moveCount = 0
        mockUi.lastFrom = -1
        mockUi.lastTo = -1
    }

    function test_dragHandleReordersOnReleaseOverTargetRow() {
        wait(100)
        var handle = findChild(rail, "rundownDragHandle0")
        verify(handle !== null)
        verify(handle.width > 0)
        verify(handle.height > 0)
        compare(rail.rowIndexAt(handle, handle.width / 2, handle.height / 2), 0)
        compare(rail.rowIndexAt(handle, handle.width / 2,
                                handle.height / 2 + rail.rowHeight + 24), 1)

        rail.beginDrag(0)
        compare(rail.dragIndex, 0)

        rail.updateDragTarget(handle, handle.width / 2, handle.height / 2 + rail.rowHeight + 24)
        compare(rail.dragTargetIndex, 1)

        rail.finishDrag(rail.dragTargetIndex)

        tryCompare(mockUi, "moveCount", 1)
        compare(mockUi.lastFrom, 0)
        compare(mockUi.lastTo, 1)
    }
}
