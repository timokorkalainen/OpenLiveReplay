# QtQuickTest: QML unit tests for app components (controller-injected, no app singleton).
find_package(Qt6 REQUIRED COMPONENTS QuickTest Qml Quick Gui Test)

qt_add_executable(tst_qml_codecsettings tst_qml_main.cpp)
target_link_libraries(tst_qml_codecsettings PRIVATE
    Qt6::QuickTest Qt6::Qml Qt6::Quick Qt6::Gui Qt6::Test olr_warnings)

olr_add_ctest(NAME tst_qml_codecsettings
    COMMAND $<TARGET_FILE:tst_qml_codecsettings> -input "${CMAKE_CURRENT_SOURCE_DIR}")
set(qt_test_path_modifications
    "QML_IMPORT_PATH=path_list_append:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}"
    "QML2_IMPORT_PATH=path_list_append:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_QML}")
set_tests_properties(tst_qml_codecsettings PROPERTIES
    LABELS "unit;ci"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QML_IMPORT_PATH=${CMAKE_SOURCE_DIR}")
set_property(TEST tst_qml_codecsettings APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "${qt_test_path_modifications}")
