pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform 1.1
import QtQuick.Controls.Universal 2.15 // Necessary for attached properties
import QtMultimedia

ApplicationWindow {
    id: appWindow
    visible: true
    width: 800
    height: 600
    title: "OpenLiveReplay"

    property var multiviewWindow: null
    property int multiviewScreenIndex: 0
    property var screenOptions: appWindow.uiManagerRef ? appWindow.uiManagerRef.screenOptions : []
    property bool screensReady: appWindow.uiManagerRef ? appWindow.uiManagerRef.screensReady : false
    property var uiManagerRef: uiManager
    property alias playbackTab: playbackTab
    property alias screenProbe: screenProbe

    Component.onCompleted: {
        appWindow.uiManagerRef.loadSettings()
        appWindow.uiManagerRef.openStreams()
        appWindow.playbackTab.selectedIndex = -1
        appWindow.playbackTab.viewMode = "multi"
        appWindow.refreshScreenOptions()
        if (!appWindow.screensReady) appWindow.screenProbe.start()
    }

    function openMultiviewWindow() {
        appWindow.uiManagerRef.requestNewWindowScene()
        if (appWindow.multiviewWindow) {
            appWindow.multiviewWindow.visible = true
            appWindow.multiviewWindow.raise()
            appWindow.multiviewWindow.requestActivate()
            return
        }

        var component = Qt.createComponent("qrc:/qt/qml/OpenLiveReplay/MultiviewWindow.qml")
        if (component.status === Component.Ready) {
            appWindow.multiviewWindow = component.createObject(appWindow, {
                uiManager: appWindow.uiManagerRef,
                owner: appWindow
            })
            if (appWindow.multiviewWindow) {
                appWindow.multiviewWindow.visibleStreamIndexes = Qt.binding(function() {
                    return appWindow.playbackTab.visibleStreamIndexes
                })
                appWindow.updateMultiviewScreen()
            }
        } else {
            console.error("Failed to load MultiviewWindow.qml:", component.errorString())
        }
    }

    function openMultiviewOnExternalDisplay() {
        if (!appWindow.uiManagerRef || appWindow.uiManagerRef.screenCount === 0) {
            console.warn("No screens detected")
            return
        }

        if (!appWindow.multiviewWindow) {
            appWindow.openMultiviewWindow()
            if (!appWindow.multiviewWindow) return
        }

        var targetIndex = Math.min(Math.max(0, appWindow.multiviewScreenIndex), appWindow.uiManagerRef.screenCount - 1)
        var targetScreen = appWindow.uiManagerRef.screenAt(targetIndex)
        if (targetScreen) {
            appWindow.multiviewWindow.screen = targetScreen
        }
        appWindow.multiviewWindow.visibility = Window.FullScreen
        appWindow.multiviewWindow.visible = true
        appWindow.multiviewWindow.raise()
        appWindow.multiviewWindow.requestActivate()
    }

    function updateMultiviewScreen() {
        if (!appWindow.multiviewWindow || !appWindow.uiManagerRef) return

        if (appWindow.uiManagerRef.screenCount > 0) {
            if (appWindow.multiviewScreenIndex < 0 || appWindow.multiviewScreenIndex >= appWindow.uiManagerRef.screenCount) {
                appWindow.multiviewScreenIndex = 0
            }
            var screenObj = appWindow.uiManagerRef.screenAt(appWindow.multiviewScreenIndex)
            if (screenObj) {
                appWindow.multiviewWindow.screen = screenObj
            }
            appWindow.multiviewWindow.visibility = Window.FullScreen
        }
    }

    function refreshScreenOptions() {
        if (appWindow.uiManagerRef) {
            appWindow.uiManagerRef.refreshScreens()
        }
    }

    function makeScreenHandler(screenIdx) {
        return function() {
            appWindow.multiviewScreenIndex = screenIdx
            appWindow.openMultiviewOnExternalDisplay()
        }
    }

    Connections {
        target: appWindow.uiManagerRef
        function onScreensChanged() {
            appWindow.updateMultiviewScreen()
        }
    }

    Timer {
        id: screenProbe
        interval: 150
        repeat: true
        onTriggered: {
            appWindow.refreshScreenOptions()
            if (appWindow.screensReady) stop()
        }
    }

    // FORCE THE THEME HERE
    Universal.theme: Universal.Dark
    Universal.accent: Universal.Red

    FolderDialog {
        id: folderDialog
        title: "Select Recording Folder"
        currentFolder: appWindow.uiManagerRef.saveLocation ? "file://" + appWindow.uiManagerRef.saveLocation : ""
        onAccepted: {
            appWindow.uiManagerRef.setSaveLocationFromUrl(folderDialog.folder)
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
                        color: "#eeeeee"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            id: recordButton
                            text: appWindow.uiManagerRef.isRecording ? "STOP RECORDING" : "START RECORDING"
                            padding: 18

                            background: Rectangle {
                                color: appWindow.uiManagerRef.isRecording ? "#d32f2f" : "#2e7d32"
                                radius: 6
                            }

                            contentItem: Text {
                                text: recordButton.text
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: appWindow.uiManagerRef.isRecording ? appWindow.uiManagerRef.stopRecording() : appWindow.uiManagerRef.startRecording()
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            id: multiviewMenuButton
                            text: "Fullscreen Multiview ▾"
                            padding: 12
                            onClicked: {
                                appWindow.refreshScreenOptions()
                                screenMenu.x = 0
                                screenMenu.y = multiviewMenuButton.height + 4
                                screenMenu.open()
                            }

                            Menu {
                                id: screenMenu
                                Repeater {
                                    model: appWindow.screenOptions
                                    delegate: MenuItem {
                                        id: screenMenuItem
                                        required property var modelData
                                        text: modelData.label
                                        onTriggered: appWindow.makeScreenHandler(modelData.index)()
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        text: appWindow.uiManagerRef.isRecording ? "● RECORDING LIVE" : "IDLE"
                        color: appWindow.uiManagerRef.isRecording ? "#ff5252" : "#666"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Text {
                            text: "Multiview Views"
                            color: "#eeeeee"
                            Layout.alignment: Qt.AlignVCenter
                        }

                        SpinBox {
                            from: 1
                            to: 16
                            stepSize: 1
                            editable: true
                            inputMethodHints: Qt.ImhDigitsOnly
                            value: appWindow.uiManagerRef.multiviewCount
                            enabled: !appWindow.uiManagerRef.isRecording
                            onValueModified: appWindow.uiManagerRef.multiviewCount = value
                        }

                        Item { Layout.fillWidth: true }
                    }

                    GroupBox {
                        title: "MIDI"
                        Layout.fillWidth: true

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            ComboBox {
                                Layout.fillWidth: true
                                model: appWindow.uiManagerRef.midiPorts
                                currentIndex: appWindow.uiManagerRef.midiPortIndex
                                onActivated: appWindow.uiManagerRef.setMidiPortIndex(currentIndex)
                            }

                            Button {
                                text: "Refresh"
                                onClicked: appWindow.uiManagerRef.refreshMidiPorts()
                            }

                            Text {
                                text: appWindow.uiManagerRef.midiConnected ? "Connected" : "Disconnected"
                                color: appWindow.uiManagerRef.midiConnected ? "#4CAF50" : "#777"
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
                                    { name: "Prev Frame", action: 7 },
                                    { name: "Next Frame", action: 3 },
                                    { name: "Jogwheel", action: 8 },
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
                                    id: midiRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Text {
                                        text: midiRow.modelData.name
                                        Layout.preferredWidth: 130
                                        color: "#eeeeee"
                                    }

                                    Text {
                                             text: (appWindow.uiManagerRef.midiBindingsVersion >= 0
                                                  ? appWindow.uiManagerRef.midiBindingLabel(midiRow.modelData.action)
                                                  : appWindow.uiManagerRef.midiBindingLabel(midiRow.modelData.action))
                                        color: appWindow.uiManagerRef.midiLearnAction === midiRow.modelData.action ? "#ff9800" : "#aaa"
                                        Layout.fillWidth: true
                                    }

                                    Text {
                                        text: (appWindow.uiManagerRef.midiLastValuesVersion >= 0
                                              ? "Last: " + appWindow.uiManagerRef.midiLastValue(midiRow.modelData.action)
                                              : "Last: " + appWindow.uiManagerRef.midiLastValue(midiRow.modelData.action))
                                        color: "#666666"
                                        Layout.preferredWidth: 80
                                    }

                                    Button {
                                        visible: midiRow.modelData.action !== 8
                                        text: appWindow.uiManagerRef.midiLearnAction === midiRow.modelData.action ? "Listening..." : "Learn"
                                        onClicked: appWindow.uiManagerRef.beginMidiLearn(midiRow.modelData.action)
                                    }

                                    Button {
                                        visible: midiRow.modelData.action === 8
                                        text: (appWindow.uiManagerRef.midiLearnAction === midiRow.modelData.action && appWindow.uiManagerRef.midiLearnMode === 0)
                                              ? "Listening..."
                                              : "Learn Ctrl"
                                        onClicked: appWindow.uiManagerRef.beginMidiLearn(midiRow.modelData.action)
                                    }

                                    Button {
                                        visible: midiRow.modelData.action === 8
                                        text: (appWindow.uiManagerRef.midiLearnAction === midiRow.modelData.action && appWindow.uiManagerRef.midiLearnMode === 1)
                                              ? "Listening..."
                                              : "Learn Fwd"
                                        onClicked: appWindow.uiManagerRef.beginMidiLearnJogForward(midiRow.modelData.action)
                                    }

                                    Button {
                                        visible: midiRow.modelData.action === 8
                                        text: (appWindow.uiManagerRef.midiLearnAction === midiRow.modelData.action && appWindow.uiManagerRef.midiLearnMode === 2)
                                              ? "Listening..."
                                              : "Learn Back"
                                        onClicked: appWindow.uiManagerRef.beginMidiLearnJogBackward(midiRow.modelData.action)
                                    }

                                    Button {
                                        text: "Clear"
                                        onClicked: appWindow.uiManagerRef.clearMidiBinding(midiRow.modelData.action)
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
                property var selectedProvider: (selectedIndex >= 0 && selectedIndex < appWindow.uiManagerRef.playbackProviders.length)
                                                ? appWindow.uiManagerRef.playbackProviders[selectedIndex]
                                               : null
                property string viewMode: "multi"
                property int gridColumns: Math.max(1, Math.ceil(Math.sqrt(Math.max(1, streamCount))))
                property int gridRows: Math.ceil(Math.max(1, streamCount) / gridColumns)
                property bool showTimeOfDay: appWindow.uiManagerRef.timeOfDayMode
                property int clockTick: 0
                property bool holdWasPlaying: false

                function formatTimecode(ms) {
                    var totalSeconds = Math.floor(ms / 1000)
                    var hours = Math.floor(totalSeconds / 3600)
                    var minutes = Math.floor((totalSeconds % 3600) / 60)
                    var seconds = totalSeconds % 60
                    var fps = Math.max(1, appWindow.uiManagerRef.recordFps)
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
                    var viewCount = Math.max(1, Math.min(16, appWindow.uiManagerRef.multiviewCount))
                    for (var i = 0; i < viewCount; ++i) {
                        indexes.push(i)
                    }
                    visibleStreamIndexes = indexes
                }

                Component.onCompleted: {
                    selectedIndex = -1
                    viewMode = "multi"
                    updateVisibleStreams()
                }

                onSelectedProviderChanged: {
                    if (viewMode === "single" && selectedProvider) {
                        selectedProvider.videoSink = singleOutput.videoSink
                    }
                }

                Connections {
                    target: appWindow.uiManagerRef
                    function onPlaybackProvidersChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                    }
                    function onStreamUrlsChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                    }
                    function onMultiviewCountChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                    }
                    function onFeedSelectRequested(index) {
                        playbackTab.selectedIndex = index
                        playbackTab.viewMode = "single"
                        appWindow.uiManagerRef.setPlaybackViewState(true, index)
                    }
                    function onMultiviewRequested() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                    }
                }

                onVisibleChanged: {
                    if (visible) {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                    }
                }

                Item {
                    id: playbackView
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Rectangle {
                        id: singleView
                        anchors.fill: parent
                        property int sourceForView: {
                            var map = appWindow.uiManagerRef.viewSlotMap
                            return (playbackTab.selectedIndex >= 0 && playbackTab.selectedIndex < map.length)
                                   ? map[playbackTab.selectedIndex] : -1
                        }
                        color: sourceForView < 0 ? "#003080" : "black"
                        border.color: sourceForView < 0 ? "#1565C0" : "#00C853"
                        border.width: 2
                        visible: playbackTab.viewMode === "single" && playbackTab.selectedIndex >= 0 && appWindow.uiManagerRef.playbackProviders.length > 0

                        VideoOutput {
                            id: singleOutput
                            anchors.fill: parent
                            fillMode: VideoOutput.PreserveAspectFit
                            visible: singleView.sourceForView >= 0
                        }

                        Text {
                            text: {
                                var src = singleView.sourceForView
                                if (playbackTab.selectedIndex < 0) return ""
                                if (src < 0) return "VIEW " + (playbackTab.selectedIndex + 1)
                                return (src < appWindow.uiManagerRef.streamNames.length && appWindow.uiManagerRef.streamNames[src].length > 0)
                                    ? appWindow.uiManagerRef.streamNames[src]
                                    : ("CAM " + (src + 1))
                            }
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
                                appWindow.uiManagerRef.setPlaybackViewState(false, -1)
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
                            id: multiViewDelegate
                            required property var modelData
                            property int streamIndex: modelData
                            property int sourceForView: {
                                var map = appWindow.uiManagerRef.viewSlotMap
                                return (multiViewDelegate.streamIndex >= 0 && multiViewDelegate.streamIndex < map.length)
                                       ? map[multiViewDelegate.streamIndex] : -1
                            }
                            color: sourceForView < 0 ? "#003080" : "black"
                            border.color: sourceForView < 0 ? "#1565C0" : "red"
                            border.width: 2
                            width: multiViewGrid.cellWidth
                            height: multiViewGrid.cellHeight

                            VideoOutput {
                                id: vOutput
                                anchors.fill: parent
                                fillMode: VideoOutput.PreserveAspectFit
                                visible: multiViewDelegate.sourceForView >= 0
                                z: 1
                                Component.onCompleted: {
                                    if (multiViewDelegate.streamIndex < appWindow.uiManagerRef.playbackProviders.length) {
                                        appWindow.uiManagerRef.playbackProviders[multiViewDelegate.streamIndex].videoSink = vOutput.videoSink
                                    }
                                }
                            }

                            onVisibleChanged: {
                                if (visible) {
                                    if (multiViewDelegate.streamIndex < appWindow.uiManagerRef.playbackProviders.length) {
                                        appWindow.uiManagerRef.playbackProviders[multiViewDelegate.streamIndex].videoSink = vOutput.videoSink
                                    }
                                }
                            }

                            Text {
                                text: {
                                    var src = multiViewDelegate.sourceForView
                                    if (src < 0) return "VIEW " + (multiViewDelegate.streamIndex + 1)
                                    return (src < appWindow.uiManagerRef.streamNames.length && appWindow.uiManagerRef.streamNames[src].length > 0)
                                        ? appWindow.uiManagerRef.streamNames[src]
                                        : ("CAM " + (src + 1))
                                }
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
                                    playbackTab.selectedIndex = multiViewDelegate.streamIndex
                                    playbackTab.viewMode = "single"
                                    appWindow.uiManagerRef.setPlaybackViewState(true, multiViewDelegate.streamIndex)
                                }
                            }
                        }
                    }
                }

                Slider {
                    id: scrubBar
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(0, appWindow.uiManagerRef.recordedDurationMs - appWindow.uiManagerRef.liveBufferMs)
                    value: appWindow.uiManagerRef.scrubPosition

                    onMoved: {
                        appWindow.uiManagerRef.seekPlayback(value)
                    }

                    background: Rectangle {
                        height: 6
                        radius: 3
                        color: "#333333"
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
                        text: playbackTab.showTimeOfDay && appWindow.uiManagerRef.recordingStartEpochMs > 0
                            ? playbackTab.formatTimeOfDay(appWindow.uiManagerRef.recordingStartEpochMs + appWindow.uiManagerRef.scrubPosition)
                            : playbackTab.formatTimecode(appWindow.uiManagerRef.scrubPosition)
                        color: "#eeeeee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea {
                            anchors.fill: parent
                            onClicked: appWindow.uiManagerRef.timeOfDayMode = !appWindow.uiManagerRef.timeOfDayMode
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "REV 5.0x"
                        onPressed: {
                            appWindow.uiManagerRef.cancelFollowLive()
                            playbackTab.holdWasPlaying = appWindow.uiManagerRef.transport.isPlaying
                            appWindow.uiManagerRef.transport.setSpeed(-5.0)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                        onReleased: {
                            appWindow.uiManagerRef.transport.setSpeed(1.0)
                            appWindow.uiManagerRef.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                        onCanceled: {
                            appWindow.uiManagerRef.transport.setSpeed(1.0)
                            appWindow.uiManagerRef.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                    }

                    Button {
                        text: "FWD 5.0x"
                        onPressed: {
                            appWindow.uiManagerRef.cancelFollowLive()
                            playbackTab.holdWasPlaying = appWindow.uiManagerRef.transport.isPlaying
                            appWindow.uiManagerRef.transport.setSpeed(5.0)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                        onReleased: {
                            appWindow.uiManagerRef.transport.setSpeed(1.0)
                            appWindow.uiManagerRef.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                        onCanceled: {
                            appWindow.uiManagerRef.transport.setSpeed(1.0)
                            appWindow.uiManagerRef.transport.setPlaying(playbackTab.holdWasPlaying)
                        }
                    }

                    Button {
                        text: appWindow.uiManagerRef.transport.isPlaying ? "PAUSE" : "PLAY"
                        onClicked: appWindow.uiManagerRef.playPause()
                        highlighted: appWindow.uiManagerRef.transport.isPlaying
                    }

                    Button {
                        text: "<"
                        onClicked: appWindow.uiManagerRef.stepFrameBack()
                    }

                    Button {
                        text: ">"
                        onClicked: appWindow.uiManagerRef.stepFrame()
                    }

                    Button {
                        text: "0.25x"
                        onClicked: {
                            appWindow.uiManagerRef.transport.setSpeed(0.25)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "0.5x"
                        onClicked: {
                            appWindow.uiManagerRef.transport.setSpeed(0.5)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "1.0x"
                        onClicked: {
                            appWindow.uiManagerRef.transport.setSpeed(1)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "2.0x"
                        onClicked: {
                            appWindow.uiManagerRef.transport.setSpeed(2.0)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
                    }

                    Button {
                        text: "Live"
                        onClicked: appWindow.uiManagerRef.goLive()
                    }

                    Button {
                        text: "Capture"
                        onClicked: appWindow.uiManagerRef.captureCurrent()
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: playbackTab.showTimeOfDay
                            ? (playbackTab.clockTick >= 0
                               ? playbackTab.formatTimeOfDay(Date.now())
                               : playbackTab.formatTimeOfDay(Date.now()))
                            : playbackTab.formatTimecode(appWindow.uiManagerRef.recordedDurationMs)
                        color: "#eeeeee"
                        font.family: "Menlo"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea {
                            anchors.fill: parent
                            onClicked: appWindow.uiManagerRef.timeOfDayMode = !appWindow.uiManagerRef.timeOfDayMode
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
                    color: "#eeeeee"
                }

                GridLayout {
                    columns: 3
                    Layout.fillWidth: true

                    Label { text: "Project Name"; }

                    TextField {
                        Layout.fillWidth: true
                        Layout.columnSpan: 2
                        text: appWindow.uiManagerRef.fileName
                        onEditingFinished: appWindow.uiManagerRef.fileName = text
                    }

                    Label { text: "Save Location"; }

                    TextField {
                        Layout.fillWidth: true
                        text: appWindow.uiManagerRef.saveLocation
                        placeholderText: "Select folder..."
                        onEditingFinished: appWindow.uiManagerRef.saveLocation = text
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
                        value: appWindow.uiManagerRef.recordWidth
                        onValueModified: appWindow.uiManagerRef.recordWidth = value
                    }

                    Label { text: "x"; }

                    SpinBox {
                        from: 240
                        to: 4320
                        stepSize: 10
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: appWindow.uiManagerRef.recordHeight
                        onValueModified: appWindow.uiManagerRef.recordHeight = value
                    }

                    Item { }

                    Label { text: "FPS"; }

                    SpinBox {
                        from: 1
                        to: 120
                        stepSize: 1
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: appWindow.uiManagerRef.recordFps
                        onValueModified: appWindow.uiManagerRef.recordFps = value
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
                        color: "#eeeeee"
                        font.bold: true
                    }

                    Button {
                        text: "+ Add Stream"
                        onClicked: appWindow.uiManagerRef.addStream()
                        enabled: !appWindow.uiManagerRef.isRecording
                    }
                }

                ListView {
                    id: streamList
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    clip: true
                    model: appWindow.uiManagerRef.streamUrls
                    spacing: 8

                    delegate: RowLayout {
                        id: streamRow
                        required property string modelData
                        required property int index
                        width: streamList.width
                        spacing: 10

                        Label {
                            text: (streamRow.index + 1) + ":"
                            Layout.preferredWidth: 20
                        }

                        Button {
                            text: (appWindow.uiManagerRef.sourceEnabledVersion >= 0
                                   && appWindow.uiManagerRef.isSourceEnabled(streamRow.index))
                                  ? "ON" : "OFF"
                            Layout.preferredWidth: 50
                            palette.button: (appWindow.uiManagerRef.sourceEnabledVersion >= 0
                                             && appWindow.uiManagerRef.isSourceEnabled(streamRow.index))
                                            ? "#2e7d32" : "#555"
                            onClicked: appWindow.uiManagerRef.toggleSourceEnabled(streamRow.index)
                        }

                        TextField {
                            Layout.preferredWidth: 160
                            text: appWindow.uiManagerRef.streamIds.length > streamRow.index ? appWindow.uiManagerRef.streamIds[streamRow.index] : ""
                            placeholderText: "ID"
                            onEditingFinished: appWindow.uiManagerRef.updateStreamId(streamRow.index, text)
                        }

                        TextField {
                            Layout.preferredWidth: 140
                            text: appWindow.uiManagerRef.streamNames.length > streamRow.index ? appWindow.uiManagerRef.streamNames[streamRow.index] : ""
                            placeholderText: "Name"
                            onEditingFinished: appWindow.uiManagerRef.updateStreamName(streamRow.index, text)
                        }

                        TextField {
                            Layout.fillWidth: true
                            text: streamRow.modelData
                            placeholderText: "rtmp://..."
                            onEditingFinished: appWindow.uiManagerRef.updateUrl(streamRow.index, text)
                        }

                        Button {
                            text: "×"
                            Layout.preferredWidth: 30
                            flat: true
                            onClicked: appWindow.uiManagerRef.removeStream(streamRow.index)
                            visible: !appWindow.uiManagerRef.isRecording
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Button {
                        text: "Save Config"
                        onClicked: appWindow.uiManagerRef.saveSettings()
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
