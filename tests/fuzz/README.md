# Parser fuzzers

libFuzzer harnesses for OpenLiveReplay's **untrusted-input parsers** — the
SRT/UDP/RTMP ingest path is the only attacker-reachable code, and the only code
that must stay crash-free on arbitrary bytes.

| Harness | Code under test | Surface |
|---|---|---|
| `fuzz_mpegtsparser`   | `recorder_engine/ingest/mpegtsparser.cpp`   | TS section (PAT/PMT) parsing, continuity, PES reassembly (incl. the unbounded-PES cap) |
| `fuzz_h26xaccessunit` | `recorder_engine/ingest/h26xaccessunit.cpp` | H.264/HEVC NAL splitting + parameter-set inspection |
| `fuzz_h26xseitimecode`| `recorder_engine/ingest/h26xseitimecode.cpp`| SMPTE 12M SEI timecode extraction (pic_timing / time_code / ATC) |
| `fuzz_rtmpprotocol`   | `recorder_engine/ingest/rtmpprotocol.cpp`   | RTMP chunk reassembly, FLV/AVCC/HEVC/AAC sequence headers, AMF0 reader |

Each target compiles its parser directly with `-fsanitize=fuzzer,address,undefined`
(separate objects from the normal build), so a finding is a real crash / leak / UB
in the parser, reproducible by replaying the saved input file.

## Build

Requires a clang that ships the libFuzzer runtime. Linux system clang has it;
on macOS use Homebrew LLVM (Apple's clang compiles `-fsanitize=fuzzer` but does
**not** ship `libclang_rt.fuzzer`).

```sh
# Linux
cmake -S . -B build-fuzz -G Ninja -DOLR_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$QT_ROOT_DIR"

# macOS (Homebrew LLVM)
cmake -S . -B build-fuzz -G Ninja -DOLR_BUILD_FUZZERS=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)" \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.1/macos"

cmake --build build-fuzz
```

## Run

```sh
# fuzz for two minutes (LeakSanitizer is off; libFuzzer manages lifetimes)
ASAN_OPTIONS=detect_leaks=0 \
  ./build-fuzz/tests/fuzz/fuzz_mpegtsparser -max_total_time=120 corpus/mpegtsparser

# reproduce a saved finding
./build-fuzz/tests/fuzz/fuzz_mpegtsparser fuzz_mpegtsparser-crash-<hash>
```

Keep an evolving corpus under `corpus/<harness>/` (git-ignored; seed it from real
captures or let libFuzzer grow it). Continuous fuzzing is **not** a PR gate — the
`Fuzz (parsers)` CI job runs the harnesses on a schedule / on demand.
