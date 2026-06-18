# SMPTE Standards Relevant to OpenLiveReplay

Last reviewed: 2026-06-18

This note captures which SMPTE standards matter for OpenLiveReplay as a
slow-motion / replay app aimed at professional live TV workflows. It is not a
complete SMPTE catalog. The useful lens is: which standards affect timecode,
sync, media transport, clean outputs, metadata, and interchange with broadcast
facilities?

Short answer: there is no single "SMPTE slow-motion replay app" standard. The
relevant standards are integration contracts around live production timing,
signal transport, ancillary data, and file interchange.

## Highest Priority

### SMPTE ST 12 family - Time and Control Code

Relevant documents:

- [ST 12-1, Time and Control Code](https://pub.smpte.org/doc/st12-1/)
- [ST 12-2, Transmission of Time Code in the Ancillary Data Space](https://pub.smpte.org/doc/st12-2/)
- [ST 12-3, Time Code for High Frame Rate Signals and Formatting in the Ancillary Data Space](https://pub.smpte.org/doc/st12-3/)

Why it matters:

- Timecode is the main practical enabler for frame-accurate inter-camera replay
  over non-genlocked contribution links.
- Jam-synced cameras can be aligned by matching equal timecode frame labels.
- Drop-frame behavior for 29.97 and 59.94 must be correct in UI, control APIs,
  recording metadata, and export.

Current repo alignment:

- `recorder_engine/timing/timecode.{h,cpp}` already provides pure SMPTE-style
  drop-frame / non-drop-frame conversion for frame counts.
- The framesync roadmap calls out Phase 3 for extracting SMPTE 12M from SRT,
  RTMP, and NDI, aligning sources, and writing `tmcd` / tags.

Recommended next work:

- Wire the existing timecode conversion into UI and WebSocket control output.
- Extract embedded timecode from H.264 / HEVC SEI and MPEG-TS / RTMP metadata
  where available.
- Persist start timecode and per-track timecode metadata in recordings.
- Surface "timecode locked / missing / discontinuity" per source.

### SMPTE ST 2059 family - PTP / Facility Timing

Relevant documents:

- [ST 2059-1, Generation and Alignment of Interface Signals to the SMPTE Epoch](https://pub.smpte.org/doc/st2059-1/)
- [ST 2059-2, SMPTE Profile for IEEE 1588 PTP in Broadcast Applications](https://pub.smpte.org/doc/st2059-2/)
- [EG 2059-10, Introduction to the New Synchronization System](https://pub.smpte.org/doc/eg2059-10/)

Why it matters:

- This is the bridge from "best effort synchronized IP ingest" to true
  broadcast facility timing.
- ST 2110 facilities depend on PTP-derived timing.
- A replay output that claims genlock readiness needs an explicit external
  timing reference tier, lock state, and fallback behavior.

Current repo alignment:

- The framesync design already proposes a `TimingReference` abstraction and
  future `PtpReference`.
- The current honest ceiling is correctly stated: non-genlocked SRT / RTMP /
  UDP ingest cannot become true genlock without a shared reference.

Recommended next work:

- Keep PTP as an architectural seam until hardware output/input requires it.
- Add lock-state telemetry before promising frame-accurate facility sync.
- Treat PTP as opt-in and observable; never silently upgrade timing behavior.

### SMPTE ST 2110 family - Professional Media Over Managed IP Networks

Relevant documents:

- [OV 2110-0, Roadmap for the 2110 Document Suite](https://pub.smpte.org/doc/ov2110-0/)
- [ST 2110-10, System Timing and Definitions](https://pub.smpte.org/doc/st2110-10/)
- [ST 2110-20, Uncompressed Active Video](https://pub.smpte.org/doc/st2110-20/)
- [ST 2110-21, Traffic Shaping and Delivery Timing for Video](https://pub.smpte.org/doc/st2110-21/)
- [ST 2110-22, Constant Bit-Rate Compressed Video](https://pub.smpte.org/doc/st2110-22/)
- [ST 2110-30, PCM Digital Audio](https://pub.smpte.org/doc/st2110-30/)
- [ST 2110-31, AES3 Transparent Transport](https://pub.smpte.org/doc/st2110-31/)
- [ST 2110-40, SMPTE ST 291-1 Ancillary Data](https://pub.smpte.org/doc/st2110-40/)
- [ST 2110-41, Fast Metadata Framework](https://pub.smpte.org/doc/st2110-41/)

Why it matters:

- ST 2110 is the professional IP interface target for facilities that have
  moved beyond baseband SDI.
- It separates video, audio, and ancillary data into separately timed RTP flows.
- Output support requires deterministic output ticks, RTP timestamps, packet
  pacing, PTP alignment, and audio/video/ANC flow coordination.

Current repo alignment:

- The broadcast output bus design already separates logical clean outputs from
  output target assignments such as NDI, DeckLink SDI / HDMI, and DeckLink IP /
  ST 2110.
- This separation is exactly the right precondition: ST 2110 output should be an
  adapter consuming backend-produced clean bus frames, not a UI scrape.

Recommended next work:

- Finish deterministic clean output bus behavior before attempting ST 2110.
- Ensure output frames carry rational frame rate, colorimetry, range,
  progressive / interlaced state, audio sample timing, and identity metadata.
- Add ST 2110 only after PTP lock state and output scheduling are explicit.
- Expect AMWA NMOS IS-04 / IS-05 / IS-07 to be needed later for discovery,
  connection management, and event/tally integration, even though those are not
  SMPTE standards.

## Very Relevant Once SDI / Hardware I/O Starts

### SDI families and roadmaps

Relevant documents:

- [EG 2111-1, SD-SDI and HD-SDI Standards Roadmap](https://pub.smpte.org/doc/eg2111-1/)
- [EG 2111-2, 3/6/12 & 24 Gbit/s SDI Standards Roadmap](https://pub.smpte.org/doc/eg2111-2/)
- [EG 2111-3, 10G-SDI Standards Roadmap](https://pub.smpte.org/doc/eg2111-3/)
- [ST 292-1, 1.5 Gb/s Signal/Data Serial Interface](https://pub.smpte.org/doc/st292-1/)
- [ST 424, 3 Gb/s Signal/Data Serial Interface](https://pub.smpte.org/doc/st424/)
- [ST 2081-1, 6G-SDI Electrical](https://pub.smpte.org/doc/st2081-1/)
- [ST 2082-1, 12G-SDI Electrical](https://pub.smpte.org/doc/st2082-1/)
- [ST 352, Payload Identification Codes for Serial Digital Interfaces](https://pub.smpte.org/doc/st352/)
- [ST 299-1 / ST 299-2, Embedded Digital Audio for HD / 3G-SDI](https://pub.smpte.org/doc/st299-1/)

Why it matters:

- DeckLink / AJA SDI output will need correct mode selection, payload IDs,
  embedded audio, timecode/ANC preservation, and field/progressive handling.
- Live TV switchers expect continuous clean video and valid audio, not UI-timed
  preview frames.

Current repo alignment:

- The clean output bus spec already says Qt must not decide broadcast timing,
  composition, freeze behavior, or audio routing.
- That is the right model for SDI: the backend owns the output clock, while the
  hardware adapter schedules frames.

Recommended next work:

- Treat SDI output as a sink adapter over the existing clean output bus.
- Keep the bus format rich enough for 10-bit 4:2:2, colorimetry, interlace, and
  audio channel layout.
- Add test modes / color bars early for hardware validation.

### SMPTE ST 291 / ST 2038 / ST 2110-40 - Ancillary Data

Relevant documents:

- [ST 291-1, Ancillary Data Packet and Space Formatting](https://pub.smpte.org/doc/st291-1/)
- [RP 291, Assigned Ancillary Identification Codes](https://pub.smpte.org/doc/rp291/)
- [ST 2038, Carriage of Ancillary Data Packets in MPEG-2 Transport Stream](https://pub.smpte.org/doc/st2038/)
- [ST 2110-40, SMPTE ST 291-1 Ancillary Data](https://pub.smpte.org/doc/st2110-40/)

Why it matters:

- Timecode, captions, AFD, HDR metadata, and other operational metadata often
  travel as ANC in SDI or as mapped ANC in TS / ST 2110.
- A professional replay app should preserve, surface, or intentionally drop ANC
  with clear behavior.

Current repo alignment:

- Native SRT already parses MPEG-TS enough to recover PAT / PMT / PES and PCR.
- Phase 3 timecode extraction should be designed so other ANC metadata can be
  carried later without another timing rewrite.

Recommended next work:

- Start with timecode extraction and validation.
- Preserve the abstraction boundary: extract ANC-ish metadata into typed engine
  side data, then let SDI / ST 2110 / file adapters decide how to map it.

## File Interchange / Archive Relevance

### MXF family

Relevant documents:

- [ST 377-1, MXF File Format Specification](https://pub.smpte.org/doc/st377-1/)
- [ST 378, MXF OP1a](https://pub.smpte.org/doc/st378/)
- [ST 379-1, MXF Generic Container](https://pub.smpte.org/doc/st379-1/)
- [ST 436-1, MXF Mappings for VBI Lines and Ancillary Data Packets](https://pub.smpte.org/doc/st436-1/)
- [EG 41 / EG 377-3, MXF Engineering Guidelines](https://pub.smpte.org/doc/eg377-3/)

Why it matters:

- OpenLiveReplay currently records MKV, which is pragmatic for development.
- Broadcast interchange, archive, and some edit workflows may expect MXF OP1a,
  AVC-Intra, XAVC, DNxHD / DNxHR, ProRes mappings, or preserved ANC.

Current repo alignment:

- FFmpeg remains linked for the Matroska muxer and encoding helpers.
- The current replay app does not need MXF to be useful, but MXF becomes
  relevant when exchanging media with broadcast MAM / NLE / archive systems.

Recommended next work:

- Do not switch core recording format just for standards alignment.
- Add export/transcode/import pathways later if a real production workflow asks
  for MXF.
- Keep internal timing metadata precise enough that an MXF exporter can be
  correct later.

## Useful But Secondary

### ST 2064 - Audio-to-Video Synchronization Measurement

Relevant documents:

- [ST 2064-1, Fingerprint Generation](https://pub.smpte.org/doc/st2064-1/)
- [ST 2064-2, Fingerprint Transport](https://pub.smpte.org/doc/st2064-2/)

Why it matters:

- These are more about measurement/interoperability than the core replay engine.
- The app already has a practical flash/beep framesync rig; ST 2064 may become
  relevant if interoperating with external A/V sync measurement systems.

Recommended next work:

- Keep the existing measurement rig as the operational acceptance instrument.
- Revisit ST 2064 only if external test equipment or facility requirements ask
  for it.

### ST 2041 / ST 337 / ST 338 / ST 2035 - Audio carriage and channel metadata

Why it matters:

- Embedded audio, non-PCM audio, AES3 transparency, and channel assignment rules
  matter for SDI / ST 2110 / MXF integration.
- The app currently normalizes to 48 kHz stereo PCM in the engine, which is a
  good MVP constraint but not a full broadcast audio model.

Recommended next work:

- Keep 48 kHz PCM as the internal baseline.
- Add channel layout metadata before multi-channel program audio becomes a goal.

## Not Immediate

These are real SMPTE standards, but they are not near-term blockers for this app:

- IMF / DCP families: relevant to distribution and cinema packaging, not live
  replay operations.
- HDR dynamic metadata families: relevant only when HDR/WCG capture and output
  become product goals.
- Film/tape-era documents: useful historically, not relevant to the current
  native IP ingest and clean output architecture.
- Device control documents such as ST 2071: interesting, but the current local
  WebSocket / StreamDeck control path is the right lightweight starting point.

## Suggested Implementation Order

1. Finish exact rational frame-rate behavior in recorder paths.
2. Wire ST 12-style timecode conversion into UI, WebSocket state, recordings,
   and tests.
3. Implement Phase 3 timecode extraction / alignment / `tmcd` write.
4. Keep strengthening the clean output bus and output health telemetry.
5. Add DeckLink SDI / HDMI output as the first professional hardware adapter.
6. Add PTP reference lock state before claiming genlocked output.
7. Add ST 2110 output only after the output bus and PTP seam are stable.
8. Consider MXF export/import only when a real production workflow requires it.

## References

- [SMPTE standards document library](https://pub.smpte.org/doc/)
- Local context:
  - `docs/native-ingest-workstream-remaining.md`
  - `docs/superpowers/specs/2026-06-17-broadcast-framesync-program-design.md`
  - `docs/superpowers/specs/2026-06-17-broadcast-output-bus-design.md`
  - `recorder_engine/timing/timecode.h`
  - `recorder_engine/ingest/mpegtsparser.h`
