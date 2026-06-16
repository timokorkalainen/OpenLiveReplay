# Native RTMP Broadcast Ingest Design

## Goal

Make native RTMP/RTMPS ingest dependable enough for live TV broadcast use before it is merged as the default RTMP path. The supported native profile includes legacy RTMP FLV and E-RTMP single-default-track ingest with H.264/AVC or HEVC video, AAC-LC audio, RTMPS transport, signed/authenticated URLs, reconnect behavior, long-running streams, arbitrary TCP fragmentation, and operator-visible failure diagnostics.

The implementation avoids FFmpeg for RTMP network receive, chunk parsing, FLV/E-RTMP demuxing, H.264/HEVC packetization, and H.264/HEVC decoding within the supported native profile. RTMP/RTMPS does not fall back to the legacy FFmpeg ingest path once the native backend is available.

## Scope

Supported for merge:

- `rtmp://` and `rtmps://` playback ingest.
- Legacy FLV AVC video packets with AVCDecoderConfigurationRecord and length-prefixed NAL units.
- E-RTMP single-track video packets using FourCC signaling for `avc1` and `hvc1`.
- HEVCDecoderConfigurationRecord parsing for `hvc1`, conversion to VPS/SPS/PPS parameter sets, and length-prefixed HEVC NAL conversion to Annex B.
- AAC-LC audio carried as legacy FLV AAC raw frames with AudioSpecificConfig.
- RTMPS with normal certificate validation, plus a clearly test-scoped insecure option for self-signed local fixtures.
- Signed URLs and authentication query strings.
- Server-driven disconnects, reconnect requests, stalls, source restart, decoder reset, and timestamp discontinuity handling.
- Explicit native failure when the stream is outside the supported profile.

Out of scope for this merge:

- Encoding or publishing RTMP/E-RTMP.
- Decoding AV1, VP9, VVC, Opus, AC-3, E-AC-3, FLAC, MP3, or non-AAC audio.
- Full multitrack recording UX. The parser must understand E-RTMP multitrack enough to select the default track or fail loudly, but recording multiple tracks from one RTMP source is a separate feature.
- Encrypted FLV payloads or DRM.
- AMF3 SCRIPTDATA beyond safely skipping/ignoring where possible.
- A software HEVC decoder.

## Standards And References

The E-RTMP implementation should follow the Veovera E-RTMP v2 release specification:

- E-RTMP extends legacy RTMP/FLV while preserving backward compatibility.
- Modern video codecs are signaled through FourCC values.
- `hvc1` is the E-RTMP FourCC for HEVC.
- E-RTMP video packets use an extended video header with packet types such as `SequenceStart`, `CodedFrames`, `CodedFramesX`, and `SequenceEnd`.
- E-RTMP connect capability signaling uses properties such as `fourCcList`, `videoFourCcInfoMap`, and `audioFourCcInfoMap`.

Reference: https://veovera.org/docs/enhanced/enhanced-rtmp-v2.html

## Architecture

The current native RTMP work should be split into small, testable layers:

- `RtmpChunkParser`: RTMP chunk stream parser and writer. It must be transactional: no parser state may change until the bytes needed for the current parse step are available and consumed.
- `RtmpAmf0`: AMF0 writer/parser for command payloads and the subset of objects, ECMA arrays, strict arrays, strings, booleans, nulls, and numbers needed for RTMP/E-RTMP command and metadata handling.
- `RtmpFlvVideo`: legacy FLV video and E-RTMP video packet parsing. This layer identifies packet format, codec, packet type, composition offset, track ID, FourCC, and raw codec payload.
- `RtmpCodecConfig`: AVCDecoderConfigurationRecord and HEVCDecoderConfigurationRecord parsing into `H26xParameterSets`, plus conversion from length-prefixed AVC/HEVC NAL payloads to Annex B.
- `NativeRtmpIngestSession`: socket lifecycle, RTMP command flow, E-RTMP capability signaling, stream selection, timestamp anchoring, decode orchestration, reconnect behavior, and callback delivery.

Existing native decode components should be reused:

- `VideoToolboxDecoder` already accepts `CompressedAccessUnit` and supports H.264 and HEVC parameter sets.
- `AudioToolboxAacDecoder` already decodes AAC-LC ADTS frames to 48 kHz stereo S16, matching the recorder audio path.
- `StreamWorker` remains responsible for jitter buffering, per-source trim, muxing, source switching, and recording output.

## Connection And Capability Signaling

The native RTMP client should send a legacy-compatible `connect` command and add E-RTMP capability information:

- `fourCcList`: include `avc1`, `hvc1`, and `mp4a`.
- `videoFourCcInfoMap`: advertise decode capability for `avc1` and `hvc1` when native video decode is available.
- `audioFourCcInfoMap`: advertise decode capability for `mp4a` when native AAC decode is available.

The client must preserve authentication data:

- The RTMP app name is derived from the first path segment.
- The play path is the remaining path plus the original query string when present.
- `tcUrl` must retain the scheme, host, port, and app path. It must not drop information that a server needs for signed URL validation unless the RTMP ecosystem convention for that field requires it.

The session must continue to accept legacy servers that ignore E-RTMP capability properties.

## Video Packet Handling

Legacy AVC handling:

1. Parse FLV video header with codec ID 7.
2. Parse `AVCPacketType`.
3. Parse signed 24-bit composition time.
4. Parse AVCDecoderConfigurationRecord from sequence headers.
5. Convert length-prefixed NAL units to Annex B.
6. Decode with `NativeVideoCodec::H264`.

E-RTMP video handling:

1. Detect the E-RTMP video header flag.
2. Parse `VideoPacketType`.
3. Parse FourCC.
4. Parse optional composition time / ModEx timestamp data according to packet type.
5. Handle `SequenceStart` for `avc1` and `hvc1`.
6. Handle `CodedFrames` and `CodedFramesX` for selected/default track video.
7. Handle `SequenceEnd` by resetting the active decoder state for that codec/track.
8. Ignore metadata packets safely after parsing their declared shape.
9. For multitrack packets, select track ID 0 when present. If track 0 is absent or ambiguous, fail native ingest with a clear reason rather than decoding the wrong track.

HEVC handling:

- Parse HEVCDecoderConfigurationRecord into VPS/SPS/PPS NAL arrays.
- Keep the NAL length-size field from the configuration record.
- Convert HEVC length-prefixed NAL payloads to Annex B using the configured length size.
- Emit `CompressedAccessUnit` with `NativeVideoCodec::Hevc`.
- Reset `VideoToolboxDecoder` when VPS/SPS/PPS changes.

Unsupported codecs must not result in a connected black source. Native ingest should fail with a clear unsupported-codec reason and must not retry the RTMP/RTMPS URL through FFmpeg.

## Audio Packet Handling

Legacy AAC is supported:

- Parse AAC AudioSpecificConfig from `AACPacketType == 0`.
- Accept AAC-LC only for decode.
- Wrap raw AAC frames as ADTS using the parsed config.
- Decode with `AudioToolboxAacDecoder`.
- Emit decoded 48 kHz stereo S16 samples on the same callback contract used by native SRT.

Unsupported audio formats:

- Log once per source with the codec/profile reason.
- Continue video-only only if video is valid and the operator-visible state makes this clear.
- Fail native ingest if the stream is configured or expected to require audio parity and no supported audio is present.

## Parser Robustness

The RTMP parser must tolerate live TCP behavior:

- one byte per read;
- headers split from payloads;
- payloads split across arbitrary read boundaries;
- multiple RTMP chunks in one read;
- interleaved chunk streams;
- chunk size changes;
- extended timestamps on fmt 0, 1, 2, and 3 continuation chunks;
- abort messages;
- ping/pong user-control events;
- window acknowledgement and peer bandwidth messages;
- malformed chunk headers and impossible payload lengths.

Parser state changes must be transactional. Incomplete input must leave previous headers, timestamp deltas, assemblies, and chunk sizes unchanged except for buffered bytes.

The parser should enforce bounded memory:

- maximum RTMP message payload size;
- maximum buffered unparsed bytes;
- maximum in-flight chunk assemblies;
- clear assemblies on abort messages.

## Timing And Synchronization

Video timestamps:

- RTMP message timestamps provide DTS in milliseconds unless E-RTMP ModEx supplies additional precision.
- Legacy AVC composition time and E-RTMP composition offsets produce PTS.
- Large forward jumps and backward jumps should re-anchor like native SRT.
- Timestamp wrap/extended timestamp behavior must be tested.

Audio timestamps:

- Audio PTS is anchored from RTMP message timestamp.
- Decoded output is 48 kHz; `DecodedAudioChunk.startSample` uses the recorder output sample rate.
- Discontinuities reset the AAC decoder and audio anchor.

Sync acceptance:

- Four coincident sources should stay within the existing SRT-style spread bound.
- Trim behavior should match SRT behavior.
- A 30-60 minute soak should show no monotonic drift beyond the configured tolerance.

## Reconnect And Failure Policy

Native RTMP must distinguish:

- transient network failure;
- server-requested reconnect;
- stream stall;
- unsupported stream profile;
- decoder capability failure;
- malformed stream.

Transient failures should allow reconnect with the existing backoff loop.

Server-requested reconnect should honor E-RTMP reconnect request semantics where practical:

- parse `onStatus` commands;
- recognize `NetConnection.Connect.ReconnectRequest`;
- use replacement `tcUrl` when provided;
- otherwise reconnect to the current URL.

Unsupported profile or decoder capability failures should not loop forever through the same native path. The source should fail explicitly on native RTMP and stop capture for that URL rather than retrying through `FfmpegIngestSession`.

The operator log must include source index, backend, URL scheme, codec/FourCC when known, and failure category.

## Backend Selection And Rollout

Native RTMP/RTMPS is the default ingest path on Apple platforms where the native backend is available.
The legacy FFmpeg RTMP/RTMPS path is no longer selected by environment override or by native-profile failure fallback.
Unsupported or malformed RTMP profiles fail clearly on the native path instead of being retried through FFmpeg.

Native SRT selection remains separate and should not be regressed.

## Testing

Unit tests:

- AMF0 object, ECMA array, strict array, nested object, number, boolean, string, null, and skip behavior.
- RTMP chunk parser arbitrary fragmentation, interleaving, chunk-size changes, extended timestamps including fmt 3 continuation, abort, malformed headers, and bounded-size rejection.
- Legacy AVC sequence header parsing and NAL conversion.
- E-RTMP video header parsing for `SequenceStart`, `CodedFrames`, `CodedFramesX`, `SequenceEnd`, metadata, and multitrack packets.
- HEVCDecoderConfigurationRecord parsing and HEVC NAL conversion.
- Signed URL app/playpath derivation.
- Backend selection, legacy override ignoring, and unsupported-profile stop policy.

Fixture E2E tests:

- AVC RTMP smoke.
- AVC RTMPS smoke.
- HEVC/E-RTMP RTMP smoke.
- HEVC/E-RTMP RTMPS smoke.
- Four-source routing with native RTMP.
- Four-source sync with native RTMP.
- Per-source trim with native RTMP.
- Connection-count behavior with live and dead RTMP sources.
- Reconnect/stall behavior.
- Negative unsupported codec case that proves no connected-black output.

Real-server tests:

- At least one real RTMP server interop run for AVC.
- At least one real E-RTMP-capable server or publisher interop run for HEVC.
- Signed URL/token interop where the server rejects missing query auth.
- RTMPS interop with a trusted certificate.

Soak tests:

- 30-minute AVC RTMP soak.
- 30-minute HEVC/E-RTMP soak.
- 60-minute combined confidence soak before flipping native default.
- Assertions: output grows, frames continue, audio continues when expected, no unbounded memory growth, no timestamp drift beyond tolerance, no decoder reset storm.

## Observability

Native RTMP logs should include concise, structured-ish messages for:

- backend selected;
- RTMP/RTMPS connected;
- negotiated legacy/E-RTMP profile;
- selected codec and FourCC;
- selected E-RTMP track ID;
- sequence-header changes;
- decoder reset;
- reconnect request;
- stall restart;
- unsupported codec/profile;
- no fallback to FFmpeg;
- parse error category.

Tests should assert key log markers so false-positive recordings do not pass.

## Rollout

Phase 1: Freeze default behavior. Keep native RTMP opt-in while hardening.

Phase 2: Harden the RTMP chunk parser and AMF0 support with adversarial unit tests.

Phase 3: Fix URL/auth preservation and legacy AVC/AAC robustness.

Phase 4: Add E-RTMP single-track video parsing and HEVC configuration parsing.

Phase 5: Wire HEVC/E-RTMP video into `VideoToolboxDecoder`.

Phase 6: Add reconnect/fallback policy and operator-visible diagnostics.

Phase 7: Expand fixture E2E coverage for AVC, HEVC, RTMP, RTMPS, reconnect, negative profiles, and sync/trim/routing.

Phase 8: Add real-server interop and soak gates.

Phase 9: Flip native RTMP default after all readiness gates pass, without an FFmpeg RTMP fallback override.

## Risks

The largest risk is mistaking fixture success for broadcast reliability. The fixture must become hostile enough to simulate real TCP fragmentation, long timestamps, reconnects, and malformed streams.

E-RTMP support can easily sprawl into full multitrack and every modern codec. This design deliberately supports AVC, HEVC, and AAC for the default/single-track ingest profile and fails clearly outside that profile.

HEVC availability is platform-dependent. On Apple platforms, `VideoToolboxDecoder` is the intended native decode boundary. Any VideoToolbox HEVC failure must trigger a clear operator-facing native failure.

Defaulting native RTMP too early could regress existing RTMP users. The default flip is intentionally late and gated by real-server and soak evidence.
