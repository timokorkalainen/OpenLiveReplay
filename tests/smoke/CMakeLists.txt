# QML smoke test.
#
# Runs qmllint over the app's QML. qmllint exits non-zero only on hard ERRORS
# (syntax, unresolved imports/types) — not on the project's pre-existing style
# warnings (Quick.layout-positioning, unqualified context-property access) —
# which is precisely the "does the QML still load and resolve" smoke semantics
# we want in the test suite. Stricter, warning-level enforcement lives in the
# dedicated CI lint job.

get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
find_program(OLR_QMLLINT NAMES qmllint HINTS "${_qt_prefix}/bin")

if(OLR_QMLLINT)
    add_test(NAME qml_smoke COMMAND "${OLR_QMLLINT}"
        -I "${_qt_prefix}/qml"
        "${CMAKE_SOURCE_DIR}/Main.qml"
        "${CMAKE_SOURCE_DIR}/MultiviewWindow.qml")
    set_tests_properties(qml_smoke PROPERTIES LABELS "smoke" TIMEOUT 60)
    message(STATUS "OpenLiveReplay tests: qml_smoke using ${OLR_QMLLINT}")
else()
    message(WARNING "qmllint not found under ${_qt_prefix}/bin; qml_smoke test disabled")
endif()

add_test(NAME ios_ffmpeg_build_config
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check_ios_ffmpeg_build_config.sh")
set_tests_properties(ios_ffmpeg_build_config PROPERTIES LABELS "smoke" TIMEOUT 10)
