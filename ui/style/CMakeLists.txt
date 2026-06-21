# OlrStyle — bespoke QQC2 custom style. Selected at runtime via QQuickStyle::setStyle
# ("OlrStyle"); the app does NOT import it (so it never shadows QtQuick.Controls types).
# Control delegates that aren't provided here fall back to the Basic style.
qt_add_qml_module(OlrStyle
    URI OlrStyle
    VERSION 1.0
    QML_FILES
        Button.qml
        ToolButton.qml
        Label.qml
        TextField.qml
        ComboBox.qml
        ItemDelegate.qml
        SpinBox.qml
        CheckBox.qml
        Switch.qml
        Slider.qml
        ScrollBar.qml
        GroupBox.qml
        Frame.qml
        TabButton.qml
        TabBar.qml
        ToolTip.qml
        Popup.qml
        Menu.qml
        MenuItem.qml
)

# The delegates import OlrTheme for tokens; link the backing module so it is on the
# build + runtime QML import path.
target_link_libraries(OlrStyle PRIVATE OlrTheme)
