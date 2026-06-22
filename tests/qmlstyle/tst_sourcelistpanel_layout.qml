import QtQuick
import QtTest
import "../../ui/components"

TestCase {
    id: tc
    name: "SourceListPanelLayout"
    when: windowShown
    width: 760
    height: 260

    QtObject {
        id: mockUi

        property bool importPreviewReady: false
        property var importPreview: ({})
        property bool isRecording: false
        property var streamUrls: ["srt://10.10.10.10:9000?streamid=very-long-camera-source-a"]
        property var streamIds: ["cam-a"]
        property var streamNames: ["Camera A"]
        property int sourceEnabledVersion: 0
        property int sourceConnectionVersion: 0
        property int sourceStatsVersion: 0
        property int sourceTrimVersion: 0

        function addStream() {}
        function removeStream(index) {}
        function toggleSourceEnabled(index) {}
        function isSourceEnabled(index) { return true }
        function isSourceConnected(index) { return true }
        function sourceLinkHealth(index) { return 1 }
        function sourceHasStats(index) { return false }
        function sourceStatsTooltip(index) { return "" }
        function sourceTrimOffset(index) { return 0 }
        function setSourceTrimOffset(index, value) {}
        function updateStreamId(index, text) {}
        function updateStreamName(index, text) {}
        function updateUrl(index, text) {}
        function hasDuplicateUrl(index) { return false }
    }

    SourceListPanel {
        id: panel
        width: 720
        height: 220
        ui: mockUi
    }

    function findControlWithPlaceholder(item, placeholder) {
        if (!item) return null
        try {
            if (item.placeholderText === placeholder) return item
        } catch (e) {
        }

        var kids = item.children || []
        for (var i = 0; i < kids.length; ++i) {
            var found = findControlWithPlaceholder(kids[i], placeholder)
            if (found) return found
        }
        return null
    }

    function test_sourceUrlFieldKeepsPrimaryHorizontalSpace() {
        wait(0)
        var urlField = findControlWithPlaceholder(panel, "rtmp://...")
        verify(urlField !== null)
        verify(urlField.width >= 360, "URL field width was " + urlField.width)
    }
}
