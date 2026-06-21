# libFuzzer harnesses for the untrusted-input parsers (the SRT/UDP/RTMP ingest
# path — the only attacker-reachable code). Opt-in and built with a clang that
# ships libFuzzer (AppleClang and LLVM clang both do):
#
#   cmake -S . -B build-fuzz -G Ninja -DOLR_BUILD_FUZZERS=ON \
#         -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"            # default compiler, or:
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz
#   ./build-fuzz/tests/fuzz/fuzz_mpegtsparser -max_total_time=120 tests/fuzz/corpus/mpegtsparser
#
# Each target compiles the parser under test directly with fuzzer+ASan+UBSan
# instrumentation (separate objects from the normal build), so a finding is a
# real crash/leak/UB in the parser, reproducible by replaying the saved input.
find_package(Qt6 REQUIRED COMPONENTS Core)

set(_olr_fuzz_flags -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -O1)
set(_ingest "${CMAKE_SOURCE_DIR}/recorder_engine/ingest")

function(olr_add_fuzzer name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE Qt6::Core)
    target_compile_options(${name} PRIVATE ${_olr_fuzz_flags})
    target_link_options(${name} PRIVATE ${_olr_fuzz_flags})
endfunction()

set(_smpte12m "${CMAKE_SOURCE_DIR}/recorder_engine/timing/smpte12m.cpp")
olr_add_fuzzer(fuzz_mpegtsparser   "${_ingest}/mpegtsparser.cpp")
olr_add_fuzzer(fuzz_h26xaccessunit
    "${_ingest}/h26xaccessunit.cpp"
    "${_ingest}/h26xseitimecode.cpp"
    "${_smpte12m}") # the harness co-fuzzes the PES->split->SEI chain
olr_add_fuzzer(fuzz_h26xseitimecode "${_ingest}/h26xseitimecode.cpp" "${_smpte12m}")
olr_add_fuzzer(fuzz_rtmpprotocol   "${_ingest}/rtmpprotocol.cpp")
