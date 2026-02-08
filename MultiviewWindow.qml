pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
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
        color: "#111111"

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
                id: streamTile
                required property var modelData
                property int streamIndex: modelData
                property int sourceForView: {
                    var map = multiviewWindow.uiManager.viewSlotMap
                    return (streamTile.streamIndex >= 0 && streamTile.streamIndex < map.length)
                           ? map[streamTile.streamIndex] : -1
                }
                color: sourceForView < 0 ? "#003080" : "black"
                border.color: sourceForView < 0 ? "#1565C0" : "#d32f2f"
                border.width: 2
                width: multiViewGrid.cellWidth
                height: multiViewGrid.cellHeight

                VideoOutput {
                    id: vOutput
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                    visible: streamTile.sourceForView >= 0
                    z: 1
                    Component.onCompleted: {
                        if (streamTile.streamIndex < multiviewWindow.uiManager.playbackProviders.length) {
                            multiviewWindow.uiManager.playbackProviders[streamTile.streamIndex].addVideoSink(vOutput.videoSink)
                        }
                    }
                    Component.onDestruction: {
                        if (streamTile.streamIndex < multiviewWindow.uiManager.playbackProviders.length) {
                            multiviewWindow.uiManager.playbackProviders[streamTile.streamIndex].removeVideoSink(vOutput.videoSink)
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.margins: 5
                    color: Qt.rgba(0, 0, 0, 0.5)
                    width: mvwLabel.implicitWidth + 10
                    height: mvwLabel.implicitHeight + 4
                    z: 5

                    Text {
                        id: mvwLabel
                        anchors.centerIn: parent
                        text: {
                            var src = streamTile.sourceForView
                            if (src < 0) return "VIEW " + (streamTile.streamIndex + 1)
                            return multiviewWindow.uiManager.sourceDisplayLabel(src)
                        }
                        color: "white"
                        font.family: "monospace"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
