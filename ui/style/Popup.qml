import QtQuick
import QtQuick.Templates as T
import OlrTheme

// Dark popup background. The app's modal popups set a dark contentItem but no background;
// under the Basic fallback that left a white frame around them (QtQuick Controls ignore
// QGuiApplication's palette). padding:0 lets a filling contentItem cover the surface with
// no geometry shift; the dark surface shows for any popup that doesn't fill itself.
T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: 0

    background: Rectangle {
        color: Theme.panelRaised
        border.width: Theme.borderW
        border.color: Theme.lineStrong
        radius: Theme.r2
    }
}
