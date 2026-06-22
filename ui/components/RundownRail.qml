pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

Frame {
    id: root

    property var ui
    property bool expanded: true
    property int selectedIndex: -1
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    readonly property int rowHeight: 104
    property int dragIndex: -1

    signal saveRequested()
    signal loadRequested()

    Layout.minimumWidth: root.expanded ? 260 : Theme.hControl
    implicitWidth: root.expanded ? 340 : Theme.hControl
    padding: Theme.s2

    function formatTimecode(ms) {
        if (root.hasUi && root.ui.recordTimecode) return root.ui.recordTimecode(ms)
        if (ms < 0) return "OPEN"
        var totalSeconds = Math.max(0, Math.floor(ms / 1000))
        var mm = Math.floor(totalSeconds / 60)
        var ss = totalSeconds % 60
        return (mm < 10 ? "0" + mm : "" + mm)
            + ":" + (ss < 10 ? "0" + ss : "" + ss)
    }

    function speedText(speed) {
        return Number(speed).toFixed(speed === Math.round(speed) ? 0 : 2) + "x"
    }

    function speedPresetIndex(speed) {
        var presets = [0.25, 0.5, 1.0, 2.0, 5.0]
        for (var i = 0; i < presets.length; ++i) {
            if (Math.abs(Number(speed) - presets[i]) < 0.01) return i
        }
        return 2
    }

    function speedPresetValue(index) {
        return [0.25, 0.5, 1.0, 2.0, 5.0][Math.max(0, Math.min(4, index))]
    }

    function moveEntry(fromIndex, toIndex) {
        if (root.hasUi && fromIndex >= 0 && toIndex >= 0 && fromIndex !== toIndex) {
            root.ui.movePlaylistEntry(fromIndex, toIndex)
        }
    }

    function insertAt(index) {
        if (root.hasUi) root.ui.insertPlaylistEntryAt(index)
    }

    background: Rectangle {
        color: Theme.panel
        border.width: Theme.borderW
        border.color: Theme.line
        radius: Theme.r1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.s2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s1

            ToolButton {
                text: root.expanded ? "<" : ">"
                checked: root.expanded
                onClicked: root.expanded = !root.expanded
                ToolTip.visible: hovered
                ToolTip.text: root.expanded ? "Collapse rundown" : "Expand rundown"
            }

            Label {
                visible: root.expanded
                Layout.fillWidth: true
                text: "RUNDOWN"
                color: Theme.textHi
                font.pixelSize: Theme.fsHeading
                font.weight: Font.DemiBold
            }

            Label {
                visible: root.expanded && root.hasUi
                text: root.hasUi ? String(root.ui.playlistCount) : "0"
                color: Theme.textDim
                font.family: Theme.fontMono
            }
        }

        Label {
            visible: !root.expanded
            Layout.alignment: Qt.AlignHCenter
            Layout.fillHeight: true
            text: root.hasUi && root.ui.playlistPlayoutActive ? "LIVE" : "RUN"
            color: root.hasUi && root.ui.playlistPlayoutActive ? Theme.recordOnAir : Theme.textDim
            font.pixelSize: Theme.fsMicro
            font.family: Theme.fontMono
            rotation: -90
        }

        RowLayout {
            visible: root.expanded
            Layout.fillWidth: true
            spacing: Theme.s1

            Button {
                text: "In"
                enabled: root.hasUi
                onClicked: root.ui.markIn()
                ToolTip.visible: hovered
                ToolTip.text: "Mark current playhead as a new entry in-point"
            }
            Button {
                text: "Out"
                enabled: root.hasUi
                onClicked: root.ui.markOut()
                ToolTip.visible: hovered
                ToolTip.text: "Close the latest open entry at the playhead"
            }
            Button {
                text: "Play"
                enabled: root.hasUi
                highlighted: root.hasUi && root.ui.playlistPlayoutActive
                onClicked: root.ui.playPlaylist(Math.max(0, root.selectedIndex))
            }
            Button {
                text: "Stop"
                enabled: root.hasUi
                onClicked: root.ui.stopPlaylistPlayout()
            }
        }

        RowLayout {
            visible: root.expanded
            Layout.fillWidth: true
            spacing: Theme.s1

            Button {
                text: "Save"
                enabled: root.hasUi
                onClicked: root.saveRequested()
            }
            Button {
                text: "Load"
                enabled: root.hasUi
                onClicked: root.loadRequested()
            }
            Button {
                text: "Clear"
                enabled: root.hasUi && root.ui.playlistCount > 0
                onClicked: clearPopup.open()
            }
            Label {
                Layout.fillWidth: true
                text: root.hasUi && root.ui.playlistDirty ? "DIRTY" : ""
                color: Theme.armed
                horizontalAlignment: Text.AlignRight
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsMicro
            }
        }

        Rectangle {
            visible: root.expanded && root.hasUi && root.ui.playlistOperationError !== ""
            Layout.fillWidth: true
            Layout.preferredHeight: errorText.implicitHeight + Theme.s2
            color: Theme.panelPressed
            border.color: Theme.armed
            border.width: Theme.borderW
            radius: Theme.r1

            Label {
                id: errorText
                anchors.fill: parent
                anchors.margins: Theme.s1
                text: root.hasUi ? root.ui.playlistOperationError : ""
                color: Theme.armed
                elide: Text.ElideRight
                font.pixelSize: Theme.fsMicro
            }
        }

        ListView {
            id: rundownList

            visible: root.expanded
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.s1
            model: root.hasUi ? root.ui.playlistModel : 0
            boundsBehavior: Flickable.StopAtBounds

            delegate: Item {
                id: row

                required property int index
                required property string label
                required property var inMs
                required property var outMs
                required property var durationMs
                required property var speed
                required property bool hasOut
                required property bool boundaryReady

                width: rundownList.width
                height: root.rowHeight

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 1

                    Button {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 18
                        text: "+"
                        enabled: root.hasUi
                        onClicked: root.insertAt(row.index)
                        ToolTip.visible: hovered
                        ToolTip.text: "Insert current playhead before this entry"
                    }

                    Rectangle {
                        id: rowSurface

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: row.index === root.ui.currentPlaylistEntryIndex
                               ? Qt.rgba(0.9, 0.22, 0.21, 0.18)
                               : (row.index === root.selectedIndex ? Theme.panelHover : Theme.panelRaised)
                        border.width: Theme.borderW
                        border.color: row.index === root.ui.currentPlaylistEntryIndex
                                      ? Theme.recordOnAir
                                      : (row.index === root.ui.nextPlaylistEntryIndex ? Theme.armed : Theme.line)
                        radius: Theme.r1

                        Drag.active: dragHandler.active
                        Drag.hotSpot.x: width / 2
                        Drag.hotSpot.y: height / 2

                        DropArea {
                            anchors.fill: parent
                            onEntered: (drag) => {
                                if (drag !== null && root.dragIndex >= 0 && root.dragIndex !== row.index) {
                                    root.moveEntry(root.dragIndex, row.index)
                                    root.dragIndex = row.index
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectedIndex = row.index
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.s2
                            spacing: Theme.s2

                            Rectangle {
                                Layout.preferredWidth: Theme.hCompact
                                Layout.fillHeight: true
                                radius: Theme.r1
                                color: dragHandler.active ? Theme.accent : Theme.panelPressed
                                border.color: Theme.line
                                border.width: Theme.borderW

                                Label {
                                    anchors.centerIn: parent
                                    text: "||"
                                    color: Theme.textDim
                                    font.family: Theme.fontMono
                                }

                                DragHandler {
                                    id: dragHandler
                                    target: null
                                    onActiveChanged: {
                                        root.dragIndex = active ? row.index : -1
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    Label {
                                        text: String(row.index + 1).padStart(2, "0")
                                        color: Theme.textDim
                                        font.family: Theme.fontMono
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: row.label
                                        color: Theme.textHi
                                        elide: Text.ElideRight
                                        font.weight: Font.DemiBold
                                    }
                                    Label {
                                        text: row.index === root.ui.currentPlaylistEntryIndex
                                              ? "LIVE"
                                              : (row.index === root.ui.nextPlaylistEntryIndex ? "NEXT" : "")
                                        color: row.index === root.ui.currentPlaylistEntryIndex
                                               ? Theme.recordOnAir
                                               : Theme.armed
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fsMicro
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    Label {
                                        text: root.formatTimecode(row.inMs)
                                        color: Theme.textBody
                                        font.family: Theme.fontMono
                                    }
                                    Label {
                                        text: row.hasOut ? root.formatTimecode(row.outMs) : "OPEN"
                                        color: row.hasOut ? Theme.textBody : Theme.armed
                                        font.family: Theme.fontMono
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: row.durationMs >= 0 ? root.formatTimecode(row.durationMs) : ""
                                        color: Theme.textDim
                                        horizontalAlignment: Text.AlignRight
                                        font.family: Theme.fontMono
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    ComboBox {
                                        Layout.preferredWidth: 82
                                        model: ["0.25x", "0.5x", "1x", "2x", "5x"]
                                        currentIndex: root.speedPresetIndex(row.speed)
                                        enabled: root.hasUi
                                        onActivated: (presetIndex) => {
                                            root.ui.setPlaylistEntrySpeed(row.index,
                                                                         root.speedPresetValue(presetIndex))
                                        }
                                    }
                                    Button {
                                        text: "Set In"
                                        enabled: root.hasUi
                                        onClicked: root.ui.setPlaylistEntryInFromPlayhead(row.index)
                                    }
                                    Button {
                                        text: "Set Out"
                                        enabled: root.hasUi
                                        onClicked: root.ui.setPlaylistEntryOutFromPlayhead(row.index)
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.alignment: Qt.AlignTop
                                spacing: Theme.s1

                                ToolButton {
                                    text: "R"
                                    enabled: root.hasUi
                                    onClicked: root.ui.recallEntry(row.index)
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Recall entry"
                                }
                                ToolButton {
                                    text: "X"
                                    enabled: root.hasUi
                                    onClicked: root.ui.removePlaylistEntry(row.index)
                                    ToolTip.visible: hovered
                                    ToolTip.text: "Remove entry"
                                }
                            }
                        }
                    }
                }
            }
        }

        Button {
            visible: root.expanded
            Layout.fillWidth: true
            text: "+"
            enabled: root.hasUi
            onClicked: root.insertAt(rundownList.count)
            ToolTip.visible: hovered
            ToolTip.text: "Insert current playhead at the end"
        }
    }

    Popup {
        id: clearPopup

        modal: true
        focus: true
        width: Math.min(root.width - Theme.s3, 260)
        height: clearBox.implicitHeight + Theme.s3
        x: (root.width - width) / 2
        y: Theme.s3
        padding: Theme.s2

        background: Rectangle {
            color: Theme.panelRaised
            border.color: Theme.lineStrong
            border.width: Theme.borderW
            radius: Theme.r1
        }

        ColumnLayout {
            id: clearBox
            anchors.fill: parent
            spacing: Theme.s2

            Label {
                text: "Clear rundown?"
                color: Theme.textHi
                font.weight: Font.DemiBold
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: "Cancel"
                    onClicked: clearPopup.close()
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Clear"
                    highlighted: true
                    onClicked: {
                        if (root.hasUi) root.ui.clearPlaylist()
                        clearPopup.close()
                    }
                }
            }
        }
    }
}
