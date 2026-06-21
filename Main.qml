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
            configDrawer.maybeAutoOpenImport()
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
        spacing: 0

        StatusStrip {
            id: statusStrip

            Layout.fillWidth: true

            ui: appWindow.uiManagerRef
            configOpen: configDrawer.opened
            recordingError: appWindow.recordingError
            onToggleConfig: configDrawer.opened ? configDrawer.close() : configDrawer.open()
            onFullscreenMultiviewRequested: (x, y) => {
                appWindow.refreshScreenOptions()
                screenMenu.x = x
                screenMenu.y = y
                screenMenu.open()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: warningText.implicitHeight + Theme.s2 * 2
            visible: appWindow.recordingWarningText !== ""
            color: "#2b2615"
            border.color: "#5b4818"
            border.width: 1

            Text {
                id: warningText

                anchors.fill: parent
                anchors.margins: Theme.s2
                text: "Warning: " + appWindow.recordingWarningText
                color: "#ffb300"
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
            }
        }

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

    ConfigDrawer {
        id: configDrawer

        ui: appWindow.uiManagerRef
        parent: Overlay.overlay
        onBrowseFolderRequested: folderDialog.open()
    }
}
