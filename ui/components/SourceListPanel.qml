pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

ColumnLayout {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    readonly property real popupWidthBase: Overlay.overlay ? Overlay.overlay.width : root.width
    readonly property real popupHeightBase: Overlay.overlay ? Overlay.overlay.height : root.height

    Layout.fillWidth: true
    spacing: 8

    function openImportPreview() {
        if (root.hasUi && root.ui.importPreviewReady && !importPreviewPopup.opened)
            importPreviewPopup.open()
    }

    function maybeAutoOpenImport() {
        if (root.hasUi && !root.ui.isRecording)
            root.openImportPreview()
    }

    RowLayout {
        Layout.fillWidth: true

        Text {
            text: "Input Sources"
            Layout.fillWidth: true
            color: "#eeeeee"
            font.bold: true
        }

        Button {
            text: "Metadata Fields"
            onClicked: metadataFieldsEditor.openEditor()
        }

        Button {
            text: "+ Add Stream"
            onClicked: if (root.hasUi) root.ui.addStream()
            enabled: root.hasUi && !root.ui.isRecording
        }
    }

    Rectangle {
        Layout.fillHeight: true
        Layout.fillWidth: true
        color: "#181818"

        ListView {
            id: streamList
            anchors.fill: parent
            clip: true
            model: root.hasUi ? root.ui.streamUrls : []
            spacing: 8

            delegate: RowLayout {
                id: streamRow
                required property string modelData
                required property int index
                width: streamList.width
                spacing: 10

                Label {
                    text: (streamRow.index + 1) + ":"
                    Layout.preferredWidth: 20
                }

                Button {
                    readonly property bool sourceEnabled: root.hasUi
                                                       && root.ui.sourceEnabledVersion >= 0
                                                       && root.ui.isSourceEnabled(streamRow.index)

                    text: sourceEnabled ? "ON" : "OFF"
                    Layout.preferredWidth: 50
                    palette.button: sourceEnabled ? "#2e7d32" : "#555"
                    onClicked: if (root.hasUi) root.ui.toggleSourceEnabled(streamRow.index)
                }

                // Live connection indicator: grey when idle, green once the source's feed is up,
                // red while recording but the feed has not connected (or has dropped).
                Rectangle {
                    id: connDot
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                    radius: width / 2
                    property bool connected: root.hasUi
                                             && root.ui.sourceConnectionVersion >= 0
                                             && root.ui.isSourceConnected(streamRow.index)
                    // 0=N/A,1=green,2=amber,3=red (native SRT only; else 0)
                    property int linkHealth: root.hasUi && root.ui.sourceStatsVersion >= 0
                                             ? root.ui.sourceLinkHealth(streamRow.index)
                                             : 0
                    color: !root.hasUi || !root.ui.isRecording
                           ? "#555"
                           : (!connDot.connected
                              ? "#d32f2f"
                              : (connDot.linkHealth === 3 ? "#d32f2f"
                                 : connDot.linkHealth === 2 ? "#f9a825"
                                 : "#2e7d32"))
                    HoverHandler { id: connHover }
                    ToolTip.visible: connHover.hovered
                    ToolTip.text: !root.hasUi || !root.ui.isRecording
                                  ? "Not recording"
                                  : (!connDot.connected
                                     ? "No signal"
                                     : (root.ui.sourceStatsVersion >= 0
                                        && root.ui.sourceHasStats(streamRow.index)
                                        ? root.ui.sourceStatsTooltip(streamRow.index)
                                        : "Connected"))
                }

                SpinBox {
                    id: trimSpin
                    from: -500
                    to: 500
                    stepSize: 33
                    editable: true
                    Layout.preferredWidth: 96
                    value: root.hasUi && root.ui.sourceTrimVersion >= 0
                           ? root.ui.sourceTrimOffset(streamRow.index) : 0
                    onValueModified: if (root.hasUi) root.ui.setSourceTrimOffset(streamRow.index, value)
                    ToolTip.visible: hovered
                    ToolTip.text: "Timeline trim (ms): + delays this camera, − advances it"
                }

                TextField {
                    Layout.preferredWidth: 160
                    text: root.hasUi && root.ui.streamIds.length > streamRow.index ? root.ui.streamIds[streamRow.index] : ""
                    placeholderText: "ID"
                    onEditingFinished: if (root.hasUi) root.ui.updateStreamId(streamRow.index, text)
                }

                TextField {
                    Layout.preferredWidth: 140
                    text: root.hasUi && root.ui.streamNames.length > streamRow.index ? root.ui.streamNames[streamRow.index] : ""
                    placeholderText: "Name"
                    onEditingFinished: if (root.hasUi) root.ui.updateStreamName(streamRow.index, text)
                }

                Button {
                    text: "Metadata"
                    Layout.preferredWidth: 90
                    onClicked: metadataEditor.openFor(streamRow.index)
                }

                TextField {
                    Layout.fillWidth: true
                    text: streamRow.modelData
                    placeholderText: "rtmp://..."
                    onEditingFinished: if (root.hasUi) root.ui.updateUrl(streamRow.index, text)
                }

                // Misconfiguration warning: another source points at this same URL.
                Label {
                    text: "⚠"
                    color: "#ff9800"
                    font.bold: true
                    Layout.preferredWidth: 16
                    visible: root.hasUi
                             && root.ui.streamUrls.length >= 0
                             && root.ui.hasDuplicateUrl(streamRow.index)
                    HoverHandler { id: dupHover }
                    ToolTip.visible: dupHover.hovered
                    ToolTip.text: "Duplicate URL — another source uses this same stream"
                }

                Button {
                    text: "×"
                    Layout.preferredWidth: 30
                    flat: true
                    onClicked: if (root.hasUi) root.ui.removeStream(streamRow.index)
                    visible: root.hasUi && !root.ui.isRecording
                }
            }
        }
    }

    // Imported Input Settings Preview
    Popup {
        id: importPreviewPopup
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // qmllint disable Quick.layout-positioning
        anchors.centerIn: Overlay.overlay ? Overlay.overlay : root
        width: Math.min(root.popupWidthBase * 0.92, 760)
        height: Math.min(root.popupHeightBase * 0.84, 580)
        // qmllint enable Quick.layout-positioning

        function metadataSummary(metadata) {
            if (!metadata || metadata.length === 0) return "metadata 0"

            var parts = []
            for (var i = 0; i < metadata.length && i < 3; ++i) {
                var row = metadata[i] || {}
                var name = row.name || ""
                var value = row.value !== undefined ? String(row.value) : ""
                if (name.length === 0 && value.length === 0) continue
                parts.push(value.length > 0 ? (name + "=" + value) : name)
            }

            var suffix = metadata.length > parts.length ? " +" + (metadata.length - parts.length) : ""
            return "metadata " + metadata.length + (parts.length > 0 ? ": " + parts.join(", ") + suffix : "")
        }

        contentItem: Rectangle {
            color: "#1f1f1f"
            radius: 0
            border.color: "#333"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    text: "Imported Input Settings"
                    color: "#eeeeee"
                    font.pixelSize: 18
                    font.bold: true
                    Layout.fillWidth: true
                }

                GridLayout {
                    columns: 2
                    Layout.fillWidth: true
                    columnSpacing: 14
                    rowSpacing: 4

                    Label {
                        text: "Project"
                        color: "#999999"
                    }

                    Text {
                        Layout.fillWidth: true
                        text: {
                            var p = root.hasUi ? root.ui.importPreview : ({})
                            var name = p.projectName || "Imported project"
                            var idText = p.projectId ? " (" + p.projectId + ")" : ""
                            return name + idText
                        }
                        color: "#eeeeee"
                        elide: Text.ElideRight
                    }

                    Label {
                        text: "Feeds"
                        color: "#999999"
                    }

                    Text {
                        Layout.fillWidth: true
                        text: {
                            var p = root.hasUi ? root.ui.importPreview : ({})
                            return p.feedCount !== undefined ? p.feedCount : ((p.feeds || []).length)
                        }
                        color: "#eeeeee"
                    }

                    Label {
                        text: "Telemetry SSE"
                        color: "#999999"
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.hasUi ? (root.ui.importPreview.telemetrySseUrl || "") : ""
                        color: "#b0b0b0"
                        elide: Text.ElideMiddle
                    }
                }

                Text {
                    text: "Applying will replace the current input sources."
                    color: "#ffcc80"
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ListView {
                        id: importFeedsList
                        width: parent.width
                        model: root.hasUi ? (root.ui.importPreview.feeds || []) : []
                        spacing: 8
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: Rectangle {
                            id: importFeedRow
                            required property var modelData
                            width: importFeedsList.width
                            height: feedLayout.implicitHeight + 16
                            color: "#181818"
                            border.color: "#333"
                            border.width: 1
                            radius: 4

                            ColumnLayout {
                                id: feedLayout
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 4

                                Text {
                                    Layout.fillWidth: true
                                    text: {
                                        var name = importFeedRow.modelData.name || "Unnamed feed"
                                        var idText = importFeedRow.modelData.id || ""
                                        return idText.length > 0 ? (name + " · " + idText) : name
                                    }
                                    color: "#eeeeee"
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: importFeedRow.modelData.url || ""
                                    color: "#aaaaaa"
                                    elide: Text.ElideMiddle
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "telemetryDelayMs " + (importFeedRow.modelData.telemetryDelayMs || 0)
                                    color: "#888888"
                                    elide: Text.ElideRight
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: importPreviewPopup.metadataSummary(importFeedRow.modelData.metadata)
                                    color: "#777777"
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "Cancel"
                        onClicked: importPreviewPopup.close()
                    }

                    Button {
                        text: "Apply"
                        enabled: root.hasUi
                                 && root.ui.importPreviewReady
                                 && !root.ui.isRecording
                        onClicked: {
                            if (root.hasUi) root.ui.applyImportPreview()
                            importPreviewPopup.close()
                        }
                    }
                }
            }
        }
    }

    // Global Metadata Fields Editor
    Popup {
        id: metadataFieldsEditor
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // qmllint disable Quick.layout-positioning
        anchors.centerIn: Overlay.overlay ? Overlay.overlay : root
        width: Math.min(root.popupWidthBase * 0.9, 600)
        height: Math.min(root.popupHeightBase * 0.8, 480)
        // qmllint enable Quick.layout-positioning

        ListModel {
            id: fieldsModel
        }

        function openEditor() {
            fieldsModel.clear()
            var defs = root.hasUi ? root.ui.metadataFieldDefinitions() : []
            if (defs && defs.length > 0) {
                for (var i = 0; i < defs.length; ++i) {
                    var d = defs[i]
                    fieldsModel.append({
                        name: d.name || "",
                        display: d.display !== undefined ? d.display : true
                    })
                }
            } else {
                fieldsModel.append({ name: "", display: true })
            }
            open()
        }

        function buildFields() {
            var result = []
            for (var i = 0; i < fieldsModel.count; ++i) {
                var row = fieldsModel.get(i)
                var name = (row.name || "").trim()
                if (name.length === 0) continue
                result.push({
                    name: name,
                    display: row.display !== undefined ? row.display : true
                })
            }
            return result
        }

        contentItem: Rectangle {
            color: "#1f1f1f"
            radius: 0
            border.color: "#333"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: "Metadata Field Definitions"
                        color: "#eeeeee"
                        font.pixelSize: 18
                        font.bold: true
                        Layout.fillWidth: true
                    }

                    Button {
                        text: "+ Field"
                        onClicked: fieldsModel.append({ name: "", display: true })
                    }
                }

                Text {
                    text: "Define the metadata fields available for all sources. Each source can then fill in its own values."
                    color: "#b0b0b0"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ListView {
                        id: fieldsList
                        model: fieldsModel
                        spacing: 8
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: RowLayout {
                            id: fieldRow
                            required property string name
                            required property bool display
                            required property int index
                            width: fieldsList.width
                            spacing: 8

                            TextField {
                                Layout.fillWidth: true
                                placeholderText: "Field name"
                                text: fieldRow.name
                                onTextEdited: fieldsModel.setProperty(fieldRow.index, "name", text)
                            }

                            CheckBox {
                                text: "Show"
                                checked: fieldRow.display
                                onToggled: fieldsModel.setProperty(fieldRow.index, "display", checked)
                            }

                            Button {
                                text: "Remove"
                                onClicked: fieldsModel.remove(fieldRow.index)
                                enabled: fieldsModel.count > 1
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "Cancel"
                        onClicked: metadataFieldsEditor.close()
                    }

                    Button {
                        text: "Save"
                        onClicked: {
                            if (root.hasUi) {
                                root.ui.setMetadataFieldDefinitions(metadataFieldsEditor.buildFields())
                                root.ui.saveSettings()
                            }
                            metadataFieldsEditor.close()
                        }
                    }
                }
            }
        }
    }

    // Per-Source Metadata Values Editor
    Popup {
        id: metadataEditor
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        // qmllint disable Quick.layout-positioning
        anchors.centerIn: Overlay.overlay ? Overlay.overlay : root
        width: Math.min(root.popupWidthBase * 0.9, 600)
        height: Math.min(root.popupHeightBase * 0.8, 480)
        // qmllint enable Quick.layout-positioning

        property int sourceIndex: -1
        property string sourceLabel: ""

        ListModel {
            id: metadataModel
        }

        function openFor(index) {
            sourceIndex = index
            var label = "Source " + (index + 1)
            if (root.hasUi
                    && root.ui.streamNames.length > index
                    && root.ui.streamNames[index].length > 0) {
                label = root.ui.streamNames[index]
            }
            sourceLabel = label
            metadataModel.clear()
            var items = root.hasUi ? root.ui.sourceMetadataItems(index) : []
            if (items && items.length > 0) {
                for (var i = 0; i < items.length; ++i) {
                    var row = items[i]
                    metadataModel.append({
                        name: row.name || "",
                        value: row.value || ""
                    })
                }
            }
            open()
        }

        function buildItems() {
            var result = []
            for (var i = 0; i < metadataModel.count; ++i) {
                var row = metadataModel.get(i)
                var name = (row.name || "").trim()
                if (name.length === 0) continue
                result.push({
                    name: name,
                    value: row.value === undefined ? "" : String(row.value)
                })
            }
            return result
        }

        contentItem: Rectangle {
            color: "#1f1f1f"
            radius: 0
            border.color: "#333"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    text: "Metadata — " + metadataEditor.sourceLabel
                    color: "#eeeeee"
                    font.pixelSize: 18
                    font.bold: true
                    Layout.fillWidth: true
                }

                Text {
                    text: metadataModel.count > 0
                          ? "Fill in values for this source. Fields are defined in Metadata Fields."
                          : "No metadata fields defined. Use the Metadata Fields button to add fields first."
                    color: "#b0b0b0"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true

                    ListView {
                        id: metadataList
                        model: metadataModel
                        spacing: 8
                        boundsBehavior: Flickable.StopAtBounds

                        delegate: RowLayout {
                            id: metaRow
                            required property string name
                            required property string value
                            required property int index
                            width: metadataList.width
                            spacing: 8

                            Label {
                                text: metaRow.name
                                Layout.preferredWidth: 180
                                color: "#cccccc"
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            TextField {
                                Layout.fillWidth: true
                                placeholderText: "Value"
                                text: metaRow.value
                                onTextEdited: metadataModel.setProperty(metaRow.index, "value", text)
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "Cancel"
                        onClicked: metadataEditor.close()
                    }

                    Button {
                        text: "Save"
                        onClicked: {
                            if (root.hasUi && metadataEditor.sourceIndex >= 0) {
                                root.ui.setSourceMetadataItems(
                                    metadataEditor.sourceIndex,
                                    metadataEditor.buildItems()
                                )
                                root.ui.saveSettings()
                            }
                            metadataEditor.close()
                        }
                    }
                }
            }
        }
    }
}
