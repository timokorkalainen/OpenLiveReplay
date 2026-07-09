# iOS GPU pipeline manual on-device validation

CI gates the iOS build compiling with the GPU pipeline enabled plus the shared Metal unit tests on
macOS. The following checks are MANUAL and must run on a physical iOS device with the GPU pipeline
enabled:

- [ ] Single-feed playback shows the correct picture with no gray flash or stall.
- [ ] Multi-feed multiview stays within the iOS GPU budget without VRAM OOM; sustained thermal load
      degrades to CPU instead of crashing the decode loop.
- [ ] Background the app, wait, then foreground it: playback resumes without stale-surface artifacts
      or crashes.
- [ ] Lock/unlock and incoming-call interruption cycles behave like background/foreground.
- [ ] `gpuReadToCpuCount` stays at one readback per unique rendered bus surface, observed through
      on-device telemetry.
