// Headless output-bus soak: drives a real OutputRuntime against a static synthetic
// OutputFrameCache through alternating play/pause segments and verifies, via a
// ContinuitySink, that the produced output stream stays frame- and audio-continuous and
// holds cadence over wall-clock time. Rung 1 of the output-stability ladder: it exercises
// the OutputRuntime/dispatcher/engine/memo/identity/rational-audio code, NOT real decode,
// real sinks, or an external consumer (that is the rung-5 NDI-receiver lane).
//
// Env: OLR_SOAK_SECONDS (default 120), OLR_SOAK_FPS_NUM/OLR_SOAK_FPS_DEN (default 30000/1001),
//      OLR_SOAK_FEEDS (default 2).
// Output (stdout), one line each:
//   SOAK bus=feed frames=.. indexGaps=.. audioSeams=.. placeholders=.. repeated=..
//   SOAK bus=multiview frames=.. indexGaps=.. audioSeams=.. placeholders=.. repeated=..
//   RUNTIME deadlineMisses=.. catchUpCapHits=.. maxLatenessNs=.. ticks=..
#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QThread>

#include <atomic>
#include <cstdio>

#include "playback/output/outputruntime.h"

namespace {

int envIntOr(const char* key, int def) {
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(key, &ok);
    return ok ? v : def;
}

// Records O(1) running continuity invariants per submitted frame. submit() runs on the
// OutputRuntime thread; the accessors are read only AFTER stopRuntime() has joined it.
class ContinuitySink final : public IOutputSink {
public:
    explicit ContinuitySink(OutputTargetKind kind) : m_kind(kind) {}

    OutputTargetKind kind() const override { return m_kind; }
    bool start(const OutputTargetAssignment&, FrameRate) override {
        m_active = true;
        return true;
    }
    void stop() override { m_active = false; }
    bool isActive() const override { return m_active; }

    bool submit(const OutputBusFrame& frame) override {
        if (m_hasLastIndex && frame.outputFrameIndex != m_lastIndex + 1) m_indexGaps++;
        m_lastIndex = frame.outputFrameIndex;
        m_hasLastIndex = true;

        // Audio tiling is only meaningful between consecutive playing (non-silent) frames.
        // A pause emits silent audio and resets the baseline; resume re-establishes it.
        if (frame.identity.audioSilent) {
            m_haveAudioBaseline = false;
        } else {
            const qint64 start = frame.audio.startSample;
            if (m_haveAudioBaseline && start != m_expectedNextStart) m_audioSeams++;
            m_expectedNextStart = start + frame.audio.sampleFrames();
            m_haveAudioBaseline = true;
        }

        if (frame.identity.videoPlaceholder) m_placeholders++;
        if (m_hasLastIdentity && m_lastIdentity.samePayloadAs(frame.identity)) m_repeated++;
        m_lastIdentity = frame.identity;
        m_hasLastIdentity = true;
        m_frames++;
        return true;
    }

    qint64 frames() const { return m_frames; }
    qint64 indexGaps() const { return m_indexGaps; }
    qint64 audioSeams() const { return m_audioSeams; }
    qint64 placeholders() const { return m_placeholders; }
    qint64 repeated() const { return m_repeated; }

private:
    OutputTargetKind m_kind;
    bool m_active = false;
    bool m_hasLastIndex = false;
    qint64 m_lastIndex = 0;
    qint64 m_indexGaps = 0;
    bool m_haveAudioBaseline = false;
    qint64 m_expectedNextStart = 0;
    qint64 m_audioSeams = 0;
    qint64 m_placeholders = 0;
    bool m_hasLastIdentity = false;
    OutputFrameIdentity m_lastIdentity;
    qint64 m_repeated = 0;
    qint64 m_frames = 0;
};

OutputFrameCache buildStaticCache(int feedCount, int width, int height, FrameRate rate,
                                  int seconds) {
    OutputFrameCache cache(feedCount, width, height);
    const qint64 frameCount = qint64(seconds + 1) * rate.numerator / rate.denominator + 4;
    for (int feed = 0; feed < feedCount; ++feed) {
        for (qint64 i = 0; i < frameCount; ++i) {
            MediaVideoFrame v = MediaVideoFrame::solidYuv420p(
                width, height, uchar(16 + ((i + feed) & 0x3F)), 128, 128);
            v.feedIndex = feed;
            v.ptsMs = rate.frameIndexToMs(i);
            cache.insertVideoFrame(v);
        }
    }
    // Contiguous, NON-zero S16 stereo audio so playing frames are non-silent (tile-checked).
    const int sampleRate = 48000;
    const int chunk = sampleRate / 10; // 100ms frames
    const qint64 totalSamples = qint64(seconds + 1) * sampleRate;
    for (int feed = 0; feed < feedCount; ++feed) {
        for (qint64 s = 0; s < totalSamples; s += chunk) {
            MediaAudioFrame a;
            a.feedIndex = feed;
            a.startSample = s;
            a.sampleRate = sampleRate;
            a.channels = 2;
            a.format = MediaSampleFormat::S16Interleaved;
            QByteArray pcm(chunk * 2 * int(sizeof(qint16)), 0);
            auto* p = reinterpret_cast<qint16*>(pcm.data());
            for (int k = 0; k < chunk * 2; ++k)
                p[k] = qint16(1000 + ((s + k) & 0x3FFF));
            a.pcm = pcm;
            cache.insertAudioFrame(a);
        }
    }
    return cache;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const int seconds = envIntOr("OLR_SOAK_SECONDS", 120);
    const int fpsNum = envIntOr("OLR_SOAK_FPS_NUM", 30000);
    const int fpsDen = envIntOr("OLR_SOAK_FPS_DEN", 1001);
    const int feeds = qMax(1, envIntOr("OLR_SOAK_FEEDS", 2));
    const int width = 64;
    const int height = 64;
    const FrameRate rate = FrameRate::fromFraction(fpsNum, fpsDen);

    OutputFrameCache cache = buildStaticCache(feeds, width, height, rate, seconds);

    std::atomic<int> playing{1};
    constexpr qint64 pauseHoldMs = 1000;

    OutputRuntime runtime(rate, feeds, width, height);
    runtime.setSnapshotProvider([&cache, &playing]() {
        OutputRuntimeSnapshot snap;
        snap.cache = cache; // shallow copy-on-write share, mirrors makeOutputSnapshot
        snap.state.playing = playing.load(std::memory_order_relaxed) != 0;
        snap.state.speed = 1.0;
        snap.state.selectedFeedIndex = 0;
        snap.state.playheadMs = snap.state.playing ? 0 : pauseHoldMs;
        return snap;
    });

    ContinuitySink feedSink(OutputTargetKind::QtPreview);
    ContinuitySink multiviewSink(OutputTargetKind::QtPreview);

    OutputTargetAssignment feedAssignment;
    feedAssignment.id = QStringLiteral("soak-feed");
    feedAssignment.sourceBus = OutputBusId::feed(0);
    feedAssignment.kind = OutputTargetKind::QtPreview;
    feedAssignment.enabled = true;

    OutputTargetAssignment multiviewAssignment;
    multiviewAssignment.id = QStringLiteral("soak-multiview");
    multiviewAssignment.sourceBus = OutputBusId::multiview();
    multiviewAssignment.kind = OutputTargetKind::QtPreview;
    multiviewAssignment.enabled = true;

    runtime.setEndpoints({{feedAssignment, &feedSink}, {multiviewAssignment, &multiviewSink}});

    runtime.startRuntime();

    // Alternate play (even segments) and pause (odd segments) across the duration.
    const int segments = 5;
    const int segmentMs = qMax(200, (seconds * 1000) / segments);
    for (int seg = 0; seg < segments; ++seg) {
        playing.store((seg % 2 == 0) ? 1 : 0, std::memory_order_relaxed);
        QThread::msleep(segmentMs);
    }

    runtime.stopRuntime(); // joins the runtime thread; sink accessors are safe afterward

    const OutputDispatchStats stats = runtime.stats();
    auto report = [](const char* bus, const ContinuitySink& s) {
        printf("SOAK bus=%s frames=%lld indexGaps=%lld audioSeams=%lld placeholders=%lld "
               "repeated=%lld\n",
               bus, (long long) s.frames(), (long long) s.indexGaps(), (long long) s.audioSeams(),
               (long long) s.placeholders(), (long long) s.repeated());
    };
    report("feed", feedSink);
    report("multiview", multiviewSink);
    printf("RUNTIME deadlineMisses=%lld catchUpCapHits=%lld maxLatenessNs=%lld ticks=%lld\n",
           (long long) stats.runtime.deadlineMisses, (long long) stats.runtime.catchUpCapHits,
           (long long) stats.runtime.maxLatenessNs, (long long) stats.ticks);
    fflush(stdout);
    return 0;
}
