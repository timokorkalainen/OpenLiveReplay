import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

Window {
    id: multiviewWindow
    width: 900
    height: 600
    visible: true
    title: "Multiview"

    property var uiManager
    property var visibleStreamIndexes: []
    property var owner

    property int streamCount: visibleStreamIndexes.length
    property int gridColumns: Math.max(1, Math.ceil(Math.sqrt(Math.max(1, streamCount))))
    property int gridRows: Math.ceil(Math.max(1, streamCount) / gridColumns)

    onClosing: function(close) {
        if (owner) owner.multiviewWindow = null
        close.accepted = true
        multiviewWindow.destroy()
    }

    Rectangle {
        anchors.fill: parent
        color: "#111"

        GridView {
            id: multiViewGrid
            anchors.fill: parent
            anchors.margins: 0
            clip: true
            interactive: false
            cellHeight: parent.height / multiviewWindow.gridRows
            cellWidth: parent.width / multiviewWindow.gridColumns

            model: multiviewWindow.visibleStreamIndexes

            delegate: Rectangle {
                required property var modelData
                property int streamIndex: modelData
                color: "black"
                border.color: "#d32f2f"
                border.width: 2
                width: multiViewGrid.cellWidth
                height: multiViewGrid.cellHeight

                VideoOutput {
                    id: vOutput
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    z: 1
                    Component.onCompleted: {
                        if (streamIndex < multiviewWindow.uiManager.playbackProviders.length) {
                            multiviewWindow.uiManager.playbackProviders[streamIndex].addVideoSink(vOutput.videoSink)
                        }
                    }
                    Component.onDestruction: {
                        if (streamIndex < multiviewWindow.uiManager.playbackProviders.length) {
                            multiviewWindow.uiManager.playbackProviders[streamIndex].removeVideoSink(vOutput.videoSink)
                        }
                    }
                }

                Text {
                    text: (streamIndex < multiviewWindow.uiManager.streamNames.length
                          && multiviewWindow.uiManager.streamNames[streamIndex].length > 0)
                          ? multiviewWindow.uiManager.streamNames[streamIndex]
                          : ("CAM " + (streamIndex + 1))
                    color: "white"
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.margins: 5
                    font.family: "Menlo"
                    font.pixelSize: 12
                    z: 5
                }
            }
        }
    }
}
