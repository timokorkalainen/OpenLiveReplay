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
//   scenarios: play1x | seekplay | reverse | stepscrub | sliderscrub | liveedge
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

    // Print the final counters in a parseable form, then quit.
    auto finish = [&]() {
        worker.stop();
        const PlaybackWorker::PlaybackCounters c = worker.counters();
        printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
               "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d\n",
               c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
               c.audioPushes, c.framesDropped, audio.resyncCount());
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

        } else {
            fprintf(stderr, "play_harness: unknown scenario '%s'\n", scen.toUtf8().constData());
            // Still print counters (all zero) so the driver gets a line, then
            // exit non-zero via the absence of a recognized scenario.
            worker.stop();
            const PlaybackWorker::PlaybackCounters c = worker.counters();
            printf("COUNTERS reposition=%d reuseSeek=%d reverseChunkSeek=%d "
                   "eofTailSeek=%d skipForward=%d audioPushes=%d framesDropped=%d resyncCount=%d\n",
                   c.reposition, c.reuseSeek, c.reverseChunkSeek, c.eofTailSeek, c.skipForward,
                   c.audioPushes, c.framesDropped, audio.resyncCount());
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
