import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

// Throwaway preview harness: renders the bespoke OlrStyle controls + mock broadcast
// bits offscreen to a PNG so the look can be reviewed headlessly. Not part of the app.
Rectangle {
    id: win
    width: 1120
    height: 640
    color: Theme.canvas

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.s3
        spacing: Theme.s3

        // --- Controls column ---
        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            spacing: Theme.s3

            Label { text: "CONTROLS"; color: Theme.textDim; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
            RowLayout {
                spacing: Theme.s2
                Button { text: "Mark In" }
                Button { text: "Recall" }
                Button { text: "GO LIVE"; highlighted: true }
                Button { text: "Off"; enabled: false }
            }
            RowLayout {
                spacing: Theme.s2
                TextField { text: "srt://10.0.0.4:9000"; Layout.preferredWidth: 220 }
                ComboBox { model: ["1080p25", "1080p50", "720p50"] }
                SpinBox { value: 12; from: 0; to: 99 }
            }
            RowLayout {
                spacing: Theme.s3
                CheckBox { text: "Drop-frame"; checked: true }
                Switch { text: "Live follow"; checked: true }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: "Speed"; color: Theme.textDim; font.pixelSize: Theme.fsMicro }
                Slider { Layout.fillWidth: true; value: 0.4 }
            }
            GroupBox {
                title: "NDI Output"
                ColumnLayout {
                    Label { text: "PGM → ndi://STUDIO-PGM" }
                    Label { text: "frames 18420 · drop 0"; color: Theme.textDim; font.pixelSize: Theme.fsMicro }
                }
            }
            TabBar {
                TabButton { text: "Control" }
                TabButton { text: "Playback" }
                TabButton { text: "Project" }
            }
        }

        Rectangle { width: 1; Layout.fillHeight: true; color: Theme.line }

        // --- Mock broadcast bits column ---
        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            Layout.fillWidth: true
            spacing: Theme.s3

            Label { text: "TALLY / STATUS"; color: Theme.textDim; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
            RowLayout {
                spacing: Theme.s2
                Repeater {
                    model: [
                        { n: "CAM1", c: Theme.ready, s: "RDY" },
                        { n: "CAM2", c: Theme.armed, s: "ARM" },
                        { n: "CAM3", c: Theme.recordOnAir, s: "LIVE" },
                        { n: "CAM4", c: Theme.idle, s: "—" },
                        { n: "CAM5", c: Theme.error, s: "NO SIG" }
                    ]
                    Rectangle {
                        required property var modelData
                        width: 116; height: 70; radius: Theme.r1
                        color: Theme.panelRaised
                        border.width: Theme.tallyBorder; border.color: modelData.c
                        Label { anchors.centerIn: parent; text: modelData.n; color: Theme.textHi }
                        Rectangle {
                            anchors { left: parent.left; bottom: parent.bottom; margins: 4 }
                            width: tl.implicitWidth + 8; height: tl.implicitHeight + 2; color: Theme.scrim; radius: 2
                            Label { id: tl; anchors.centerIn: parent; text: modelData.s; color: modelData.c; font.pixelSize: Theme.fsMicro; font.family: Theme.fontMono }
                        }
                    }
                }
            }

            Label { text: "TIMECODE"; color: Theme.textDim; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
            Label { text: "01:12:44:18"; color: Theme.textHi; font.family: Theme.fontMono; font.pixelSize: Theme.fsTc }

            Label { text: "SCRUB TIMELINE (mock)"; color: Theme.textDim; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
            Rectangle {
                Layout.fillWidth: true; height: 44; radius: Theme.r1; color: Theme.panel; border.color: Theme.line; border.width: 1
                Rectangle { x: 0; width: parent.width * 0.72; height: 6; anchors.verticalCenter: parent.verticalCenter; color: Theme.line; radius: 3
                    Rectangle { width: parent.width * 0.55; height: parent.height; color: Theme.accent; radius: 3 } }
                Rectangle { x: parent.width * 0.32; width: parent.width * 0.18; height: 14; anchors.verticalCenter: parent.verticalCenter; color: Qt.rgba(0.18, 0.48, 1, 0.25); border.color: Theme.accent; border.width: 1 }
                Repeater { model: [0.18, 0.45, 0.6]
                    Rectangle { required property real modelData; x: parent.width * modelData; width: 2; height: parent.height; color: Theme.armed } }
            }

            Label { text: "RUNDOWN (mock)"; color: Theme.textDim; font.pixelSize: Theme.fsMicro; font.weight: Font.DemiBold }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Repeater {
                    model: [
                        { i: "01", n: "GOAL  C3", d: ":04", sp: "0.5x", st: "LIVE", c: Theme.recordOnAir },
                        { i: "02", n: "SAVE  C1", d: ":06", sp: "1x", st: "ARM", c: Theme.armed },
                        { i: "03", n: "FOUL  C2", d: ":03", sp: "0.25x", st: "", c: "transparent" }
                    ]
                    Rectangle {
                        required property var modelData
                        Layout.fillWidth: true; height: Theme.hCompact
                        color: modelData.st === "LIVE" ? Qt.rgba(0.9, 0.22, 0.21, 0.18) : Theme.panelRaised
                        border.width: modelData.st === "ARM" ? 1 : 0; border.color: Theme.armed
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: Theme.s2; anchors.rightMargin: Theme.s2; spacing: Theme.s2
                            Label { text: modelData.i; color: Theme.textDim; font.family: Theme.fontMono }
                            Label { text: modelData.n; color: Theme.textHi; font.family: Theme.fontMono }
                            Item { Layout.fillWidth: true }
                            Label { text: modelData.sp; color: Theme.armed; font.pixelSize: Theme.fsMicro; font.family: Theme.fontMono }
                            Label { text: modelData.d; color: Theme.textBody; font.family: Theme.fontMono }
                            Label { text: modelData.st; color: modelData.c === "transparent" ? Theme.textDim : modelData.c; font.pixelSize: Theme.fsMicro; font.family: Theme.fontMono }
                        }
                    }
                }
            }
        }
    }

    Timer {
        interval: 400; running: true; repeat: false
        onTriggered: win.contentItem.grabToImage(function(res) {
            res.saveToFile(Qt.resolvedUrl("gallery_out.png").toString().replace("file://", ""))
            Qt.quit()
        })
    }
}
