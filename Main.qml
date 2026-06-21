pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform 1.1
import QtMultimedia
import OlrTheme

ApplicationWindow {
    id: appWindow
    visible: true
    width: 1200
    height: 760
    // Floor below which controls would be lost: the OS can't shrink past it, and
    // every tab scrolls so the floor only needs to cover one transport row.
    minimumWidth: Theme.windowMinW
    minimumHeight: Theme.windowMinH
    color: Theme.canvas
    title: "OpenLiveReplay"

    property var multiviewWindow: null
    property int multiviewScreenIndex: 0
    property var screenOptions: appWindow.uiManagerRef ? appWindow.uiManagerRef.screenOptions : []
    property bool screensReady: appWindow.uiManagerRef ? appWindow.uiManagerRef.screensReady : false
    // uiManager is a C++ context property injected at the root context that the
    // linter cannot resolve. This is the only unqualified access in the file;
    // every call site uses appWindow.uiManagerRef instead.
    // qmllint disable unqualified
    property var uiManagerRef: uiManager
    // qmllint enable unqualified
    property alias playbackTab: playbackTab
    property alias screenProbe: screenProbe
    // Last recording-start failure reason, surfaced near the record button.
    // Cleared on a successful recording start.
    property string recordingError: ""
    // Soft warning from recordingWarning signal (e.g. feed-count exceeds safe
    // benchmark limit). Has its OWN surface — recordingStarted/recordingFailed
    // do NOT clear this; it auto-dismisses after ~8 s or on manual close.
    property string recordingWarningText: ""

    function showRecordingWarning(msg) {
        appWindow.recordingWarningText = msg
        recordingWarningDismissTimer.restart()
    }

    Timer {
        id: recordingWarningDismissTimer
        interval: 8000
        repeat: false
        onTriggered: appWindow.recordingWarningText = ""
    }

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
        function onRecordingFailed(reason) {
            appWindow.recordingError = reason
        }
        function onRecordingStarted() {
            appWindow.recordingError = ""
        }
        function onImportPreviewChanged() {
            if (appWindow.uiManagerRef.importPreviewReady
                    && !appWindow.uiManagerRef.isRecording
                    && !importPreviewPopup.opened) {
                importPreviewPopup.open()
            }
        }
        function onRecordingWarning(msg) {
            // Soft warning gets its OWN property so that the recordingStarted
            // handler (which clears recordingError) cannot erase it (C2).
            appWindow.showRecordingWarning(msg)
        }
        function onRecordingStopped() {
            appWindow.recordingWarningText = ""
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
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

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
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: appWindow.recordingError !== ""
                              ? ("⚠ " + appWindow.recordingError)
                              : (appWindow.uiManagerRef.isRecording ? "● RECORDING LIVE" : "IDLE")
                        color: appWindow.recordingError !== ""
                               ? "#ffb300"
                               : (appWindow.uiManagerRef.isRecording ? "#ff5252" : "#666")
                    }

                    // Soft recording warning (e.g. feeds exceed the benchmarked safe
                    // count). Separate from recordingError so onRecordingStarted /
                    // onRecordingFailed (which clear recordingError) cannot erase it;
                    // auto-dismissed by recordingWarningDismissTimer. (C2)
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: appWindow.recordingWarningText !== ""
                        text: "⚠ " + appWindow.recordingWarningText
                        color: "#ffb300"
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

                    Frame {
                        id: midiCard
                        Layout.fillWidth: true
                        property bool expanded: false

                        background: Rectangle {
                            color: "#1b1b1b"
                            radius: 6
                            border.color: "#333"
                            border.width: 1
                        }

                        contentItem: ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: "MIDI Controller"
                                    color: "#eeeeee"
                                    font.bold: true
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Item { Layout.fillWidth: true }

                                ToolButton {
                                    text: midiCard.expanded ? "▾" : "▸"
                                    onClicked: midiCard.expanded = !midiCard.expanded
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                visible: midiCard.expanded

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
                    }

                    Frame {
                        id: streamDeckCard
                        Layout.fillWidth: true
                        property bool expanded: false
                        visible: appWindow.uiManagerRef.streamDeck.supported

                        background: Rectangle {
                            color: "#1b1b1b"
                            radius: 6
                            border.color: "#333"
                            border.width: 1
                        }

                        contentItem: ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: "Stream Deck"
                                    color: "#eeeeee"
                                    font.bold: true
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: appWindow.uiManagerRef.streamDeck.connected
                                          ? appWindow.uiManagerRef.streamDeck.deviceName
                                          : "Disconnected"
                                    color: appWindow.uiManagerRef.streamDeck.connected ? "#4CAF50" : "#777"
                                    verticalAlignment: Text.AlignVCenter
                                }

                                ToolButton {
                                    text: streamDeckCard.expanded ? "▾" : "▸"
                                    onClicked: streamDeckCard.expanded = !streamDeckCard.expanded
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                visible: streamDeckCard.expanded

                                GroupBox {
                                    title: "Stream Deck"
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        Text {
                                            text: appWindow.uiManagerRef.streamDeck.connected
                                                  ? "Connected: " + appWindow.uiManagerRef.streamDeck.deviceName
                                                    + " (" + appWindow.uiManagerRef.streamDeck.deviceModel + ")"
                                                  : "No Stream Deck connected"
                                            color: appWindow.uiManagerRef.streamDeck.connected ? "#4CAF50" : "#aaa"
                                        }

                                        Text {
                                            visible: !appWindow.uiManagerRef.streamDeck.driverAppInstalled
                                            text: "Install “Elgato Stream Deck Connect” from the App Store and enable the Stream Deck Device Driver in the iPadOS Settings app, then connect the deck via USB-C."
                                            color: "#ff9800"
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }

                                        Button {
                                            visible: appWindow.uiManagerRef.streamDeck.simulatorAvailable
                                            text: "Show Stream Deck Simulator"
                                            onClicked: appWindow.uiManagerRef.streamDeck.showSimulator()
                                        }
                                    }
                                }

                                GroupBox {
                                    title: "Button Mapping"
                                    Layout.fillWidth: true
                                    visible: appWindow.uiManagerRef.streamDeck.connected

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: "Click Learn, then press a key or turn/press a dial."
                                                color: "#aaaaaa"
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                            }
                                            Button {
                                                text: "Reset to default"
                                                onClicked: appWindow.uiManagerRef.resetStreamDeckDefaults()
                                            }
                                        }

                                        Repeater {
                                            model: [
                                                { name: "Record",      action: 9,  gesture: "key or dial" },
                                                { name: "Play/Pause",  action: 0,  gesture: "key or dial" },
                                                { name: "Go Live",     action: 4,  gesture: "key or dial" },
                                                { name: "Capture",     action: 5,  gesture: "key or dial" },
                                                { name: "Prev Frame",  action: 7,  gesture: "key or dial" },
                                                { name: "Next Frame",  action: 3,  gesture: "key or dial" },
                                                { name: "Rewind 5×",   action: 1,  gesture: "key or dial" },
                                                { name: "Forward 5×",  action: 2,  gesture: "key or dial" },
                                                { name: "Multiview",   action: 6,  gesture: "key or dial" },
                                                { name: "Jog",         action: 8,  gesture: "turn a dial" },
                                                { name: "Shuttle",     action: 10, gesture: "turn a dial" },
                                                { name: "Timecode",    action: 20, gesture: "press a key" },
                                                { name: "Speed",       action: 21, gesture: "press a key" },
                                                { name: "Feed 1",      action: 100, gesture: "key or dial" },
                                                { name: "Feed 2",      action: 101, gesture: "key or dial" },
                                                { name: "Feed 3",      action: 102, gesture: "key or dial" },
                                                { name: "Feed 4",      action: 103, gesture: "key or dial" },
                                                { name: "Feed 5",      action: 104, gesture: "key or dial" },
                                                { name: "Feed 6",      action: 105, gesture: "key or dial" },
                                                { name: "Feed 7",      action: 106, gesture: "key or dial" },
                                                { name: "Feed 8",      action: 107, gesture: "key or dial" }
                                            ]

                                            delegate: RowLayout {
                                                id: sdRow
                                                required property var modelData
                                                Layout.fillWidth: true
                                                spacing: 8

                                                Text {
                                                    text: sdRow.modelData.name
                                                    color: "#eeeeee"
                                                    Layout.preferredWidth: 110
                                                }
                                                Text {
                                                    text: (appWindow.uiManagerRef.streamDeckBindingsVersion >= 0
                                                           ? appWindow.uiManagerRef.streamDeckBindingLabel(sdRow.modelData.action)
                                                           : "")
                                                    color: appWindow.uiManagerRef.streamDeckLearnAction === sdRow.modelData.action
                                                           ? "#ff9800" : "#aaa"
                                                    Layout.fillWidth: true
                                                }
                                                Button {
                                                    text: appWindow.uiManagerRef.streamDeckLearnAction === sdRow.modelData.action
                                                          ? "Listening… (" + sdRow.modelData.gesture + ")"
                                                          : "Learn"
                                                    onClicked: appWindow.uiManagerRef.beginStreamDeckLearn(sdRow.modelData.action)
                                                }
                                                Button {
                                                    text: "Clear"
                                                    onClicked: appWindow.uiManagerRef.clearStreamDeckBinding(sdRow.modelData.action)
                                                }
                                            }
                                        }
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
                property var pgmProvider: appWindow.uiManagerRef.pgmPreviewProvider
                property var multiviewProvider: appWindow.uiManagerRef.multiviewPreviewProvider
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

                function reattachPreviewProviders() {
                    singleOutput.attachProvider(pgmProvider)
                    multiviewBusOutput.attachProvider(multiviewProvider)
                }

                Component.onCompleted: {
                    selectedIndex = -1
                    viewMode = "multi"
                    updateVisibleStreams()
                    reattachPreviewProviders()
                }

                Connections {
                    target: appWindow.uiManagerRef
                    function onPlaybackProvidersChanged() {
                        playbackTab.selectedIndex = -1
                        playbackTab.viewMode = "multi"
                        playbackTab.updateVisibleStreams()
                        appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                        playbackTab.reattachPreviewProviders()
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
                        border.color: sourceForView < 0 ? Theme.line : Theme.ready
                        border.width: 2
                        visible: playbackTab.viewMode === "single" && playbackTab.selectedIndex >= 0 && playbackTab.pgmProvider !== null

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

                            Component.onCompleted: attachProvider(playbackTab.pgmProvider)
                            Component.onDestruction: attachProvider(null)
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            anchors.margins: 6
                            color: Qt.rgba(0, 0, 0, 0.5)
                            width: singleViewLabel.implicitWidth + 12
                            height: singleViewLabel.implicitHeight + 6

                            Text {
                                id: singleViewLabel
                                anchors.centerIn: parent
                                text: {
                                    var src = singleView.sourceForView
                                    if (playbackTab.selectedIndex < 0) return ""
                                    if (src < 0) return "VIEW " + (playbackTab.selectedIndex + 1)
                                    return appWindow.uiManagerRef.sourceDisplayLabel(src)
                                }
                                color: "white"
                                font.family: "monospace"
                                font.pixelSize: 14
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                playbackTab.selectedIndex = -1
                                playbackTab.viewMode = "multi"
                                appWindow.uiManagerRef.setPlaybackViewState(false, -1)
                            }
                        }

                    }

                    VideoOutput {
                        id: multiviewBusOutput
                        anchors.fill: parent
                        fillMode: VideoOutput.PreserveAspectFit
                        visible: playbackTab.viewMode === "multi" && playbackTab.multiviewProvider !== null
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

                        Component.onCompleted: attachProvider(playbackTab.multiviewProvider)
                        Component.onDestruction: attachProvider(null)
                    }

                    GridView {
                        id: multiViewGrid
                        anchors.fill: parent
                        anchors.margins: 0
                        visible: playbackTab.viewMode === "multi"
                        z: 1
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
                            color: "transparent"
                            border.color: sourceForView < 0 ? Theme.line : Theme.ready
                            border.width: 2
                            width: multiViewGrid.cellWidth
                            height: multiViewGrid.cellHeight

                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.margins: 5
                                color: Qt.rgba(0, 0, 0, 0.5)
                                width: multiViewLabel.implicitWidth + 10
                                height: multiViewLabel.implicitHeight + 4
                                z: 5

                                Text {
                                    id: multiViewLabel
                                    anchors.centerIn: parent
                                    text: {
                                        var src = multiViewDelegate.sourceForView
                                        if (src < 0) return "VIEW " + (multiViewDelegate.streamIndex + 1)
                                        return appWindow.uiManagerRef.sourceDisplayLabel(src)
                                    }
                                    color: "white"
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                }
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

                GroupBox {
                    id: playbackTelemetryPanel
                    title: "Telemetry"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    visible: telemetryRows.length > 0
                    property var telemetryRows: appWindow.uiManagerRef.telemetryVersion >= 0
                                                ? appWindow.uiManagerRef.telemetryRowsAtPlayhead()
                                                : []

                    ScrollView {
                        anchors.fill: parent
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                        ColumnLayout {
                            width: parent.width
                            spacing: 6

                            Repeater {
                                model: playbackTelemetryPanel.telemetryRows

                                delegate: RowLayout {
                                    id: telemetryRow
                                    required property var modelData
                                    width: parent.width
                                    spacing: 10

                                    Text {
                                        text: {
                                            var name = telemetryRow.modelData.feedName || ""
                                            var idText = telemetryRow.modelData.feedId || ""
                                            return name.length > 0 ? (idText + " " + name) : idText
                                        }
                                        color: "#eeeeee"
                                        font.bold: true
                                        elide: Text.ElideRight
                                        Layout.preferredWidth: 180
                                    }

                                    Text {
                                        text: telemetryRow.modelData.summary || ""
                                        color: "#b0b0b0"
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
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
                    // While the user is dragging, hold the handle at their drag
                    // position; otherwise follow the playhead. Without the
                    // pressed guard, scrubPositionChanged (~30/s while
                    // recording) yanks the handle back mid-drag.
                    value: scrubBar.pressed ? scrubBar.value : appWindow.uiManagerRef.scrubPosition

                    onMoved: {
                        appWindow.uiManagerRef.seekPlayback(value)
                    }
                    onPressedChanged: {
                        // On release (pressed → false) flush the final scrub
                        // target and end the coalesce gesture.
                        if (!scrubBar.pressed) {
                            appWindow.uiManagerRef.endScrubGesture()
                        }
                    }

                    background: Rectangle {
                        height: 6
                        radius: 3
                        color: "#333333"
                        Rectangle {
                            width: scrubBar.visualPosition * parent.width
                            height: parent.height
                            color: Theme.accent
                            radius: 3
                        }
                    }

                    handle: Rectangle {
                        implicitWidth: 0
                        implicitHeight: 0
                        width: 0
                        height: 0
                        visible: false
                    }
                }

                // Tier3 replay cue list: mark in/out at the playhead, recall as a
                // frame-perfect armed cut (pre-rolled, no flash). "Play Playlist"
                // runs the rundown — it auto-advances across each entry boundary with
                // a frame-perfect cut, honoring per-entry speed; a manual scrub or
                // recall exits playout.
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    Button { text: "Mark In";  onClicked: appWindow.uiManagerRef.markIn() }
                    Button { text: "Mark Out"; onClicked: appWindow.uiManagerRef.markOut() }
                    Button { text: "Recall 0"; onClicked: appWindow.uiManagerRef.recallEntry(0) }
                    Button {
                        text: "Play Playlist"
                        onClicked: appWindow.uiManagerRef.playPlaylist(0)
                    }
                    Button {
                        text: "Stop Playout"
                        onClicked: appWindow.uiManagerRef.stopPlaylistPlayout()
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    spacing: 12

                    Text {
                        // Single source of truth (UIManager): the same string the
                        // Stream Deck shows. Do not reformat here.
                        text: appWindow.uiManagerRef.playbackTimecode
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
                        text: appWindow.uiManagerRef.transport.isPlaying ? "PAUSE" : "PLAY"
                        onClicked: appWindow.uiManagerRef.playPause()
                        highlighted: appWindow.uiManagerRef.transport.isPlaying
                    }

                    Button {
                        text: "-5.0x"
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
                        text: "<"
                        onClicked: appWindow.uiManagerRef.stepFrameBack()
                    }

                    Button {
                        text: ">"
                        onClicked: appWindow.uiManagerRef.stepFrame()
                    }

                    Button {
                        text: "0.25x"
                        onPressed: {
                            appWindow.uiManagerRef.cancelFollowLive()
                            playbackTab.holdWasPlaying = appWindow.uiManagerRef.transport.isPlaying
                            appWindow.uiManagerRef.transport.setSpeed(0.25)
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
                        text: "0.5x"
                        onPressed: {
                            appWindow.uiManagerRef.cancelFollowLive()
                            playbackTab.holdWasPlaying = appWindow.uiManagerRef.transport.isPlaying
                            appWindow.uiManagerRef.transport.setSpeed(0.5)
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
                        text: "2.0x"
                        onPressed: {
                            appWindow.uiManagerRef.cancelFollowLive()
                            playbackTab.holdWasPlaying = appWindow.uiManagerRef.transport.isPlaying
                            appWindow.uiManagerRef.transport.setSpeed(2.0)
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
                        text: "5.0x"
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
                        text: "Live"
                        onClicked: {
                            appWindow.uiManagerRef.goLive()
                            appWindow.uiManagerRef.transport.setSpeed(1.0)
                            appWindow.uiManagerRef.transport.setPlaying(true)
                        }
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

            // --- Project Tab --- (scrollable: its stacked settings exceed any normal
            // window height; the horizontal scrollbar reaches wide source/NDI rows.)
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AsNeeded

                ColumnLayout {
                    id: projectTab
                    spacing: 12
                    width: parent.width

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

                    ComboBox {
                        Layout.fillWidth: true
                        textRole: "label"
                        model: ListModel {
                            ListElement { label: "23.976"; num: 24000; den: 1001 }
                            ListElement { label: "24"; num: 24; den: 1 }
                            ListElement { label: "25"; num: 25; den: 1 }
                            ListElement { label: "29.97"; num: 30000; den: 1001 }
                            ListElement { label: "30"; num: 30; den: 1 }
                            ListElement { label: "50"; num: 50; den: 1 }
                            ListElement { label: "59.94"; num: 60000; den: 1001 }
                            ListElement { label: "60"; num: 60; den: 1 }
                            ListElement { label: "120"; num: 120; den: 1 }
                        }
                        currentIndex: {
                            for (var i = 0; i < model.count; ++i) {
                                var preset = model.get(i)
                                if (preset.num === appWindow.uiManagerRef.recordFpsNumerator
                                        && preset.den === appWindow.uiManagerRef.recordFpsDenominator) {
                                    return i
                                }
                            }
                            return -1
                        }
                        displayText: currentIndex >= 0 ? currentText : appWindow.uiManagerRef.recordFps + " fps"
                        enabled: !appWindow.uiManagerRef.isRecording
                        onActivated: {
                            var preset = model.get(currentIndex)
                            appWindow.uiManagerRef.setRecordFrameRate(preset.num, preset.den)
                        }
                    }

                    Item { }
                    Item { }
                    Item { }

                    Label { text: "Audio output latency (ms)"; }

                    SpinBox {
                        from: 0
                        to: 500
                        stepSize: 10
                        editable: true
                        inputMethodHints: Qt.ImhDigitsOnly
                        value: appWindow.uiManagerRef.audioOutputLatencyMs
                        onValueModified: appWindow.uiManagerRef.audioOutputLatencyMs = value
                        ToolTip.visible: hovered
                        ToolTip.text: "Compensate audio-output device delay (HDMI/Bluetooth). Raise until lip-sync is correct on this output."
                    }

                    Item { }
                    Item { }
                    Item { }
                }

                CodecSettingsPanel {
                    Layout.fillWidth: true
                    controller: appWindow.uiManagerRef
                }

                GroupBox {
                    id: ndiOutputSettings
                    title: "NDI Outputs"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(320, Math.max(150, 48 + (rows.length + 1) * 44))
                    property var rows: appWindow.uiManagerRef.broadcastOutputsVersion >= 0
                                       ? appWindow.uiManagerRef.ndiOutputRows()
                                       : []
                    function statusColor(severity) {
                        if (severity === "ok")
                            return "#2e7d32"
                        if (severity === "warning")
                            return "#f9a825"
                        if (severity === "error")
                            return "#d32f2f"
                        return "#666"
                    }

                    ScrollView {
                        id: ndiOutputScroll
                        anchors.fill: parent
                        clip: true
                        contentWidth: Math.max(availableWidth, ndiOutputTable.implicitWidth)
                        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded

                        ColumnLayout {
                            id: ndiOutputTable
                            width: Math.max(ndiOutputScroll.availableWidth, implicitWidth)
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 744
                                spacing: 8

                                Label {
                                    text: "Bus"
                                    Layout.preferredWidth: 92
                                    font.bold: true
                                }

                                Label {
                                    text: "Output"
                                    Layout.preferredWidth: 56
                                    font.bold: true
                                }

                                Label {
                                    text: "Name"
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 160
                                    font.bold: true
                                }

                                Label {
                                    text: "State"
                                    Layout.preferredWidth: 112
                                    font.bold: true
                                }

                                Label {
                                    text: "Frames"
                                    Layout.preferredWidth: 96
                                    font.bold: true
                                }

                                Label {
                                    text: "Health"
                                    Layout.preferredWidth: 124
                                    font.bold: true
                                }

                                Label {
                                    text: "Last"
                                    Layout.preferredWidth: 72
                                    font.bold: true
                                }
                            }

                            Repeater {
                                model: ndiOutputSettings.rows

                                delegate: RowLayout {
                                    id: ndiOutputRow
                                    required property var modelData
                                    property var statusData: appWindow.uiManagerRef.broadcastOutputStatusVersion >= 0
                                                             ? appWindow.uiManagerRef.ndiOutputStatus(modelData.id)
                                                             : ({})
                                    readonly property bool hasSinkStatus: !!statusData.hasSinkStatus
                                    readonly property var displayedFrames: hasSinkStatus
                                                                         ? (statusData.sinkSubmittedFrames || 0)
                                                                         : (statusData.framesSubmitted || 0)
                                    width: ndiOutputTable.width
                                    Layout.minimumWidth: 744
                                    spacing: 8

                                    Label {
                                        text: ndiOutputRow.modelData.label || ""
                                        Layout.preferredWidth: 92
                                        elide: Text.ElideRight
                                    }

                                    Switch {
                                        id: ndiOutputSwitch
                                        Layout.preferredWidth: 56
                                        checked: !!ndiOutputRow.modelData.enabled
                                        onToggled: appWindow.uiManagerRef.setNdiOutputEnabled(
                                                       ndiOutputRow.modelData.busKind,
                                                       ndiOutputRow.modelData.feedIndex,
                                                       checked)
                                    }

                                    TextField {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 160
                                        enabled: ndiOutputSwitch.checked
                                        text: ndiOutputRow.modelData.senderName || ""
                                        selectByMouse: true
                                        onEditingFinished: appWindow.uiManagerRef.setNdiOutputSenderName(
                                                               ndiOutputRow.modelData.busKind,
                                                               ndiOutputRow.modelData.feedIndex,
                                                               text)
                                    }

                                    RowLayout {
                                        Layout.preferredWidth: 112
                                        spacing: 6

                                        Rectangle {
                                            id: outputStatusDot
                                            Layout.preferredWidth: 12
                                            Layout.preferredHeight: 12
                                            radius: width / 2
                                            color: ndiOutputSettings.statusColor(
                                                       ndiOutputRow.statusData.statusSeverity || "off")
                                            HoverHandler { id: outputStatusHover }
                                            ToolTip.visible: outputStatusHover.hovered
                                            ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: ndiOutputRow.statusData.statusState || "Off"
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        Layout.preferredWidth: 96
                                        text: ndiOutputRow.displayedFrames
                                              + "/" + (ndiOutputRow.statusData.attemptedFrames || 0)
                                        horizontalAlignment: Text.AlignRight
                                        HoverHandler { id: outputFramesHover }
                                        ToolTip.visible: outputFramesHover.hovered
                                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                                    }

                                    Label {
                                        Layout.preferredWidth: 124
                                        text: "F " + (ndiOutputRow.statusData.sinkFailures || 0)
                                              + " SF " + (ndiOutputRow.statusData.sinkFailedFrames || 0)
                                              + " D " + (ndiOutputRow.statusData.sinkDroppedFrames || 0)
                                              + " Q " + (ndiOutputRow.statusData.currentQueueDepth || 0)
                                              + "/" + (ndiOutputRow.statusData.maxQueueDepth || 0)
                                              + " G " + (ndiOutputRow.statusData.deliveryGaps || 0)
                                        elide: Text.ElideRight
                                        HoverHandler { id: outputHealthHover }
                                        ToolTip.visible: outputHealthHover.hovered
                                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                                    }

                                    Label {
                                        Layout.preferredWidth: 72
                                        text: ndiOutputRow.statusData.lastOutputFrameIndex >= 0
                                              ? ("#" + ndiOutputRow.statusData.lastOutputFrameIndex)
                                              : "-"
                                        horizontalAlignment: Text.AlignRight
                                        HoverHandler { id: outputLastHover }
                                        ToolTip.visible: outputLastHover.hovered
                                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                                    }
                                }
                            }
                        }
                    }
                }

                GroupBox {
                    title: "External Input Settings"
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            TextField {
                                id: importSettingsUrlField
                                Layout.fillWidth: true
                                text: appWindow.uiManagerRef.importSettingsUrl
                                placeholderText: "https://provider.example/project-settings.json"
                                enabled: !appWindow.uiManagerRef.isRecording
                                onEditingFinished: appWindow.uiManagerRef.importSettingsUrl = text
                            }

                            Button {
                                text: "Read Settings"
                                enabled: !appWindow.uiManagerRef.isRecording
                                onClicked: {
                                    appWindow.uiManagerRef.importSettingsUrl = importSettingsUrlField.text
                                    appWindow.uiManagerRef.readImportSettings()
                                }
                            }
                        }

                        Text {
                            visible: appWindow.uiManagerRef.importPreviewError !== ""
                            text: appWindow.uiManagerRef.importPreviewError
                            color: "#ff9800"
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                visible: appWindow.uiManagerRef.importPreviewReady
                                text: "Preview Imported Sources"
                                enabled: appWindow.uiManagerRef.importPreviewReady
                                onClicked: importPreviewPopup.open()
                            }

                            Text {
                                visible: appWindow.uiManagerRef.importPreviewReady
                                text: {
                                    var p = appWindow.uiManagerRef.importPreview
                                    var count = p.feedCount !== undefined ? p.feedCount : ((p.feeds || []).length)
                                    return count + " imported feed" + (count === 1 ? "" : "s") + " ready"
                                }
                                color: "#8bc34a"
                                Layout.alignment: Qt.AlignVCenter
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }
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
                        text: "Metadata Fields"
                        onClicked: metadataFieldsEditor.openEditor()
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

                        // Live connection indicator: grey when idle, green
                        // once the source's feed is up, red while recording
                        // but the feed has not connected (or has dropped).
                        Rectangle {
                            id: connDot
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            radius: width / 2
                            property bool connected: appWindow.uiManagerRef.sourceConnectionVersion >= 0
                                                     && appWindow.uiManagerRef.isSourceConnected(streamRow.index)
                            // 0=N/A,1=green,2=amber,3=red (native SRT only; else 0)
                            property int linkHealth: appWindow.uiManagerRef.sourceStatsVersion >= 0
                                                     ? appWindow.uiManagerRef.sourceLinkHealth(streamRow.index)
                                                     : 0
                            color: !appWindow.uiManagerRef.isRecording
                                   ? "#555"
                                   : (!connDot.connected
                                      ? "#d32f2f"
                                      : (connDot.linkHealth === 3 ? "#d32f2f"
                                         : connDot.linkHealth === 2 ? "#f9a825"
                                         : "#2e7d32"))
                            HoverHandler { id: connHover }
                            ToolTip.visible: connHover.hovered
                            ToolTip.text: !appWindow.uiManagerRef.isRecording
                                          ? "Not recording"
                                          : (!connDot.connected
                                             ? "No signal"
                                             : (appWindow.uiManagerRef.sourceStatsVersion >= 0
                                                && appWindow.uiManagerRef.sourceHasStats(streamRow.index)
                                                ? appWindow.uiManagerRef.sourceStatsTooltip(streamRow.index)
                                                : "Connected"))
                        }

                        SpinBox {
                            id: trimSpin
                            from: -500
                            to: 500
                            stepSize: 33   // ≈ 1 frame @30fps; stored value is ms
                            editable: true
                            Layout.preferredWidth: 96
                            value: appWindow.uiManagerRef.sourceTrimVersion >= 0
                                   ? appWindow.uiManagerRef.sourceTrimOffset(streamRow.index) : 0
                            onValueModified: appWindow.uiManagerRef.setSourceTrimOffset(streamRow.index, value)
                            ToolTip.visible: hovered
                            ToolTip.text: "Timeline trim (ms): + delays this camera, − advances it"
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

                        Button {
                            text: "Metadata"
                            Layout.preferredWidth: 90
                            onClicked: metadataEditor.openFor(streamRow.index)
                        }

                        TextField {
                            Layout.fillWidth: true
                            text: streamRow.modelData
                            placeholderText: "rtmp://..."
                            onEditingFinished: appWindow.uiManagerRef.updateUrl(streamRow.index, text)
                        }

                        // Misconfiguration warning: another source points at
                        // this same URL (two workers pulling one stream).
                        // Reading streamUrls keeps the binding reactive to edits.
                        Label {
                            text: "⚠"
                            color: "#ff9800"
                            font.bold: true
                            Layout.preferredWidth: 16
                            visible: appWindow.uiManagerRef.streamUrls.length >= 0
                                     && appWindow.uiManagerRef.hasDuplicateUrl(streamRow.index)
                            HoverHandler { id: dupHover }
                            ToolTip.visible: dupHover.hovered
                            ToolTip.text: "Duplicate URL — another source uses this same stream"
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

                // ─── Imported Input Settings Preview ───
                Popup {
                    id: importPreviewPopup
                    modal: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    // A Popup is shown in Overlay.overlay and is not laid out by
                    // the enclosing Layout, so this geometry/anchors are correct
                    // (the layout-positioning check is a false positive here).
                    // qmllint disable Quick.layout-positioning
                    anchors.centerIn: Overlay.overlay
                    width: Math.min(appWindow.width * 0.92, 760)
                    height: Math.min(appWindow.height * 0.84, 580)
                    // qmllint enable Quick.layout-positioning

                    function metadataSummary(metadata) {
                        if (!metadata || metadata.length === 0) return "metadata 0"

                        var parts = []
                        for (var i = 0; i < metadata.length && i < 3; ++i) {
                            var row = metadata[i] || {}
                            var name = row.name || ""
                            var value = row.value !== undefined ? String(row.value) : ""
                            if (name.length === 0 && value.length === 0) continue
                            parts.push(value.length > 0 ? (name + "=" + value) : name)
                        }

                        var suffix = metadata.length > parts.length ? " +" + (metadata.length - parts.length) : ""
                        return "metadata " + metadata.length + (parts.length > 0 ? ": " + parts.join(", ") + suffix : "")
                    }

                    contentItem: Rectangle {
                        color: "#1f1f1f"
                        radius: 0
                        border.color: "#333"
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            Text {
                                text: "Imported Input Settings"
                                color: "#eeeeee"
                                font.pixelSize: 18
                                font.bold: true
                                Layout.fillWidth: true
                            }

                            GridLayout {
                                columns: 2
                                Layout.fillWidth: true
                                columnSpacing: 14
                                rowSpacing: 4

                                Label {
                                    text: "Project"
                                    color: "#999999"
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: {
                                        var p = appWindow.uiManagerRef.importPreview
                                        var name = p.projectName || "Imported project"
                                        var idText = p.projectId ? " (" + p.projectId + ")" : ""
                                        return name + idText
                                    }
                                    color: "#eeeeee"
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: "Feeds"
                                    color: "#999999"
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: {
                                        var p = appWindow.uiManagerRef.importPreview
                                        return p.feedCount !== undefined ? p.feedCount : ((p.feeds || []).length)
                                    }
                                    color: "#eeeeee"
                                }

                                Label {
                                    text: "Telemetry SSE"
                                    color: "#999999"
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: appWindow.uiManagerRef.importPreview.telemetrySseUrl || ""
                                    color: "#b0b0b0"
                                    elide: Text.ElideMiddle
                                }
                            }

                            Text {
                                text: "Applying will replace the current input sources."
                                color: "#ffcc80"
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                contentWidth: availableWidth
                                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                                ListView {
                                    id: importFeedsList
                                    width: parent.width
                                    model: appWindow.uiManagerRef.importPreview.feeds || []
                                    spacing: 8
                                    boundsBehavior: Flickable.StopAtBounds

                                    delegate: Rectangle {
                                        id: importFeedRow
                                        required property var modelData
                                        width: importFeedsList.width
                                        height: feedLayout.implicitHeight + 16
                                        color: "#181818"
                                        border.color: "#333"
                                        border.width: 1
                                        radius: 4

                                        ColumnLayout {
                                            id: feedLayout
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 4

                                            Text {
                                                Layout.fillWidth: true
                                                text: {
                                                    var name = importFeedRow.modelData.name || "Unnamed feed"
                                                    var idText = importFeedRow.modelData.id || ""
                                                    return idText.length > 0 ? (name + " · " + idText) : name
                                                }
                                                color: "#eeeeee"
                                                font.bold: true
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: importFeedRow.modelData.url || ""
                                                color: "#aaaaaa"
                                                elide: Text.ElideMiddle
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: "telemetryDelayMs " + (importFeedRow.modelData.telemetryDelayMs || 0)
                                                color: "#888888"
                                                elide: Text.ElideRight
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: importPreviewPopup.metadataSummary(importFeedRow.modelData.metadata)
                                                color: "#777777"
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Item { Layout.fillWidth: true }

                                Button {
                                    text: "Cancel"
                                    onClicked: importPreviewPopup.close()
                                }

                                Button {
                                    text: "Apply"
                                    enabled: appWindow.uiManagerRef.importPreviewReady
                                             && !appWindow.uiManagerRef.isRecording
                                    onClicked: {
                                        appWindow.uiManagerRef.applyImportPreview()
                                        importPreviewPopup.close()
                                    }
                                }
                            }
                        }
                    }
                }

                // ─── Global Metadata Fields Editor ───
                Popup {
                    id: metadataFieldsEditor
                    modal: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    // A Popup is shown in Overlay.overlay and is not laid out by
                    // the enclosing Layout, so this geometry/anchors are correct
                    // (the layout-positioning check is a false positive here).
                    // qmllint disable Quick.layout-positioning
                    anchors.centerIn: Overlay.overlay
                    width: Math.min(appWindow.width * 0.9, 600)
                    height: Math.min(appWindow.height * 0.8, 480)
                    // qmllint enable Quick.layout-positioning

                    ListModel {
                        id: fieldsModel
                    }

                    function openEditor() {
                        fieldsModel.clear()
                        var defs = appWindow.uiManagerRef.metadataFieldDefinitions()
                        if (defs && defs.length > 0) {
                            for (var i = 0; i < defs.length; ++i) {
                                var d = defs[i]
                                fieldsModel.append({
                                    name: d.name || "",
                                    display: d.display !== undefined ? d.display : true
                                })
                            }
                        } else {
                            fieldsModel.append({ name: "", display: true })
                        }
                        open()
                    }

                    function buildFields() {
                        var result = []
                        for (var i = 0; i < fieldsModel.count; ++i) {
                            var row = fieldsModel.get(i)
                            var name = (row.name || "").trim()
                            if (name.length === 0) continue
                            result.push({
                                name: name,
                                display: row.display !== undefined ? row.display : true
                            })
                        }
                        return result
                    }

                    contentItem: Rectangle {
                        color: "#1f1f1f"
                        radius: 0
                        border.color: "#333"
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Layout.fillWidth: true

                                Text {
                                    text: "Metadata Field Definitions"
                                    color: "#eeeeee"
                                    font.pixelSize: 18
                                    font.bold: true
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: "+ Field"
                                    onClicked: fieldsModel.append({ name: "", display: true })
                                }
                            }

                            Text {
                                text: "Define the metadata fields available for all sources. Each source can then fill in its own values."
                                color: "#b0b0b0"
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }

                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true

                                ListView {
                                    id: fieldsList
                                    model: fieldsModel
                                    spacing: 8
                                    boundsBehavior: Flickable.StopAtBounds

                                    delegate: RowLayout {
                                        id: fieldRow
                                        required property string name
                                        required property bool display
                                        required property int index
                                        width: fieldsList.width
                                        spacing: 8

                                        TextField {
                                            Layout.fillWidth: true
                                            placeholderText: "Field name"
                                            text: fieldRow.name
                                            onTextEdited: fieldsModel.setProperty(fieldRow.index, "name", text)
                                        }

                                        CheckBox {
                                            text: "Show"
                                            checked: fieldRow.display
                                            onToggled: fieldsModel.setProperty(fieldRow.index, "display", checked)
                                        }

                                        Button {
                                            text: "Remove"
                                            onClicked: fieldsModel.remove(fieldRow.index)
                                            enabled: fieldsModel.count > 1
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Item { Layout.fillWidth: true }

                                Button {
                                    text: "Cancel"
                                    onClicked: metadataFieldsEditor.close()
                                }

                                Button {
                                    text: "Save"
                                    onClicked: {
                                        appWindow.uiManagerRef.setMetadataFieldDefinitions(
                                            metadataFieldsEditor.buildFields()
                                        )
                                        appWindow.uiManagerRef.saveSettings()
                                        metadataFieldsEditor.close()
                                    }
                                }
                            }
                        }
                    }
                }

                // ─── Per-Source Metadata Values Editor ───
                Popup {
                    id: metadataEditor
                    modal: true
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    // A Popup is shown in Overlay.overlay and is not laid out by
                    // the enclosing Layout, so this geometry/anchors are correct
                    // (the layout-positioning check is a false positive here).
                    // qmllint disable Quick.layout-positioning
                    anchors.centerIn: Overlay.overlay
                    width: Math.min(appWindow.width * 0.9, 600)
                    height: Math.min(appWindow.height * 0.8, 480)
                    // qmllint enable Quick.layout-positioning

                    property int sourceIndex: -1
                    property string sourceLabel: ""

                    ListModel {
                        id: metadataModel
                    }

                    function openFor(index) {
                        sourceIndex = index
                        var label = "Source " + (index + 1)
                        if (appWindow.uiManagerRef.streamNames.length > index
                                && appWindow.uiManagerRef.streamNames[index].length > 0) {
                            label = appWindow.uiManagerRef.streamNames[index]
                        }
                        sourceLabel = label
                        metadataModel.clear()
                        var items = appWindow.uiManagerRef.sourceMetadataItems(index)
                        if (items && items.length > 0) {
                            for (var i = 0; i < items.length; ++i) {
                                var row = items[i]
                                metadataModel.append({
                                    name: row.name || "",
                                    value: row.value || ""
                                })
                            }
                        }
                        open()
                    }

                    function buildItems() {
                        var result = []
                        for (var i = 0; i < metadataModel.count; ++i) {
                            var row = metadataModel.get(i)
                            var name = (row.name || "").trim()
                            if (name.length === 0) continue
                            result.push({
                                name: name,
                                value: row.value === undefined ? "" : String(row.value)
                            })
                        }
                        return result
                    }

                    contentItem: Rectangle {
                        color: "#1f1f1f"
                        radius: 0
                        border.color: "#333"
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            Text {
                                text: "Metadata — " + metadataEditor.sourceLabel
                                color: "#eeeeee"
                                font.pixelSize: 18
                                font.bold: true
                                Layout.fillWidth: true
                            }

                            Text {
                                text: metadataModel.count > 0
                                      ? "Fill in values for this source. Fields are defined in Metadata Fields."
                                      : "No metadata fields defined. Use the Metadata Fields button to add fields first."
                                color: "#b0b0b0"
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }

                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true

                                ListView {
                                    id: metadataList
                                    model: metadataModel
                                    spacing: 8
                                    boundsBehavior: Flickable.StopAtBounds

                                    delegate: RowLayout {
                                        id: metaRow
                                        required property string name
                                        required property string value
                                        required property int index
                                        width: metadataList.width
                                        spacing: 8

                                        Label {
                                            text: metaRow.name
                                            Layout.preferredWidth: 180
                                            color: "#cccccc"
                                            font.bold: true
                                            elide: Text.ElideRight
                                        }

                                        TextField {
                                            Layout.fillWidth: true
                                            placeholderText: "Value"
                                            text: metaRow.value
                                            onTextEdited: metadataModel.setProperty(metaRow.index, "value", text)
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Item { Layout.fillWidth: true }

                                Button {
                                    text: "Cancel"
                                    onClicked: metadataEditor.close()
                                }

                                Button {
                                    text: "Save"
                                    onClicked: {
                                        if (metadataEditor.sourceIndex >= 0) {
                                            appWindow.uiManagerRef.setSourceMetadataItems(
                                                metadataEditor.sourceIndex,
                                                metadataEditor.buildItems()
                                            )
                                            appWindow.uiManagerRef.saveSettings()
                                        }
                                        metadataEditor.close()
                                    }
                                }
                            }
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
            } // ScrollView (Project tab)
        }
    }
}
