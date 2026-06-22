# Verifies the bespoke OlrStyle custom QQC2 style loads + applies the Theme tokens.
# Kept in its own dir (not under tests/qml) so the other QuickTest exe — which scans
# its -input dir recursively for tst_*.qml — does not try to run it without the style
# modules linked.
find_package(Qt6 REQUIRED COMPONENTS QuickTest Qml Quick Gui Test QuickControls2)

qt_add_executable(tst_olrstyle tst_olrstyle_main.cpp)
target_link_libraries(tst_olrstyle PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

add_test(NAME tst_olrstyle
    COMMAND tst_olrstyle -input "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(tst_olrstyle PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_statusstrip_onair tst_statusstrip_onair_main.cpp)
target_link_libraries(tst_statusstrip_onair PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

add_test(NAME tst_statusstrip_onair
    COMMAND tst_statusstrip_onair -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_statusstrip_onair.qml")
set_tests_properties(tst_statusstrip_onair PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_rundownrail_drag tst_rundownrail_drag_main.cpp)
target_link_libraries(tst_rundownrail_drag PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

add_test(NAME tst_rundownrail_drag
    COMMAND tst_rundownrail_drag -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_rundownrail_drag.qml")
set_tests_properties(tst_rundownrail_drag PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

# Dev-only headless preview renderer (no add_test): renders a QML file under OlrStyle
# to a PNG for visual review. Links the style modules so it never dlopens a plugin.
qt_add_executable(uipreview uipreview_main.cpp)
target_link_libraries(uipreview PRIVATE
    Qt6::Qml Qt6::Quick Qt6::Gui Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

# Linux ld defaults to --as-needed and drops the OlrTheme/OlrStyle backing libraries —
# they are referenced only through a runtime QML import (no C++ symbol), so their QML
# modules never self-register and the engine reports "module not installed". Keep them
# linked, matching macOS/ld64 (which retains them; reproduce the drop there with
# -dead_strip_dylibs).
if(UNIX AND NOT APPLE)
    target_link_options(tst_olrstyle PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_statusstrip_onair PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_rundownrail_drag PRIVATE "LINKER:--no-as-needed")
    target_link_options(uipreview PRIVATE "LINKER:--no-as-needed")
endif()
