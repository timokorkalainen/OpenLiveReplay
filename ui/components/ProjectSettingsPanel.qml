pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme
import "../.."

ColumnLayout {
    id: root

    property var ui
    property bool previewMode: false
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    signal browseFolderRequested()

    Layout.fillWidth: true
    enabled: root.hasUi || root.previewMode
    spacing: 12

    QtObject {
        id: previewController

        property bool h264EncodeAvailable: false
        property bool benchmarkRunning: false
        property string recordCodec: "mpeg2"
        property var benchmarkResult: ({})

        signal benchmarkProgress(int n, bool sustained)
        signal benchmarkFinished()

        function runBenchmark() {}
        function cancelBenchmark() {}
    }

    Text {
        text: "Project Settings"
        font.bold: true
        color: "#eeeeee"
    }

    GridLayout {
        columns: 3
        Layout.fillWidth: true

        Label { text: "Project Name" }

        TextField {
            Layout.fillWidth: true
            Layout.columnSpan: 2
            text: root.hasUi ? root.ui.fileName : ""
            onEditingFinished: if (root.hasUi) root.ui.fileName = text
        }

        Label { text: "Save Location" }

        TextField {
            Layout.fillWidth: true
            text: root.hasUi ? root.ui.saveLocation : ""
            placeholderText: "Select folder..."
            onEditingFinished: if (root.hasUi) root.ui.saveLocation = text
        }

        Button {
            text: "Browse..."
            onClicked: root.browseFolderRequested()
        }
    }

    GridLayout {
        columns: 5
        Layout.fillWidth: true

        Label { text: "Resolution" }

        SpinBox {
            from: 320
            to: 7680
            stepSize: 10
            editable: true
            inputMethodHints: Qt.ImhDigitsOnly
            value: root.hasUi ? root.ui.recordWidth : 1920
            onValueModified: if (root.hasUi) root.ui.recordWidth = value
        }

        Label { text: "x" }

        SpinBox {
            from: 240
            to: 4320
            stepSize: 10
            editable: true
            inputMethodHints: Qt.ImhDigitsOnly
            value: root.hasUi ? root.ui.recordHeight : 1080
            onValueModified: if (root.hasUi) root.ui.recordHeight = value
        }

        Item { }

        Label { text: "FPS" }

        ComboBox {
            Layout.fillWidth: true
            textRole: "label"
            model: ListModel {
                ListElement { label: "23.976"; num: 24000; den: 1001 }
                ListElement { label: "24"; num: 24; den: 1 }
                ListElement { label: "25"; num: 25; den: 1 }
                ListElement { label: "29.97"; num: 30000; den: 1001 }
                ListElement { label: "30"; num: 30; den: 1 }
                ListElement { label: "50"; num: 50; den: 1 }
                ListElement { label: "59.94"; num: 60000; den: 1001 }
                ListElement { label: "60"; num: 60; den: 1 }
                ListElement { label: "120"; num: 120; den: 1 }
            }
            currentIndex: {
                if (!root.hasUi) return 4
                for (var i = 0; i < model.count; ++i) {
                    var preset = model.get(i)
                    if (preset.num === root.ui.recordFpsNumerator
                            && preset.den === root.ui.recordFpsDenominator) {
                        return i
                    }
                }
                return -1
            }
            displayText: currentIndex >= 0
                         ? currentText
                         : (root.hasUi ? root.ui.recordFps + " fps" : "")
            enabled: root.hasUi ? !root.ui.isRecording : true
            onActivated: {
                if (!root.hasUi) return
                var preset = model.get(currentIndex)
                root.ui.setRecordFrameRate(preset.num, preset.den)
            }
        }

        Item { }
        Item { }
        Item { }

        Label { text: "Audio output latency (ms)" }

        SpinBox {
            from: 0
            to: 500
            stepSize: 10
            editable: true
            inputMethodHints: Qt.ImhDigitsOnly
            value: root.hasUi ? root.ui.audioOutputLatencyMs : 0
            onValueModified: if (root.hasUi) root.ui.audioOutputLatencyMs = value
            ToolTip.visible: hovered
            ToolTip.text: "Compensate audio-output device delay (HDMI/Bluetooth). Raise until lip-sync is correct on this output."
        }

        Item { }
        Item { }
        Item { }
    }

    CodecSettingsPanel {
        Layout.fillWidth: true
        controller: root.hasUi ? root.ui : (root.previewMode ? previewController : null)
    }
}
