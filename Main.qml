pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform 1.1
import QtQuick.Controls.Universal 2.15 // Necessary for attached properties
import QtMultimedia
import Recorder.Types 1.0

ApplicationWindow {
    visible: true
    width: 800
    height: 600
    title: "OpenLiveReplay"

    Component.onCompleted: {
        uiManager.loadSettings()
        uiManager.openStreams()
        playbackTab.selectedIndex = -1
        playbackTab.singleViewActive = false
    }

    // FORCE THE THEME HERE
    Universal.theme: Universal.Dark
    Universal.accent: Universal.Red

    FolderDialog {
        id: folderDialog
        title: "Select Recording Folder"
        currentFolder: uiManager.saveLocation ? "file://" + uiManager.saveLocation : ""
        onAccepted: {
            uiManager.setSaveLocationFromUrl(folderDialog.folder)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        TabBar {
            id: mainTabs
            Layout.fillWidth: true

            TabButton { text: "Control" }
            TabButton { text: "Playback" }
            TabButton { text: "Project" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: mainTabs.currentIndex

            // --- Control Tab ---
            ColumnLayout {
                spacing: 16
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    text: "Runtime Control"
                    font.bold: true
                    color: "#eee"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Button {
                        id: recordButton
                        text: uiManager.isRecording ? "STOP RECORDING" : "START RECORDING"
                        padding: 18

                        background: Rectangle {
                            color: uiManager.isRecording ? "#d32f2f" : "#2e7d32"
                            radius: 6
                        }

                        contentItem: Text {
                            text: recordButton.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: uiManager.isRecording ? uiManager.stopRecording() : uiManager.startRecording()
                    }

                    Button {
                        id: controlSpacer
                        visible: false
                    }

                    Item { Layout.fillWidth: true }
                }

                Text {
                    text: uiManager.isRecording ? "● RECORDING LIVE" : "IDLE"
                    color: uiManager.isRecording ? "#ff5252" : "#666"
                }

                Item { Layout.fillHeight: true }
            }

            // --- Playback Tab ---
            ColumnLayout {
                id: playbackTab
                spacing: 12
                Layout.fillWidth: true
                Layout.fillHeight: true

                property int selectedIndex: -1
                property var visibleStreamIndexes: []
                property int streamCount: visibleStreamIndexes.length
                property var selectedProvider: (selectedIndex >= 0 && selectedIndex < uiManager.playbackProviders.length)
                                               ? uiManager.playbackProviders[selectedIndex]
                                               : null
                property string viewMode: "multi"
                property int gridColumns: Math.max(1, Math.ceil(Math.sqrt(Math.max(1, streamCount))))
                property int gridRows: Math.ceil(Math.max(1, streamCount) / gridColumns)

                function formatTimecode(ms) {
                    var totalSeconds = Math.floor(ms / 1000)
                    var hours = Math.floor(totalSeconds / 3600)
                    var minutes = Math.floor((totalSeconds % 3600) / 60)
                    var seconds = totalSeconds % 60
                    var frames = Math.floor((ms % 1000) / (1000 / 30))

                    var hh = hours < 10 ? "0" + hours : "" + hours
                    var mm = minutes < 10 ? "0" + minutes : "" + minutes
                    var ss = seconds < 10 ? "0" + seconds : "" + seconds
                    var ff = frames < 10 ? "0" + frames : "" + frames
                    return hh + ":" + mm + ":" + ss + "." + ff
                }

                function updateVisibleStreams() {
                    var indexes = []
                    var total = Math.max(uiManager.streamUrls.length, uiManager.playbackProviders.length)
                    if (uiManager.streamUrls.length > 0) {
                        for (var i = 0; i < uiManager.streamUrls.length; ++i) {
                            var url = uiManager.streamUrls[i]
                            if (url && url.trim().length > 0) {
                                indexes.push(i)
                            }
                        }
                    }
                    if (indexes.length === 0 && total > 0) {
                        for (var j = 0; j < total; ++j) {
                            indexes.push(j)
                        }
                    }
                    visibleStreamIndexes = indexes
                }

                Component.onCompleted: {
                    selectedIndex = -1
                    viewMode = "multi"
                    updateVisibleStreams()
                }

                Connections {
                    target: uiManager
                    function onPlaybackProvidersChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                    }
                    function onStreamUrlsChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                    }
                }

                onVisibleChanged: {
                    if (visible) {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                    }
                }

                Item {
                    id: playbackView
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Rectangle {
                        id: singleView
                        anchors.fill: parent
                        color: "black"
                        border.color: "#00C853"
                        border.width: 2
                        visible: playbackTab.viewMode === "single" && playbackTab.selectedIndex >= 0 && uiManager.playbackProviders.length > 0

                        VideoOutput {
                            id: singleOutput
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectFit
                        }

                        Text {
                            text: playbackTab.selectedIndex >= 0 ? ("CAM " + (playbackTab.selectedIndex + 1)) : ""
                            color: "white"
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.margins: 6
                            font.family: "Menlo"
                            font.pixelSize: 14
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                playbackTab.selectedIndex = -1
                                playbackTab.viewMode = "multi"
                            }
                        }

                        onVisibleChanged: {
                            if (visible && playbackTab.selectedProvider) {
                                playbackTab.selectedProvider.videoSink = singleOutput.videoSink
                            }
                        }
                    }

                    GridView {
                        id: multiViewGrid
                        anchors.fill: parent
                        anchors.margins: 0
                        visible: playbackTab.viewMode === "multi"
                        clip: true
                        interactive: false
                        cellHeight: parent.height / playbackTab.gridRows
                        cellWidth: parent.width / playbackTab.gridColumns

                        model: playbackTab.visibleStreamIndexes

                        delegate: Rectangle {
                            required property var modelData
                            property int streamIndex: modelData
                            color: "black"
                            border.color: "red"
                            border.width: 2
                            width: multiViewGrid.cellWidth
                            height: multiViewGrid.cellHeight

                            VideoOutput {
                                id: vOutput
                                anchors.fill: parent
                                fillMode: VideoOutput.PreserveAspectFit
                                z: 1
                                Component.onCompleted: {
                                        if (streamIndex < uiManager.playbackProviders.length) {
                                            uiManager.playbackProviders[streamIndex].videoSink = vOutput.videoSink
                                    }
                                }
                            }

                            onVisibleChanged: {
                                if (visible) {
                                    if (streamIndex < uiManager.playbackProviders.length) {
                                        uiManager.playbackProviders[streamIndex].videoSink = vOutput.videoSink
                                    }
                                }
                            }

                            Text {
                                text: "CAM " + (streamIndex + 1)
                                color: "white"
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.margins: 5
                                font.family: "Menlo"
                                font.pixelSize: 12
                                z: 5
                            }

                            MouseArea {
                                anchors.fill: parent
                                z: 2
                                onClicked: {
                                    playbackTab.selectedIndex = streamIndex
                                    playbackTab.viewMode = "single"
                                }
                            }
                        }
                    }
                }

                Slider {
                    id: scrubBar
                    Layout.fillWidth: true
                    from: 0
                    to: uiManager.recordedDurationMs
                    value: uiManager.scrubPosition

                    onMoved: {
                        uiManager.seekPlayback(value)
                    }

                    background: Rectangle {
                        height: 6
                        radius: 3
                        color: "#333"
                        Rectangle {
                            width: scrubBar.visualPosition * parent.width
                            height: parent.height
                            color: "#007AFF"
                            radius: 3
                        }
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    spacing: 12

                    Text {
                        text: playbackTab.formatTimecode(uiManager.scrubPosition)
                        color: "#eee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "REV 5.0x"
                        onClicked: {
                            uiManager.transport.setSpeed(-5.0)
                            uiManager.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: uiManager.transport.isPlaying ? "PAUSE" : "PLAY"
                        onClicked: uiManager.transport.setPlaying(!uiManager.transport.isPlaying);
                        highlighted: uiManager.transport.isPlaying
                    }

                    Button {
                        text: ">"
                        onClicked: {
                            uiManager.transport.step(1)
                            uiManager.transport.setPlaying(false)
                        }
                    }

                    Button {
                        text: "0.25x"
                        onClicked: {
                            uiManager.transport.setSpeed(0.25)
                            uiManager.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "0.5x"
                        onClicked: {
                            uiManager.transport.setSpeed(0.5)
                            uiManager.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "1.0x"
                        onClicked: {
                            uiManager.transport.setSpeed(1)
                            uiManager.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "2.0x"
                        onClicked: {
                            uiManager.transport.setSpeed(2.0)
                            uiManager.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "Live"
                        onClicked: {
                            uiManager.transport.setSpeed(1.0)
                            uiManager.scrubToLive();
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: playbackTab.formatTimecode(uiManager.recordedDurationMs)
                        color: "#eee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                    }
                }
            }

            // --- Project Tab ---
            ColumnLayout {
                spacing: 12
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    text: "Project Settings"
                    font.bold: true
                    color: "#eee"
                }

                GridLayout {
                    columns: 3
                    Layout.fillWidth: true

                    Label { text: "Project Name"; }

                    TextField {
                        Layout.fillWidth: true
                        Layout.columnSpan: 2
                        text: uiManager.fileName
                        onEditingFinished: uiManager.fileName = text
                    }

                    Label { text: "Save Location"; }

                    TextField {
                        Layout.fillWidth: true
                        text: uiManager.saveLocation
                        placeholderText: "Select folder..."
                        onEditingFinished: uiManager.saveLocation = text
                    }

                    Button {
                        text: "Browse..."
                        onClicked: folderDialog.open()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "Input Sources"
                        Layout.fillWidth: true
                        color: "#eee"
                        font.bold: true
                    }

                    Button {
                        text: "+ Add Stream"
                        onClicked: uiManager.addStream()
                        enabled: !uiManager.isRecording
                    }
                }

                ListView {
                    id: streamList
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    clip: true
                    model: uiManager.streamUrls
                    spacing: 8

                    delegate: RowLayout {
                        required property string modelData
                        required property int index
                        width: streamList.width
                        spacing: 10

                        Label {
                            text: (parent.index + 1) + ":"
                            width: 20
                        }

                        TextField {
                            Layout.fillWidth: true
                            text: parent.modelData
                            placeholderText: "rtmp://..."
                            onEditingFinished: uiManager.updateUrl(parent.index, text)
                        }

                        Button {
                            text: "×"
                            width: 30
                            flat: true
                            onClicked: uiManager.removeStream(index)
                            visible: !uiManager.isRecording
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Button {
                        text: "Save Config"
                        onClicked: uiManager.saveSettings()
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
