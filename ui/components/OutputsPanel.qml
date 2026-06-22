pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

GroupBox {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    readonly property var rows: root.hasUi && root.ui.broadcastOutputsVersion >= 0
                                ? root.ui.ndiOutputRows()
                                : []
    readonly property int tableMinimumWidth: 760

    title: "NDI Outputs"
    Layout.fillWidth: true
    Layout.preferredHeight: Math.min(320, Math.max(150, 48 + (rows.length + 1) * 44))

    function statusColor(severity) {
        if (severity === "ok")
            return "#2e7d32"
        if (severity === "warning")
            return "#f9a825"
        if (severity === "error")
            return "#d32f2f"
        return "#666"
    }

    ScrollView {
        id: ndiOutputScroll
        anchors.fill: parent
        clip: true
        contentWidth: Math.max(availableWidth, root.tableMinimumWidth)
        ScrollBar.horizontal.policy: ScrollBar.AsNeeded
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: ndiOutputTable
            width: ndiOutputScroll.contentWidth
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: root.tableMinimumWidth
                spacing: 8

                Label {
                    text: "Bus"
                    Layout.preferredWidth: 92
                    font.bold: true
                }

                Label {
                    text: "Output"
                    Layout.preferredWidth: 56
                    font.bold: true
                }

                Label {
                    text: "Name"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 160
                    font.bold: true
                }

                Label {
                    text: "State"
                    Layout.preferredWidth: 112
                    font.bold: true
                }

                Label {
                    text: "Frames"
                    Layout.preferredWidth: 96
                    font.bold: true
                }

                Label {
                    text: "Health"
                    Layout.preferredWidth: 124
                    font.bold: true
                }

                Label {
                    text: "Last"
                    Layout.preferredWidth: 72
                    font.bold: true
                }
            }

            Repeater {
                model: root.rows

                delegate: RowLayout {
                    id: ndiOutputRow
                    required property var modelData
                    property var statusData: root.hasUi && root.ui.broadcastOutputStatusVersion >= 0
                                             ? root.ui.ndiOutputStatus(modelData.id)
                                             : ({})
                    readonly property bool hasSinkStatus: !!statusData.hasSinkStatus
                    readonly property var displayedFrames: hasSinkStatus
                                                             ? (statusData.sinkSubmittedFrames || 0)
                                                             : (statusData.framesSubmitted || 0)
                    width: ndiOutputTable.width
                    Layout.minimumWidth: root.tableMinimumWidth
                    spacing: 8

                    Label {
                        text: ndiOutputRow.modelData.label || ""
                        Layout.preferredWidth: 92
                        elide: Text.ElideRight
                    }

                    Switch {
                        id: ndiOutputSwitch
                        Layout.preferredWidth: 56
                        checked: !!ndiOutputRow.modelData.enabled
                        onToggled: if (root.hasUi) {
                            root.ui.setNdiOutputEnabled(
                                        ndiOutputRow.modelData.busKind,
                                        ndiOutputRow.modelData.feedIndex,
                                        checked)
                        }
                    }

                    TextField {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 160
                        enabled: ndiOutputSwitch.checked
                        text: ndiOutputRow.modelData.senderName || ""
                        selectByMouse: true
                        onEditingFinished: if (root.hasUi) {
                            root.ui.setNdiOutputSenderName(
                                        ndiOutputRow.modelData.busKind,
                                        ndiOutputRow.modelData.feedIndex,
                                        text)
                        }
                    }

                    RowLayout {
                        Layout.preferredWidth: 112
                        spacing: 6

                        Rectangle {
                            id: outputStatusDot
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            radius: width / 2
                            color: root.statusColor(
                                       ndiOutputRow.statusData.statusSeverity || "off")
                            HoverHandler { id: outputStatusHover }
                            ToolTip.visible: outputStatusHover.hovered
                            ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                        }

                        Label {
                            Layout.fillWidth: true
                            text: ndiOutputRow.statusData.statusState || "Off"
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        Layout.preferredWidth: 96
                        text: ndiOutputRow.displayedFrames
                              + "/" + (ndiOutputRow.statusData.attemptedFrames || 0)
                        horizontalAlignment: Text.AlignRight
                        HoverHandler { id: outputFramesHover }
                        ToolTip.visible: outputFramesHover.hovered
                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                    }

                    Label {
                        Layout.preferredWidth: 124
                        text: "F " + (ndiOutputRow.statusData.sinkFailures || 0)
                              + " SF " + (ndiOutputRow.statusData.sinkFailedFrames || 0)
                              + " D " + (ndiOutputRow.statusData.sinkDroppedFrames || 0)
                              + " Q " + (ndiOutputRow.statusData.currentQueueDepth || 0)
                              + "/" + (ndiOutputRow.statusData.maxQueueDepth || 0)
                              + " G " + (ndiOutputRow.statusData.deliveryGaps || 0)
                        elide: Text.ElideRight
                        HoverHandler { id: outputHealthHover }
                        ToolTip.visible: outputHealthHover.hovered
                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                    }

                    Label {
                        Layout.preferredWidth: 72
                        text: ndiOutputRow.statusData.lastOutputFrameIndex >= 0
                              ? ("#" + ndiOutputRow.statusData.lastOutputFrameIndex)
                              : "-"
                        horizontalAlignment: Text.AlignRight
                        HoverHandler { id: outputLastHover }
                        ToolTip.visible: outputLastHover.hovered
                        ToolTip.text: ndiOutputRow.statusData.diagnostic || ""
                    }
                }
            }
        }
    }
}
