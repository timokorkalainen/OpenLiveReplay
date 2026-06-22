import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../ui/components"
import OlrTheme

Rectangle {
    width: 1120
    height: 680
    color: Theme.canvas

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        StatusStrip {
            Layout.fillWidth: true
        }

        PgmStage {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        TransportDock {
            Layout.fillWidth: true
        }
    }

    ConfigDrawer {
        id: drawer
        parent: parent
    }
}
