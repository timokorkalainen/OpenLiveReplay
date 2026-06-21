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
                    return pgmStage.visibleStreamIndexes
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
            if (sourceListPanel) sourceListPanel.maybeAutoOpenImport()
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

                    BindingsPanel {
                        Layout.fillWidth: true
                        ui: appWindow.uiManagerRef
                    }
                }
            }

            // --- Playback Tab ---
            ColumnLayout {
                id: playbackTab
                spacing: 12
                Layout.fillWidth: true
                Layout.fillHeight: true

                PgmStage {
                    id: pgmStage
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ui: appWindow.uiManagerRef
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

                TransportDock {
                    Layout.fillWidth: true
                    ui: appWindow.uiManagerRef
                }
            }

            // --- Project Tab --- (vertically scrollable: its stacked settings exceed any
            // normal window height. Content is pinned to the viewport width, so wide blocks
            // — the NDI Outputs table, source rows — scroll within their own inner views.)
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    id: projectTab
                    spacing: 12
                    width: parent.width

                    ProjectSettingsPanel {
                        Layout.fillWidth: true
                        ui: appWindow.uiManagerRef
                        onBrowseFolderRequested: folderDialog.open()
                    }

                    OutputsPanel {
                        Layout.fillWidth: true
                        ui: appWindow.uiManagerRef
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
                                onClicked: sourceListPanel.openImportPreview()
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

                SourceListPanel {
                    id: sourceListPanel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ui: appWindow.uiManagerRef
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
