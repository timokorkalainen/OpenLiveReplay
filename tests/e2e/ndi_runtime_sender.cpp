#include "playback/output/ndisink.h"
#include "playback/output/outputdispatcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>

namespace {

constexpr double kPi = 3.14159265358979323846;
std::atomic<bool> g_stop{false};

void handleSignal(int) {
    g_stop = true;
}

QString argValue(const QStringList& args, const QString& name, const QString& fallback) {
    const int index = args.indexOf(name);
    if (index >= 0 && index + 1 < args.size()) return args.at(index + 1);
    return fallback;
}

qint64 audioBoundarySample(qint64 frameIndex, FrameRate rate) {
    if (!rate.isValid() || frameIndex <= 0) return 0;
    return (frameIndex * qint64(48000) * rate.denominator) / rate.numerator;
}

MediaAudioFrame makeAudio(qint64 frameIndex, FrameRate rate) {
    const qint64 startSample = audioBoundarySample(frameIndex, rate);
    const qint64 endSample = audioBoundarySample(frameIndex + 1, rate);
    const int sampleFrames = int(qMax<qint64>(0, endSample - startSample));

    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = startSample;
    audio.sampleRate = 48000;
    audio.channels = 2;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm.resize(sampleFrames * audio.channels * int(sizeof(qint16)));

    auto* out = reinterpret_cast<qint16*>(audio.pcm.data());
    for (int sample = 0; sample < sampleFrames; ++sample) {
        const double phase = double(startSample + sample) * 2.0 * kPi * 1000.0 / 48000.0;
        const auto value = qint16(std::lround(std::sin(phase) * 12000.0));
        out[sample * 2] = value;
        out[sample * 2 + 1] = qint16(-value);
    }
    return audio;
}

void insertFrame(OutputFrameCache* cache, qint64 frameIndex, FrameRate rate, int width,
                 int height) {
    MediaVideoFrame video =
        MediaVideoFrame::solidYuv420p(width, height, uchar(64 + (frameIndex % 128)), 96, 160);
    video.feedIndex = 0;
    video.ptsMs = rate.frameIndexToMs(frameIndex);
    video.outputFrameIndex = frameIndex;
    cache->insertVideoFrame(video);
    cache->insertAudioFrame(makeAudio(frameIndex, rate));
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QString senderName =
        argValue(args, QStringLiteral("--name"), QStringLiteral("OLR NDI Runtime Sender"));
    const int seconds = argValue(args, QStringLiteral("--seconds"), QStringLiteral("12")).toInt();
    const int width = argValue(args, QStringLiteral("--width"), QStringLiteral("640")).toInt();
    const int height = argValue(args, QStringLiteral("--height"), QStringLiteral("480")).toInt();
    const int fps = argValue(args, QStringLiteral("--fps"), QStringLiteral("30")).toInt();

    if (senderName.trimmed().isEmpty() || seconds <= 0 || width <= 0 || height <= 0 || fps <= 0) {
        fprintf(stderr, "ndi_runtime_sender: invalid arguments\n");
        return 2;
    }

    const FrameRate rate = FrameRate::fromFraction(fps, 1);
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("ndi-e2e-runtime-sender");
    assignment.kind = OutputTargetKind::Ndi;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), senderName);

    NdiOutputSink sink;
    OutputDispatcher dispatcher(rate, 1, width, height);
    dispatcher.setEndpoints({{assignment, &sink}});
    if (!sink.isActive()) {
        const NdiOutputStatus status = sink.status();
        fprintf(stderr, "ndi_runtime_sender: failed to start sender '%s': %s\n",
                qPrintable(senderName), qPrintable(status.message));
        return status.state == NdiOutputState::RuntimeUnavailable ? 77 : 1;
    }

    fprintf(stderr, "[ndi-runtime-sender] source=%s seconds=%d\n", qPrintable(senderName), seconds);

    OutputFrameCache cache(1, width, height);
    PlaybackStateSnapshot state;
    state.playing = true;
    state.speed = 1.0;
    state.selectedFeedIndex = 0;

    QElapsedTimer timer;
    timer.start();
    qint64 frameIndex = 0;
    const qint64 frameDurationMs = qMax<qint64>(1, qint64(1000) / fps);
    const qint64 deadlineMs = qint64(seconds) * 1000;

    while (!g_stop && timer.elapsed() < deadlineMs) {
        const qint64 dueMs = frameIndex * qint64(1000) / fps;
        if (timer.elapsed() < dueMs) {
            QThread::msleep(qulonglong(qMin<qint64>(10, dueMs - timer.elapsed())));
            continue;
        }

        insertFrame(&cache, frameIndex, rate, width, height);
        state.playheadMs = rate.frameIndexToMs(frameIndex);
        dispatcher.dispatchTick(cache, state);

        const NdiOutputStatus status = sink.status();
        if (status.state != NdiOutputState::Active) {
            fprintf(stderr, "ndi_runtime_sender: sender failed at frame %lld: %s\n",
                    static_cast<long long>(frameIndex), qPrintable(status.message));
            return 1;
        }
        ++frameIndex;
        QCoreApplication::processEvents();

        if (frameDurationMs > 1) QThread::msleep(1);
    }

    return 0;
}
