# QML lint gate (mirrors the CI "qmllint" job so local == CI).
#
# Runs qmllint over EVERY QML file in the module. qmllint exits non-zero only on
# hard ERRORS by default, so the high-signal categories (unqualified access and
# layout-positioning misuse) are promoted to errors to make them a real gate;
# known false positives are suppressed inline with `// qmllint disable`.

get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
find_program(OLR_QMLLINT NAMES qmllint HINTS "${_qt_prefix}/bin")

if(OLR_QMLLINT)
    add_test(NAME qml_smoke COMMAND "${OLR_QMLLINT}"
        --unqualified error --Quick.layout-positioning error
        -I "${_qt_prefix}/qml"
        "${CMAKE_SOURCE_DIR}/Main.qml"
        "${CMAKE_SOURCE_DIR}/MultiviewWindow.qml"
        "${CMAKE_SOURCE_DIR}/CodecSettingsPanel.qml")
    set_tests_properties(qml_smoke PROPERTIES LABELS "smoke;ci" TIMEOUT 60)
    message(STATUS "OpenLiveReplay tests: qml_smoke using ${OLR_QMLLINT}")
else()
    message(WARNING "qmllint not found under ${_qt_prefix}/bin; qml_smoke test disabled")
endif()

add_test(NAME ios_ffmpeg_build_config
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check_ios_ffmpeg_build_config.sh")
set_tests_properties(ios_ffmpeg_build_config PROPERTIES LABELS "smoke;ci" TIMEOUT 10)
