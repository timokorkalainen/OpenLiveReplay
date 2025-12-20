import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    width: 800
    height: 600
    visible: true
    title: "Multi-Track Replay System"
    color: "#1e1e1e" // Professional dark theme

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 30; spacing: 20

        // 1. TRACK COUNT SELECTOR
        RowLayout {
            Label { text: "NUMBER OF TRACKS:"; color: "white"; font.bold: true }
            SpinBox {
                id: countSelector
                from: 1; to: 16
                value: 4
                enabled: !replayManager.isRecording // LOCK during recording
                onValueModified: replayManager.setTrackCount(value)
            }
        }

        // 2. DYNAMIC INPUT LIST
        ScrollView {
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: 10

                Repeater {
                    model: replayManager.trackUrls

                    delegate: RowLayout {
                        width: parent.width
                        spacing: 10

                        Rectangle {
                            width: 50; height: 35; color: "#222"; radius: 4
                            Text { anchors.centerIn: parent; text: "CH" + (index + 1); color: "white"; font.bold: true }
                        }

                        TextField {
                            id: urlInput
                            Layout.fillWidth: true
                            placeholderText: "Stream URL..."
                            text: modelData
                            color: "white"
                            font.family: "Monospace"
                            background: Rectangle { color: "#1a1a1a"; border.color: "#333"; radius: 4 }
                        }

                        Button {
                            text: "APPLY"
                            // Layout hint for professional look
                            implicitWidth: 80
                            implicitHeight: 35

                            // Visual feedback: Highlight if the URL in the box differs from what ReplayManager has
                            palette.buttonText: urlInput.text !== modelData ? "#4CAF50" : "white"

                            onClicked: {
                                replayManager.applyTrackSource(index, urlInput.text)
                                urlInput.focus = false // Clear focus to show it was accepted
                            }
                        }

                        // Status Indicator
                        Rectangle {
                            width: 10; height: 10; radius: 5
                            color: (replayManager.isRecording && urlInput.text === modelData && urlInput.text !== "") ? "#4CAF50" : "#333"
                            ToolTip.visible: ma.containsMouse
                            ToolTip.text: "Active stream"
                            MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true }
                        }
                    }
                }
            }
        }

        // 3. MASTER START/STOP
        Button {
            Layout.fillWidth: true; Layout.preferredHeight: 60
            text: replayManager.isRecording ? "STOP" : "START RECORDING (" + countSelector.value + " TRACKS)"
            onClicked: {
                if (replayManager.isRecording) replayManager.stopRecording()
                else replayManager.startRecording("file:///Users/timo.korkalainen/Desktop/replay.mkv")
            }
        }
    }
}
