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

    edge: Qt.RightEdge
    width: Math.min(root.parent ? root.parent.width : Theme.bpSM, Theme.bpSM * 1.4)
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
                contentHeight: sourcesPanel.height
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                SourceListPanel {
                    id: sourcesPanel

                    width: sourcesScroll.availableWidth
                    height: sourcesScroll.availableHeight
                    ui: root.ui
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
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                ProjectSettingsPanel {
                    id: projectPanel

                    width: projectScroll.availableWidth
                    ui: root.ui
                    previewMode: !root.hasUi
                    onBrowseFolderRequested: root.browseFolderRequested()
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
    }
}
