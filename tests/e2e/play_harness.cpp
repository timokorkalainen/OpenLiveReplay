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
//   COUNTERS reposition=.. reuseSeek=.. reverseChunkSeek=.. eofTailSeek=.. \
//            skipForward=.. audioPushes=.. framesDropped=..
// Per-second SEC telemetry is emitted by the instrumented worker to stderr when
// OLR_PB_TELEMETRY is set in the environment (passed through transparently).
//
// usage: play_harness <file.mkv> <scenario> [viewCount]
//   scenarios: play1x | seekplay | reverse | stepscrub | sliderscrub | liveedge | seekflash |
//              farback
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
#include "playback/output/outputdispatcher.h"

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
    worker.start();

    const int64_t durMs = probeDurationMs(file);

    // Post-seek placeholder baseline for the seekflash scenario. A cold start
    // legitimately emits placeholders during warmup (the OutputRuntime ticks
    // against an empty cache before the first frame decodes), so the seekflash
    // gate measures the DELTA across the test seek, not the whole-run total.
    // -1 = never snapshotted (every non-seekflash scenario reports delta 0).
    auto* basePh = new qint64(-1);

    // Print the final counters in a parseable form, then quit.
    auto finish = [&, basePh]() {
        // Read the output stats BEFORE stop(): the worker's run() tears down the
        // OutputRuntime on exit (shutdownOutputGraph nulls m_outputRuntime), so a
        // post-stop outputStats() would return a zeroed struct (delta would go
        // negative). The other counters live on the worker/audio and survive stop.
        const OutputDispatchStats os = worker.outputStats();
        worker.stop();
        const PlaybackWorker::PlaybackCounters c = worker.counters();
        const qint64 phDelta = (*basePh < 0) ? 0 : (os.placeholderFrames - *basePh);
        printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
               "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
               "placeholderFramesDelta=%lld skippedDuplicateFrames=%lld cacheGeneration=%lld\n",
               c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
               c.audioPushes, c.framesDropped, audio.resyncCount(), (long long) phDelta,
               (long long) os.skippedDuplicateFrames, (long long) worker.cacheGeneration());
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
            QTimer::singleShot(1500, &app, [&, basePh]() {
                *basePh = worker.outputStats().placeholderFrames;
                fprintf(stderr, "### farback basePh=%lld; far-backward seek to 0 ###\n",
                        (long long) *basePh);
                transport.seek(0);
                worker.seekTo(0);
            });
            QTimer::singleShot(4500, &app, finish);

        } else {
            fprintf(stderr, "play_harness: unknown scenario '%s'\n", scen.toUtf8().constData());
            // Still print counters (all zero) so the driver gets a line, then
            // exit non-zero via the absence of a recognized scenario.
            const OutputDispatchStats os = worker.outputStats(); // before stop() tears it down
            worker.stop();
            const PlaybackWorker::PlaybackCounters c = worker.counters();
            const qint64 phDelta = (*basePh < 0) ? 0 : (os.placeholderFrames - *basePh);
            printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
                   "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d "
                   "placeholderFramesDelta=%lld skippedDuplicateFrames=%lld cacheGeneration=%lld\n",
                   c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
                   c.audioPushes, c.framesDropped, audio.resyncCount(), (long long) phDelta,
                   (long long) os.skippedDuplicateFrames, (long long) worker.cacheGeneration());
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
