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

    // THIS MUST BE AT THE TOP LEVEL
    function refresh() {
        console.log("PreviewWindow: Refreshing player...")
        // If you are using our custom PlaybackWorker,
        // you might not even need this, but to fix the error:
    }

    GridLayout {
        anchors.fill: parent
        anchors.margins: 2
        columns: 2 // You can make this dynamic: Math.ceil(Math.sqrt(uiManager.playbackProviders.length))

        Repeater {
            model: uiManager.playbackProviders

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "black"
                border.color: "#333"
                border.width: 1

                VideoOutput {
                    id: vOutput
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    Component.onCompleted: {
                        console.log("Connecting Sink for Track " + index)
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
}
