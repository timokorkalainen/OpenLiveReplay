// Tier-(a) sender: builds marker OutputBusFrames and submits them to a REAL NdiOutputSink at
// the nominal cadence for the run. The minimal output path (sink + transport only). Exits 77
// (SKIP) if the sink cannot start (no NDI runtime). usage: ndi_output_sender <source-name>
// <seconds>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>

#include "playback/output/mediaframe.h"
#include "playback/output/ndisink.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputtargetassignment.h"
#include "tests/e2e/ndi_output_marker.h"

namespace {
constexpr int kSkip = 77;

MediaVideoFrame markerVideo(const NdiOutputMarkerConfig& mk, qint64 frameIndex) {
    MediaVideoFrame v = MediaVideoFrame::solidYuv420p(mk.width, mk.height, 128, 128, 128);
    v.feedIndex = 0;
    v.ptsMs = frameIndex * 1000 * mk.fpsDen / mk.fpsNum;
    v.planeY = ndiMarkerLumaPlane(mk, frameIndex); // overwrite luma with the marker
    return v;
}

MediaAudioFrame markerAudio(const NdiOutputMarkerConfig& mk, qint64 frameIndex) {
    MediaAudioFrame a;
    a.feedIndex = 0;
    a.startSample = frameIndex * ndiMarkerSamplesPerFrame(mk);
    a.sampleRate = mk.sampleRate;
    a.channels = mk.channels;
    a.format = MediaSampleFormat::S16Interleaved;
    a.pcm = ndiMarkerAudioS16(mk, frameIndex);
    return a;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_output_sender <source-name> <seconds>\n");
        return 2;
    }
    const QString senderName = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();

    NdiOutputMarkerConfig mk;
    const FrameRate rate = FrameRate::fromFraction(mk.fpsNum, mk.fpsDen);

    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("ndi-output-sender");
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.kind = OutputTargetKind::Ndi;
    assignment.enabled = true;
    assignment.settings.insert(QStringLiteral("senderName"), senderName);

    NdiOutputSink sink;
    if (!sink.start(assignment, rate)) {
        fprintf(stderr, "[ndi_output_sender] NdiOutputSink start failed (no runtime?) - SKIP\n");
        return kSkip;
    }

    const double periodMs = 1000.0 * mk.fpsDen / mk.fpsNum;
    QElapsedTimer run;
    run.start();
    qint64 frameIndex = 0;
    while (run.elapsed() < qint64(seconds * 1000.0)) {
        OutputBusFrame frame;
        frame.bus = OutputBusId::feed(0);
        frame.outputFrameIndex = frameIndex;
        frame.video = markerVideo(mk, frameIndex);
        frame.audio = markerAudio(mk, frameIndex);
        frame.sampledPlayheadMs = frame.video.ptsMs;
        // Stamp the programme timecode the way OutputBusEngine does (playhead x 10000), so the
        // lane drives a real, advancing timecode onto the wire instead of the synthesize default.
        frame.programmeTimecode100ns = qMax<qint64>(0, frame.sampledPlayheadMs) * 10000;
        sink.submit(frame);
        ++frameIndex;
        const qint64 targetMs = qint64(frameIndex * periodMs);
        const qint64 sleepMs = targetMs - run.elapsed();
        if (sleepMs > 0) QThread::msleep(static_cast<unsigned long>(sleepMs));
    }
    sink.stop();
    fprintf(stderr, "[ndi_output_sender] sent %lld frames as '%s'\n", (long long) frameIndex,
            senderName.toUtf8().constData());
    return 0;
}
