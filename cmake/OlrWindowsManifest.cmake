include_guard(GLOBAL)

set(OLR_WINDOWS_APPLICATION_MANIFEST
    "${CMAKE_SOURCE_DIR}/cmake/olr_windows_application.manifest")
file(TO_CMAKE_PATH "${OLR_WINDOWS_APPLICATION_MANIFEST}"
    OLR_WINDOWS_APPLICATION_MANIFEST)
set(_OLR_WINDOWS_MANIFEST_RESOURCE
    "${CMAKE_BINARY_DIR}/olr_windows_application_manifest.rc")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/olr_windows_application_manifest.rc"
    "${_OLR_WINDOWS_MANIFEST_RESOURCE}"
    @ONLY)

function(_olr_embed_windows_manifest_in_directory directory)
    get_property(_olr_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_olr_target IN LISTS _olr_targets)
        get_target_property(_olr_type "${_olr_target}" TYPE)
        get_target_property(_olr_imported "${_olr_target}" IMPORTED)
        get_target_property(_olr_source_dir "${_olr_target}" SOURCE_DIR)
        file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/" _olr_repo_prefix)
        file(TO_CMAKE_PATH "${_olr_source_dir}/" _olr_target_prefix)
        string(FIND "${_olr_target_prefix}" "${_olr_repo_prefix}" _olr_repo_index)
        if(_olr_type STREQUAL "EXECUTABLE" AND
           NOT _olr_imported AND
           _olr_repo_index EQUAL 0)
            get_target_property(_olr_embedded "${_olr_target}"
                OLR_WINDOWS_APPLICATION_MANIFEST_EMBEDDED)
            if(NOT _olr_embedded)
                target_sources("${_olr_target}" PRIVATE
                    "${_OLR_WINDOWS_MANIFEST_RESOURCE}")
                set_property(TARGET "${_olr_target}" PROPERTY
                    OLR_WINDOWS_APPLICATION_MANIFEST_EMBEDDED TRUE)
            endif()
        endif()
    endforeach()

    get_property(_olr_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_olr_subdirectory IN LISTS _olr_subdirectories)
        _olr_embed_windows_manifest_in_directory("${_olr_subdirectory}")
    endforeach()
endfunction()

function(olr_embed_all_windows_executables)
    if(WIN32)
        _olr_embed_windows_manifest_in_directory("${CMAKE_SOURCE_DIR}")
    endif()
endfunction()
