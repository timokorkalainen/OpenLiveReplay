import QtQuick
import "../../ui/components"

Item {
    width: 720
    height: 520

    // ui is omitted: a load/paint smoke. The component guards live bindings when
    // ui is absent, so this verifies layout + style without live data.
    BindingsPanel { anchors.fill: parent }
}
