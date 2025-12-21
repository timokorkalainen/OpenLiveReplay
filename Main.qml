import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform 1.1
import QtQuick.Controls.Universal 2.15 // Necessary for attached properties
import Recorder.Types 1.0

ApplicationWindow {
    visible: true
    width: 800
    height: 600
    title: "OpenLiveReplay"

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
        anchors.margins: 20
        spacing: 15

        Text {
            text: "Session Settings"
            font.bold: true
            color: "#eee"
        }

        GridLayout {
            columns: 3 // Changed to 3 to fit the Browse button
            Layout.fillWidth: true


            Label { text: "Project Name"; }

            TextField {
                Layout.fillWidth: true
                Layout.columnSpan: 2 // Span across to the end
                text: uiManager.fileName
                onEditingFinished: uiManager.fileName = text
            }

            Label { text: "Save Location"; }

            TextField {
                id: pathField
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

        // --- Stream URLs Header with Add Button ---
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
                width: streamList.width
                spacing: 10

                Label {
                    text: (index + 1) + ":"
                    width: 20
                }

                TextField {
                    Layout.fillWidth: true
                    text: modelData
                    placeholderText: "rtmp://..."
                    // We use the specific updateUrl method to handle C++ indexing
                    onEditingFinished: uiManager.updateUrl(index, text)
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

        // --- Bottom Action Bar ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                text: "Save Config"
                onClicked: uiManager.saveSettings()
            }

            Item { Layout.fillWidth: true } // Spacer

            Button {
                id: recordButton
                text: uiManager.isRecording ? "STOP RECORDING" : "START RECORDING"
                padding: 15

                // Red/Green Toggle Logic
                background: Rectangle {
                    color: uiManager.isRecording ? "#d32f2f" : "#2e7d32"
                }

                contentItem: Text {
                    text: recordButton.text
                }

                onClicked: uiManager.isRecording ? uiManager.stopRecording() : uiManager.startRecording()
            }
        }

        Text {
            text: uiManager.isRecording ? "● RECORDING LIVE" : "IDLE"
            color: uiManager.isRecording ? "#ff5252" : "#666"
        }
    }

    PreviewWindow {
        id: monitorWindow
    }

    Connections {
        target: uiManager

        // Signal is "recordingStarted", so handler is "onRecordingStarted"
        function onRecordingStarted() {
            console.log("UI: Recording signal received");
            monitorWindow.visible = true;
            startTimer.start();
        }

        // Signal is "recordingStopped", so handler is "onRecordingStopped"
        function onRecordingStopped() {
            console.log("UI: Stop signal received");
            // monitorWindow.visible = false; // Optional: keep it open to review
        }
    }

    Timer {
        id: startTimer
        interval: 500
        onTriggered: monitorWindow.refresh()
    }
}
