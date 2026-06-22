import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 380
    height: 680
    color: Theme.canvas

    ListModel {
        id: rundownModel
        ListElement {
            index: 0
            label: "goal-cam3.mkv"
            inMs: 5000
            outMs: 12000
            durationMs: 7000
            speed: 0.5
            hasOut: true
            boundaryReady: true
        }
        ListElement {
            index: 1
            label: "save-cam1.mkv"
            inMs: 22000
            outMs: 27500
            durationMs: 5500
            speed: 1.0
            hasOut: true
            boundaryReady: true
        }
        ListElement {
            index: 2
            label: "open-final.mkv"
            inMs: 31000
            outMs: -1
            durationMs: -1
            speed: 0.25
            hasOut: false
            boundaryReady: false
        }
    }

    QtObject {
        id: mockUi

        property var playlistModel: rundownModel
        property int playlistCount: rundownModel.count
        property int currentPlaylistEntryIndex: 0
        property int nextPlaylistEntryIndex: 1
        property bool playlistPlayoutActive: true
        property bool playlistDirty: true
        property string playlistFilePath: "/clips/final-rundown.json"
        property string playlistOperationError: ""

        function recordTimecode(ms) {
            if (ms < 0) return "OPEN"
            var totalSeconds = Math.max(0, Math.floor(ms / 1000))
            var mm = Math.floor(totalSeconds / 60)
            var ss = totalSeconds % 60
            return (mm < 10 ? "0" + mm : "" + mm)
                + ":" + (ss < 10 ? "0" + ss : "" + ss)
        }

        function markIn() {}
        function markOut() {}
        function playPlaylist(index) {}
        function stopPlaylistPlayout() {}
        function clearPlaylist() {}
        function insertPlaylistEntryAt(index) {}
        function recallEntry(index) {}
        function removePlaylistEntry(index) {}
        function movePlaylistEntry(fromIndex, toIndex) {}
        function setPlaylistEntrySpeed(index, speed) {}
        function setPlaylistEntryInFromPlayhead(index) {}
        function setPlaylistEntryOutFromPlayhead(index) {}
    }

    RundownRail {
        anchors.fill: parent
        anchors.margins: Theme.s2
        ui: mockUi
        expanded: true
    }
}
