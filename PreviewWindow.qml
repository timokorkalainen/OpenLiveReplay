import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtMultimedia
import Recorder.Types 1.0

Window {
    id: previewWindow
    width: 1280; height: 720
    visible: uiManager.isRecording // Open automatically when recording
    color: "black"

    // FORCE THE THEME HERE
    //Universal.theme: Universal.Dark
    //Universal.accent: Universal.Red

    // THIS MUST BE AT THE TOP LEVEL
    function refresh() {
        console.log("PreviewWindow: Refreshing player...")
        // If you are using our custom PlaybackWorker,
        // you might not even need this, but to fix the error:
    }

    ColumnLayout {
        anchors.fill: parent

        GridLayout {
            Layout.fillWidth: true
            anchors.margins: 2
            columns: Math.ceil(Math.sqrt(uiManager.playbackProviders.length));

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
                            // We "give" the VideoOutput's sink to our C++ provider
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

            // This binding makes the slider handle move as the video plays
            value: uiManager.scrubPosition

            // When the user starts dragging, we update the Transport
            onMoved: {
                uiManager.transport.seek(value)
            }

            // Custom styling for a "pro" look
            background: Rectangle {
                height: 6
                radius: 3
                color: "#333"
                Rectangle {
                    width: scrubBar.visualPosition * parent.width
                    height: parent.height
                    color: "#007AFF" // iOS/Mac Blue
                    radius: 3
                }
            }
        }

        // --- The Transport Controls ---
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

        function formatTime(ms) {
            let totalSeconds = Math.floor(ms / 1000);
            let minutes = Math.floor(totalSeconds / 60);
            let seconds = totalSeconds % 60;
            return minutes + ":" + (seconds < 10 ? "0" : "") + seconds;
        }
    }
}
