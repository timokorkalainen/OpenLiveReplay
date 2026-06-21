# OlrTheme — design-token singleton module. Imported by both the app and the OlrStyle
# custom style; depends on nothing, so there is no module cycle.
set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(OlrTheme
    URI OlrTheme
    VERSION 1.0
    QML_FILES
        Theme.qml
)
