# OpenLiveReplay test suite — enabled via -DOLR_BUILD_TESTS=ON.
#
# Desktop tests run headless. On Windows, FFmpeg and SRT paths are explicit
# cache entries; on macOS, the engine test library keeps using Homebrew FFmpeg
# the same way the desktop app does.
#
# Layout:
#   unit/   fast Qt Test unit tests (logic, no live capture)
#   smoke/  QML static-load smoke check (qmllint over the QML module)
#   e2e/    end-to-end recording harness driven by synthetic FFmpeg streams

find_package(Qt6 REQUIRED COMPONENTS Test Core Concurrent Network WebSockets)

function(olr_expect_compile_failure name source)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(${name}_compiled
        "${CMAKE_BINARY_DIR}/negcompile/${name}"
        SOURCES "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/${source}"
        CMAKE_FLAGS
            "-DCMAKE_CXX_STANDARD=17"
            "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
            "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}"
        LINK_LIBRARIES Qt6::Core
        OUTPUT_VARIABLE compile_output)
    if(${name}_compiled)
        message(FATAL_ERROR
            "Forbidden GPU capability probe ${name} compiled successfully:\n${compile_output}")
    endif()
    foreach(expected_diagnostic IN LISTS ARGN)
        if(NOT compile_output MATCHES "${expected_diagnostic}")
            message(FATAL_ERROR
                "Forbidden GPU capability probe ${name} failed without expected diagnostic "
                "'${expected_diagnostic}':\n${compile_output}")
        endif()
    endforeach()
endfunction()

function(olr_add_compile_control name source expectation)
    set(target "${name}_compile_control")
    add_library(${target} STATIC EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/${source}")
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Qt6::Core)

    set(control_args
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DTARGET=${target}"
        "-DCONFIG=$<CONFIG>")
    if(expectation STREQUAL "PASS")
        list(APPEND control_args "-DEXPECT_COMPILE=TRUE")
    elseif(expectation STREQUAL "BASE_PROTECTED_FAILURE")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_CLASS=GpuSurface"
            "-DEXPECTED_MEMBER=nativeHandle"
            "-DEXPECTED_ACCESS=[Pp]rotected")
    elseif(expectation STREQUAL "DERIVED_PROTECTED_FAILURE")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_CLASS=DerivedSurface"
            "-DEXPECTED_MEMBER=nativeHandle"
            "-DEXPECTED_ACCESS=[Pp]rotected")
    elseif(expectation STREQUAL "REJECT_UNRELATED_FAILURE")
        list(APPEND control_args
            "-DVERIFY_REJECTION=TRUE"
            "-DEXPECTED_MEMBER=nativeHandle"
            "-DEXPECTED_ACCESS=[Pp]rotected")
    elseif(expectation STREQUAL "WITH_READ_VOID")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=withRead"
            "-DEXPECTED_ACCESS=callback.must.return.void")
    else()
        message(FATAL_ERROR "Unknown compile-control expectation: ${expectation}")
    endif()

    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND} ${control_args}
            -P "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/run_compile_control.cmake")
    set_tests_properties(${name} PROPERTIES LABELS "gpu-capability;ci")
endfunction()

function(olr_add_gpu_submission_compile_control name source expectation)
    set(target "${name}_compile_control")
    add_library(${target} STATIC EXCLUDE_FROM_ALL
        "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/${source}")
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Qt6::Core)

    set(control_args
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DTARGET=${target}"
        "-DCONFIG=$<CONFIG>")
    target_compile_definitions(${target} PRIVATE ${ARGN})
    if(expectation STREQUAL "PASS")
        list(APPEND control_args "-DEXPECT_COMPILE=TRUE")
    elseif(expectation STREQUAL "NO_TRACK")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=track"
            "-DEXPECTED_ACCESS=(private|no.member|has.no.member)")
    elseif(expectation STREQUAL "OWNER_REQUIRED")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=submit"
            "-DEXPECTED_ACCESS=(no.matching|candidate.expects|requires)")
    elseif(expectation STREQUAL "REGISTRY_PAIRING")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=registerRetire"
            "-DEXPECTED_ACCESS=(no.member|has.no.member|not.a.member|has.not.been.declared|undeclared)")
    elseif(expectation STREQUAL "VIEW_UNWRAP")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=nativeHandle"
            "-DEXPECTED_ACCESS=[Pp]rotected")
    elseif(expectation STREQUAL "RAW_READBACK")
        list(APPEND control_args
            "-DEXPECTED_CLASS=GpuRhiContext"
            "-DEXPECTED_MEMBER=importAndReadback"
            "-DEXPECTED_ACCESS=[Pp]rivate")
    elseif(expectation STREQUAL "TOKEN_CONSTRUCTION")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=DeadDeviceToken"
            "-DEXPECTED_ACCESS=[Pp]rivate")
    elseif(expectation STREQUAL "DIRECT_ABANDON")
        list(APPEND control_args
            "-DEXPECT_COMPILE=FALSE"
            "-DEXPECTED_MEMBER=abandonAllNoWait"
            "-DEXPECTED_ACCESS=(cannot.convert|no.matching|candidate)")
    else()
        message(FATAL_ERROR "Unknown GPU submission compile-control expectation: ${expectation}")
    endif()

    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND} ${control_args}
            -P "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/run_compile_control.cmake")
    set_tests_properties(${name} PROPERTIES LABELS "gpu-capability;ci")
endfunction()

olr_expect_compile_failure(gpu_native_handle_access native_handle_access.cpp
    "GpuSurface" "nativeHandle" "[Pp]rotected")
olr_add_compile_control(gpu_native_handle_access
    native_handle_access.cpp BASE_PROTECTED_FAILURE)
olr_add_compile_control(gpu_native_handle_access_pass native_handle_access_pass.cpp PASS)
olr_expect_compile_failure(gpu_native_handle_derived_access
    native_handle_derived_access.cpp "DerivedSurface" "nativeHandle" "[Pp]rotected")
olr_add_compile_control(gpu_native_handle_derived_access
    native_handle_derived_access.cpp DERIVED_PROTECTED_FAILURE)
olr_add_compile_control(gpu_native_handle_derived_access_pass
    native_handle_derived_access_pass.cpp PASS)
olr_add_compile_control(gpu_native_handle_access_unrelated_error_rejected
    unrelated_compile_error.cpp REJECT_UNRELATED_FAILURE)
olr_expect_compile_failure(gpu_withread_return_raw withread_return_raw.cpp)
olr_expect_compile_failure(gpu_withread_return_integer withread_return_integer.cpp)
olr_expect_compile_failure(gpu_withread_return_wrapper withread_return_wrapper.cpp)
olr_add_compile_control(gpu_withread_return_raw
    withread_return_raw.cpp WITH_READ_VOID)
olr_add_compile_control(gpu_withread_return_integer
    withread_return_integer.cpp WITH_READ_VOID)
olr_add_compile_control(gpu_withread_return_wrapper
    withread_return_wrapper.cpp WITH_READ_VOID)
olr_add_gpu_submission_compile_control(gpu_op_track_access gpu_op_track_access.cpp NO_TRACK)
olr_add_gpu_submission_compile_control(
    gpu_submit_without_owner gpu_submit_without_owner.cpp OWNER_REQUIRED)
olr_add_gpu_submission_compile_control(
    gpu_registry_pairing gpu_registry_pairing.cpp REGISTRY_PAIRING)
olr_add_gpu_submission_compile_control(
    gpu_submission_view_unwrap gpu_submission_view_unwrap.cpp VIEW_UNWRAP)
olr_add_gpu_submission_compile_control(
    gpu_raw_readback_access gpu_raw_readback_access.cpp RAW_READBACK)
olr_add_gpu_submission_compile_control(
    gpu_token_construction gpu_token_abandon.cpp TOKEN_CONSTRUCTION
    OLR_NEGCOMPILE_TOKEN_CONSTRUCTION=1)
olr_add_gpu_submission_compile_control(
    gpu_token_direct_abandon gpu_token_abandon.cpp DIRECT_ABANDON
    OLR_NEGCOMPILE_DIRECT_ABANDON=1)
olr_add_gpu_submission_compile_control(
    gpu_submission_pass gpu_submission_pass.cpp PASS)
olr_add_gpu_submission_compile_control(
    gpu_submit_stub_adapter_pass gpu_submit_stub_adapter_pass.cpp PASS)
if(WIN32)
    olr_add_gpu_submission_compile_control(
        gpu_submit_d3d11_adapter_pass gpu_submit_d3d11_adapter_pass.cpp PASS
        OLR_GPU_PIPELINE_BUILD=1)
endif()
if(APPLE)
    olr_add_gpu_submission_compile_control(
        gpu_submit_metal_adapter_pass gpu_submit_metal_adapter_pass.mm PASS
        OLR_GPU_PIPELINE_BUILD=1)
endif()
set_tests_properties(
    gpu_submission_pass
    PROPERTIES DEPENDS "gpu_submission_view_unwrap;gpu_submit_without_owner;gpu_registry_pairing;gpu_token_construction;gpu_token_direct_abandon")
set_tests_properties(
    gpu_submit_stub_adapter_pass
    PROPERTIES DEPENDS gpu_submit_without_owner)
if(WIN32)
    set_tests_properties(gpu_submit_d3d11_adapter_pass PROPERTIES DEPENDS gpu_submit_without_owner)
endif()
if(APPLE)
    set_tests_properties(gpu_submit_metal_adapter_pass PROPERTIES DEPENDS gpu_submit_without_owner)
endif()
set_tests_properties(
    gpu_native_handle_access_pass
    gpu_native_handle_access_unrelated_error_rejected
    PROPERTIES
    DEPENDS gpu_native_handle_access)
set_tests_properties(gpu_native_handle_derived_access_pass PROPERTIES
    DEPENDS gpu_native_handle_derived_access)

function(olr_add_gpu_compile_control_alias name target expected_member expected_access)
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=${target}
            -DCONFIG=$<CONFIG>
            -DEXPECT_COMPILE=FALSE
            -DEXPECTED_CLASS=${ARGV4}
            -DEXPECTED_MEMBER=${expected_member}
            -DEXPECTED_ACCESS=${expected_access}
            -P "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/run_compile_control.cmake")
    set_tests_properties(${name} PROPERTIES LABELS "gpu-capability;ci")
endfunction()

function(olr_add_gpu_compile_pass_alias name target)
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=${target}
            -DCONFIG=$<CONFIG>
            -DEXPECT_COMPILE=TRUE
            -P "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/run_compile_control.cmake")
    set_tests_properties(${name} PROPERTIES LABELS "gpu-capability;ci")
endfunction()

# Task 7's matrix names make the locked regex execute every exact capability
# control. Historical names above stay available to earlier task reports.
olr_add_gpu_compile_control_alias(gpu_negcompile_native_handle_base
    gpu_native_handle_access_compile_control nativeHandle "[Pp]rotected" GpuSurface)
olr_add_gpu_compile_control_alias(gpu_negcompile_native_handle_derived
    gpu_native_handle_derived_access_compile_control
    nativeHandle "[Pp]rotected" DerivedSurface)
olr_add_gpu_compile_control_alias(gpu_negcompile_submission_view_unwrap
    gpu_submission_view_unwrap_compile_control nativeHandle "[Pp]rotected")
olr_add_gpu_compile_control_alias(gpu_negcompile_submit_without_owner
    gpu_submit_without_owner_compile_control submit "(no.matching|candidate.expects|requires)")
olr_add_gpu_compile_control_alias(gpu_negcompile_registry_pairing
    gpu_registry_pairing_compile_control registerRetire
    "(no.member|has.no.member|not.a.member|has.not.been.declared|undeclared)")
olr_add_gpu_compile_control_alias(gpu_negcompile_token_construction
    gpu_token_construction_compile_control DeadDeviceToken "[Pp]rivate")
olr_add_gpu_compile_control_alias(gpu_negcompile_token_direct_abandon
    gpu_token_direct_abandon_compile_control abandonAllNoWait
    "(cannot.convert|no.matching|candidate)")
olr_add_gpu_compile_control_alias(gpu_negcompile_op_track_access
    gpu_op_track_access_compile_control track "(private|no.member|has.no.member)")
olr_add_gpu_compile_pass_alias(gpu_compile_pass_native_handle_base
    gpu_native_handle_access_pass_compile_control)
olr_add_gpu_compile_pass_alias(gpu_compile_pass_native_handle_derived
    gpu_native_handle_derived_access_pass_compile_control)
olr_add_gpu_compile_pass_alias(gpu_compile_pass_submission
    gpu_submission_pass_compile_control)
set_tests_properties(gpu_compile_pass_native_handle_base PROPERTIES
    DEPENDS gpu_negcompile_native_handle_base)
set_tests_properties(gpu_compile_pass_native_handle_derived PROPERTIES
    DEPENDS gpu_negcompile_native_handle_derived)
set_tests_properties(gpu_compile_pass_submission PROPERTIES
    DEPENDS "gpu_negcompile_submission_view_unwrap;gpu_negcompile_submit_without_owner;gpu_negcompile_registry_pairing;gpu_negcompile_token_construction;gpu_negcompile_token_direct_abandon;gpu_negcompile_op_track_access")
olr_expect_compile_failure(gpu_lease_move lease_move.cpp)
olr_expect_compile_failure(gpu_backing_authority_spoof backing_authority_spoof.cpp)
olr_expect_compile_failure(gpu_token_authority_spoof token_authority_spoof.cpp)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gpu_capability_source_audit
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/gpu/gpu_capability_source_audit.py"
        --source-root "${CMAKE_SOURCE_DIR}"
        --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json")
set_tests_properties(gpu_capability_source_audit PROPERTIES
    LABELS "gpu-capability;ci" TIMEOUT 90)
add_test(NAME gpu_capability_source_audit_perf
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/gpu/gpu_capability_source_audit.py"
        --source-root "${CMAKE_SOURCE_DIR}"
        --performance-only)
set_tests_properties(gpu_capability_source_audit_perf PROPERTIES
    LABELS "gpu-capability;ci" RUN_SERIAL TRUE TIMEOUT 180)
if(WIN32 AND MINGW AND OLR_GPU_PIPELINE
        AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_test(NAME gpu_capability_mingw_provenance_perf
        COMMAND "${CMAKE_COMMAND}" -E env
            "OLR_TASK9_REFERENCE_COMPILER=${CMAKE_CXX_COMPILER}"
            "OLR_TASK9_REQUIRED_FAMILY=GNU"
            "${Python3_EXECUTABLE}" -m unittest
            tests.gpu.test_gpu_capability_provenance.ProvenanceScannerParityTests.test_compiled_scanner_is_at_least_twice_reference_throughput)
    set_tests_properties(gpu_capability_mingw_provenance_perf PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "gpu-capability;ci"
        RUN_SERIAL TRUE
        TIMEOUT 180)
endif()
if(UNIX AND NOT WIN32)
    add_test(NAME gpu_capability_calibration_smoke
        COMMAND "${Python3_EXECUTABLE}" -m unittest
            tests.gpu.test_gpu_capability_runner.ProcessCoordinatorTests.test_task9_native_calibration_smoke_one_configuration)
    set_tests_properties(gpu_capability_calibration_smoke PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "gpu-capability-smoke"
        RUN_SERIAL TRUE
        TIMEOUT 175)
endif()
add_executable(gpu_lease_release_probe
    "${CMAKE_SOURCE_DIR}/tests/gpu/gpu_lease_release_probe.cpp")
target_include_directories(gpu_lease_release_probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(gpu_lease_release_probe PRIVATE Qt6::Core)
target_compile_definitions(gpu_lease_release_probe PRIVATE QT_NO_DEBUG)
add_test(NAME gpu_lease_release_probe COMMAND gpu_lease_release_probe)
set_tests_properties(gpu_lease_release_probe PROPERTIES LABELS "gpu-capability;ci")
add_test(NAME gpu_compile_pass_diagnostic_fixtures
    COMMAND ${CMAKE_COMMAND}
        -DVERIFY_DIAGNOSTIC_FIXTURES=TRUE
        -P "${CMAKE_SOURCE_DIR}/tests/gpu/negcompile/run_compile_control.cmake")
set_tests_properties(gpu_compile_pass_diagnostic_fixtures PROPERTIES
    LABELS "gpu-capability;ci")
if(WIN32)
    olr_expect_compile_failure(gpu_d3d11_texture_access d3d11_texture_access.cpp)
    olr_expect_compile_failure(gpu_d3d11_device_access d3d11_device_access.cpp)
    olr_expect_compile_failure(gpu_d3d11_subresource_access d3d11_subresource_access.cpp)
    olr_expect_compile_failure(gpu_import_edge_device_escape import_edge_device_escape.cpp)
endif()

# --- Locate FFmpeg ----------------------------------------------------------
if(WIN32)
    set(OLR_FFMPEG_ROOT "" CACHE PATH "Root directory of the Windows FFmpeg install")
    find_path(OLR_FFMPEG_INCLUDE
        NAMES libavutil/frame.h
        HINTS "${OLR_FFMPEG_ROOT}/include")
    find_library(OLR_FFMPEG_AVUTIL_LIBRARY
        NAMES avutil
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_AVFORMAT_LIBRARY
        NAMES avformat
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_AVCODEC_LIBRARY
        NAMES avcodec
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_SWSCALE_LIBRARY
        NAMES swscale
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    find_library(OLR_FFMPEG_SWRESAMPLE_LIBRARY
        NAMES swresample
        HINTS "${OLR_FFMPEG_ROOT}/lib" "${OLR_FFMPEG_ROOT}/bin")
    if(NOT OLR_FFMPEG_INCLUDE
            OR NOT OLR_FFMPEG_AVUTIL_LIBRARY
            OR NOT OLR_FFMPEG_AVFORMAT_LIBRARY
            OR NOT OLR_FFMPEG_AVCODEC_LIBRARY
            OR NOT OLR_FFMPEG_SWSCALE_LIBRARY
            OR NOT OLR_FFMPEG_SWRESAMPLE_LIBRARY)
        message(FATAL_ERROR "OpenLiveReplay Windows tests require OLR_FFMPEG_ROOT with FFmpeg include and library paths")
    endif()
else()
    # Locate homebrew FFmpeg (Apple Silicon or Intel).
    if(NOT DEFINED OLR_BREW_PREFIX)
        execute_process(COMMAND brew --prefix
            OUTPUT_VARIABLE OLR_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT OLR_BREW_PREFIX OR NOT EXISTS "${OLR_BREW_PREFIX}")
        set(OLR_BREW_PREFIX /opt/homebrew)
    endif()
    set(OLR_FFMPEG_INCLUDE "${OLR_BREW_PREFIX}/include")
    set(OLR_FFMPEG_LIBDIR  "${OLR_BREW_PREFIX}/lib")
    set(OLR_FFMPEG_AVUTIL_LIBRARY avutil)
    set(OLR_FFMPEG_AVFORMAT_LIBRARY avformat)
    set(OLR_FFMPEG_AVCODEC_LIBRARY avcodec)
    set(OLR_FFMPEG_SWSCALE_LIBRARY swscale)
    set(OLR_FFMPEG_SWRESAMPLE_LIBRARY swresample)
    message(STATUS "OpenLiveReplay tests: FFmpeg prefix = ${OLR_BREW_PREFIX}")
endif()

if(WIN32)
    set(OLR_SRT_ROOT "" CACHE PATH "Root directory of the Windows SRT install")
    find_path(OLR_SRT_INCLUDE_DIR
        NAMES srt/srt.h
        HINTS "${OLR_SRT_ROOT}/include")
    find_library(OLR_SRT_LIBRARY
        NAMES srt
        HINTS "${OLR_SRT_ROOT}/lib" "${OLR_SRT_ROOT}/bin")
    if(NOT OLR_SRT_INCLUDE_DIR OR NOT OLR_SRT_LIBRARY)
        message(FATAL_ERROR "OpenLiveReplay Windows native SRT requires OLR_SRT_ROOT or SRT include/library paths")
    endif()
endif()

include("${CMAKE_SOURCE_DIR}/tests/cmake/OlrWindowsTestRuntime.cmake")
if(WIN32)
    get_property(_olr_existing_tests DIRECTORY PROPERTY TESTS)
    olr_apply_windows_test_runtime(${_olr_existing_tests})
endif()

# --- Lightweight engine sources used by fast unit tests ---------------------
qt_add_library(olr_test_core STATIC
    "${CMAKE_SOURCE_DIR}/appenv.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/recordingclock.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/heartbeat.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/driftestimator.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timecode.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/smpte12m.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timecodealigner.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/sourceoffsetestimator.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/sourceclock.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/timingreference.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/ptpservo.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/ptpreference.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/timing/udpptpclient.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ingestsession.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/mpegtsparser.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/h26xaccessunit.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/colorvui.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/codec/colorvui.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeframecopy.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtconnectdiagnostics.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtconnectdiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtaddress.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtaddress.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrturloptions.h"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrturloptions.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/rtmpprotocol.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputtargetassignment.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/broadcastoutputsettings.cpp"
    "${CMAKE_SOURCE_DIR}/settingsmanager.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectsettingsimporter.cpp"
    "${CMAKE_SOURCE_DIR}/project/projectimportclient.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbacktransport.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/sseparser.cpp"
    "${CMAKE_SOURCE_DIR}/telemetry/telemetryclient.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlprotocol.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlstate.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/controlwebsocketserver.cpp"
    "${CMAKE_SOURCE_DIR}/websocket/pendingcommandregistry.cpp"
    "${CMAKE_SOURCE_DIR}/streamdeck/streamdeckmappingstore.cpp"
)
target_include_directories(olr_test_core PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_core PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_core
    PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets "${OLR_FFMPEG_AVUTIL_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
if(WIN32)
    target_link_libraries(olr_test_core PRIVATE ws2_32)
endif()

if(APPLE)
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_audiotoolbox.mm")
    target_link_libraries(olr_test_core PRIVATE "-framework AudioToolbox")
elseif(WIN32)
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/mediafoundationruntime.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/mediafoundationruntime.h"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp")
    target_link_libraries(olr_test_core PRIVATE mfplat mf mfuuid ole32)
else()
    target_sources(olr_test_core PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativeaacdecoder_stub.cpp")
endif()

# --- Engine sources that DO need FFmpeg (muxer / capture / recording) -------
qt_add_library(olr_test_engine STATIC
    "${CMAKE_SOURCE_DIR}/recorder_engine/muxer.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/streamworker.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/replaymanager.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/codec/avcc.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/benchmarkplan.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/codecbenchmark.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/syntheticframes.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/realcodecrunners.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/benchmarkcache.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/runcodecbenchmark.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/benchmark/recordgate.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/h26xseitimecode.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/ndiframeconvert.cpp"
    "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativendiingestsession.cpp"
)
target_include_directories(olr_test_engine PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/recorder_engine"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_engine PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_engine
    PUBLIC olr_test_core Qt6::Core Qt6::Concurrent
           "${OLR_FFMPEG_AVFORMAT_LIBRARY}" "${OLR_FFMPEG_AVCODEC_LIBRARY}"
           "${OLR_FFMPEG_AVUTIL_LIBRARY}" "${OLR_FFMPEG_SWSCALE_LIBRARY}"
           "${OLR_FFMPEG_SWRESAMPLE_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
# Expose the friend-class seam to every TU that sees olr_test_engine (the
# library TU itself and all linking test executables). Production app targets
# never link olr_test_engine, so this define never reaches production code.
target_compile_definitions(olr_test_engine PUBLIC OLR_UNIT_TEST=1)

if(OLR_GPU_PIPELINE)
    target_compile_definitions(olr_test_engine PUBLIC OLR_GPU_PIPELINE_BUILD=1)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/gpudecodedframe.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/gpuencodepump.cpp")
    target_link_libraries(olr_test_engine PRIVATE olr_test_playback)
endif()

if(APPLE)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativertmpingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_videotoolbox.mm"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_videotoolbox.mm")
    target_compile_definitions(olr_test_engine PUBLIC
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_include_directories(olr_test_engine PUBLIC "${OLR_BREW_PREFIX}/opt/srt/include")
    target_link_directories(olr_test_engine PUBLIC "${OLR_BREW_PREFIX}/opt/srt/lib")
    target_link_libraries(olr_test_engine
        PRIVATE srt "-framework VideoToolbox" "-framework CoreMedia"
                "-framework CoreFoundation" "-framework CoreVideo" "-framework IOSurface")
elseif(WIN32)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativesrtingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativertmpingestsession.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_mediafoundation.cpp")
    target_compile_definitions(olr_test_engine PUBLIC
        OLR_NATIVE_SRT_AVAILABLE=1
        OLR_NATIVE_RTMP_AVAILABLE=1)
    target_include_directories(olr_test_engine PUBLIC "${OLR_SRT_INCLUDE_DIR}")
    target_link_libraries(olr_test_engine
        PRIVATE mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32 ws2_32 "${OLR_SRT_LIBRARY}")
elseif(NOT WIN32)
    target_sources(olr_test_engine PRIVATE
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_stub.cpp"
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_stub.cpp")
endif()

# --- Playback sources (need Qt Multimedia for QVideoFrame; ffmpeg for the
#     worker, which #includes <libavformat/...> and demuxes the fixture). -----
qt_add_library(olr_test_playback STATIC
    "${CMAKE_SOURCE_DIR}/playback/trackbuffer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/frameindex.cpp"
    "${CMAKE_SOURCE_DIR}/playback/replayplaylist.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playlistentriesmodel.cpp"
    "${CMAKE_SOURCE_DIR}/playback/cutschedule.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playlistplayout.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioframequeue.cpp"
    "${CMAKE_SOURCE_DIR}/playback/playbackworker.cpp"
    "${CMAKE_SOURCE_DIR}/playback/frameprovider.cpp"
    "${CMAKE_SOURCE_DIR}/playback/telemetrytimelinereader.cpp"
    "${CMAKE_SOURCE_DIR}/playback/audioplayer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputframeclock.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/framehandle.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/colormetadatapolicy.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputframecache.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputtargetassignment.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputbusengine.cpp"
    "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframereadbacktelemetry.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/gpureadbacktelemetry.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/formatcanon.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputdispatcher.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/broadcastoutputstatus.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/outputruntime.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/queuedoutputsink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/asyncgpureadbacksink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/gpureadbackring.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/sinkgpucapability.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/yuv420pcompositor.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/qtpreviewsink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/ndiabi.h"
    "${CMAKE_SOURCE_DIR}/playback/output/ndiruntimepaths.h"
    "${CMAKE_SOURCE_DIR}/playback/output/ndisink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/sinkcapabilityprobe.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/decklinksink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/st2110framer.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/ajasink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/omtsink.cpp"
    "${CMAKE_SOURCE_DIR}/playback/output/iotargets/iotargetsinkfactory.cpp"
)
target_include_directories(olr_test_playback PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${OLR_FFMPEG_INCLUDE}")
if(NOT WIN32)
    target_link_directories(olr_test_playback PUBLIC "${OLR_FFMPEG_LIBDIR}")
endif()
target_link_libraries(olr_test_playback
    PUBLIC olr_test_engine Qt6::Core Qt6::Multimedia Qt6::Gui
           "${OLR_FFMPEG_AVFORMAT_LIBRARY}" "${OLR_FFMPEG_AVCODEC_LIBRARY}"
           "${OLR_FFMPEG_AVUTIL_LIBRARY}" "${OLR_FFMPEG_SWSCALE_LIBRARY}"
           "${OLR_FFMPEG_SWRESAMPLE_LIBRARY}"
    PRIVATE olr_warnings olr_sanitize)
if(APPLE)
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_apple.mm"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_apple.mm"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/applegpusurface_apple.mm")
    target_link_libraries(olr_test_playback
        PRIVATE "-framework CoreFoundation" "-framework CoreVideo" "-framework IOSurface"
                "-framework Metal")
else()
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp")
    if(WIN32)
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_win.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/wingpuimportedge.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/d3d11gpusurface.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/d3dfence.cpp")
        target_link_libraries(olr_test_playback
            PRIVATE mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32)
    else()
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_stub.cpp")
    endif()
endif()

if(NOT OLR_GPU_PIPELINE)
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpureadbackretainer.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuopscope.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpudevicelossmonitor.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpurecoverycoordinator.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpugeneration.cpp")
endif()

if(OLR_GPU_PIPELINE)
    target_compile_definitions(olr_test_playback PUBLIC OLR_GPU_PIPELINE_BUILD=1)
    target_link_libraries(olr_test_playback PRIVATE Qt6::GuiPrivate)
    target_sources(olr_test_playback PRIVATE
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpucompositor.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpubudget.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpudevicelossmonitor.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpurecoverycoordinator.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuseekprefetch.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurfaceallocator.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframedata.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframeretirequeue.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpureadbackretainer.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuopscope.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpugeneration.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpulifecyclesink.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosmemoryheadroom.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpupolicy.cpp")
    if(APPLE)
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpucompositor_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/vtkeepsurfaceimporter_apple.mm")
        target_link_libraries(olr_test_playback
            PRIVATE Qt6::GuiPrivate "-framework Metal" "-framework QuartzCore")
    else()
        target_sources(olr_test_playback PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_stub.cpp")
        if(WIN32)
            target_sources(olr_test_playback PRIVATE
                "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_win.cpp")
        else()
            target_sources(olr_test_playback PRIVATE
                "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_stub.cpp")
        endif()
    endif()

    qt_add_library(olr_test_gpu STATIC
        "${CMAKE_SOURCE_DIR}/playback/gpu/olrrhi.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpucompositor.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpubudget.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpudevicelossmonitor.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpurecoverycoordinator.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuseekprefetch.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurfaceallocator.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframedata.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframereadbacktelemetry.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuframeretirequeue.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpulifecyclesink.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosmemoryheadroom.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/iosgpupolicy.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpupipelineconfig.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpureadbackretainer.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuopscope.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpugeneration.cpp"
        "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_stub.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/formatcanon.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/framehandle.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/gpureadbacktelemetry.cpp"
        "${CMAKE_SOURCE_DIR}/playback/output/yuv420pcompositor.cpp")
    target_include_directories(olr_test_gpu PUBLIC "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(olr_test_gpu PUBLIC OLR_UNIT_TEST=1)
    target_link_libraries(olr_test_gpu
        PUBLIC Qt6::Core Qt6::Gui Qt6::GuiPrivate
        PRIVATE olr_warnings olr_sanitize)
    if(APPLE)
        target_sources(olr_test_gpu PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/applegpusurface_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpucompositor_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/decodedonefence_apple.mm"
            "${CMAKE_SOURCE_DIR}/playback/gpu/vtkeepsurfaceimporter_apple.mm")
        target_link_libraries(olr_test_gpu
            PUBLIC "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
                   "-framework CoreFoundation" "-framework IOSurface")
    elseif(WIN32)
        target_sources(olr_test_gpu PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_win.cpp"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpufence_win.cpp"
            "${CMAKE_SOURCE_DIR}/playback/output/win/d3dfence.cpp")
        target_link_libraries(olr_test_gpu
            PRIVATE d3d11 dxgi ole32)
    else()
        target_sources(olr_test_gpu PRIVATE
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpusurface_stub.cpp"
            "${CMAKE_SOURCE_DIR}/playback/gpu/gpurhicontext_stub.cpp")
    endif()

    qt_add_shaders(olr_test_gpu "olr_passthrough_shaders"
        PREFIX "/olr/shaders"
        BASE "${CMAKE_SOURCE_DIR}/playback/gpu/shaders"
        FILES
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.vert"
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/passthrough.frag")
    qt_add_shaders(olr_test_gpu "olr_grid_shaders"
        PREFIX "/olr/shaders"
        BASE "${CMAKE_SOURCE_DIR}/playback/gpu/shaders"
        GLSL "330,440"
        FILES
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/grid.vert"
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/grid_nn.frag"
            "${CMAKE_SOURCE_DIR}/playback/gpu/shaders/grid_quality.frag")
endif()

add_subdirectory(gpu_fault)
add_subdirectory(unit)
if(OLR_GPU_PIPELINE)
    add_subdirectory(perf)
endif()
if(APPLE OR WIN32)  # tst_ingestbackendselector is Apple/Windows-only (native ingest)
    target_link_libraries(tst_ingestbackendselector PRIVATE olr_test_engine)
endif()
add_subdirectory(smoke)
add_subdirectory(e2e)
add_subdirectory(qml)
add_subdirectory(qmlstyle)

if(WIN32)
    olr_add_ctest(NAME windows_test_runtime_legacy_fallback
        COMMAND "${CMAKE_COMMAND}"
            -DOLR_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPROBE_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/legacy_runtime_probe
            -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
            -DCTEST_COMMAND_PATH=${CMAKE_CTEST_COMMAND}
            -DPROBE_GENERATOR=${CMAKE_GENERATOR}
            -P "${CMAKE_SOURCE_DIR}/tests/cmake/run_windows_test_runtime_legacy_probe.cmake")
    set_tests_properties(windows_test_runtime_legacy_fallback PROPERTIES
        LABELS "formal;ci" TIMEOUT 60)

    olr_add_ctest(NAME windows_test_runtime_audit
        COMMAND "${CMAKE_COMMAND}"
            -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DQT_RUNTIME_DIR=$<TARGET_FILE_DIR:Qt6::Core>
            -DQT_PLUGIN_DIR=${OLR_QT_TEST_PLUGIN_DIR}
            -DFFMPEG_RUNTIME_DIR=${OLR_FFMPEG_TEST_RUNTIME_DIR}
            -DSRT_RUNTIME_DIR=${OLR_SRT_TEST_RUNTIME_DIR}
            -P "${CMAKE_SOURCE_DIR}/tests/cmake/audit_windows_test_runtime.cmake")
    set_tests_properties(windows_test_runtime_audit PROPERTIES
        LABELS "formal;ci" TIMEOUT 30)
endif()
