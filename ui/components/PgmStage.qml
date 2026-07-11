pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import OlrTheme

Item {
    id: root
    property var ui
    property int selectedIndex: -1
    property int selectedSourceIndex: -1
    property var visibleStreamIndexes: []
    readonly property int streamCount: visibleStreamIndexes.length
    readonly property QtObject pgmProvider: ui ? ui.pgmPreviewProvider : null
    readonly property QtObject multiviewProvider: ui ? ui.multiviewPreviewProvider : null
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

    function viewForSource(sourceIndex) {
        if (!root.hasUi) return -1
        var map = root.ui.viewSlotMap || []
        for (var i = 0; i < map.length; ++i) {
            if (map[i] === sourceIndex) return i
        }
        return -1
    }

    function selectSource(sourceIndex) {
        if (!root.hasUi || sourceIndex < 0) return
        var viewIndex = root.viewForSource(sourceIndex)
        if (viewIndex < 0) return
        root.selectedSourceIndex = sourceIndex
        root.selectedIndex = viewIndex
        root.viewMode = "single"
        root.ui.setPlaybackViewState(true, viewIndex)
    }

    function selectViewSlot(viewIndex) {
        var sourceIndex = root.sourceForView(viewIndex)
        if (sourceIndex < 0) return
        root.selectedIndex = viewIndex
        root.selectSource(sourceIndex)
    }

    function resetToMulti() {
        root.selectedIndex = -1
        root.selectedSourceIndex = -1
        root.viewMode = "multi"
        root.updateVisibleStreams()
        if (root.hasUi) {
            root.ui.setPlaybackViewState(false, -1)
        }
    }

    function applyPlaybackViewState() {
        if (!root.hasUi) return
        if (root.ui.playbackSingleView) {
            var viewIndex = root.ui.playbackSelectedIndex
            var sourceIndex = root.sourceForView(viewIndex)
            if (viewIndex >= 0 && sourceIndex >= 0) {
                root.selectedIndex = viewIndex
                root.selectedSourceIndex = sourceIndex
                root.viewMode = "single"
                return
            }
        }
        root.selectedIndex = -1
        root.selectedSourceIndex = -1
        root.viewMode = "multi"
        root.updateVisibleStreams()
    }

    function rebindSelectedSource() {
        if (!root.hasUi || root.viewMode !== "single" || root.selectedSourceIndex < 0) return
        var viewIndex = root.viewForSource(root.selectedSourceIndex)
        if (viewIndex < 0) {
            root.resetToMulti()
            return
        }
        root.selectedIndex = viewIndex
        root.ui.setPlaybackViewState(true, viewIndex)
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
        singleOutput.provider = root.pgmProvider
        multiviewBusOutput.provider = root.multiviewProvider
    }

    Component.onCompleted: {
        root.selectedIndex = -1
        root.viewMode = "multi"
        root.updateVisibleStreams()
        root.reattachProviders()
        root.applyPlaybackViewState()
    }

    Connections {
        target: root.hasUi ? root.ui : null
        ignoreUnknownSignals: !root.hasUi
        function onPlaybackProvidersChanged() {
            root.selectedIndex = -1
            root.selectedSourceIndex = -1
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
        function onViewSlotMapChanged() {
            if (root.viewMode === "single")
                root.rebindSelectedSource()
            else
                root.updateVisibleStreams()
        }
        function onFeedSelectRequested(index) {
            root.selectSource(index)
        }
        function onPlaybackViewStateChanged() {
            root.applyPlaybackViewState()
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
        property int sourceForView: root.selectedSourceIndex
        color: sourceForView < 0 ? Theme.panelPressed : "black"
        border.color: sourceForView < 0 ? Theme.line : Theme.recordOnAir
        border.width: 2
        visible: root.viewMode === "single" && root.selectedSourceIndex >= 0 && root.pgmProvider !== null

        PreviewSurface {
            id: singleOutput
            anchors.fill: parent
            provider: root.pgmProvider
            active: singleView.visible
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
                    if (src < 0) return ""
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

    PreviewSurface {
        id: multiviewBusOutput
        anchors.fill: parent
        visible: root.viewMode === "multi" && root.multiviewProvider !== null
        z: 0
        provider: root.multiviewProvider
        active: multiviewBusOutput.visible
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
                    root.selectViewSlot(multiViewDelegate.streamIndex)
                }
            }
        }
    }
}
