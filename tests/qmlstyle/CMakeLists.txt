# Verifies the bespoke OlrStyle custom QQC2 style loads + applies the Theme tokens.
# Kept in its own dir (not under tests/qml) so the other QuickTest exe — which scans
# its -input dir recursively for tst_*.qml — does not try to run it without the style
# modules linked.
find_package(Qt6 REQUIRED COMPONENTS QuickTest Qml Quick Gui Test QuickControls2)

qt_add_executable(tst_olrstyle tst_olrstyle_main.cpp)
target_link_libraries(tst_olrstyle PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_olrstyle
    COMMAND $<TARGET_FILE:tst_olrstyle> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_olrstyle.qml")
set_tests_properties(tst_olrstyle PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_statusstrip_onair tst_statusstrip_onair_main.cpp)
target_link_libraries(tst_statusstrip_onair PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_statusstrip_onair
    COMMAND $<TARGET_FILE:tst_statusstrip_onair> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_statusstrip_onair.qml")
set_tests_properties(tst_statusstrip_onair PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_pgmstage_mapping
    tst_pgmstage_mapping_main.cpp
    ../../playback/framepreviewitem.cpp
    ../../playback/framepreviewitem.h)
target_link_libraries(tst_pgmstage_mapping PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2 Qt6::Multimedia
    OlrTheme OlrStyle olr_test_playback olr_warnings)

olr_add_ctest(NAME tst_pgmstage_mapping
    COMMAND $<TARGET_FILE:tst_pgmstage_mapping> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_pgmstage_mapping.qml")
set_tests_properties(tst_pgmstage_mapping PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_rundownrail_drag tst_rundownrail_drag_main.cpp)
target_link_libraries(tst_rundownrail_drag PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_rundownrail_drag
    COMMAND $<TARGET_FILE:tst_rundownrail_drag> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_rundownrail_drag.qml")
set_tests_properties(tst_rundownrail_drag PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_scrubtimeline tst_scrubtimeline_main.cpp)
target_link_libraries(tst_scrubtimeline PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_scrubtimeline
    COMMAND $<TARGET_FILE:tst_scrubtimeline> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_scrubtimeline.qml")
set_tests_properties(tst_scrubtimeline PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_transportdock_layout tst_transportdock_layout_main.cpp)
target_link_libraries(tst_transportdock_layout PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_transportdock_layout
    COMMAND $<TARGET_FILE:tst_transportdock_layout> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_transportdock_layout.qml")
set_tests_properties(tst_transportdock_layout PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

qt_add_executable(tst_sourcelistpanel_layout tst_sourcelistpanel_layout_main.cpp)
target_link_libraries(tst_sourcelistpanel_layout PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)

olr_add_ctest(NAME tst_sourcelistpanel_layout
    COMMAND $<TARGET_FILE:tst_sourcelistpanel_layout> -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_sourcelistpanel_layout.qml")
set_tests_properties(tst_sourcelistpanel_layout PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml;QML2_IMPORT_PATH=${CMAKE_BINARY_DIR}/qml")

# These tests select the dynamic OlrStyle QQC2 style at runtime. Linking the
# backing QML module does not make its generated MODULE_LIBRARY a build
# dependency, so ensure the plugin is present on the existing QML import path.
set(olr_style_tests
    tst_olrstyle
    tst_statusstrip_onair
    tst_pgmstage_mapping
    tst_rundownrail_drag
    tst_scrubtimeline
    tst_transportdock_layout
    tst_sourcelistpanel_layout)
foreach(style_test IN LISTS olr_style_tests)
    add_dependencies(${style_test} OlrStyleplugin OlrThemeplugin)
endforeach()
set(qt_test_path_modifications
    "QML_IMPORT_PATH=path_list_append:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}"
    "QML2_IMPORT_PATH=path_list_append:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}")
set_property(TEST ${olr_style_tests} APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "${qt_test_path_modifications}")

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
    target_link_options(tst_pgmstage_mapping PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_rundownrail_drag PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_scrubtimeline PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_transportdock_layout PRIVATE "LINKER:--no-as-needed")
    target_link_options(tst_sourcelistpanel_layout PRIVATE "LINKER:--no-as-needed")
    target_link_options(uipreview PRIVATE "LINKER:--no-as-needed")
endif()
