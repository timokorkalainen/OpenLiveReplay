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
        set_property(TEST ${ARGN} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${OLR_SRT_TEST_RUNTIME_DIR}"
            "PATH=path_list_prepend:${OLR_FFMPEG_TEST_RUNTIME_DIR}"
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            "QT_PLUGIN_PATH=path_list_append:${OLR_QT_TEST_PLUGIN_DIR}")
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
