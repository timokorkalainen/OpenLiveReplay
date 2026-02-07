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
                spacing: 12
                Layout.fillWidth: true
                Layout.fillHeight: true

                GridLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    anchors.margins: 2
                    columns: Math.ceil(Math.sqrt(uiManager.playbackProviders.length))

                    Repeater {
                        model: uiManager.playbackProviders

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "black"
                            border.color: "red"
                            border.width: 2

                            VideoOutput {
                                id: vOutput
                                anchors.fill: parent
                                fillMode: VideoOutput.PreserveAspectFit
                                Component.onCompleted: {
                                    modelData.videoSink = vOutput.videoSink
                                }
                            }

                            Text {
                                text: "CAM " + (index + 1)
                                color: "white"
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                anchors.margins: 5
                                font.family: "Monospace"
                                font.pixelSize: 12
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
