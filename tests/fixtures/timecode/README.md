# Timecode fixture provenance

Binary fixtures in this directory are retained only when their origin can be
reproduced and their encoder family can be verified. Constructed branch-level
vectors live in `tst_h26xseitimecode.cpp` and are not described as encoder
output.

## `hevc_hm_time_code.265`

- Encoder: Fraunhofer/JVET HM-18.0, official tag `HM-18.0`, commit
  `fb4486d5`, <https://vcgit.hhi.fraunhofer.de/jvet/HM>.
- License: BSD 3-Clause, upstream `COPYING`.
- Build: CMake/Ninja, MinGW GCC 13.1.0, Release. The old source needed
  `-Wno-error=array-bounds` to build with GCC 13; no encoder source was edited.
- Input: one all-zero 64x64 8-bit 4:2:0 frame, created with
  `fsutil file createnew black64.yuv 6144`.
- Syntax: one-frame Annex-B HEVC stream with SPS VUI timing/HRD, picture-timing
  SEI, and prefix-SEI payload type 136. The time-code message is
  `10:11:12:13`, counting type 1, discontinuity set, with a 6-bit signed
  offset of -7. HM's `FrameRate=30` VUI maps to the exact rate 30000/1001.
- SHA-256: `A5E73871ACCC11D6D670C10F0988BC264294C2E959B25CACC2573DD619B65737`.

Generation command (paths abbreviated):

```text
TAppEncoder -c cfg/encoder_intra_main.cfg
  --InputFile=black64.yuv --BitstreamFile=hevc_hm_time_code.265
  --ReconFile=hm-rec.yuv --SourceWidth=64 --SourceHeight=64
  --FrameRate=30 --FrameSkip=0 --FramesToBeEncoded=1
  --InputBitDepth=8 --InputChromaFormat=420
  --VuiParametersPresent=1 --TargetBitrate=100000 --SEIPictureTiming=1
  --SEITimeCodeEnabled=1 --SEITimeCodeNumClockTs=1
  --SEITimeCodeTimeStampFlag=1 --SEITimeCodeFieldBasedFlag=0
  --SEITimeCodeCountingType=1 --SEITimeCodeFullTsFlag=1
  --SEITimeCodeDiscontinuityFlag=1 --SEITimeCodeCntDroppedFlag=0
  --SEITimeCodeNumFrames=13 --SEITimeCodeSecondsValue=12
  --SEITimeCodeMinutesValue=11 --SEITimeCodeHoursValue=10
  --SEITimeCodeOffsetLength=6 --SEITimeCodeTimeOffset=-7
```

## `hevc_shm_time_code.265`

- Encoder: Fraunhofer/JVET SHM-12.4 (HM-16.10), official tag `SHM-12.4`,
  commit `3a70cb4c`,
  <https://vcgit.hhi.fraunhofer.de/jvet/SHM/-/tags/SHM-12.4>.
- License: BSD 3-Clause, upstream `COPYING`.
- Build: upstream Linux makefiles under Git-for-Windows `sh`, MinGW GCC
  13.1.0. Old-source GCC diagnostics were demoted from errors; no encoder
  source was edited. `-ldl` was omitted for the native Windows link.
- Input: the same one-frame all-zero 64x64 8-bit 4:2:0 source.
- Syntax: single-layer Annex-B HEVC with SPS VUI timing/HRD, picture-timing
  SEI, and prefix-SEI payload type 136. The time-code message is
  `01:02:03:04`, counting type 0, no discontinuity and no offset.
- SHA-256: `7A81F2A1A9D427982DFB3AF88828DCA474C21E93F85077A195EB0323A43462CB`.

Generation command (paths abbreviated):

```text
TAppEncoderStatic -c cfg/encoder_intra_scalable.cfg
  --NumLayers=1 --InputFile0=black64.yuv --ReconFile0=shm-rec.yuv
  --BitstreamFile=hevc_shm_time_code.265 --FrameRate0=30
  --SourceWidth0=64 --SourceHeight0=64 --FramesToBeEncoded=1
  --InputBitDepth0=8 --InternalBitDepth0=8 --QP0=32
  --IntraPeriod0=1 --DecodingRefreshType=1
  --VuiParametersPresent=1 --TargetBitrate0=100000 --SEIPictureTiming=1
  --SEITimeCodeEnabled=1 --SEITimeCodeNumClockTs=1
  --SEITimeCodeTimeStampFlag=1 --SEITimeCodeFieldBasedFlag=0
  --SEITimeCodeCountingType=0 --SEITimeCodeFullTsFlag=1
  --SEITimeCodeDiscontinuityFlag=0 --SEITimeCodeCntDroppedFlag=0
  --SEITimeCodeNumFrames=4 --SEITimeCodeSecondsValue=3
  --SEITimeCodeMinutesValue=2 --SEITimeCodeHoursValue=1
  --SEITimeCodeOffsetLength=0 --SEITimeCodeTimeOffset=0
```

## `h264_x264_pic_timing.264` (negative family evidence)

- Encoder: VideoLAN x264 official `master`, commit
  `0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee`,
  <https://code.videolan.org/videolan/x264>.
- License: GPL-2.0-or-later, upstream `COPYING` and source-file notices.
- Build: Git-for-Windows `sh`, MinGW GCC 13.1.0; no source edits.
- Input: the same one-frame all-zero 64x64 8-bit 4:2:0 source.
- Syntax/result: x264 emits SPS VUI timing and a `pic_timing` SEI when
  `--pic-struct` is enabled, but deliberately writes every
  `clock_timestamp_flag` as zero (`encoder/set.c`, beside the comment that
  clock timestamps are not standardised). The expected extractor result is
  therefore no timecode evidence, not a fabricated `00:00:00:00` label.
- SHA-256: `EDEF2151A6CCFA404C8474AF4FFDB05B59B3F6357C18BEE78131548291ACF47B`.

Build and generation commands (paths abbreviated):

```text
git clone https://code.videolan.org/videolan/x264.git x264
git -C x264 checkout 0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee
cd x264
./configure --host=x86_64-w64-mingw32 --disable-asm --disable-opencl --enable-static
mingw32-make -j4
x264.exe --demuxer raw --input-res 64x64 --input-csp i420
  --fps 30000/1001 --frames 1 --keyint 1 --pic-struct
  --output h264_x264_pic_timing.264 black64.yuv
```

## `h264_jm_pic_timing.264` (negative family evidence)

- Encoder: Fraunhofer/JVET JM official `master`, commit
  `8b34eee1576952dc2a04cd2fdb52febfde4030b2`,
  <https://vcgit.hhi.fraunhofer.de/jvet/JM>.
- License: the upstream ITU-T/ISO reference-software notices in
  `COPYRIGHT_ITU.txt`, `COPYRIGHT_ISO_IEC.txt`, and `disclaimer.txt`.
- Build: CMake/Ninja, MinGW GCC 13.1.0, Release. Three build-compatibility-only
  adjustments were required for the modern Windows toolchain: guard the old
  `intptr_t` typedef and use GCC `inline` in `source/lib/lcommon/win32.h`, fix
  three invalid `% 1.5s` formats in `source/app/lencod/report.c`, and link
  `ws2_32` from `source/app/lencod/CMakeLists.txt`. No SEI or encoder behaviour
  was changed.
- Input: the same one-frame all-zero 64x64 8-bit 4:2:0 source.
- Syntax/result: `SEIVUI32Pulldown=1` produces SPS VUI timing and a genuine JM
  `pic_timing` SEI. JM assigns every `clock_timestamp_flag[i] = FALSE` in
  `source/app/lencod/sei.c`, so the expected extractor result is no timecode
  evidence.
- SHA-256: `BBF0025AD3009CB6376D1880547962D9AA360DE73DD63046FA44AED36855B66D`.

Build and generation commands (paths abbreviated):

```text
git clone https://vcgit.hhi.fraunhofer.de/jvet/JM.git JM
git -C JM checkout 8b34eee1576952dc2a04cd2fdb52febfde4030b2
cmake -S JM -B JM-build -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe
  -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
  -DCMAKE_C_FLAGS=-Wno-error
cmake --build JM-build --target lencod -j4
lencod.exe -d cfg/encoder.cfg
  -p InputFile=black64.yuv -p OutputFile=h264_jm_pic_timing.264
  -p ReconFile=h264_jm_recon.yuv -p StatsFile=h264_jm_stats.dat
  -p SourceWidth=64 -p SourceHeight=64 -p OutputWidth=64 -p OutputHeight=64
  -p FramesToBeEncoded=1 -p FrameRate=24 -p SEIVUI32Pulldown=1
  -p NumberBFrames=0 -p ProfileIDC=100 -p LevelIDC=10
```

These files are deliberately retained as negative captures: they prove the
actual behaviour of two real H.264 encoder families and prevent the parser
from turning syntactically present `pic_timing` metadata into false confident
timecode evidence. Patching either encoder's clock flags would instead create
a project-authored vector and would not establish upstream family support.

## Registered ITU-T T.35 result

No `registered_atc_h264.264` fixture is present. H.264/H.265 define the T.35
country envelope but assign the body to registered providers; the approved
plan did not identify a published ATC registration. ATSC `GA94` user-data type
3 is caption `cc_data`, and SMPTE ST 334-2 CDP timecode is a VANC structure,
not a GA94 ATC-in-SEI mapping. Tests intentionally prove these inputs are
ignored. Adding a purported registered-ATC vector would mislabel caption or
private data.
