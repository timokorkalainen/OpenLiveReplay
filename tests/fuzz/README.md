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

## Seeds & corpus

Committed, structurally-valid seed inputs live under `seeds/<harness>/` (a valid
PAT/PMT/PES stream incl. a length-0 PES, Annex-B AUs, an SEI NAL, RTMP chunks).
They matter: without them a cold run mostly bounces off the front-door checks
(e.g. mpegts needs a valid PAT→PMT before any PES/cap code runs). A grown corpus
lives under `corpus/<harness>/` (git-ignored).

## Run

```sh
# fuzz for two minutes, starting from the seeds, growing a local corpus
ASAN_OPTIONS=detect_leaks=0 \
  ./build-fuzz/tests/fuzz/fuzz_mpegtsparser -max_total_time=120 \
    corpus/fuzz_mpegtsparser tests/fuzz/seeds/fuzz_mpegtsparser

# reproduce a saved finding
./build-fuzz/tests/fuzz/fuzz_mpegtsparser fuzz_mpegtsparser-crash-<hash>
```

Continuous fuzzing is **not** a PR gate. The `Fuzz (parsers, manual)` CI job is
`workflow_dispatch`-only (run it on demand; a nightly `schedule:` can be added
later); it seeds from `seeds/<harness>/` and fails the run on any crash.
