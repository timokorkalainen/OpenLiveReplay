include_guard(GLOBAL)

function(_olr_prepend_test_environment test_name variable prefix)
    get_property(_olr_environment TEST "${test_name}" PROPERTY ENVIRONMENT)
    set(_olr_existing_value "")
    set(_olr_filtered_environment "")
    foreach(_olr_entry IN LISTS _olr_environment)
        if(_olr_entry MATCHES "^${variable}=(.*)$")
            if(_olr_existing_value STREQUAL "")
                set(_olr_existing_value "${CMAKE_MATCH_1}")
            else()
                string(APPEND _olr_existing_value ";${CMAKE_MATCH_1}")
            endif()
        else()
            list(APPEND _olr_filtered_environment "${_olr_entry}")
        endif()
    endforeach()

    set(_olr_value "${prefix}")
    if(NOT _olr_existing_value STREQUAL "")
        string(APPEND _olr_value ";${_olr_existing_value}")
    endif()
    string(REPLACE ";" "\\;" _olr_value "${_olr_value}")
    list(APPEND _olr_filtered_environment "${variable}=${_olr_value}")
    set_property(TEST "${test_name}" PROPERTY ENVIRONMENT
        "${_olr_filtered_environment}")
endfunction()

function(_olr_validate_windows_test_runtime)
    get_property(_olr_validated GLOBAL
        PROPERTY OLR_WINDOWS_TEST_RUNTIME_VALIDATED)
    if(_olr_validated)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER)
        message(FATAL_ERROR
            "Windows test runtime requires CMAKE_CXX_COMPILER")
    endif()
    if(NOT EXISTS "${CMAKE_CXX_COMPILER}" OR
       IS_DIRECTORY "${CMAKE_CXX_COMPILER}")
        message(FATAL_ERROR
            "Windows test runtime compiler does not exist: ${CMAKE_CXX_COMPILER}")
    endif()
    get_filename_component(_olr_compiler_runtime_dir
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(NOT IS_DIRECTORY "${_olr_compiler_runtime_dir}")
        message(FATAL_ERROR
            "Windows test compiler runtime directory does not exist: ${_olr_compiler_runtime_dir}")
    endif()

    foreach(root_name IN ITEMS OLR_FFMPEG_ROOT OLR_SRT_ROOT)
        if(NOT DEFINED ${root_name} OR "${${root_name}}" STREQUAL "")
            message(FATAL_ERROR
                "Windows test runtime requires ${root_name}")
        endif()
        if(NOT IS_DIRECTORY "${${root_name}}")
            message(FATAL_ERROR
                "Windows test runtime root does not exist: ${root_name}=${${root_name}}")
        endif()
        foreach(runtime_subdir IN ITEMS bin lib)
            if(NOT IS_DIRECTORY "${${root_name}}/${runtime_subdir}")
                message(FATAL_ERROR
                    "Windows test runtime directory does not exist: ${${root_name}}/${runtime_subdir}")
            endif()
        endforeach()
    endforeach()

    set_property(GLOBAL PROPERTY OLR_WINDOWS_TEST_RUNTIME_VALIDATED TRUE)
endfunction()

function(olr_configure_windows_test_runtime)
    cmake_parse_arguments(ARG "" "" "TESTS" ${ARGN})
    if(NOT WIN32)
        return()
    endif()

    _olr_validate_windows_test_runtime()
    get_filename_component(_olr_compiler_runtime_dir
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_olr_runtime_dirs
        "${_olr_compiler_runtime_dir}"
        "$<TARGET_FILE_DIR:Qt6::Core>"
        "${OLR_FFMPEG_ROOT}/bin"
        "${OLR_FFMPEG_ROOT}/lib"
        "${OLR_SRT_ROOT}/bin"
        "${OLR_SRT_ROOT}/lib")
    set(_olr_qml_dir "$<TARGET_FILE_DIR:Qt6::Core>/../qml")

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
            set(_olr_runtime_modification_dirs ${_olr_runtime_dirs})
            list(REVERSE _olr_runtime_modification_dirs)
            foreach(runtime_dir IN LISTS _olr_runtime_modification_dirs)
                set_property(TEST "${test_name}" APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${runtime_dir}")
            endforeach()
            foreach(_olr_qml_variable IN ITEMS QML_IMPORT_PATH QML2_IMPORT_PATH)
                set_property(TEST "${test_name}" APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION
                    "${_olr_qml_variable}=path_list_prepend:${_olr_qml_dir}")
            endforeach()
        else()
            list(JOIN _olr_runtime_dirs ";" _olr_runtime_path)
            string(APPEND _olr_runtime_path ";$ENV{PATH}")
            string(REPLACE ";" "\\;" _olr_runtime_path
                "${_olr_runtime_path}")
            set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT
                "PATH=${_olr_runtime_path}")
            foreach(_olr_qml_variable IN ITEMS QML_IMPORT_PATH QML2_IMPORT_PATH)
                _olr_prepend_test_environment(
                    "${test_name}" "${_olr_qml_variable}" "${_olr_qml_dir}")
            endforeach()
        endif()
    endforeach()
endfunction()
