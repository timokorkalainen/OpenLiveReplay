include_guard(GLOBAL)

function(olr_require_target_dependencies target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "required target does not exist: ${target_name}")
    endif()

    get_property(_olr_dependencies
        TARGET "${target_name}"
        PROPERTY MANUALLY_ADDED_DEPENDENCIES)
    set(_olr_missing)
    foreach(_olr_required IN LISTS ARGN)
        if(NOT TARGET "${_olr_required}")
            list(APPEND _olr_missing "${_olr_required} (target absent)")
            continue()
        endif()
        list(FIND _olr_dependencies "${_olr_required}" _olr_index)
        if(_olr_index EQUAL -1)
            list(APPEND _olr_missing "${_olr_required}")
        endif()
    endforeach()
    if(_olr_missing)
        list(JOIN _olr_missing ", " _olr_missing_text)
        message(FATAL_ERROR
            "${target_name} is missing required generated target dependencies: "
            "${_olr_missing_text}")
    endif()
endfunction()
