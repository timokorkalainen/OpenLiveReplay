# QtQuickTest: QML unit tests for app components (controller-injected, no app singleton).
find_package(Qt6 REQUIRED COMPONENTS QuickTest Qml Quick Gui Test)

qt_add_executable(tst_qml_codecsettings tst_qml_main.cpp)
target_link_libraries(tst_qml_codecsettings PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test olr_warnings)

add_test(NAME tst_qml_codecsettings
    COMMAND tst_qml_codecsettings -input "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(tst_qml_codecsettings PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_SOURCE_DIR}")
