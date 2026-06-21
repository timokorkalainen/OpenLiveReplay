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
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Dev-only headless preview renderer (no add_test): renders a QML file under OlrStyle
# to a PNG for visual review. Links the style modules so it never dlopens a plugin.
qt_add_executable(uipreview uipreview_main.cpp)
target_link_libraries(uipreview PRIVATE
    Qt6::Qml Qt6::Quick Qt6::Gui Qt6::QuickControls2
    OlrTheme OlrStyle olr_warnings)
