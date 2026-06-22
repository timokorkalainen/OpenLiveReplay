pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

Drawer {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    signal browseFolderRequested()

    function openImportPreview() {
        sourcesPanel.openImportPreview()
    }

    function maybeAutoOpenImport() {
        sourcesPanel.maybeAutoOpenImport()
    }

    edge: Qt.RightEdge
    width: root.parent && root.parent.width < Theme.bpSM
           ? root.parent.width
           : Math.min(root.parent ? root.parent.width : Theme.bpSM, Theme.bpSM * 1.4)
    height: root.parent ? root.parent.height : 0
    modal: true
    dim: true

    background: Rectangle {
        color: Theme.panel
        border.color: Theme.line
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 0

        TabBar {
            id: sectionTabs

            Layout.fillWidth: true

            TabButton { text: "Sources" }
            TabButton { text: "Outputs" }
            TabButton { text: "Project" }
            TabButton { text: "Bindings" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sectionTabs.currentIndex

            ScrollView {
                id: sourcesScroll

                clip: true
                contentWidth: sourcesScroll.availableWidth
                contentHeight: sourcesContent.implicitHeight
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                ColumnLayout {
                    id: sourcesContent

                    width: sourcesScroll.availableWidth
                    spacing: Theme.s2

                    GroupBox {
                        id: externalInputSettings

                        title: "External Input Settings"
                        Layout.fillWidth: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: Theme.s2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.s2

                                TextField {
                                    id: importSettingsUrlField

                                    Layout.fillWidth: true
                                    text: root.hasUi ? root.ui.importSettingsUrl : ""
                                    placeholderText: "https://provider.example/project-settings.json"
                                    enabled: root.hasUi && !root.ui.isRecording
                                    onEditingFinished: if (root.hasUi) root.ui.importSettingsUrl = text
                                }

                                Button {
                                    text: "Read"
                                    enabled: root.hasUi && !root.ui.isRecording
                                    onClicked: {
                                        root.ui.importSettingsUrl = importSettingsUrlField.text
                                        root.ui.readImportSettings()
                                    }
                                }
                            }

                            Text {
                                visible: root.hasUi && root.ui.importPreviewError !== ""
                                text: root.hasUi ? root.ui.importPreviewError : ""
                                color: Theme.warning
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.s2

                                Button {
                                    visible: root.hasUi && root.ui.importPreviewReady
                                    text: "Preview Imported Sources"
                                    enabled: root.hasUi && root.ui.importPreviewReady
                                    onClicked: root.openImportPreview()
                                }

                                Text {
                                    visible: root.hasUi && root.ui.importPreviewReady
                                    text: {
                                        if (!root.hasUi) return ""
                                        var p = root.ui.importPreview
                                        var count = p.feedCount !== undefined ? p.feedCount : ((p.feeds || []).length)
                                        return count + " imported feed" + (count === 1 ? "" : "s") + " ready"
                                    }
                                    color: Theme.ready
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    SourceListPanel {
                        id: sourcesPanel

                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(220, sourcesScroll.availableHeight - externalInputSettings.implicitHeight - Theme.s2)
                        ui: root.ui
                    }
                }
            }

            ScrollView {
                id: outputsScroll

                clip: true
                contentWidth: outputsScroll.availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                OutputsPanel {
                    id: outputsPanel

                    width: outputsScroll.availableWidth
                    ui: root.ui
                }
            }

            ScrollView {
                id: projectScroll

                clip: true
                contentWidth: projectScroll.availableWidth
                contentHeight: projectContent.implicitHeight
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                ColumnLayout {
                    id: projectContent

                    width: projectScroll.availableWidth
                    spacing: Theme.s2

                    GroupBox {
                        title: "View Layout"
                        Layout.fillWidth: true

                        RowLayout {
                            anchors.fill: parent
                            spacing: Theme.s2

                            Label {
                                text: "Multiview Views"
                                Layout.fillWidth: true
                            }

                            SpinBox {
                                from: 1
                                to: 16
                                stepSize: 1
                                editable: true
                                inputMethodHints: Qt.ImhDigitsOnly
                                value: root.hasUi ? root.ui.multiviewCount : 4
                                enabled: root.hasUi && !root.ui.isRecording
                                onValueModified: if (root.hasUi) root.ui.multiviewCount = value
                            }
                        }
                    }

                    ProjectSettingsPanel {
                        id: projectPanel

                        Layout.fillWidth: true
                        ui: root.ui
                        previewMode: !root.hasUi
                        onBrowseFolderRequested: root.browseFolderRequested()
                    }
                }
            }

            ScrollView {
                id: bindingsScroll

                clip: true
                contentWidth: bindingsScroll.availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                BindingsPanel {
                    id: bindingsPanel

                    width: bindingsScroll.availableWidth
                    ui: root.ui
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.s2
            spacing: Theme.s2

            Button {
                text: "Save Config"
                enabled: root.hasUi
                onClicked: root.ui.saveSettings()
            }

            Item { Layout.fillWidth: true }
        }
    }
}
