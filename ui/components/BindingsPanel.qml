pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import OlrTheme

ColumnLayout {
    id: root

    property var ui
    readonly property bool hasUi: root.ui !== undefined && root.ui !== null
    readonly property var streamDeck: root.hasUi ? root.ui.streamDeck : null
    readonly property bool hasStreamDeck: root.streamDeck !== undefined && root.streamDeck !== null

    Layout.fillWidth: true
    spacing: 12

    Frame {
        id: midiCard
        Layout.fillWidth: true
        property bool expanded: false

        background: Rectangle {
            color: Theme.panelRaised
            radius: 6
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            Layout.fillWidth: true
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: "MIDI Controller"
                    color: Theme.textHi
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter
                }

                Item { Layout.fillWidth: true }

                ToolButton {
                    text: midiCard.expanded ? "▾" : "▸"
                    onClicked: midiCard.expanded = !midiCard.expanded
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 12
                visible: midiCard.expanded

                GroupBox {
                    title: "MIDI"
                    Layout.fillWidth: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ComboBox {
                            Layout.fillWidth: true
                            model: root.hasUi ? root.ui.midiPorts : []
                            currentIndex: root.hasUi ? root.ui.midiPortIndex : -1
                            onActivated: if (root.hasUi) root.ui.setMidiPortIndex(currentIndex)
                        }

                        Button {
                            text: "Refresh"
                            onClicked: if (root.hasUi) root.ui.refreshMidiPorts()
                        }

                        Text {
                            text: root.hasUi && root.ui.midiConnected ? "Connected" : "Disconnected"
                            color: root.hasUi && root.ui.midiConnected ? Theme.ready : Theme.textDim
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                GroupBox {
                    title: "MIDI Mapping"
                    Layout.fillWidth: true

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Repeater {
                            model: [
                                { name: "Play/Pause", action: 0 },
                                { name: "Rewind 5.0x", action: 1 },
                                { name: "Forward 5.0x", action: 2 },
                                { name: "Prev Frame", action: 7 },
                                { name: "Next Frame", action: 3 },
                                { name: "Jogwheel", action: 8 },
                                { name: "Go Live", action: 4 },
                                { name: "Capture", action: 5 },
                                { name: "Multiview", action: 6 },
                                { name: "Feed 1", action: 100 },
                                { name: "Feed 2", action: 101 },
                                { name: "Feed 3", action: 102 },
                                { name: "Feed 4", action: 103 },
                                { name: "Feed 5", action: 104 },
                                { name: "Feed 6", action: 105 },
                                { name: "Feed 7", action: 106 },
                                { name: "Feed 8", action: 107 }
                            ]

                            delegate: RowLayout {
                                id: midiRow
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: midiRow.modelData.name
                                    Layout.preferredWidth: 130
                                    color: Theme.textHi
                                }

                                Text {
                                    text: root.hasUi && root.ui.midiBindingsVersion >= 0
                                          ? root.ui.midiBindingLabel(midiRow.modelData.action)
                                          : (root.hasUi ? root.ui.midiBindingLabel(midiRow.modelData.action) : "")
                                    color: root.hasUi && root.ui.midiLearnAction === midiRow.modelData.action ? Theme.armed : Theme.textBody
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "Last: " + (root.hasUi && root.ui.midiLastValuesVersion >= 0
                                                      ? root.ui.midiLastValue(midiRow.modelData.action)
                                                      : (root.hasUi ? root.ui.midiLastValue(midiRow.modelData.action) : ""))
                                    color: Theme.textDim
                                    Layout.preferredWidth: 80
                                }

                                Button {
                                    visible: midiRow.modelData.action !== 8
                                    text: root.hasUi && root.ui.midiLearnAction === midiRow.modelData.action ? "Listening..." : "Learn"
                                    onClicked: if (root.hasUi) root.ui.beginMidiLearn(midiRow.modelData.action)
                                }

                                Button {
                                    visible: midiRow.modelData.action === 8
                                    text: (root.hasUi && root.ui.midiLearnAction === midiRow.modelData.action && root.ui.midiLearnMode === 0)
                                          ? "Listening..."
                                          : "Learn Ctrl"
                                    onClicked: if (root.hasUi) root.ui.beginMidiLearn(midiRow.modelData.action)
                                }

                                Button {
                                    visible: midiRow.modelData.action === 8
                                    text: (root.hasUi && root.ui.midiLearnAction === midiRow.modelData.action && root.ui.midiLearnMode === 1)
                                          ? "Listening..."
                                          : "Learn Fwd"
                                    onClicked: if (root.hasUi) root.ui.beginMidiLearnJogForward(midiRow.modelData.action)
                                }

                                Button {
                                    visible: midiRow.modelData.action === 8
                                    text: (root.hasUi && root.ui.midiLearnAction === midiRow.modelData.action && root.ui.midiLearnMode === 2)
                                          ? "Listening..."
                                          : "Learn Back"
                                    onClicked: if (root.hasUi) root.ui.beginMidiLearnJogBackward(midiRow.modelData.action)
                                }

                                Button {
                                    text: "Clear"
                                    onClicked: if (root.hasUi) root.ui.clearMidiBinding(midiRow.modelData.action)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Frame {
        id: streamDeckCard
        Layout.fillWidth: true
        property bool expanded: false
        visible: !root.hasUi || (root.hasStreamDeck && root.streamDeck.supported)

        background: Rectangle {
            color: Theme.panelRaised
            radius: 6
            border.color: Theme.line
            border.width: 1
        }

        contentItem: ColumnLayout {
            Layout.fillWidth: true
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Text {
                    text: "Stream Deck"
                    color: Theme.textHi
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: root.hasStreamDeck && root.streamDeck.connected
                          ? root.streamDeck.deviceName
                          : "Disconnected"
                    color: root.hasStreamDeck && root.streamDeck.connected ? Theme.ready : Theme.textDim
                    verticalAlignment: Text.AlignVCenter
                }

                ToolButton {
                    text: streamDeckCard.expanded ? "▾" : "▸"
                    onClicked: streamDeckCard.expanded = !streamDeckCard.expanded
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 12
                visible: streamDeckCard.expanded

                GroupBox {
                    title: "Stream Deck"
                    Layout.fillWidth: true

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Text {
                            text: root.hasStreamDeck && root.streamDeck.connected
                                  ? "Connected: " + root.streamDeck.deviceName
                                    + " (" + root.streamDeck.deviceModel + ")"
                                  : "No Stream Deck connected"
                            color: root.hasStreamDeck && root.streamDeck.connected ? Theme.ready : Theme.textBody
                        }

                        Text {
                            visible: root.hasStreamDeck && !root.streamDeck.driverAppInstalled
                            text: "Install “Elgato Stream Deck Connect” from the App Store and enable the Stream Deck Device Driver in the iPadOS Settings app, then connect the deck via USB-C."
                            color: Theme.armed
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Button {
                            visible: root.hasStreamDeck && root.streamDeck.simulatorAvailable
                            text: "Show Stream Deck Simulator"
                            onClicked: if (root.hasUi) root.ui.streamDeck.showSimulator()
                        }
                    }
                }

                GroupBox {
                    title: "Button Mapping"
                    Layout.fillWidth: true
                    visible: root.hasStreamDeck && root.streamDeck.connected

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: "Click Learn, then press a key or turn/press a dial."
                                    color: Theme.textBody
                                    Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                            Button {
                                text: "Reset to default"
                                onClicked: if (root.hasUi) root.ui.resetStreamDeckDefaults()
                            }
                        }

                        Repeater {
                            model: [
                                { name: "Record",      action: 9,  gesture: "key or dial" },
                                { name: "Play/Pause",  action: 0,  gesture: "key or dial" },
                                { name: "Go Live",     action: 4,  gesture: "key or dial" },
                                { name: "Capture",     action: 5,  gesture: "key or dial" },
                                { name: "Prev Frame",  action: 7,  gesture: "key or dial" },
                                { name: "Next Frame",  action: 3,  gesture: "key or dial" },
                                { name: "Rewind 5×",   action: 1,  gesture: "key or dial" },
                                { name: "Forward 5×",  action: 2,  gesture: "key or dial" },
                                { name: "Multiview",   action: 6,  gesture: "key or dial" },
                                { name: "Jog",         action: 8,  gesture: "turn a dial" },
                                { name: "Shuttle",     action: 10, gesture: "turn a dial" },
                                { name: "Timecode",    action: 20, gesture: "press a key" },
                                { name: "Speed",       action: 21, gesture: "press a key" },
                                { name: "Feed 1",      action: 100, gesture: "key or dial" },
                                { name: "Feed 2",      action: 101, gesture: "key or dial" },
                                { name: "Feed 3",      action: 102, gesture: "key or dial" },
                                { name: "Feed 4",      action: 103, gesture: "key or dial" },
                                { name: "Feed 5",      action: 104, gesture: "key or dial" },
                                { name: "Feed 6",      action: 105, gesture: "key or dial" },
                                { name: "Feed 7",      action: 106, gesture: "key or dial" },
                                { name: "Feed 8",      action: 107, gesture: "key or dial" }
                            ]

                            delegate: RowLayout {
                                id: sdRow
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    text: sdRow.modelData.name
                                    color: Theme.textHi
                                    Layout.preferredWidth: 110
                                }
                                Text {
                                    text: root.hasUi && root.ui.streamDeckBindingsVersion >= 0
                                          ? root.ui.streamDeckBindingLabel(sdRow.modelData.action)
                                          : ""
                                    color: root.hasUi && root.ui.streamDeckLearnAction === sdRow.modelData.action
                                           ? Theme.armed : Theme.textBody
                                    Layout.fillWidth: true
                                }
                                Button {
                                    text: root.hasUi && root.ui.streamDeckLearnAction === sdRow.modelData.action
                                          ? "Listening… (" + sdRow.modelData.gesture + ")"
                                          : "Learn"
                                    onClicked: if (root.hasUi) root.ui.beginStreamDeckLearn(sdRow.modelData.action)
                                }
                                Button {
                                    text: "Clear"
                                    onClicked: if (root.hasUi) root.ui.clearStreamDeckBinding(sdRow.modelData.action)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
