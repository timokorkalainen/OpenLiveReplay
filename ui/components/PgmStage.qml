pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import OlrTheme

Item {
    id: root
    property var ui
    property int selectedIndex: -1
    property var visibleStreamIndexes: []
    readonly property int streamCount: visibleStreamIndexes.length
    property var pgmProvider: ui ? ui.pgmPreviewProvider : null
    property var multiviewProvider: ui ? ui.multiviewPreviewProvider : null
    property string viewMode: "multi"
    readonly property int gridColumns: Math.max(1, Math.ceil(Math.sqrt(Math.max(1, streamCount))))
    readonly property int gridRows: Math.ceil(Math.max(1, streamCount) / gridColumns)
    readonly property bool hasUi: ui !== null && ui !== undefined

    Layout.fillWidth: true
    Layout.fillHeight: true

    function sourceForView(index) {
        if (!root.hasUi) return -1
        var map = root.ui.viewSlotMap || []
        return (index >= 0 && index < map.length) ? map[index] : -1
    }

    function sourceLabel(sourceIndex, viewIndex) {
        if (sourceIndex < 0 || !root.hasUi) return "VIEW " + (viewIndex + 1)
        return root.ui.sourceDisplayLabel(sourceIndex)
    }

    function resetToMulti() {
        root.selectedIndex = -1
        root.viewMode = "multi"
        root.updateVisibleStreams()
        if (root.hasUi) {
            root.ui.setPlaybackViewState(false, -1)
        }
    }

    function updateVisibleStreams() {
        var indexes = []
        var viewCount = root.hasUi ? Math.max(1, Math.min(16, root.ui.multiviewCount)) : 4
        for (var i = 0; i < viewCount; ++i) {
            indexes.push(i)
        }
        root.visibleStreamIndexes = indexes
    }

    function reattachProviders() {
        singleOutput.attachProvider(root.pgmProvider)
        multiviewBusOutput.attachProvider(root.multiviewProvider)
    }

    Component.onCompleted: {
        root.selectedIndex = -1
        root.viewMode = "multi"
        root.updateVisibleStreams()
        root.reattachProviders()
    }

    Connections {
        target: root.hasUi ? root.ui : null
        ignoreUnknownSignals: !root.hasUi
        function onPlaybackProvidersChanged() {
            root.selectedIndex = -1
            root.viewMode = "multi"
            root.updateVisibleStreams()
            root.ui.setPlaybackViewState(false, -1)
            root.reattachProviders()
        }
        function onStreamUrlsChanged() {
            root.resetToMulti()
        }
        function onMultiviewCountChanged() {
            root.resetToMulti()
        }
        function onFeedSelectRequested(index) {
            root.selectedIndex = index
            root.viewMode = "single"
            root.ui.setPlaybackViewState(true, index)
        }
        function onMultiviewRequested() {
            root.resetToMulti()
        }
    }

    onVisibleChanged: {
        if (visible && root.hasUi) {
            root.resetToMulti()
        }
    }

    Rectangle {
        id: singleView
        anchors.fill: parent
        property int sourceForView: root.sourceForView(root.selectedIndex)
        color: sourceForView < 0 ? Theme.panelPressed : "black"
        border.color: sourceForView < 0 ? Theme.line : Theme.ready
        border.width: 2
        visible: root.viewMode === "single" && root.selectedIndex >= 0 && root.pgmProvider !== null

        VideoOutput {
            id: singleOutput
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            property var attachedProvider: null

            function attachProvider(provider) {
                if (attachedProvider === provider) return
                if (attachedProvider) {
                    attachedProvider.removeVideoSink(videoSink)
                }
                attachedProvider = provider
                if (attachedProvider) {
                    attachedProvider.addVideoSink(videoSink)
                }
            }

            Component.onCompleted: attachProvider(root.pgmProvider)
            Component.onDestruction: attachProvider(null)
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 6
            color: Theme.scrim
            width: singleViewLabel.implicitWidth + 12
            height: singleViewLabel.implicitHeight + 6

            Text {
                id: singleViewLabel
                anchors.centerIn: parent
                text: {
                    var src = singleView.sourceForView
                    if (root.selectedIndex < 0) return ""
                    return root.sourceLabel(src, root.selectedIndex)
                }
                color: Theme.textHi
                font.family: Theme.fontMono
                font.pixelSize: 14
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.resetToMulti()
        }
    }

    VideoOutput {
        id: multiviewBusOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
        visible: root.viewMode === "multi" && root.multiviewProvider !== null
        z: 0
        property var attachedProvider: null

        function attachProvider(provider) {
            if (attachedProvider === provider) return
            if (attachedProvider) {
                attachedProvider.removeVideoSink(videoSink)
            }
            attachedProvider = provider
            if (attachedProvider) {
                attachedProvider.addVideoSink(videoSink)
            }
        }

        Component.onCompleted: attachProvider(root.multiviewProvider)
        Component.onDestruction: attachProvider(null)
    }

    GridView {
        id: multiViewGrid
        anchors.fill: parent
        anchors.margins: 0
        visible: root.viewMode === "multi"
        z: 1
        clip: true
        interactive: false
        cellHeight: parent.height / root.gridRows
        cellWidth: parent.width / root.gridColumns

        model: root.visibleStreamIndexes

        delegate: Rectangle {
            id: multiViewDelegate
            required property var modelData
            property int streamIndex: modelData
            property int sourceForView: root.sourceForView(multiViewDelegate.streamIndex)
            color: "transparent"
            border.color: sourceForView < 0 ? Theme.line : Theme.ready
            border.width: 2
            width: multiViewGrid.cellWidth
            height: multiViewGrid.cellHeight

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 5
                color: Theme.scrim
                width: multiViewLabel.implicitWidth + 10
                height: multiViewLabel.implicitHeight + 4
                z: 5

                Text {
                    id: multiViewLabel
                    anchors.centerIn: parent
                    text: root.sourceLabel(multiViewDelegate.sourceForView, multiViewDelegate.streamIndex)
                    color: Theme.textHi
                    font.family: Theme.fontMono
                    font.pixelSize: 12
                }
            }

            MouseArea {
                anchors.fill: parent
                z: 2
                onClicked: {
                    root.selectedIndex = multiViewDelegate.streamIndex
                    root.viewMode = "single"
                    if (root.hasUi) {
                        root.ui.setPlaybackViewState(true, multiViewDelegate.streamIndex)
                    }
                }
            }
        }
    }
}
