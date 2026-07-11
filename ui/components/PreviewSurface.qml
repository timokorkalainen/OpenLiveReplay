pragma ComponentBehavior: Bound
import QtQuick
import QtMultimedia
import Recorder.Types

Item {
    id: root
    property QtObject provider: null
    property bool active: true
    readonly property bool usingDirectPreview: Qt.platform.os === "linux"

    Loader {
        anchors.fill: parent
        sourceComponent: root.usingDirectPreview ? directPreview : nativePreview
    }

    Component {
        id: directPreview

        FramePreviewItem {
            anchors.fill: parent
            provider: root.provider
            active: root.active && root.visible
        }
    }

    Component {
        id: nativePreview

        VideoOutput {
            id: previewOutput
            anchors.fill: parent
            property QtObject attachedProvider: null

            fillMode: VideoOutput.PreserveAspectFit

            function selectedProvider() {
                return (root.active && root.visible && root.provider) ? root.provider : null
            }

            function updateAttachment() {
                previewOutput.attachProvider(previewOutput.selectedProvider())
            }

            // qmllint disable missing-property
            function attachProvider(provider) {
                if (previewOutput.attachedProvider === provider) return
                var previousProvider = previewOutput.attachedProvider
                previewOutput.attachedProvider = null
                if (previousProvider
                        && typeof previousProvider.removeVideoSink === "function") {
                    previousProvider.removeVideoSink(videoSink)
                }
                previewOutput.attachedProvider = provider
                if (previewOutput.attachedProvider) {
                    previewOutput.attachedProvider.addVideoSink(videoSink)
                }
            }
            // qmllint enable missing-property

            Connections {
                target: root
                function onProviderChanged() { previewOutput.updateAttachment() }
                function onActiveChanged() { previewOutput.updateAttachment() }
                function onVisibleChanged() { previewOutput.updateAttachment() }
            }

            Component.onCompleted: updateAttachment()
            Component.onDestruction: attachProvider(null)
        }
    }
}
