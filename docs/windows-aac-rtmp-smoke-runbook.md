# Windows AAC / RTMP live smoke-test runbook

A repeatable manual procedure for validating the **Windows Media Foundation AAC
decoder** (`recorder_engine/ingest/nativeaacdecoder_mediafoundation.cpp`) and the
**native RTMP / SRT ingest** on a real Windows box. These `#ifdef _WIN32` paths
are compile-checked and unit-tested in CI (see below), but the live audio
pitch/speed, channel up-mix, and graceful-degradation behaviours can only be
confirmed on real hardware with a real transmitter.

> **What CI already covers (and what it does not).**
> `.github/workflows/build.yml` (the `windows-latest` job) now builds and runs
> two deterministic Media Foundation unit tests on every dispatch:
> `tst_nativeaacdecoder` (feeds a canned base64 AAC-LC ADTS fixture through the
> in-box MS AAC Decoder MFT — proves the MFT accepts our
> `MF_MT_USER_DATA`/AudioSpecificConfig blob and produces non-silent PCM) and
> `tst_nativevideodecoder` (probes the MF video-decode capability report). Those
> are canned and need no network. **Everything in this runbook is the part CI
> cannot do**: real pitch/speed perception, real mono→stereo audio output, the
> missing-MFT degradation path on an older SKU, and a live mid-stream
> renegotiation. None of it is gated by CI; it requires a physical Windows
> machine and a transmitter.
>
> This runbook has **not** been executed — it is written from the source on a
> macOS worktree. Treat every "Expected" below as the contract to verify, not a
> recorded pass.

---

## 0. Prerequisites

| Need | Detail |
| --- | --- |
| **Windows SKU with the AAC MFT** | Windows 10/11 Desktop (Home/Pro/Enterprise) ships the Microsoft AAC Decoder MFT (`CLSID_CMSAACDecMFT`). Windows **N** SKUs and Server Core ship *without* the Media Feature Pack — use one of those (or an N SKU) for the §4 "decoder unavailable" case. |
| **HEVC Video Extensions** (optional) | Only needed to exercise HEVC video alongside AAC. Absent it, `caps.hevc=false` and HEVC sources fail (by design, §4 of the workstream doc). AAC audio tests below use H.264 video and do **not** require it. |
| **A built OpenLiveReplay.exe** | `./build-scripts/build_windows_app.sh` (see `docs/windows-build.md`). The deps step also drops `ffmpeg.exe`, `ffprobe.exe`, and `srt-live-transmit.exe` into `windows_build/dist/{ffmpeg,srt}/bin` — use *that* ffmpeg as the transmitter so the encoder set matches what the app links. |
| **A transmitter** | The bundled `ffmpeg.exe` (commands below) **or** OBS Studio (Settings → Stream → Custom, an RTMP URL, AAC audio). |
| **A way to hear / measure audio** | Headphones, plus optionally a tone/spectrum analyzer (e.g. a phone tuner app) to confirm a 1 kHz reference tone stays at 1 kHz (correct pitch) and 5 s of source is 5 s of playback (correct speed). |

**Conventions used below**

- `FFMPEG=windows_build/dist/ffmpeg/bin/ffmpeg.exe` (run from Git Bash at the repo root).
- The app ingests `rtmp://`/`rtmps://` via native RTMP and `srt://` via native SRT.
  RTMP needs a listener the app connects to, **or** point the app at a source it
  can pull. The simplest loopback is `ffmpeg` publishing to an RTMP server you
  run locally (e.g. `nginx-rtmp` or `mediamtx`), with the app pulling the same
  URL. SRT supports `mode=caller` directly against `srt-live-transmit` /
  `ffmpeg` in listener mode.
- Replace `HOST:PORT` / `app/stream` with your local relay's address.
- The native SRT ingest is **caller-only and rejects encrypted URLs** (workstream
  doc §3) — do not add `passphrase=`/`pbkeylen=` here.

**Where to look for results.** App log (stderr / the app's log pane) and the
telemetry/health surface:

- Per-source **health dot** is backend-tagged (`srtHealth` / `rtmpHealth`); a
  healthy decoding source is green.
- AAC decode failures are logged via the `hrMessage()` strings in
  `nativeaacdecoder_mediafoundation.cpp`, e.g.
  `Media Foundation AAC decoder is unavailable (HRESULT 0x…)`,
  `Media Foundation AAC input type setup failed (HRESULT 0x…)`
  (this is the `MF_E_INVALIDMEDIATYPE` symptom),
  `NativeAacDecoder: only AAC-LC is supported`. **Note on log rate:** these AAC
  *audio* decode-error logs are emitted **per occurrence on both SRT and RTMP**
  (`NativeSrtIngestSession`/`NativeRtmpIngestSession::log` do not throttle the
  audio path), so a sustained decode failure produces one log line per failed
  audio frame — that is expected, not a separate bug. (The once-/5 s throttle in
  `nativesrtingestsession.cpp` applies only to the SRT *video* decode-error path,
  not to these AAC audio errors.)

---

## Test 1 — MFT accepts the AudioSpecificConfig blob (no `MF_E_INVALIDMEDIATYPE`)

**What it proves.** `configureTypes()` builds a 14-byte `MF_MT_USER_DATA`
(HEAACWAVEINFO tail + 2-byte AudioSpecificConfig) and calls
`SetInputType`/`SetOutputType`. If the blob layout is wrong the MFT returns
`MF_E_INVALIDMEDIATYPE` and the source never decodes.

**Sender (48 kHz stereo AAC-LC over RTMP):**

```bash
"$FFMPEG" -hide_banner -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 \
  -c:a aac -b:a 128k -ac 2 -ar 48000 \
  -f flv "rtmp://HOST:PORT/app/stream"
```

(The native `aac` encoder is AAC-LC, matching the decoder's AAC-LC-only envelope.
`-re` paces at real time. For an SRT variant, swap the last line for
`-f mpegts "srt://HOST:PORT?mode=caller"` and point the app at the same `srt://` URL.)

**Steps.** Start the relay, start the sender, point OpenLiveReplay at the URL,
begin a recording.

**Expected.**
- Source health goes green; video shows the `testsrc2` pattern.
- **No** `MF_E_INVALIDMEDIATYPE` / "input type setup failed" in the log.
- Audio is present (a steady 1 kHz tone). This is the baseline "the MFT took our
  config" pass — pitch/speed correctness is Test 2.

---

## Test 2 — Correct pitch & speed at 44.1 kHz *and* 48 kHz

**What it proves.** The decoder requests PCM at the source rate, reads back the
*actually negotiated* rate (`storeNegotiatedOutputFormat`), and
`appendResampledStereo()` resamples to the engine-canonical **48 kHz** (the
48 kHz path is a passthrough; 44.1 kHz exercises the linear resampler). A bug
here makes 44.1 kHz audio play slightly fast/sharp or slow/flat.

**Sender A — 48 kHz (resampler passthrough):** as Test 1.

**Sender B — 44.1 kHz (resampler active):**

```bash
"$FFMPEG" -hide_banner -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=44100" \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 \
  -c:a aac -b:a 128k -ac 2 -ar 44100 \
  -f flv "rtmp://HOST:PORT/app/stream"
```

**Steps.** Run each sender in turn; record ~30 s; play back.

**Expected.**
- Both record/play with audio.
- The 1 kHz reference tone reads as **1 kHz** on a tuner/analyzer for both
  sources (a 44.1k→48k speed error would shift it to ~1088 Hz; the inverse to
  ~919 Hz). A clean tone with no chipmunk/slow-mo artefact confirms pitch.
- Wall-clock duration of playback matches the recorded duration (no drift from a
  mis-set rate) — confirms speed.

---

## Test 3 — Mono AAC source → both stereo channels present

**What it proves.** `appendResampledStereo()` duplicates the single source
channel into L and R for mono input (`rightAt` falls back to the left sample when
`sourceChannels < 2`). The engine contract is always 48 kHz **stereo**.

**Sender (mono AAC-LC):**

```bash
"$FFMPEG" -hide_banner -re \
  -f lavfi -i "testsrc2=size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 \
  -c:a aac -b:a 96k -ac 1 -ar 48000 \
  -f flv "rtmp://HOST:PORT/app/stream"
```

(Note `-ac 1`. A 44.1 kHz mono variant — `-ar 44100` — additionally re-checks the
resampler on a mono frame.)

**Expected.**
- Source decodes; audio is audible in **both** ears / both meters at equal level
  (not left-only, not silent on one side).
- No `only AAC-LC is supported` or channel-count errors in the log.

---

## Test 4 — Graceful degradation when the AAC MFT is absent (older / N SKU)

**What it proves.** On a SKU without the AAC Decoder MFT, `createTransform()`
fails both `MFTEnumEx` and the `CoCreateInstance(CLSID_CMSAACDecMFT, …)` fallback
and returns the error `Media Foundation AAC decoder is unavailable`. The app must
**drop audio but keep video** rather than crash or drop the source.

**Setup.** Use a Windows **N** SKU / Server Core **without** the Media Feature
Pack (do not install it), or a VM imaged that way. Use any Test 1–3 sender.

**Expected.**
- Video continues (source stays connected, frames keep arriving).
- Log shows `Media Foundation AAC decoder is unavailable (HRESULT 0x…)`, emitted
  per failed audio frame on both SRT and RTMP (see the log-rate note in §0). The
  *app* must not spin or crash regardless of the log volume.
- App does not crash; the recording is video-only (audio dropped).
- *(Contrast on a normal SKU: installing the Media Feature Pack and repeating
  should restore audio — confirms the gate is the MFT, not our config.)*

---

## Test 5 — `MF_E_TRANSFORM_STREAM_CHANGE` on a mid-stream renegotiation (§2)

**What it proves.** When the MFT renegotiates its output mid-stream,
`processFrame()` catches `MF_E_TRANSFORM_STREAM_CHANGE`, re-asserts the explicit
PCM output type via `applyOutputType()`, re-caches the negotiated rate/channels,
and continues — **bounded**, with no infinite `ProcessOutput` loop and no
pitch/channel corruption afterwards. This path has never run on a real device.

**How to trigger.** Force the source's AAC format to change *within one
connection* so the decoder's configured `(rate, channels, audioObjectType)`
changes and/or the MFT re-emits a new output type. Two practical ways:

1. **Channel-count change** — publish a segment as stereo, then as mono (or vice
   versa) on the *same* RTMP/SRT URL without the app reconnecting. With `ffmpeg`,
   the cleanest reproduction is a concat of two differently-encoded inputs:

   ```bash
   # Pre-make two 10 s clips that differ only in audio channel count.
   "$FFMPEG" -hide_banner -f lavfi -i "testsrc2=size=1280x720:rate=30" \
     -f lavfi -i "sine=frequency=1000:sample_rate=48000" -t 10 \
     -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2 -ar 48000 stereo.flv
   "$FFMPEG" -hide_banner -f lavfi -i "testsrc2=size=1280x720:rate=30" \
     -f lavfi -i "sine=frequency=1000:sample_rate=48000" -t 10 \
     -c:v libx264 -pix_fmt yuv420p -c:a aac -ac 1 -ar 48000 mono.flv

   # Stream them back to back on one connection (re-encode so the live stream is
   # continuous; the audio config flips at the 10 s boundary).
   printf "file 'stereo.flv'\nfile 'mono.flv'\n" > list.txt
   "$FFMPEG" -hide_banner -re -f concat -safe 0 -i list.txt \
     -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 60 \
     -c:a aac -b:a 128k -f flv "rtmp://HOST:PORT/app/stream"
   ```

   (A **sample-rate** flip — 48000 then 44100 — is the other useful variant; swap
   `-ar` between the two clips. Sustained re-keying of the resampler is the part
   most worth observing.)

2. **OBS** — start streaming, then change OBS audio sample rate / channels in
   Settings → Audio and apply, which forces a fresh AAC config on the live feed.

**Expected.**
- Audio survives the boundary: a brief glitch is acceptable, but the tone
  recovers to correct **pitch** and correct **channel layout** for the new
  segment (mono segment → both channels per Test 3; 44.1k segment → 1 kHz per
  Test 2).
- The app **does not hang or spin** at the transition (no unbounded loop), CPU
  does not peg, and the source stays green.
- No persistent decode-error spam after the renegotiation settles.

> **Companion watch item (§2).** While running Tests 1–5 under sustained RTMP
> load, keep an eye on **CPU**. The RTMP caller invokes `decoder.reset()` per
> audio frame (~21 ms); the MF decoder makes that a flush-only reset (keeps the
> MFT alive, rebuilds only on a real config change). If CPU is high under steady
> RTMP, profile the MFT-reuse path — that is the open hardening follow-up, not a
> pass/fail gate here.

---

## Quick checklist

| # | Check | Pass condition |
| --- | --- | --- |
| 1 | MFT accepts AudioSpecificConfig | No `MF_E_INVALIDMEDIATYPE` / "input type setup failed"; audio present |
| 2 | 44.1 kHz & 48 kHz pitch/speed | 1 kHz tone reads 1 kHz on both; playback duration == record duration |
| 3 | Mono → stereo | Equal audio in both channels; no channel error |
| 4 | Missing-MFT degradation (N SKU) | Video continues, audio dropped, throttled "unavailable" log, no crash |
| 5 | `STREAM_CHANGE` renegotiation | Audio recovers correctly across the boundary; no hang/loop; source stays green |

---

## Honest status

This runbook is a **procedure, not a test report**. As of writing it has not been
executed: it was authored from the source on a macOS worktree, where the
`#ifdef _WIN32` Media Foundation paths cannot run. The CI unit coverage
(`tst_nativeaacdecoder`, `tst_nativevideodecoder`) is likewise only *defined*
here — its green status is verifiable only once `.github/workflows/build.yml`
runs on GitHub's `windows-latest` runner. Everything above requires a physical
Windows machine and a live transmitter to confirm.
