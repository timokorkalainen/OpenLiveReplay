import QtQuick
import QtQuick.Layouts
import QtTest
import "../../ui/components"
import OlrTheme

TestCase {
    id: tc
    name: "TransportDockLayout"
    when: windowShown
    width: 1200
    height: 280

    QtObject {
        id: mockTransport

        property bool isPlaying: false

        function setSpeed(speed) {}
        function setPlaying(playing) { isPlaying = playing }
    }

    ListModel {
        id: emptyPlaylistModel
    }

    QtObject {
        id: mockUi

        property var transport: mockTransport
        property string playbackTimecode: "00:00:00"
        property bool timeOfDayMode: false
        property int recordedDurationMs: 60000
        property int liveBufferMs: 1000
        property int scrubPosition: 12000
        property var playlistModel: emptyPlaylistModel

        function recordTimecode(ms) { return String(ms) }
        function playPause() { mockTransport.isPlaying = !mockTransport.isPlaying }
        function cancelFollowLive() {}
        function stepFrameBack() {}
        function stepFrame() {}
        function goLive() {}
        function captureCurrent() {}
        function seekPlayback(ms) {}
        function endScrubGesture() {}
    }

    Item {
        id: wideHarness
        width: 1120
        height: 120

        TransportDock {
            id: wideDock
            anchors.fill: parent
            ui: mockUi
        }
    }

    Item {
        id: compactHarness
        y: 140
        width: 520
        height: 120

        TransportDock {
            id: compactDock
            anchors.fill: parent
            ui: mockUi
        }
    }

    function test_desktopTransportKeyHierarchy() {
        wait(0)
        var play = findChild(wideDock, "transportPlayButton")
        var live = findChild(wideDock, "transportLiveButton")
        var capture = findChild(wideDock, "transportCaptureButton")
        var rewind = findChild(wideDock, "transportRewindButton")
        var shuttleFrame = findChild(wideDock, "transportShuttleFrame")
        verify(play !== null)
        verify(live !== null)
        verify(capture !== null)
        verify(rewind !== null)
        verify(shuttleFrame !== null)

        verify(!wideDock.compact)
        compare(play.implicitHeight, Theme.hPrimary)
        compare(live.implicitHeight, Theme.hTransport)
        compare(capture.implicitHeight, Theme.hAction)
        compare(rewind.implicitHeight, Theme.hControl)
        compare(live.text, "GO LIVE")
        verify(play.height >= live.height)
        verify(live.height >= capture.height)
        verify(capture.height >= rewind.height)
        verify(shuttleFrame.height < live.height)
    }

    function test_compactTransportKeepsCondensedLabels() {
        wait(0)
        var play = findChild(compactDock, "transportPlayButton")
        var live = findChild(compactDock, "transportLiveButton")
        var capture = findChild(compactDock, "transportCaptureButton")
        var rewind = findChild(compactDock, "transportRewindButton")
        var quarter = findChild(compactDock, "transportQuarterButton")
        verify(play !== null)
        verify(live !== null)
        verify(capture !== null)
        verify(rewind !== null)
        verify(quarter !== null)

        verify(compactDock.compact)
        compare(play.implicitHeight, Theme.hControl)
        compare(live.implicitHeight, Theme.hControl)
        compare(capture.implicitHeight, Theme.hControl)
        compare(rewind.implicitHeight, Theme.hControl)
        compare(live.text, "Live")
        compare(capture.text, "Cap")
        compare(rewind.text, "-5")
        compare(quarter.text, "¼")
        compare(Math.round(rewind.width), compactDock.compactShuttleWidth)
    }
}
