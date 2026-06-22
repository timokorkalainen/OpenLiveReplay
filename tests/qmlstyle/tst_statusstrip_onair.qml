import QtQuick
import QtTest
import "../../ui/components"
import OlrTheme

TestCase {
    id: tc
    name: "StatusStripOnAir"
    when: windowShown
    width: 640
    height: 96

    QtObject {
        id: mockUi

        property bool isRecording: true
        property bool playbackSingleView: true
        property int playbackSelectedIndex: 1
        property int multiviewCount: 4
        property int playbackViewStateVersion: 0
        property int sourceConnectionVersion: 0
        property int sourceStatsVersion: 0
        property int recordedDurationMs: 0
        property string playbackTimecode: "00:00:00"
        property bool timeOfDayMode: false
        property var connectedStates: [true, false, true, true]
        property var healthStates: [1, 3, 3, 2]

        function isSourceConnected(index) { return connectedStates[index] === true }
        function sourceLinkHealth(index) { return healthStates[index] || 0 }
        function recordTimecode(ms) { return "00:00:00" }
        function startRecording() {}
        function stopRecording() {}
    }

    StatusStrip {
        id: strip
        width: tc.width
        ui: mockUi
    }

    function init() {
        mockUi.isRecording = true
        mockUi.playbackSingleView = true
        mockUi.playbackSelectedIndex = 1
        mockUi.connectedStates = [true, false, true, true]
        mockUi.healthStates = [1, 3, 3, 2]
        mockUi.playbackViewStateVersion += 1
        mockUi.sourceConnectionVersion += 1
        mockUi.sourceStatsVersion += 1
    }

    function test_selectedSingleViewSourceIsOnAirBeforeHealth() {
        compare(strip.tallyColor(1), Theme.recordOnAir)
    }

    function test_otherSourcesFollowConnectionAndHealth() {
        compare(strip.tallyColor(0), Theme.ready)
        compare(strip.tallyColor(2), Theme.error)
        compare(strip.tallyColor(3), Theme.armed)
    }

    function test_multiviewDoesNotShowSingleOnAirTally() {
        mockUi.playbackSingleView = false
        mockUi.connectedStates = [true, true, true, true]
        mockUi.healthStates = [1, 1, 1, 1]
        mockUi.playbackViewStateVersion += 1
        compare(strip.tallyColor(1), Theme.ready)
    }

    function test_notRecordingIsIdle() {
        mockUi.isRecording = false
        compare(strip.tallyColor(1), Theme.idle)
    }
}
