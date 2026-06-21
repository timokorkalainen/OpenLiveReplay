// Headless playback harness: drives the REAL PlaybackWorker against a recorded
// MKV and exercises the scenarios the scheduler rework must satisfy. It is the
// empirical gate for the redesign — the 2-view 1x case must report 0
// repositions (the old decode loop stormed ~90 hard-seeks/s).
//
// The worker is unchanged from production: N FrameProviders, a PlaybackTransport
// (30fps), an AudioPlayer (started + UNMUTED so the audio release path actually
// runs and audioPushes increments), and a single active audio view (0). On a
// host with a default audio device the sink opens for real; with none it warns
// and no-ops, which is fine — the worker still enqueues/releases/clears and
// increments the counters we assert on.
//
// At the end the worker's counters() are printed as one parseable line:
//   COUNTERS reposition=.. reuseSeek=.. reverseChunkSeek=.. eofTailSeek=..
//            skipForward=.. audioPushes=.. framesDropped=..
// Per-second SEC telemetry is emitted by the instrumented worker to stderr when
// OLR_PB_TELEMETRY is set in the environment (passed through transparently).
//
// usage: play_harness <file.mkv> <scenario> [viewCount]
//   scenarios: play1x | seekplay | reverse | stepscrub | sliderscrub | liveedge | seekflash |
//              farback | armedcut | armedcut-back | armedcut-seekrace | armedcut-rearm-seek |
//              playlist | armedcut-h264 | armedcut-h264-back
#include <QCoreApplication>
#include <QTimer>
#include <QList>
#include <functional>
#include <cstdio>
#include <cstdlib>

#include "playback/frameprovider.h"
#include "playback/playbacktransport.h"
#include "playback/audioplayer.h"
#include "playback/playbackworker.h"
#include "playback/playlistplayout.h"
#include "playback/output/outputdispatcher.h"
#include "playback/output/outputtargetassignment.h"

namespace {
constexpr int kFrameDurMs = 33; // ~30fps step granularity

// Probe the fixture's duration so scenarios that seek "near the end" target a
// real position. Falls back to a conservative default if ffprobe is missing.
int64_t probeDurationMs(const QString& file) {
    // Best-effort: avoid a hard ffprobe dependency in the binary; the driver
    // script records a known-length fixture, so a fixed assumption is safe for
    // the scenarios here (they clamp internally). 25s default matches the
    // record length in run_playback_e2e.sh for liveedge.
    Q_UNUSED(file);
    return 25000;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: play_harness <file.mkv> <scenario> [viewCount]\n");
        return 2;
    }
    const QString file = QString::fromUtf8(argv[1]);
    const QString scen = QString::fromUtf8(argv[2]);
    const int views = (argc > 3) ? atoi(argv[3]) : 2;

    QList<FrameProvider*> providers;
    for (int i = 0; i < views; ++i)
        providers.append(new FrameProvider());

    PlaybackTransport transport;
    transport.setFps(30);

    AudioPlayer audio;
    audio.start(48000, 2);
    // Unmuted so the worker's audio release path runs and audioPushes ticks.
    // If no device is present the sink simply warns/no-ops; the worker still
    // enqueues/releases and counts.
    audio.setMuted(false);

    // Optional output-latency offset for the resync-coupling e2e (default 0).
    audio.setOutputLatencyOffsetMs(qEnvironmentVariableIntValue("OLR_AUDIO_LATENCY_MS"));

    PlaybackWorker worker(providers, &transport, &audio);
    worker.openFile(file);
    worker.setActiveAudioView(0); // route audio for view 0
    // Tier (b): enable a real NDI output when requested, so the worker's
    // decode->cache->output-bus->NdiOutputSink path is exercised end to end. The output bus is
    // selectable via OLR_NDI_OUTPUT_BUS (feed|pgm|multiview, default feed) so each broadcast
    // output bus is validated over real NDI, not just feed(0):
    //   feed      -> renderSingleSource(feed 0)            (full frame)
    //   pgm       -> renderPgm(selectedFeedIndex, def 0)   (selected feed, full frame)
    //   multiview -> renderMultiview()/composeGrid()       (grid; 1:1 for a single feed)
    // For a single-feed marker playback at the source's own dimensions, pgm and the 1-cell
    // multiview composite are both identity copies, so the full-frame marker stays decodable
    // and the receiver probe can apply the same continuity/sync gate to every bus.
    const QByteArray ndiSender = qgetenv("OLR_NDI_OUTPUT_SENDER");
    if (!ndiSender.isEmpty()) {
        const QByteArray busEnv = qgetenv("OLR_NDI_OUTPUT_BUS").trimmed().toLower();
        OutputBusId bus = OutputBusId::feed(0);
        if (busEnv == "pgm")
            bus = OutputBusId::pgm();
        else if (busEnv == "multiview")
            bus = OutputBusId::multiview();
        OutputTargetAssignment ndi;
        ndi.id = QStringLiteral("ndi-tier-b");
        ndi.sourceBus = bus;
        ndi.kind = OutputTargetKind::Ndi;
        ndi.enabled = true;
        ndi.settings.insert(QStringLiteral("senderName"), QString::fromUtf8(ndiSender));
        worker.setExternalOutputTargets({ndi});
        fprintf(stderr, "[play_harness] NDI output bus=%s\n",
                busEnv.isEmpty() ? "feed" : busEnv.constData());
    }
    worker.start();

    const int64_t durMs = probeDurationMs(file);

    // Post-seek placeholder baseline for the seekflash scenario. A cold start
    // legitimately emits placeholders during warmup (the OutputRuntime ticks
    // against an empty cache before the first frame decodes), so the seekflash
    // gate measures the DELTA across the test seek, not the whole-run total.
    // -1 = never snapshotted (every non-seekflash scenario reports delta 0).
    auto* basePh = new qint64(-1);
    // Same DELTA approach for HELD frames: farback asserts the worker double-buffer
    // (Task 2), not the dispatcher hold-last, keeps real frames visible — so
    // heldFrames must NOT spike across the jump. A masking regression (publishing
    // the half-built staging cache) would show held frames for the whole fill.
    // -1 = never snapshotted (other scenarios report delta 0).
    auto* baseHeld = new qint64(-1);
    // Max |landed playhead - expected in-point| over the playlist scenario's boundary
    // cuts: a direct, NON-self-referential frame-accuracy check that each boundary cut
    // landed AT the next entry's in-point (the divergence gate only proves the epoch
    // tracks itself). 0 for non-playlist scenarios.
    auto* maxLandErr = new qint64(0);
    // armNextCut return value for armedcut-h264: 1 = armed (pre-roll bank accepted
    // the cut), 0 = rejected (H.264 guard fired — pre-roll bank skipped H.264
    // streams, bank empty). -1 = not applicable / armNextCut not called.
    int armNextCutArmed = -1;

    // Print the final counters in a parseable form, then quit.
    auto finish = [&, basePh, baseHeld, maxLandErr]() {
        // Read the output stats BEFORE stop(): the worker's run() tears down the
        // OutputRuntime on exit (shutdownOutputGraph nulls m_outputRuntime), so a
        // post-stop outputStats() would return a zeroed struct (delta would go
        // negative). The other counters live on the worker/audio and survive stop.
        const OutputDispatchStats os = worker.outputStats();
        worker.stop();
        const PlaybackWorker::PlaybackCounters c = worker.counters();
        const qint64 phDelta = (*basePh < 0) ? 0 : (os.placeholderFrames - *basePh);
        const qint64 heldDelta = (*baseHeld < 0) ? 0 : (os.heldFrames - *baseHeld);
        printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
               "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
               "placeholderFramesDelta=%lld skippedDuplicateFrames=%lld cacheGeneration=%lld "
               "heldFramesDelta=%lld maxClockDivergenceMs=%lld cutsFired=%d cutFollowReposition=%d "
               "maxBoundaryLandingErrMs=%lld armNextCutArmed=%d decodedVideoFrames=%lld "
               "stagingVideoFramesDecoded=%lld\n",
               c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
               c.audioPushes, c.framesDropped, audio.resyncCount(), (long long) phDelta,
               (long long) os.skippedDuplicateFrames, (long long) worker.cacheGeneration(),
               (long long) heldDelta, (long long) os.maxClockDivergenceMs, worker.cutsFired(),
               c.cutFollowReposition, (long long) *maxLandErr, armNextCutArmed,
               (long long) c.decodedVideoFrames, (long long) c.stagingVideoFramesDecoded);
        fflush(stdout);
        app.quit();
    };

    // Scenario kickoff at t=1500ms: the worker's run() has an open/retry loop
    // (~500ms+) before the file is decodable, so give it a generous lead.
    QTimer::singleShot(1500, &app, [&]() {
        fprintf(stderr, "### SCENARIO %s views=%d dur=%lldms ###\n", scen.toUtf8().constData(),
                views, (long long) durMs);

        if (scen == "play1x") {
            // Steady 1x playback from the start. The headline storm metric:
            // a correct windowed scheduler does this with ZERO repositions.
            transport.seek(0);
            transport.setSpeed(1.0);
            transport.setPlaying(true);
            QTimer::singleShot(12000, &app, finish);

        } else if (scen == "seekplay") {
            // Seek to the middle, then play 1x. Tests post-seek storm.
            transport.setSpeed(1.0);
            transport.seek(8000);
            worker.seekTo(8000);
            transport.setPlaying(true);
            QTimer::singleShot(10000, &app, finish);

        } else if (scen == "reverse") {
            // Play 1x for 2s, then -5x rewind. Tests reverse thrash.
            transport.seek(8000);
            worker.seekTo(8000);
            transport.setSpeed(1.0);
            transport.setPlaying(true);
            QTimer::singleShot(2000, &app, [&]() {
                fprintf(stderr, "### switching to -5x ###\n");
                transport.setSpeed(-5.0);
            });
            QTimer::singleShot(6000, &app, finish);

        } else if (scen == "stepscrub") {
            // Paused frame-step backward 20x at 150ms intervals — like a user
            // tapping the back-step key. Tests burst-per-step repositioning.
            transport.seek(12000);
            worker.seekTo(12000);
            transport.setPlaying(false);
            int* n = new int(0);
            QTimer* t = new QTimer(&app);
            QObject::connect(t, &QTimer::timeout, &app, [&, n, t]() {
                if (*n >= 20) {
                    t->stop();
                    finish();
                    return;
                }
                const int64_t pos = transport.currentPos() - kFrameDurMs;
                transport.seek(pos);
                worker.seekTo(pos);
                (*n)++;
            });
            t->start(150);

        } else if (scen == "sliderscrub") {
            // The slider path post-§7: the UI calls BOTH transport.seek() and
            // worker.seekTo() for each scrub target. Drive a series of in- and
            // out-of-window positions, in both playing and paused states, then
            // assert via counters (in-window scrubs should reuse, not storm).
            struct Step {
                int64_t pos;
                bool play;
            };
            // In-window deltas (a few frames apart) interleaved with larger
            // out-of-window jumps; first paused, later playing.
            auto* steps = new QList<Step>{
                {6000, false},  {6066, false},  {6132, false},  {6099, false}, // in-window paused
                {12000, false}, {12066, false}, {12033, false},                // jump + in-window
                {3000, false},  {3066, false},                // backward jump + in-window
                {9000, true},   {9066, true},   {9132, true}, // in-window playing
                {15000, true},  {15066, true},                // jump + in-window playing
            };
            auto* i = new int(0);
            QTimer* t = new QTimer(&app);
            QObject::connect(t, &QTimer::timeout, &app, [&, steps, i, t]() {
                if (*i >= steps->size()) {
                    t->stop();
                    steps->clear();
                    finish();
                    return;
                }
                const Step s = steps->at(*i);
                transport.setPlaying(s.play);
                transport.setSpeed(1.0);
                transport.seek(s.pos);
                worker.seekTo(s.pos);
                (*i)++;
            });
            t->start(400); // give each scrub time to settle in-window

        } else if (scen == "liveedge") {
            // Seek near the end and play forward into EOF. Tests the live-EOF
            // tail-hold (no spin / no storm at the file edge).
            const int64_t near = qMax<int64_t>(0, durMs - 1000);
            transport.setSpeed(1.0);
            transport.seek(near);
            worker.seekTo(near);
            transport.setPlaying(true);
            QTimer::singleShot(6000, &app, finish);

        } else if (scen == "seekflash") {
            // Prove a seek introduces NO NEW gray placeholder. A cold start emits
            // placeholders during warmup (empty-cache ticks before the first
            // decode), so we measure the POST-SEEK delta, not the whole-run
            // total. Warm up: seek to 2000, play 1x ~1500ms so the cache holds
            // real frames AND a real frame has been delivered. Snapshot the
            // placeholder count, then seek FORWARD to 12000 and play ~3000ms.
            // With Task 1 (cache not cleared on reposition) the cache keeps the
            // warmup frames, so videoFrameAt(feed,12000) returns the largest
            // pts<=12000 (a stale-but-real warmup frame) until the 12000 frames
            // decode in — no NEW placeholder → delta 0.
            transport.setSpeed(1.0);
            transport.seek(2000);
            worker.seekTo(2000);
            transport.setPlaying(true);
            // After warmup: snapshot the placeholder baseline, then test-seek.
            QTimer::singleShot(1500, &app, [&, basePh]() {
                *basePh = worker.outputStats().placeholderFrames;
                fprintf(stderr, "### seekflash basePh=%lld; seeking 2000->12000 ###\n",
                        (long long) *basePh);
                transport.seek(12000);
                worker.seekTo(12000);
            });
            QTimer::singleShot(4500, &app, finish);

        } else if (scen == "farback") {
            // Far-BACKWARD seek: play near EOF, then jump all the way back to 0.
            // The live cache holds only end-of-file frames at the instant the
            // playhead snaps to 0, so this is the worst case for the seek flash.
            // The CommitGate (Task 1) + worker double-buffer (Task 2) must keep
            // placeholderFramesDelta==0 across the jump, and a committed cache
            // generation (>=1) must be recorded for the reposition. Mirror the
            // seekflash baseline-delta pattern: warm up so a real frame is
            // delivered + cached, snapshot the placeholder baseline BEFORE the
            // backward seek, then jump to 0 and play on.
            const int64_t near = qMax<int64_t>(0, durMs - 1000);
            transport.setSpeed(1.0);
            transport.seek(near);
            worker.seekTo(near);
            transport.setPlaying(true);
            // After warmup (~1.5s near EOF): snapshot baseline, then far-back seek.
            QTimer::singleShot(1500, &app, [&, basePh, baseHeld]() {
                const OutputDispatchStats b = worker.outputStats();
                *basePh = b.placeholderFrames;
                *baseHeld = b.heldFrames;
                fprintf(stderr,
                        "### farback basePh=%lld baseHeld=%lld; far-backward seek to 0 ###\n",
                        (long long) *basePh, (long long) *baseHeld);
                transport.seek(0);
                worker.seekTo(0);
            });
            QTimer::singleShot(4500, &app, finish);

        } else if (scen == "armedcut") {
            // Tier3 ARMED CUT: prove a recalled cut snaps to the target frame
            // with NO gray flash and NO reposition fallback. Play from the head
            // ~1s so the output cache holds real frames and a real frame has been
            // delivered, snapshot the placeholder baseline, then arm a cut to the
            // clip midpoint. The worker pre-rolls [target, target+span] into a
            // private staging cache on its SECOND AVFormatContext and atomically
            // promotes it at a scheduled output frame — the playhead re-bases to
            // the target without ever invoking repositionTo. Continue ~3s through
            // the cut. The gate: placeholderFramesDelta==0 (no flash) AND
            // reposition==0 (the cut did NOT fall back to the coarse seek path).
            transport.setSpeed(1.0);
            transport.seek(0);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh]() {
                *basePh = worker.outputStats().placeholderFrames;
                const int64_t target = durMs / 2;
                fprintf(stderr, "### armedcut basePh=%lld; arming cut to %lldms ###\n",
                        (long long) *basePh, (long long) target);
                worker.armNextCut(target);
            });
            // Rapid re-arm (simulates a double "Recall") to a DIFFERENT target
            // while the first cut to durMs/2 is still in flight: exercises the safe
            // re-arm QUEUE. The second arm must not reset the staging state mid-cut
            // (no concurrent staging-cache clear during the output swap); instead it
            // is queued and applied by the worker once the first cut clears, firing
            // a SECOND frame-accurate cut to the latest target. The gate asserts
            // cutsFired==2, proving the queued target fired rather than being
            // dropped. The target is FORWARD of the first (durMs*3/4 > durMs/2) so
            // the cut keeps reposition==0: a backward re-base would (correctly) trip
            // the run loop's backward-jump path to resync the PRIMARY decoder bank
            // (which the cut does not move) — a decoder resync, not a cut-fallback,
            // but indistinguishable at the reposition gate, so the test stays
            // forward to keep that gate a clean "the cut never fell back" invariant.
            QTimer::singleShot(1050, &app, [&]() {
                worker.armNextCut(durMs * 3 / 4);
                fprintf(stderr,
                        "### armedcut re-arm (queued double-recall to durMs*3/4) issued ###\n");
            });
            QTimer::singleShot(4000, &app, finish);

        } else if (scen == "armedcut-back") {
            // Tier3 ARMED CUT, BACKWARD target (the dominant EVS replay case). A
            // backward cut is frame-accurate and flash-free just like a forward
            // one, but it differs structurally: the cut swaps only the OUTPUT
            // cache + re-bases the playhead, leaving the PRIMARY decoder bank at
            // the old (forward) position, so the worker resyncs it via a reactive
            // backward-jump repositionTo on its next pass. That reposition is
            // NON-CLEARING (Tier 1/2), so it must not flash — but the risk it
            // introduces is a DRY CACHE: the promoted staging covers only
            // [target, target+kStagingSpanMs] (~800ms of wall-clock playout), and
            // if the resync lags past that the playhead outruns the coverage. That
            // does NOT show as gray (placeholderFramesDelta) — m_holdLastFrame
            // paints the last-good frame and bumps heldFrames instead. So this
            // scenario plays LONG past the staging span and the gate asserts
            // heldFramesDelta==0 (the metric the placeholder gate misses), not
            // reposition==0 (the benign resync legitimately repositions).
            transport.setSpeed(1.0);
            transport.seek(durMs * 3 / 4); // start AHEAD of the cut target
            worker.seekTo(durMs * 3 / 4);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh, baseHeld]() {
                const OutputDispatchStats b = worker.outputStats();
                *basePh = b.placeholderFrames;
                *baseHeld = b.heldFrames;
                const int64_t target = durMs / 4; // strictly EARLIER than the playhead
                fprintf(stderr,
                        "### armedcut-back basePh=%lld baseHeld=%lld; arming BACKWARD cut to "
                        "%lldms ###\n",
                        (long long) *basePh, (long long) *baseHeld, (long long) target);
                worker.armNextCut(target);
            });
            // Play ~8s after the cut so the playhead runs several seconds past
            // target+kStagingSpanMs — the only window where a dry cache (frozen
            // frame / heldFrames spike) could surface if the resync ever lagged.
            QTimer::singleShot(9000, &app, finish);

        } else if (scen == "armedcut-seekrace") {
            // Manual-seek-vs-in-flight-cut policy: arm a cut, then issue an explicit
            // operator seekTo to a DIFFERENT target before the cut fires. The seek
            // must WIN — the cut aborts (cutsFired==0), the seek services the jump
            // via repositionTo (reposition>=1), and no gray is painted. A regression
            // (cut fires despite the seek) would snap the output back to the cut
            // target the operator moved away from.
            transport.setSpeed(1.0);
            transport.seek(0);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh]() {
                *basePh = worker.outputStats().placeholderFrames;
                fprintf(stderr, "### armedcut-seekrace basePh=%lld; arming cut to %lldms ###\n",
                        (long long) *basePh, (long long) (durMs / 2));
                worker.armNextCut(durMs / 2);
            });
            // Manual seek ~80ms later — while the cut is still staging (it fires
            // ~kStagingSpan+kCutLead after arming), so the generation bump aborts it.
            QTimer::singleShot(1080, &app, [&]() {
                worker.seekTo(durMs / 5);
                fprintf(stderr,
                        "### armedcut-seekrace manual seekTo %lldms (must cancel the cut) ###\n",
                        (long long) (durMs / 5));
            });
            QTimer::singleShot(5000, &app, finish);

        } else if (scen == "armedcut-rearm-seek") {
            // COMBINED recall+seek: arm a cut, queue a re-arm (second Recall while
            // the first cut is in flight), THEN manual seek. The seek is the newest
            // explicit action — both the in-flight cut and the queued re-arm must be
            // cancelled (cutsFired==0). Exercises the queue-time generation gating
            // (the worker must drop a re-arm whose queue-time seek gen was superseded).
            transport.setSpeed(1.0);
            transport.seek(0);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh]() {
                *basePh = worker.outputStats().placeholderFrames;
                fprintf(stderr, "### armedcut-rearm-seek basePh=%lld; arming cut to %lldms ###\n",
                        (long long) *basePh, (long long) (durMs / 2));
                worker.armNextCut(durMs / 2);
            });
            QTimer::singleShot(1050, &app, [&]() {
                worker.armNextCut(durMs / 3); // queued re-arm (cut still in flight)
                fprintf(stderr, "### armedcut-rearm-seek queued re-arm to %lldms ###\n",
                        (long long) (durMs / 3));
            });
            QTimer::singleShot(1100, &app, [&]() {
                worker.seekTo(durMs / 5); // manual seek — must cancel cut AND re-arm
                fprintf(stderr, "### armedcut-rearm-seek manual seekTo %lldms (cancels both) ###\n",
                        (long long) (durMs / 5));
            });
            QTimer::singleShot(5000, &app, finish);

        } else if (scen == "playlist") {
            // EVS rundown AUTO-PLAYOUT: drive PlaylistPlayout over a few forward
            // segments (one slow-motion) on the recorded fixture, exactly as the
            // UIManager controller does — arm each entry's out -> next-in boundary as
            // a frame-perfect cut (fire-at-out-point), advance on each fire, and apply
            // the next entry's speed. Proves the worker's fire-at-playhead cut + the
            // playout state machine end to end against the real decode pipeline,
            // including a slow-motion segment (the boundary fire-frame honors speed).
            auto* playout = new PlaylistPlayout();
            auto* lastCuts = new int(0);
            auto mk = [](qint64 in, qint64 out, double spd) {
                ReplayEntry e;
                e.clipPath = QStringLiteral("clip");
                e.inMs = in;
                e.outMs = out;
                e.speed = spd;
                return e;
            };
            QVector<ReplayEntry> entries;
            entries << mk(2000, 5000, 1.0) << mk(12000, 14000, 0.5) << mk(18000, 20000, 1.0);
            const int nEntries = int(entries.size());
            playout->start(entries, 0); // PlaylistPlayout copies the entries
            const ReplayEntry first = entries.first();
            transport.setSpeed(first.speed);
            transport.seek(first.inMs);
            worker.seekTo(first.inMs);
            transport.setPlaying(true);
            *lastCuts = worker.cutsFired();
            // Snapshot placeholder/held baseline after warmup, before the first boundary.
            QTimer::singleShot(1200, &app, [&, basePh, baseHeld, nEntries]() {
                const OutputDispatchStats b = worker.outputStats();
                *basePh = b.placeholderFrames;
                *baseHeld = b.heldFrames;
                fprintf(stderr, "### playlist basePh=%lld baseHeld=%lld; %d entries ###\n",
                        (long long) *basePh, (long long) *baseHeld, nEntries);
            });
            QTimer* mon = new QTimer(&app);
            QObject::connect(mon, &QTimer::timeout, &app, [&, playout, lastCuts, maxLandErr]() {
                const int cuts = worker.cutsFired();
                if (cuts > *lastCuts) {
                    *lastCuts = cuts;
                    const auto cur = playout->onBoundaryFired();
                    if (cur.has_value()) {
                        transport.setSpeed(cur->speed); // apply the next entry's speed
                        // Frame-accuracy: the cut just re-based the playhead to this
                        // entry's in-point. The landed playhead (sampled up to one
                        // monitor interval after the fire) must be at/just past the
                        // in-point — a direct check the boundary landed where intended.
                        const qint64 landed = transport.currentPos();
                        const qint64 err = qAbs(landed - cur->inMs);
                        if (err > *maxLandErr) *maxLandErr = err;
                        fprintf(stderr,
                                "### playlist advanced to entry %d in=%lld landed=%lld err=%lld "
                                "speed=%.2f ###\n",
                                playout->currentIndex(), (long long) cur->inMs, (long long) landed,
                                (long long) err, cur->speed);
                    }
                }
                const auto b = playout->evaluate(transport.currentPos(), transport.speed(), 1500);
                if (b.valid) {
                    worker.armNextCut(b.targetMs, b.fireAtMs);
                    fprintf(stderr, "### playlist arm boundary fireAt=%lld -> in=%lld ###\n",
                            (long long) b.fireAtMs, (long long) b.targetMs);
                }
            });
            mon->start(16);
            QTimer::singleShot(14000, &app, finish);

        } else if (scen == "armedcut-h264") {
            // H.264 FRAME-PERFECT ARMED CUT: now that openPrerollContext() builds a
            // NativeVideoDecoder for H.264 streams, armNextCut returns TRUE and the
            // worker pre-rolls [target, target+span] into the staging cache, firing a
            // frame-accurate cut with NO gray flash and NO reposition fallback.
            // Mirrors the MPEG-2 "armedcut" scenario exactly (arm + queued re-arm →
            // cutsFired==2) on the H.264 fixture.
            // Gates:
            //   armNextCutArmed==1  -> pre-roll bank accepted the arm (HW decode OK)
            //   cutsFired==2        -> first cut + queued re-arm both fired
            //   placeholderFramesDelta==0 -> no gray flash across the cut
            //   heldFramesDelta<=20 -> the promoted staging cache was NOT empty/dry
            //                          (an empty cache would hold-last → held spike)
            //   reposition==0       -> cut fired; no coarse-seek fallback
            //   maxClockDivergenceMs<=1500 -> epoch re-anchored at cut
            //   stagingVideoFramesDecoded>=15 -> the pre-roll bank actually HW-decoded
            //                          the target window into staging (direct proof)
            //   decodedVideoFrames>=30 -> the PRIMARY H.264 HW decoder ran (corroborates
            //                          the fixture is genuinely H.264; primary bank only,
            //                          NOT the staging bank — staging is stagingVideoFramesDecoded)
            transport.setSpeed(1.0);
            transport.seek(0);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh, baseHeld]() {
                const OutputDispatchStats b = worker.outputStats();
                *basePh = b.placeholderFrames;
                *baseHeld = b.heldFrames;
                const int64_t target = durMs / 2;
                armNextCutArmed = worker.armNextCut(target) ? 1 : 0;
                fprintf(
                    stderr,
                    "### armedcut-h264 basePh=%lld baseHeld=%lld; armNextCut(%lldms) returned %d "
                    "(0=guard fired, 1=armed) ###\n",
                    (long long) *basePh, (long long) *baseHeld, (long long) target,
                    armNextCutArmed);
            });
            // Rapid re-arm (queued double-recall) while the first cut is in flight,
            // mirroring the MPEG-2 armedcut scenario (drives cutsFired==2).
            QTimer::singleShot(1050, &app, [&]() {
                worker.armNextCut(durMs * 3 / 4);
                fprintf(stderr,
                        "### armedcut-h264 re-arm (queued double-recall to durMs*3/4) ###\n");
            });
            QTimer::singleShot(4000, &app, finish);

        } else if (scen == "armedcut-h264-back") {
            // H.264 ARMED CUT, BACKWARD target — the H.264 analogue of armedcut-back
            // and the dominant EVS replay direction. This is the path the PR's new
            // native pre-roll uniquely exercises: the backward cut sets
            // m_decoderFollowMs, so the worker resyncs the PRIMARY H.264 bank via the
            // deterministic decoder-follow (cutFollowReposition==1) through
            // repositionTo — whose post-seek drain intentionally does NOT flush the
            // native decoder (per-AU statelessness; see playbackworker.cpp). It also
            // exercises fillStaging's post-seek nativeDecoder->reset() on a real
            // backward av_seek_frame. Mirrors armedcut-back exactly (seek ahead, arm
            // a strictly-earlier cut, play long past the staging span) but on the
            // H.264 fixture. The gate asserts heldFramesDelta<=20 (no frozen-frame
            // stall if the resync lags), placeholderFramesDelta==0, framesDropped==0,
            // maxClockDivergenceMs<=1500, cutFollowReposition==1, reposition<=2, plus
            // the staging non-vacuity floor stagingVideoFramesDecoded>=15.
            transport.setSpeed(1.0);
            transport.seek(durMs * 3 / 4); // start AHEAD of the cut target
            worker.seekTo(durMs * 3 / 4);
            transport.setPlaying(true);
            QTimer::singleShot(1000, &app, [&, basePh, baseHeld]() {
                const OutputDispatchStats b = worker.outputStats();
                *basePh = b.placeholderFrames;
                *baseHeld = b.heldFrames;
                const int64_t target = durMs / 4; // strictly EARLIER than the playhead
                armNextCutArmed = worker.armNextCut(target) ? 1 : 0;
                fprintf(stderr,
                        "### armedcut-h264-back basePh=%lld baseHeld=%lld; arming BACKWARD cut to "
                        "%lldms returned %d (0=guard fired, 1=armed) ###\n",
                        (long long) *basePh, (long long) *baseHeld, (long long) target,
                        armNextCutArmed);
            });
            // Play ~8s after the cut so the playhead runs several seconds past
            // target+kStagingSpanMs — the window where a dry cache (frozen frame /
            // heldFrames spike) would surface if the native resync ever lagged.
            QTimer::singleShot(9000, &app, finish);

        } else {
            fprintf(stderr, "play_harness: unknown scenario '%s'\n", scen.toUtf8().constData());
            // Still print counters (all zero) so the driver gets a line, then
            // exit non-zero via the absence of a recognized scenario.
            const OutputDispatchStats os = worker.outputStats(); // before stop() tears it down
            worker.stop();
            const PlaybackWorker::PlaybackCounters c = worker.counters();
            const qint64 phDelta = (*basePh < 0) ? 0 : (os.placeholderFrames - *basePh);
            const qint64 heldDelta = (*baseHeld < 0) ? 0 : (os.heldFrames - *baseHeld);
            printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
                   "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
                   "placeholderFramesDelta=%lld skippedDuplicateFrames=%lld cacheGeneration=%lld "
                   "heldFramesDelta=%lld maxClockDivergenceMs=%lld cutsFired=%d "
                   "cutFollowReposition=%d maxBoundaryLandingErrMs=%lld armNextCutArmed=%d "
                   "decodedVideoFrames=%lld stagingVideoFramesDecoded=%lld\n",
                   c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
                   c.audioPushes, c.framesDropped, audio.resyncCount(), (long long) phDelta,
                   (long long) os.skippedDuplicateFrames, (long long) worker.cacheGeneration(),
                   (long long) heldDelta, (long long) os.maxClockDivergenceMs, worker.cutsFired(),
                   c.cutFollowReposition, (long long) *maxLandErr, armNextCutArmed,
                   (long long) c.decodedVideoFrames, (long long) c.stagingVideoFramesDecoded);
            fflush(stdout);
            ::exit(2);
        }
    });

    // Hard safety net: never hang a CI job even if a scenario timer is dropped.
    QTimer::singleShot(40000, &app, [&]() {
        fprintf(stderr, "play_harness: safety timeout — forcing finish\n");
        finish();
    });

    const int rc = app.exec();

    // Release providers (worker is already stopped in finish()).
    for (auto* p : providers)
        delete p;
    return rc;
}
