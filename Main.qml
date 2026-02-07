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
        playbackTab.viewMode = "multi"
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
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    width: parent.width
                    spacing: 16
                    Layout.fillWidth: true

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

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        text: uiManager.isRecording ? "● RECORDING LIVE" : "IDLE"
                        color: uiManager.isRecording ? "#ff5252" : "#666"
                    }

                    GroupBox {
                        title: "MIDI"
                        Layout.fillWidth: true

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            ComboBox {
                                Layout.fillWidth: true
                                model: uiManager.midiPorts
                                currentIndex: uiManager.midiPortIndex
                                onActivated: uiManager.setMidiPortIndex(currentIndex)
                            }

                            Button {
                                text: "Refresh"
                                onClicked: uiManager.refreshMidiPorts()
                            }

                            Text {
                                text: uiManager.midiConnected ? "Connected" : "Disconnected"
                                color: uiManager.midiConnected ? "#4CAF50" : "#777"
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    GroupBox {
                        title: "MIDI Mapping"
                        Layout.fillWidth: true

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Repeater {
                                model: [
                                    { name: "Play/Pause", action: 0 },
                                    { name: "Rewind 5.0x", action: 1 },
                                    { name: "Forward 5.0x", action: 2 },
                                    { name: "Next Frame", action: 3 },
                                    { name: "Go Live", action: 4 },
                                    { name: "Capture", action: 5 },
                                    { name: "Multiview", action: 6 },
                                    { name: "Feed 1", action: 100 },
                                    { name: "Feed 2", action: 101 },
                                    { name: "Feed 3", action: 102 },
                                    { name: "Feed 4", action: 103 },
                                    { name: "Feed 5", action: 104 },
                                    { name: "Feed 6", action: 105 },
                                    { name: "Feed 7", action: 106 },
                                    { name: "Feed 8", action: 107 }
                                ]

                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Text {
                                        text: modelData.name
                                        Layout.preferredWidth: 130
                                        color: "#eee"
                                    }

                                    Text {
                                             text: (uiManager.midiBindingsVersion >= 0
                                                 ? uiManager.midiBindingLabel(modelData.action)
                                                 : uiManager.midiBindingLabel(modelData.action))
                                        color: uiManager.midiLearnAction === modelData.action ? "#ff9800" : "#aaa"
                                        Layout.fillWidth: true
                                    }

                                    Button {
                                        text: uiManager.midiLearnAction === modelData.action ? "Listening..." : "Learn"
                                        onClicked: uiManager.beginMidiLearn(modelData.action)
                                    }

                                    Button {
                                        text: "Clear"
                                        onClicked: uiManager.clearMidiBinding(modelData.action)
                                    }
                                }
                            }
                        }
                    }
                }
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
                property bool showTimeOfDay: uiManager.timeOfDayMode
                property int clockTick: 0
                property bool holdWasPlaying: false

                function formatTimecode(ms) {
                    var totalSeconds = Math.floor(ms / 1000)
                    var hours = Math.floor(totalSeconds / 3600)
                    var minutes = Math.floor((totalSeconds % 3600) / 60)
                    var seconds = totalSeconds % 60
                    var fps = Math.max(1, uiManager.recordFps)
                    var frames = Math.floor((ms % 1000) / (1000 / fps))

                    var hh = hours < 10 ? "0" + hours : "" + hours
                    var mm = minutes < 10 ? "0" + minutes : "" + minutes
                    var ss = seconds < 10 ? "0" + seconds : "" + seconds
                    var ff = frames < 10 ? "0" + frames : "" + frames
                    return hh + ":" + mm + ":" + ss + "." + ff
                }

                function formatTimeOfDay(epochMs) {
                    if (epochMs <= 0) return "--:--:--"
                    var d = new Date(epochMs)
                    var hh = d.getHours() < 10 ? "0" + d.getHours() : "" + d.getHours()
                    var mm = d.getMinutes() < 10 ? "0" + d.getMinutes() : "" + d.getMinutes()
                    var ss = d.getSeconds() < 10 ? "0" + d.getSeconds() : "" + d.getSeconds()
                    return hh + ":" + mm + ":" + ss
                }

                Timer {
                    id: clockTimer
                    interval: 500
                    running: playbackTab.showTimeOfDay
                    repeat: true
                    onTriggered: playbackTab.clockTick = playbackTab.clockTick + 1
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
                        uiManager.setPlaybackViewState(false, -1)
                    }
                    function onStreamUrlsChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        uiManager.setPlaybackViewState(false, -1)
                    }
                    function onFeedSelectRequested(index) {
                        playbackTab.selectedIndex = index
                        playbackTab.viewMode = "single"
                        uiManager.setPlaybackViewState(true, index)
                    }
                    function onMultiviewRequested() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        uiManager.setPlaybackViewState(false, -1)
                    }
                }

                onVisibleChanged: {
                    if (visible) {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        uiManager.setPlaybackViewState(false, -1)
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
                            text: playbackTab.selectedIndex >= 0
                                  ? ((playbackTab.selectedIndex < uiManager.streamNames.length && uiManager.streamNames[playbackTab.selectedIndex].length > 0)
                                     ? uiManager.streamNames[playbackTab.selectedIndex]
                                     : ("CAM " + (playbackTab.selectedIndex + 1)))
                                  : ""
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
                                uiManager.setPlaybackViewState(false, -1)
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
                                text: (streamIndex < uiManager.streamNames.length && uiManager.streamNames[streamIndex].length > 0)
                                      ? uiManager.streamNames[streamIndex]
                                      : ("CAM " + (streamIndex + 1))
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
                                    uiManager.setPlaybackViewState(true, streamIndex)
                                }
                            }
                        }
                    }
                }

                Slider {
                    id: scrubBar
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(0, uiManager.recordedDurationMs - uiManager.liveBufferMs)
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
                        text: playbackTab.showTimeOfDay && uiManager.recordingStartEpochMs > 0
                              ? playbackTab.formatTimeOfDay(uiManager.recordingStartEpochMs + uiManager.scrubPosition)
                              : playbackTab.formatTimecode(uiManager.scrubPosition)
                        color: "#eee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea {
                            anchors.fill: parent
                            onClicked: uiManager.timeOfDayMode = !uiManager.timeOfDayMode
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "REV 5.0x"
                        onPressed: {
                            playbackTab.holdWasPlaying = uiManager.transport.isPlaying
                            uiManager.transport.setSpeed(-5.0)
                            uiManager.transport.setPlaying(true)
                        }
                        onReleased: {
                            uiManager.transport.setSpeed(1.0)
                            uiManager.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                        onCanceled: {
                            uiManager.transport.setSpeed(1.0)
                            uiManager.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                    }

                    Button {
                        text: "FWD 5.0x"
                        onPressed: {
                            playbackTab.holdWasPlaying = uiManager.transport.isPlaying
                            uiManager.transport.setSpeed(5.0)
                            uiManager.transport.setPlaying(true)
                        }
                        onReleased: {
                            uiManager.transport.setSpeed(1.0)
                            uiManager.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                        onCanceled: {
                            uiManager.transport.setSpeed(1.0)
                            uiManager.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                    }

                    Button {
                        text: uiManager.transport.isPlaying ? "PAUSE" : "PLAY"
                        onClicked: uiManager.playPause()
                        highlighted: uiManager.transport.isPlaying
                    }

                    Button {
                        text: ">"
                        onClicked: uiManager.stepFrame()
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
                        onClicked: uiManager.goLive()
                    }

                    Button {
                        text: "Capture"
                        onClicked: uiManager.captureCurrent()
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: playbackTab.showTimeOfDay
                            ? (playbackTab.clockTick >= 0
                               ? playbackTab.formatTimeOfDay(Date.now())
                               : playbackTab.formatTimeOfDay(Date.now()))
                            : playbackTab.formatTimecode(uiManager.recordedDurationMs)
                        color: "#eee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea {
                            anchors.fill: parent
                            onClicked: uiManager.timeOfDayMode = !uiManager.timeOfDayMode
                        }
                        onVisibleChanged: clockTimer.restart()
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

                GridLayout {
                    columns: 5
                    Layout.fillWidth: true

                    Label { text: "Resolution"; }

                    SpinBox {
                        from: 320
                        to: 7680
                        stepSize: 10
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: uiManager.recordWidth
                        onValueModified: uiManager.recordWidth = value
                    }

                    Label { text: "x"; }

                    SpinBox {
                        from: 240
                        to: 4320
                        stepSize: 10
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: uiManager.recordHeight
                        onValueModified: uiManager.recordHeight = value
                    }

                    Item { }

                    Label { text: "FPS"; }

                    SpinBox {
                        from: 1
                        to: 120
                        stepSize: 1
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: uiManager.recordFps
                        onValueModified: uiManager.recordFps = value
                    }

                    Item { }
                    Item { }
                    Item { }
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
                            Layout.preferredWidth: 140
                            text: uiManager.streamNames.length > parent.index ? uiManager.streamNames[parent.index] : ""
                            placeholderText: "Name"
                            onEditingFinished: uiManager.updateStreamName(parent.index, text)
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
