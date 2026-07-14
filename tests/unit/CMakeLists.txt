# Fast Qt Test unit tests. Each is a standalone executable registered with CTest.

function(olr_add_unit_test name)
    qt_add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ARGN} olr_warnings olr_sanitize)
    if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${name}>
                $<TARGET_FILE_DIR:${name}>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying ${name} runtime DLLs")
    endif()
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    # Headless + deterministic; fail fast rather than hang CI.
    set_tests_properties(${name} PROPERTIES
        LABELS "unit;ci"
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

olr_add_unit_test(tst_recordingclock   olr_test_core)
olr_add_unit_test(tst_heartbeat        olr_test_core)
olr_add_unit_test(tst_driftestimator   olr_test_core)
olr_add_unit_test(tst_timecode         olr_test_core)
olr_add_unit_test(tst_smpte12m         olr_test_core)
olr_add_unit_test(tst_timecodealigner  olr_test_core)
olr_add_unit_test(tst_sourceoffsetestimator olr_test_core)
olr_add_unit_test(tst_sourceclock      olr_test_core)
olr_add_unit_test(tst_timingreference  olr_test_core)
olr_add_unit_test(tst_ptpservo         olr_test_core)
olr_add_unit_test(tst_ptpreference     olr_test_core)
olr_add_unit_test(tst_udpptpclient     olr_test_core)
olr_add_unit_test(tst_controlprotocol  olr_test_core)
olr_add_unit_test(tst_appenv           olr_test_core)
olr_add_unit_test(tst_controlwebsocketserver olr_test_core)
olr_add_unit_test(tst_pendingcommandregistry olr_test_core)
olr_add_unit_test(tst_controlstate    olr_test_core)
olr_add_unit_test(tst_settingsmanager  olr_test_core)
olr_add_unit_test(tst_projectsettingsimporter olr_test_core)
olr_add_unit_test(tst_mainqml_wiring)
target_compile_definitions(tst_mainqml_wiring PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
olr_add_unit_test(tst_playbacktransport olr_test_core)
olr_add_unit_test(tst_livefollowpolicy olr_test_core)
olr_add_unit_test(tst_sseparser olr_test_core)
olr_add_unit_test(tst_telemetryclient olr_test_core)
olr_add_unit_test(tst_muxer            olr_test_engine)
# Exercises NativeSrtIngestSession, which is compiled only on Apple/Windows
# (Linux uses the ingest stubs), so these tests are platform-gated.
if(APPLE OR WIN32)
    olr_add_unit_test(tst_ingestbackendselector olr_test_core)
    # Drives a real loopback SRT connection through NativeSrtIngestSession::run()
    # and requestStop() concurrently; the threading regression it guards is caught
    # under TSan (registered as a TSan target in .githooks/pre-push and CI).
    olr_add_unit_test(tst_srtingest_teardown olr_test_engine)
    # Guards the audio-FIFO crackle fix: clock jitter must not fragment the
    # sample-contiguous decoded-audio buffer.
    olr_add_unit_test(tst_srtaudiofifo olr_test_engine)
endif()
olr_add_unit_test(tst_mpegtsparser     olr_test_core)
olr_add_unit_test(tst_mpegtsparser_pcr olr_test_core)
olr_add_unit_test(tst_h26xaccessunit   olr_test_core)
olr_add_unit_test(tst_colorvui olr_test_core)
if(APPLE)
    olr_add_unit_test(tst_colortags_apple olr_test_core)
    add_library(olr_test_colortags_apple STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/colortags_apple.mm")
    target_include_directories(olr_test_colortags_apple PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_colortags_apple
        PUBLIC Qt6::Core olr_test_core "-framework CoreMedia" "-framework CoreFoundation"
               "-framework CoreVideo")
    target_link_libraries(tst_colortags_apple PRIVATE olr_test_colortags_apple)
endif()
olr_add_unit_test(tst_h26xseitimecode  olr_test_engine)
olr_add_unit_test(tst_nativesrtaddress olr_test_core)
olr_add_unit_test(tst_nativevideodecoder olr_test_core)
olr_add_unit_test(tst_nativeframecopy olr_test_core)
olr_add_unit_test(tst_ndiframeconvert olr_test_engine)
olr_add_unit_test(tst_ndiingest       olr_test_engine)
olr_add_unit_test(tst_ndimarkerpattern olr_test_core)
target_sources(tst_ndimarkerpattern PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/e2e/ndi_marker_pattern.cpp")
olr_add_unit_test(tst_ndioutputmarker)
target_sources(tst_ndioutputmarker PRIVATE "${CMAKE_SOURCE_DIR}/tests/e2e/ndi_output_marker.cpp")
target_include_directories(tst_ndioutputmarker PRIVATE "${CMAKE_SOURCE_DIR}")
olr_add_unit_test(tst_ndirecvanalysis)
target_sources(tst_ndirecvanalysis PRIVATE "${CMAKE_SOURCE_DIR}/tests/e2e/ndi_recv_analysis.cpp")
target_include_directories(tst_ndirecvanalysis PRIVATE "${CMAKE_SOURCE_DIR}")
olr_add_unit_test(tst_rtmpprotocol     olr_test_core)
if(APPLE)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_videotoolbox.mm")
    target_include_directories(olr_test_nativevideodecoder PRIVATE "${OLR_FFMPEG_INCLUDE}")
    target_link_directories(olr_test_nativevideodecoder PUBLIC "${OLR_FFMPEG_LIBDIR}")
    target_link_libraries(olr_test_nativevideodecoder
        PUBLIC Qt6::Core avutil "-framework VideoToolbox" "-framework CoreMedia"
                "-framework CoreFoundation" "-framework CoreVideo")
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
elseif(WIN32)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_mediafoundation.cpp")
    target_include_directories(olr_test_nativevideodecoder PRIVATE "${OLR_FFMPEG_INCLUDE}")
    target_link_libraries(olr_test_nativevideodecoder
        PUBLIC Qt6::Core olr_test_core
        PRIVATE mfplat mf mfuuid d3d11 dxgi ole32)
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
elseif(NOT WIN32)
    add_library(olr_test_nativevideodecoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/ingest/nativevideodecoder_stub.cpp")
    target_link_libraries(olr_test_nativevideodecoder PUBLIC Qt6::Core)
    target_link_libraries(tst_nativevideodecoder PRIVATE olr_test_nativevideodecoder)
endif()
target_compile_definitions(olr_test_nativevideodecoder PUBLIC OLR_UNIT_TEST=1)
olr_add_unit_test(tst_wingpuimportedge olr_test_playback)
if(WIN32)
    add_library(olr_test_wingpuimport INTERFACE)
    target_link_libraries(olr_test_wingpuimport INTERFACE olr_test_playback)
    target_link_libraries(tst_wingpuimportedge PRIVATE olr_test_wingpuimport)
else()
    add_library(olr_test_wingpuimport STATIC
        "${CMAKE_SOURCE_DIR}/playback/output/win/wingpuimportedge_stub.cpp")
    target_include_directories(olr_test_wingpuimport PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_wingpuimport PUBLIC Qt6::Core)
    target_link_libraries(tst_wingpuimportedge PRIVATE olr_test_wingpuimport)
endif()
olr_add_unit_test(tst_nativeaacdecoder olr_test_core)
olr_add_unit_test(tst_telemetrytimelinereader olr_test_engine olr_test_playback)
olr_add_unit_test(tst_replaymanager_telemetry olr_test_engine olr_test_playback)
olr_add_unit_test(tst_replaymanager_timecode olr_test_engine)
olr_add_unit_test(tst_trackbuffer      olr_test_playback)
olr_add_unit_test(tst_audioframequeue  olr_test_playback)
olr_add_unit_test(tst_outputframeclock olr_test_playback)
olr_add_unit_test(tst_outputframecache olr_test_playback)
olr_add_unit_test(tst_outputtargetassignment olr_test_playback)
olr_add_unit_test(tst_broadcastoutputsettings olr_test_core)
olr_add_unit_test(tst_outputbusengine olr_test_playback)
olr_add_unit_test(tst_framehandle olr_test_playback)
olr_add_unit_test(tst_gpusurface olr_test_playback)
olr_add_unit_test(tst_gpusurfacelease olr_test_playback)
olr_add_unit_test(tst_yuv420pcompositor olr_test_playback)
olr_add_unit_test(tst_formatcanon olr_test_playback)
olr_add_unit_test(tst_qtpreviewsink olr_test_playback)
if(WIN32)
    set_tests_properties(tst_qtpreviewsink PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endif()
olr_add_unit_test(tst_colormetadatapolicy olr_test_playback)
olr_add_unit_test(tst_colormetadataplumb olr_test_playback)
olr_add_unit_test(tst_outputdispatcher olr_test_playback)
olr_add_unit_test(tst_outputdispatcher_holdlast olr_test_playback)
olr_add_unit_test(tst_gpureadbacktelemetry olr_test_playback)
olr_add_unit_test(tst_outputdispatch_gpustats olr_test_playback)
olr_add_unit_test(tst_sinkgpucapability olr_test_playback)
olr_add_unit_test(tst_sinkcapabilityprobe olr_test_playback)
olr_add_unit_test(tst_decklinksink olr_test_playback)
olr_add_unit_test(tst_st2110framer olr_test_playback)
olr_add_unit_test(tst_ajasink olr_test_playback)
olr_add_unit_test(tst_omtsink olr_test_playback)
olr_add_unit_test(tst_iotargetsinkfactory olr_test_playback)
olr_add_unit_test(tst_outputruntime olr_test_playback)
olr_add_unit_test(tst_queuedoutputsink olr_test_playback)
olr_add_unit_test(tst_ndisink olr_test_playback)
olr_add_unit_test(tst_commitgate olr_test_playback)
target_compile_definitions(tst_commitgate PRIVATE OLR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
olr_add_unit_test(tst_sharedcacheslot olr_test_playback)
olr_add_unit_test(tst_frameindex olr_test_playback)
olr_add_unit_test(tst_replayplaylist olr_test_playback)
olr_add_unit_test(tst_playlistentriesmodel olr_test_playback)
olr_add_unit_test(tst_cutschedule olr_test_playback)
olr_add_unit_test(tst_playlistplayout olr_test_playback)
olr_add_unit_test(tst_seekcoalescer olr_test_playback)
olr_add_unit_test(tst_audioplayer_mutefade olr_test_playback)
qt_add_executable(tst_ndi_runtime_smoke tst_ndi_runtime_smoke.cpp)
target_link_libraries(tst_ndi_runtime_smoke PRIVATE Qt6::Test olr_test_playback olr_warnings olr_sanitize)
add_test(NAME tst_ndi_runtime_smoke COMMAND tst_ndi_runtime_smoke)
set_tests_properties(tst_ndi_runtime_smoke PROPERTIES
    LABELS "ndi-runtime"
    TIMEOUT 420
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=420000")
olr_add_unit_test(tst_streamdeckmappingstore olr_test_core)
olr_add_unit_test(tst_srt_health olr_test_core)
olr_add_unit_test(tst_rtmp_health      olr_test_core)
olr_add_unit_test(tst_videocodecchoice)
# Header-only test: no olr_test_core link, so add the project root include manually.
target_include_directories(tst_videocodecchoice PRIVATE "${CMAKE_SOURCE_DIR}")
olr_add_unit_test(tst_avcc olr_test_engine)
olr_add_unit_test(tst_benchmarkplan olr_test_engine)
olr_add_unit_test(tst_codecbenchmark olr_test_engine)
olr_add_unit_test(tst_nativevideoencoder olr_test_engine)
if(APPLE)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_videotoolbox.mm")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}" "${OLR_FFMPEG_INCLUDE}")
    target_link_directories(olr_test_nativevideoencoder PUBLIC "${OLR_FFMPEG_LIBDIR}")
    target_link_libraries(olr_test_nativevideoencoder
        PUBLIC Qt6::Core olr_test_engine
        PRIVATE "${OLR_FFMPEG_AVUTIL_LIBRARY}" "-framework VideoToolbox" "-framework CoreMedia"
                "-framework CoreFoundation" "-framework CoreVideo" "-framework IOSurface")
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
elseif(WIN32)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_mediafoundation.cpp")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}" "${OLR_FFMPEG_INCLUDE}")
    target_link_directories(olr_test_nativevideoencoder PUBLIC "${OLR_FFMPEG_LIBDIR}")
    target_link_libraries(olr_test_nativevideoencoder
        PUBLIC Qt6::Core olr_test_engine
        PRIVATE "${OLR_FFMPEG_AVUTIL_LIBRARY}" mfplat mf mfuuid mfreadwrite strmiids d3d11 dxgi ole32)
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
elseif(NOT WIN32)
    add_library(olr_test_nativevideoencoder STATIC
        "${CMAKE_SOURCE_DIR}/recorder_engine/codec/nativevideoencoder_stub.cpp")
    target_include_directories(olr_test_nativevideoencoder PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(olr_test_nativevideoencoder PUBLIC Qt6::Core olr_test_engine)
    target_link_libraries(tst_nativevideoencoder PRIVATE olr_test_nativevideoencoder)
endif()
olr_add_unit_test(tst_h264_roundtrip olr_test_engine)
target_link_libraries(tst_h264_roundtrip PRIVATE olr_test_nativevideoencoder)
if(OLR_GPU_PIPELINE)
    target_link_libraries(tst_h264_roundtrip
        PRIVATE olr_test_nativevideodecoder olr_test_playback olr_test_gpu)
    target_compile_definitions(tst_h264_roundtrip PRIVATE OLR_GPU_PIPELINE_BUILD=1)
endif()
olr_add_unit_test(tst_vtiosurface olr_test_engine)
target_link_libraries(tst_vtiosurface PRIVATE
    olr_test_nativevideoencoder
    olr_test_nativevideodecoder)
olr_add_unit_test(tst_realcodecbenchmark olr_test_engine)
target_link_libraries(tst_realcodecbenchmark PRIVATE olr_test_nativevideoencoder olr_test_nativevideodecoder)
olr_add_unit_test(tst_benchmarkcache olr_test_engine)
olr_add_unit_test(tst_recordgate olr_test_engine)
olr_add_unit_test(tst_streamworker_gpuencode olr_test_engine)

if(OLR_GPU_PIPELINE)
    olr_add_unit_test(tst_shadertoolchain olr_test_gpu)
    olr_add_unit_test(tst_olrrhi olr_test_gpu)
    olr_add_unit_test(tst_gpurhicontext olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpuframedata olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_decodedonefence olr_test_gpu)
    olr_add_unit_test(tst_gpufence olr_test_gpu)
    olr_add_unit_test(tst_gpugeneration olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_iosgpupolicy olr_test_gpu)
    olr_add_unit_test(tst_iosgpulifecycle olr_test_gpu)
    olr_add_unit_test(tst_devicelossmonitor olr_test_gpu)
    olr_add_unit_test(tst_gpu_devicelost_worker olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpu_devicelost_resume olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpubudget olr_test_gpu)
    olr_add_unit_test(tst_gpu_budget_stress olr_test_gpu)
    olr_add_unit_test(tst_gpuseekprefetch olr_test_gpu)
    olr_add_unit_test(tst_gpu_encode_surface olr_test_engine olr_test_playback)
    target_link_libraries(tst_gpu_encode_surface PRIVATE olr_test_nativevideoencoder)
    olr_add_unit_test(tst_gpuencodepump olr_test_engine olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpusurfaceallocator olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_evictionguard olr_test_playback)
    olr_add_unit_test(tst_stagingfence olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpureadbackring olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_asyncgpureadbacksink olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_playbackworker olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpu_sync_stress olr_test_playback olr_test_gpu)
    olr_add_unit_test(tst_gpu_microstress olr_test_playback)
    olr_add_unit_test(tst_gpucompositor olr_test_gpu)
    olr_add_unit_test(tst_gpucompositor_stress olr_test_gpu)
    target_link_libraries(tst_outputbusengine PRIVATE olr_test_gpu)

    set(gpu_mutation_root "${CMAKE_CURRENT_BINARY_DIR}/gpu-retirement-mutations")
    function(olr_generate_gpu_mutation output source mutation)
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}"
                -DGENERATE_MUTATION=${mutation}
                -DINPUT_SOURCE=${source}
                -DOUTPUT_SOURCE=${output}
                -P "${CMAKE_SOURCE_DIR}/tests/mutations/gpu_retirement_mutations.cmake"
            DEPENDS
                "${source}"
                "${CMAKE_SOURCE_DIR}/tests/mutations/gpu_retirement_mutations.cmake"
            VERBATIM)
        set_source_files_properties("${output}" PROPERTIES GENERATED TRUE)
    endfunction()

    function(olr_configure_gpu_retirement_mutant target mutation_definition)
        target_compile_definitions(${target} PRIVATE ${mutation_definition} OLR_UNIT_TEST=1)
        target_link_libraries(${target} PRIVATE
            Qt6::Test olr_test_playback olr_test_gpu olr_warnings olr_sanitize)
        if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}>
                    $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS
                COMMENT "Copying ${target} runtime DLLs")
        endif()
    endfunction()

    set(gpu_compatibility_overlay "${gpu_mutation_root}/compatibility")
    set(gpu_compatibility_header
        "${gpu_compatibility_overlay}/playback/gpu/gpusubmission.h")
    olr_generate_gpu_mutation("${gpu_compatibility_header}"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpusubmission.h" COMPATIBILITY)
    qt_add_executable(gpu_retirement_compatibility_mutant
        "${CMAKE_SOURCE_DIR}/tests/unit/tst_gpusurfacelease.cpp"
        "${gpu_compatibility_header}")
    target_include_directories(gpu_retirement_compatibility_mutant BEFORE PRIVATE
        "${gpu_compatibility_overlay}")
    olr_configure_gpu_retirement_mutant(gpu_retirement_compatibility_mutant
        OLR_MUTATE_GPU_COMPATIBILITY=1)

    set(gpu_loss_generation_overlay "${gpu_mutation_root}/loss-generation")
    set(gpu_loss_generation_header
        "${gpu_loss_generation_overlay}/playback/gpu/gpudevicelossmonitor.h")
    olr_generate_gpu_mutation("${gpu_loss_generation_header}"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpudevicelossmonitor.h" LOSS_GENERATION)
    qt_add_executable(gpu_retirement_loss_generation_mutant
        "${CMAKE_SOURCE_DIR}/tests/unit/tst_devicelossmonitor.cpp"
        "${gpu_loss_generation_header}")
    target_include_directories(gpu_retirement_loss_generation_mutant BEFORE PRIVATE
        "${gpu_loss_generation_overlay}")
    olr_configure_gpu_retirement_mutant(gpu_retirement_loss_generation_mutant
        OLR_MUTATE_GPU_LOSS_GENERATION=1)

    set(gpu_publication_source "${gpu_mutation_root}/gpu_retire_publication.cpp")
    olr_generate_gpu_mutation("${gpu_publication_source}"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp" RETIRE_PUBLICATION)
    add_library(olr_gpu_retirement_publication_mutant OBJECT "${gpu_publication_source}")
    set_target_properties(olr_gpu_retirement_publication_mutant PROPERTIES AUTOMOC OFF)
    target_include_directories(olr_gpu_retirement_publication_mutant PRIVATE
        "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(olr_gpu_retirement_publication_mutant PRIVATE
        OLR_MUTATE_GPU_RETIRE_PUBLICATION=1 OLR_UNIT_TEST=1)
    target_link_libraries(olr_gpu_retirement_publication_mutant PRIVATE olr_test_playback)
    qt_add_executable(gpu_retirement_publication_mutant
        "${CMAKE_SOURCE_DIR}/tests/unit/tst_gpusurfacelease.cpp"
        $<TARGET_OBJECTS:olr_gpu_retirement_publication_mutant>)
    olr_configure_gpu_retirement_mutant(gpu_retirement_publication_mutant
        OLR_MUTATE_GPU_RETIRE_PUBLICATION=1)

    set(gpu_quarantine_source "${gpu_mutation_root}/gpu_quarantine_transfer.cpp")
    olr_generate_gpu_mutation("${gpu_quarantine_source}"
        "${CMAKE_SOURCE_DIR}/playback/gpu/gpuretireregistry.cpp" QUARANTINE_TRANSFER)
    add_library(olr_gpu_quarantine_transfer_mutant OBJECT "${gpu_quarantine_source}")
    set_target_properties(olr_gpu_quarantine_transfer_mutant PROPERTIES AUTOMOC OFF)
    target_include_directories(olr_gpu_quarantine_transfer_mutant PRIVATE
        "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(olr_gpu_quarantine_transfer_mutant PRIVATE
        OLR_MUTATE_GPU_QUARANTINE_TRANSFER=1 OLR_UNIT_TEST=1)
    target_link_libraries(olr_gpu_quarantine_transfer_mutant PRIVATE olr_test_playback)
    qt_add_executable(gpu_retirement_quarantine_mutant
        "${CMAKE_SOURCE_DIR}/tests/unit/tst_gpusurfacelease.cpp"
        $<TARGET_OBJECTS:olr_gpu_quarantine_transfer_mutant>)
    olr_configure_gpu_retirement_mutant(gpu_retirement_quarantine_mutant
        OLR_MUTATE_GPU_QUARANTINE_TRANSFER=1)

    function(olr_add_gpu_retirement_mutation_test name baseline mutant test_function
             expected_test_name expected_failure_token)
        add_test(NAME gpu_retirement_mutation_${name}
            COMMAND "${CMAKE_COMMAND}"
                -DMUTANT_NAME=${name}
                -DBASELINE_EXECUTABLE=$<TARGET_FILE:${baseline}>
                -DMUTANT_EXECUTABLE=$<TARGET_FILE:${mutant}>
                -DTEST_FUNCTION=${test_function}
                -DEXPECTED_TEST_NAME=${expected_test_name}
                -DEXPECTED_FAILURE_TOKEN=${expected_failure_token}
                -P "${CMAKE_SOURCE_DIR}/tests/mutations/gpu_retirement_mutations.cmake")
        set_tests_properties(gpu_retirement_mutation_${name} PROPERTIES
            LABELS "gpu-capability;unit;ci;mutation"
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()

    olr_add_gpu_retirement_mutation_test(compatibility tst_gpusurfacelease
        gpu_retirement_compatibility_mutant
        fusedSubmissionRejectsIncompatibleEvidenceBeforeCallback
        "TestGpuSurfaceLease::fusedSubmissionRejectsIncompatibleEvidenceBeforeCallback()"
        result.outcome)
    olr_add_gpu_retirement_mutation_test(loss_generation tst_devicelossmonitor
        gpu_retirement_loss_generation_mutant
        validatedRecoveryRejectsMismatchedLossGeneration
        "TestDeviceLossMonitor::validatedRecoveryRejectsMismatchedLossGeneration()"
        called)
    olr_add_gpu_retirement_mutation_test(retire_publication tst_gpusurfacelease
        gpu_retirement_publication_mutant fusedSubmissionPublishesSubmittedOwners
        "TestGpuSurfaceLease::fusedSubmissionPublishesSubmittedOwners()"
        registry.pendingRetainCount)
    olr_add_gpu_retirement_mutation_test(quarantine_transfer tst_gpusurfacelease
        gpu_retirement_quarantine_mutant fusedSubmissionQuarantinesZeroSignal
        "TestGpuSurfaceLease::fusedSubmissionQuarantinesZeroSignal()"
        registry.pendingRetainCount)
endif()
