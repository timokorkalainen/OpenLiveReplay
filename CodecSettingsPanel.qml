pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Codec selector + benchmark panel.
// controller is injected at the use site (duck-typed) so this component can be
// unit-tested in isolation with a mock controller (Task 6).
Item {
    id: root

    // Duck-typed controller — may be null briefly; guard every access.
    property var controller: null

    // Task 6 asserts H.264 option availability via this helper.
    readonly property bool h264Selectable: root.controller && root.controller.h264EncodeAvailable

    // Task 6 asserts benchmark running state via this helper (effective visible
    // cascades from window state in offscreen tests; this does not).
    readonly property bool benchmarkActive: root.controller ? root.controller.benchmarkRunning : false

    // Force codec back to mpeg2 when H.264 is selected but hardware is unavailable.
    Connections {
        target: root.controller
        enabled: root.controller !== null

        function onH264EncodeAvailableChanged() {
            if (root.controller && !root.controller.h264EncodeAvailable
                    && root.controller.recordCodec === "h264") {
                root.controller.recordCodec = "mpeg2"
            }
        }

        function onBenchmarkProgress(n, sustained) {
            progressLabel.text = "Feed " + n + " — " + (sustained ? "sustained" : "dropped")
        }

        // Clear the per-step progress line when the run ends or is cancelled,
        // so the panel doesn't permanently display the last step's text.
        function onBenchmarkFinished() {
            progressLabel.text = ""
        }

    }

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight

    ColumnLayout {
        id: layout
        anchors.fill: parent
        spacing: 8

        // ── Codec selector ────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Recording Codec"
                color: "#eeeeee"
            }

            ComboBox {
                id: codecSelector
                objectName: "codecSelector"
                Layout.fillWidth: true

                // Two entries; H.264 text is amended when hardware is absent.
                model: {
                    var h264Label = root.h264Selectable
                        ? "H.264 (hardware)"
                        : "H.264 (hardware) (no hardware)"
                    return ["MPEG-2 (software)", h264Label]
                }

                currentIndex: (root.controller && root.controller.recordCodec === "h264") ? 1 : 0

                onActivated: {
                    if (!root.controller) return
                    root.controller.recordCodec = (currentIndex === 1) ? "h264" : "mpeg2"
                }

                delegate: ItemDelegate {
                    id: codecDelegate
                    required property int index
                    required property string modelData
                    width: codecSelector.width
                    text: modelData

                    // Grey out the H.264 row when hardware is unavailable.
                    enabled: codecDelegate.index === 1 ? root.h264Selectable : true
                    opacity: enabled ? 1.0 : 0.4
                    highlighted: codecSelector.highlightedIndex === codecDelegate.index
                }
            }
        }

        // ── Benchmark controls ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                id: runBenchmarkButton
                objectName: "runBenchmarkButton"
                text: "Run Benchmark"
                enabled: root.controller ? !root.controller.benchmarkRunning : false
                onClicked: {
                    if (root.controller) {
                        progressLabel.text = ""
                        root.controller.runBenchmark()
                    }
                }
            }

            BusyIndicator {
                id: benchmarkBusy
                objectName: "benchmarkBusy"
                visible: root.controller ? root.controller.benchmarkRunning : false
                running: visible
            }

            Button {
                id: cancelBenchmarkButton
                objectName: "cancelBenchmarkButton"
                text: "Cancel"
                visible: root.controller ? root.controller.benchmarkRunning : false
                onClicked: {
                    if (root.controller) root.controller.cancelBenchmark()
                }
            }

            Text {
                id: progressLabel
                objectName: "benchmarkProgressLabel"
                text: ""
                color: "#aaaaaa"
                visible: text !== ""
            }
        }

        // ── Benchmark results ─────────────────────────────────────────────
        Text {
            id: benchmarkResultText
            objectName: "benchmarkResultText"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#eeeeee"

            text: {
                if (!root.controller) return "Not benchmarked yet"
                var r = root.controller.benchmarkResult
                if (!r || typeof r !== "object") return "Not benchmarked yet"

                var keys = Object.keys(r)
                if (keys.length === 0) return "Not benchmarked yet"

                var h264Feeds  = (typeof r.h264SafeFeeds  === "number") ? r.h264SafeFeeds  : "?"
                var mpeg2Feeds = (typeof r.mpeg2SafeFeeds === "number") ? r.mpeg2SafeFeeds : "?"
                var rec        = r.recommended || ""
                var h264Avail  = (r.h264Available === true)
                var h264Measured = (typeof r.h264SafeFeeds === "number" && r.h264SafeFeeds >= 0)

                var lines = []
                lines.push("MPEG-2 (software): " + mpeg2Feeds + " safe feeds")
                if (h264Avail && h264Measured) {
                    lines.push("H.264 (hardware): " + h264Feeds + " safe feeds")
                } else if (h264Avail) {
                    lines.push("H.264 (hardware): not benchmarked")
                } else {
                    lines.push("H.264 (hardware): not available")
                }
                if (rec === "h264") {
                    lines.push("Recommended: H.264 — " + h264Feeds + " feeds")
                } else if (rec === "mpeg2") {
                    lines.push("Recommended: MPEG-2 — " + mpeg2Feeds + " feeds")
                } else if (rec !== "") {
                    lines.push("Recommended: " + rec)
                }
                return lines.join("\n")
            }
        }
    }
}
