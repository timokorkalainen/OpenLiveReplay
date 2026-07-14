include_guard(GLOBAL)

function(olr_configure_windows_test_runtime)
    cmake_parse_arguments(ARG "" "" "TESTS" ${ARGN})
    if(NOT WIN32)
        return()
    endif()

    get_filename_component(_olr_compiler_runtime_dir
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_olr_runtime_dirs
        "${_olr_compiler_runtime_dir}"
        "$<TARGET_FILE_DIR:Qt6::Core>"
        "${OLR_FFMPEG_ROOT}/bin"
        "${OLR_FFMPEG_ROOT}/lib"
        "${OLR_SRT_ROOT}/bin"
        "${OLR_SRT_ROOT}/lib")

    foreach(test_name IN LISTS ARG_TESTS)
        get_property(_olr_test_environment TEST "${test_name}"
            PROPERTY ENVIRONMENT)
        if(NOT "QT_QPA_PLATFORM=offscreen" IN_LIST _olr_test_environment)
            set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen")
        endif()
        set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
            foreach(runtime_dir IN LISTS _olr_runtime_dirs)
                set_property(TEST "${test_name}" APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${runtime_dir}")
            endforeach()
        else()
            list(JOIN _olr_runtime_dirs ";" _olr_runtime_path)
            string(APPEND _olr_runtime_path ";$ENV{PATH}")
            string(REPLACE ";" "\\;" _olr_runtime_path
                "${_olr_runtime_path}")
            set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT
                "PATH=${_olr_runtime_path}")
        endif()
    endforeach()
endfunction()
