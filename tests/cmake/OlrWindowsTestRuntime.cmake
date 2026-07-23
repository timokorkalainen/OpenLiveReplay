if(WIN32)
    get_filename_component(_olr_ffmpeg_libdir "${OLR_FFMPEG_AVFORMAT_LIBRARY}" DIRECTORY)
    get_filename_component(_olr_ffmpeg_runtime_dir "${_olr_ffmpeg_libdir}/../bin" ABSOLUTE)
    get_filename_component(_olr_srt_libdir "${OLR_SRT_LIBRARY}" DIRECTORY)
    get_filename_component(_olr_srt_runtime_dir "${_olr_srt_libdir}/../bin" ABSOLUTE)
    set(OLR_FFMPEG_TEST_RUNTIME_DIR "${_olr_ffmpeg_runtime_dir}" CACHE INTERNAL "")
    set(OLR_SRT_TEST_RUNTIME_DIR "${_olr_srt_runtime_dir}" CACHE INTERNAL "")
    set(OLR_QT_TEST_PLUGIN_DIR
        "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}" CACHE INTERNAL "")
endif()

function(olr_apply_windows_test_runtime)
    if(WIN32)
        foreach(test_name IN LISTS ARGN)
            get_property(runtime_applied TEST ${test_name} PROPERTY OLR_WINDOWS_RUNTIME_APPLIED)
            if(runtime_applied)
                if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
                    get_property(current_modifications TEST ${test_name}
                        PROPERTY ENVIRONMENT_MODIFICATION)
                    string(FIND "${current_modifications}"
                        "PATH=path_list_prepend:${OLR_SRT_TEST_RUNTIME_DIR}" srt_offset)
                    string(FIND "${current_modifications}"
                        "PATH=path_list_prepend:${OLR_FFMPEG_TEST_RUNTIME_DIR}" ffmpeg_offset)
                    string(FIND "${current_modifications}"
                        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>" qt_offset)
                    string(FIND "${current_modifications}"
                        "QT_PLUGIN_PATH=path_list_prepend:${OLR_QT_TEST_PLUGIN_DIR}"
                        plugin_offset)
                    if(NOT srt_offset EQUAL -1 AND NOT ffmpeg_offset EQUAL -1 AND
                       NOT qt_offset EQUAL -1 AND NOT plugin_offset EQUAL -1)
                        continue()
                    endif()
                else()
                    get_property(current_environment TEST ${test_name} PROPERTY ENVIRONMENT)
                    string(FIND "${current_environment}" "${OLR_SRT_TEST_RUNTIME_DIR}"
                        srt_offset)
                    string(FIND "${current_environment}" "${OLR_FFMPEG_TEST_RUNTIME_DIR}"
                        ffmpeg_offset)
                    string(FIND "${current_environment}" "$<TARGET_FILE_DIR:Qt6::Core>"
                        qt_offset)
                    string(FIND "${current_environment}" "${OLR_QT_TEST_PLUGIN_DIR}"
                        plugin_offset)
                    if(NOT srt_offset EQUAL -1 AND NOT ffmpeg_offset EQUAL -1 AND
                       NOT qt_offset EQUAL -1 AND NOT plugin_offset EQUAL -1)
                        continue()
                    endif()
                endif()
            endif()
            if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
                set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${OLR_SRT_TEST_RUNTIME_DIR}"
                    "PATH=path_list_prepend:${OLR_FFMPEG_TEST_RUNTIME_DIR}"
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                    "QT_PLUGIN_PATH=path_list_prepend:${OLR_QT_TEST_PLUGIN_DIR}")
            else()
                set(runtime_path
                    "$<TARGET_FILE_DIR:Qt6::Core>;${OLR_FFMPEG_TEST_RUNTIME_DIR};${OLR_SRT_TEST_RUNTIME_DIR};$ENV{PATH}")
                string(REPLACE ";" "\\;" runtime_path "${runtime_path}")
                set(plugin_path "${OLR_QT_TEST_PLUGIN_DIR}")
                if(NOT "$ENV{QT_PLUGIN_PATH}" STREQUAL "")
                    string(APPEND plugin_path ";$ENV{QT_PLUGIN_PATH}")
                endif()
                string(REPLACE ";" "\\;" plugin_path "${plugin_path}")
                set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
                    "PATH=${runtime_path}"
                    "QT_PLUGIN_PATH=${plugin_path}")
            endif()
            set_property(TEST ${test_name} PROPERTY OLR_WINDOWS_RUNTIME_APPLIED TRUE)
        endforeach()
    endif()
endfunction()

function(olr_add_ctest)
    cmake_parse_arguments(ARG "" "NAME" "COMMAND" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_COMMAND)
        message(FATAL_ERROR "olr_add_ctest requires NAME and COMMAND")
    endif()

    add_test(NAME ${ARG_NAME} COMMAND ${ARG_COMMAND})
    olr_apply_windows_test_runtime(${ARG_NAME})
endfunction()
