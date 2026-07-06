#include <QCoreApplication>
#include <QDebug>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include "playback/gpu/gpusurface.h"
#include "playback/gpu/gpufence.h"
#include "playback/output/asyncgpureadbacksink.h"
#include "playback/output/framehandle.h"
#include "playback/output/iotargets/ajasink.h"
#include "playback/output/iotargets/decklinksink.h"
#include "playback/output/iotargets/omtsink.h"
#include "playback/output/outputbusengine.h"
#include "tests/e2e/ndi_output_marker.h"

#include <memory>
#include <optional>
#include <utility>

namespace {

struct TargetConfig {
    QString cliKind;
    OutputTargetKind outputKind = OutputTargetKind::QtPreview;
    bool wrapReadback = true;
    bool deckLinkGpuInput = false;
    QString cadence;
};

class RecordingFrameStore {
public:
    bool append(const OutputBusFrame& frame) {
        frames.push_back(frame);
        return true;
    }

    QVector<OutputBusFrame> frames;
};

class MarkerGpuSurface final : public GpuSurface {
public:
    MarkerGpuSurface(int width, int height, FramePixelFormat format) {
        m_desc.format = format;
        m_desc.width = width;
        m_desc.height = height;
    }

    GpuSurfaceDesc desc() const override { return m_desc; }
    bool isValid() const override { return m_desc.width > 0 && m_desc.height > 0; }
    void* nativeHandle() const override { return const_cast<MarkerGpuSurface*>(this); }
    void retainUntilFenceRetired(uint64_t fenceValue) override { m_pendingFence = fenceValue; }
    uint64_t pendingFenceValue() const override { return m_pendingFence; }

private:
    GpuSurfaceDesc m_desc;
    uint64_t m_pendingFence = 0;
};

class ReadyFence final : public GpuFence {
public:
    uint64_t signal() override { return 1; }
    bool wait(uint64_t value, int) override { return value <= 1; }
    uint64_t completedValue() const override { return 1; }
};

class MarkerGpuFrameData final : public IFrameData {
public:
    MarkerGpuFrameData(CpuPlanes planes, std::shared_ptr<GpuSurface> surface)
        : m_planes(std::move(planes)), m_surface(std::move(surface)),
          m_fence(std::make_shared<ReadyFence>()) {
        if (m_surface) m_surface->retainUntilFenceRetired(1);
    }

    bool isGpuBacked() const override { return true; }
    CpuPlanes readToCpu(FramePixelFormat target) const override {
        return target == FramePixelFormat::Yuv420p ? m_planes : CpuPlanes{};
    }
    CpuPlanes cachedCpuPlanes(FramePixelFormat target) const override { return readToCpu(target); }
    GpuSurface* gpuSurface() const override { return m_surface.get(); }
    std::shared_ptr<GpuFence> gpuFence() const override { return m_fence; }
    FramePixelFormat nativeFormat() const override {
        return m_surface ? m_surface->desc().format : m_planes.format;
    }

private:
    CpuPlanes m_planes;
    std::shared_ptr<GpuSurface> m_surface;
    std::shared_ptr<GpuFence> m_fence;
};

class RecordingAjaBackend final : public IAjaSenderBackend {
public:
    explicit RecordingAjaBackend(RecordingFrameStore* store) : m_store(store) {}

    bool isRuntimeAvailable() const override { return true; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return true; }
    void closeDevice() override {}
    bool transferFrame(const OutputBusFrame& frame) override {
        return m_store && m_store->append(frame);
    }

private:
    RecordingFrameStore* m_store = nullptr;
};

class RecordingOmtBackend final : public IOmtSenderBackend {
public:
    explicit RecordingOmtBackend(RecordingFrameStore* store) : m_store(store) {}

    bool isRuntimeAvailable() const override { return true; }
    bool createSender(const QString& senderName, FrameRate rate) override {
        m_senderName = senderName;
        m_rate = rate;
        return !m_senderName.trimmed().isEmpty() && m_rate.isValid();
    }
    void destroySender() override {}
    bool sendFrame(const OutputBusFrame& frame) override {
        return m_store && m_store->append(frame);
    }

private:
    RecordingFrameStore* m_store = nullptr;
    QString m_senderName;
    FrameRate m_rate;
};

class RecordingDeckLinkBackend final : public IDeckLinkSenderBackend {
public:
    RecordingDeckLinkBackend(RecordingFrameStore* store, bool gpuTextureInput)
        : m_store(store), m_gpuTextureInput(gpuTextureInput) {}

    bool isRuntimeAvailable() const override { return true; }
    bool deviceSupportsGpuTextureInput() const override { return m_gpuTextureInput; }
    bool openDevice(const OutputTargetAssignment&, FrameRate) override { return true; }
    void closeDevice() override {}
    bool scheduleFrame(OutputBusFrame frame) override {
        ++cpuFrames;
        return m_store && m_store->append(frame);
    }
    bool scheduleGpuFrame(OutputBusFrame frame) override {
        ++gpuFrames;
        return m_store && m_store->append(frame);
    }
    bool scheduleSt2110Frame(OutputBusFrame frame, St2110VideoFrame st2110Frame) override {
        st2110Frames.append(std::move(st2110Frame));
        return scheduleFrame(std::move(frame));
    }
    bool scheduleGpuSt2110Frame(OutputBusFrame frame, St2110VideoFrame st2110Frame) override {
        st2110Frames.append(std::move(st2110Frame));
        return scheduleGpuFrame(std::move(frame));
    }

    int cpuFrames = 0;
    int gpuFrames = 0;
    QVector<St2110VideoFrame> st2110Frames;

private:
    RecordingFrameStore* m_store = nullptr;
    bool m_gpuTextureInput = false;
};

std::optional<TargetConfig> targetConfigFor(const QString& kind) {
    if (kind == QStringLiteral("aja")) {
        return TargetConfig{kind, OutputTargetKind::Aja, true, false, QStringLiteral("continuous")};
    }
    if (kind == QStringLiteral("omt")) {
        return TargetConfig{kind, OutputTargetKind::Omt, true, false, QStringLiteral("continuous")};
    }
    if (kind == QStringLiteral("decklink-readback")) {
        return TargetConfig{kind, OutputTargetKind::DeckLinkSdiHdmi, true, false,
                            QStringLiteral("continuous")};
    }
    if (kind == QStringLiteral("decklink-gpu")) {
        return TargetConfig{kind, OutputTargetKind::DeckLinkSdiHdmi, false, true,
                            QStringLiteral("gpu-native")};
    }
    if (kind == QStringLiteral("decklink-st2110-readback")) {
        return TargetConfig{kind, OutputTargetKind::DeckLinkIpSt2110, true, false,
                            QStringLiteral("continuous")};
    }
    if (kind == QStringLiteral("decklink-st2110-gpu")) {
        return TargetConfig{kind, OutputTargetKind::DeckLinkIpSt2110, false, true,
                            QStringLiteral("gpu-native")};
    }
    return std::nullopt;
}

OutputTargetAssignment assignmentFor(const TargetConfig& target) {
    OutputTargetAssignment assignment;
    assignment.id = QStringLiteral("iotarget-%1").arg(target.cliKind);
    assignment.kind = target.outputKind;
    assignment.sourceBus = OutputBusId::pgm();
    assignment.enabled = true;
    if (target.outputKind == OutputTargetKind::Omt) {
        assignment.settings.insert(QStringLiteral("senderName"),
                                   QStringLiteral("OLR I/O Target Marker"));
    }
    return assignment;
}

MediaAudioFrame markerAudioFrame(const NdiOutputMarkerConfig& cfg, qint64 frameIndex) {
    MediaAudioFrame audio;
    audio.feedIndex = 0;
    audio.startSample = frameIndex * qint64(ndiMarkerSamplesPerFrame(cfg));
    audio.sampleRate = cfg.sampleRate;
    audio.channels = cfg.channels;
    audio.format = MediaSampleFormat::S16Interleaved;
    audio.pcm = ndiMarkerAudioS16(cfg, frameIndex);
    return audio;
}

OutputBusFrame markerBusFrame(const NdiOutputMarkerConfig& cfg, FrameRate rate, qint64 frameIndex,
                              bool gpuBacked) {
    CpuPlanes planes;
    planes.format = FramePixelFormat::Yuv420p;
    planes.width = cfg.width;
    planes.height = cfg.height;
    planes.stride[0] = cfg.width;
    planes.stride[1] = (cfg.width + 1) / 2;
    planes.stride[2] = (cfg.width + 1) / 2;
    const int chromaHeight = (cfg.height + 1) / 2;
    planes.plane[0] = ndiMarkerLumaPlane(cfg, frameIndex);
    planes.plane[1] = QByteArray(planes.stride[1] * chromaHeight, char(128));
    planes.plane[2] = QByteArray(planes.stride[2] * chromaHeight, char(128));

    const qint64 ptsMs = rate.frameIndexToMs(frameIndex);
    FrameMetadata meta;
    meta.key.feedIndex = 0;
    meta.key.ptsMs = ptsMs;
    meta.key.format = FramePixelFormat::Yuv420p;
    meta.key.width = cfg.width;
    meta.key.height = cfg.height;
    meta.outputFrameIndex = frameIndex;
    meta.sampledPlayheadMs = ptsMs;
    meta.stride[0] = planes.stride[0];
    meta.stride[1] = planes.stride[1];
    meta.stride[2] = planes.stride[2];
    meta.decodedSequence = frameIndex;

    OutputBusFrame frame;
    frame.bus = OutputBusId::pgm();
    frame.outputFrameIndex = frameIndex;
    frame.sampledPlayheadMs = ptsMs;
    frame.programmeTimecode100ns = ptsMs * 10000;
    if (gpuBacked) {
        meta.gpuGeneration = 1;
        auto surface = std::make_shared<MarkerGpuSurface>(cfg.width, cfg.height, planes.format);
        frame.video = FrameHandle(
            std::make_shared<MarkerGpuFrameData>(std::move(planes), std::move(surface)), meta);
    } else {
        frame.video = makeCpuFrameHandle(std::move(planes), meta);
    }
    frame.audio = markerAudioFrame(cfg, frameIndex);
    frame.identity = outputFrameIdentityFor(frame);
    return frame;
}

bool audioHasSignal(const MediaAudioFrame& audio) {
    for (const char sample : audio.pcm) {
        if (sample != '\0') return true;
    }
    return false;
}

struct ContinuityReport {
    qint64 framesReceived = 0;
    qint64 maxGapFrames = 0;
    qint64 drops = 0;
    qint64 avSyncMaxFrames = 0;
};

std::optional<ContinuityReport> analyzeFrames(const QVector<OutputBusFrame>& frames,
                                              const NdiOutputMarkerConfig& cfg) {
    ContinuityReport report;
    report.framesReceived = frames.size();

    const qint64 samplesPerFrame = ndiMarkerSamplesPerFrame(cfg);
    qint64 previousMarker = -1;
    bool havePrevious = false;

    for (const OutputBusFrame& frame : frames) {
        const CpuPlanes planes = frame.video.readToCpu(FramePixelFormat::Yuv420p);
        if (!planes.isValid() || planes.plane[0].isEmpty()) {
            qWarning() << "captured frame is not valid YUV420P";
            return std::nullopt;
        }

        const auto* luma = reinterpret_cast<const uchar*>(planes.plane[0].constData());
        const qint64 marker = ndiMarkerDecodeIndex(cfg, luma, planes.stride[0]);
        if (marker < 0) {
            qWarning() << "failed to decode marker index";
            return std::nullopt;
        }

        if (havePrevious) {
            if (marker <= previousMarker) {
                ++report.drops;
            } else {
                const qint64 gapFrames = marker - previousMarker - 1;
                report.maxGapFrames = qMax(report.maxGapFrames, gapFrames);
                report.drops += gapFrames;
            }
        }
        previousMarker = marker;
        havePrevious = true;

        if (samplesPerFrame <= 0) {
            qWarning() << "invalid marker audio samples per frame";
            return std::nullopt;
        }
        const qint64 audioFrameIndex = frame.audio.startSample / samplesPerFrame;
        report.avSyncMaxFrames = qMax(report.avSyncMaxFrames, qAbs(marker - audioFrameIndex));

        const bool flashVideo = ndiMarkerDecodeFlash(cfg, luma, planes.stride[0]);
        if (flashVideo != audioHasSignal(frame.audio)) {
            qWarning() << "marker A/V flash mismatch at frame" << marker;
            return std::nullopt;
        }
    }

    return report;
}

QByteArray st2110ExpectedCpuEssence(const OutputBusFrame& frame) {
    const CpuPlanes planes = frame.video.readToCpu(FramePixelFormat::Yuv420p);
    if (!planes.isValid()) return {};

    QByteArray essence;
    essence.reserve(planes.plane[0].size() + planes.plane[1].size() + planes.plane[2].size());
    essence.append(planes.plane[0]);
    essence.append(planes.plane[1]);
    essence.append(planes.plane[2]);
    return essence;
}

QByteArray st2110ExpectedGpuEssence(const OutputBusFrame& frame) {
    return QByteArray::number(frame.outputFrameIndex) + ':' +
           QByteArray::number(frame.identity.videoHash) + ':' +
           QByteArray::number(frame.video.metadata().gpuGeneration);
}

bool validateSt2110Frames(const QVector<OutputBusFrame>& frames,
                          const QVector<St2110VideoFrame>& st2110Frames,
                          const NdiOutputMarkerConfig& cfg, bool gpuNative) {
    constexpr quint32 kExpectedSsrc = 0x4f4c5231u;
    if (st2110Frames.size() != frames.size()) {
        qWarning() << "ST2110 frame count mismatch" << st2110Frames.size() << frames.size();
        return false;
    }

    for (qsizetype i = 0; i < frames.size(); ++i) {
        const OutputBusFrame& frame = frames.at(i);
        const St2110VideoFrame& st2110 = st2110Frames.at(i);
        const QByteArray expected =
            gpuNative ? st2110ExpectedGpuEssence(frame) : st2110ExpectedCpuEssence(frame);
        if (expected.isEmpty() || st2110.essence != expected) {
            qWarning() << "ST2110 essence mismatch at frame" << i
                       << "outputFrameIndex=" << frame.outputFrameIndex
                       << "expectedSize=" << expected.size()
                       << "actualSize=" << st2110.essence.size()
                       << "expectedHead=" << expected.left(16).toHex()
                       << "actualHead=" << st2110.essence.left(16).toHex();
            return false;
        }
        if (!gpuNative) {
            const auto* luma = reinterpret_cast<const uchar*>(st2110.essence.constData());
            const qint64 marker = ndiMarkerDecodeIndex(cfg, luma, cfg.width);
            if (marker != frame.outputFrameIndex) {
                qWarning() << "ST2110 marker mismatch" << marker << frame.outputFrameIndex;
                return false;
            }
        }
        const quint32 expectedRtp =
            quint32(((frame.programmeTimecode100ns * qint64(90000)) / qint64(10000000)) &
                    qint64(0xFFFFFFFFu));
        if (st2110.rtpTimestamp90k != expectedRtp || st2110.payloadType != quint8(96) ||
            st2110.ssrc != kExpectedSsrc || !st2110.markerLast) {
            qWarning() << "ST2110 framer metadata mismatch at frame" << i;
            return false;
        }
    }
    return true;
}

std::unique_ptr<IOutputSink> wrapIfNeeded(std::unique_ptr<IOutputSink> inner,
                                          const TargetConfig& target) {
    if (!target.wrapReadback) return inner;
    return std::make_unique<AsyncGpuReadbackSink>(std::move(inner), 3, FramePixelFormat::Yuv420p,
                                                  SinkGpuCapability::NeedsContinuousCadence);
}

} // namespace

int runIoTargetMarkerSender(const QStringList& arguments) {
    if (arguments.size() != 3) {
        qWarning() << "usage: iotarget_marker_sender"
                   << "aja|omt|decklink-readback|decklink-gpu"
                   << "frames";
        return 2;
    }

    const QString kind = arguments.at(1);
    const auto target = targetConfigFor(kind);
    if (!target) {
        qWarning() << "unsupported I/O target kind" << kind;
        return 2;
    }

    bool ok = false;
    const int frameCount = arguments.at(2).toInt(&ok);
    if (!ok || frameCount <= 0) {
        qWarning() << "invalid frame count" << arguments.at(2);
        return 2;
    }

    RecordingFrameStore store;
    RecordingAjaBackend ajaBackend(&store);
    RecordingOmtBackend omtBackend(&store);
    RecordingDeckLinkBackend deckLinkBackend(&store, target->deckLinkGpuInput);

    std::unique_ptr<IOutputSink> inner;
    switch (target->outputKind) {
    case OutputTargetKind::Aja:
        inner = std::make_unique<AjaOutputSink>(&ajaBackend);
        break;
    case OutputTargetKind::Omt:
        inner = std::make_unique<OmtOutputSink>(&omtBackend);
        break;
    case OutputTargetKind::DeckLinkSdiHdmi:
        inner = std::make_unique<DeckLinkOutputSink>(OutputTargetKind::DeckLinkSdiHdmi,
                                                     &deckLinkBackend);
        break;
    case OutputTargetKind::DeckLinkIpSt2110:
        inner = std::make_unique<DeckLinkOutputSink>(OutputTargetKind::DeckLinkIpSt2110,
                                                     &deckLinkBackend);
        break;
    case OutputTargetKind::QtPreview:
    case OutputTargetKind::Ndi:
        qWarning() << "unsupported harness target" << int(target->outputKind);
        return 2;
    }

    std::unique_ptr<IOutputSink> sink = wrapIfNeeded(std::move(inner), *target);
    const FrameRate rate = FrameRate::fromFraction(30, 1);
    const OutputTargetAssignment assignment = assignmentFor(*target);
    if (!sink->start(assignment, rate)) {
        const OutputSinkStatus status = sink->outputStatus();
        qWarning() << "failed to start sink" << status.state << status.message;
        return 3;
    }

    NdiOutputMarkerConfig markerConfig;
    markerConfig.fpsNum = rate.numerator;
    markerConfig.fpsDen = rate.denominator;

    constexpr int kReadbackFlushFrames = 16;
    const int submittedFrames = frameCount + (target->wrapReadback ? kReadbackFlushFrames : 0);
    for (int i = 0; i < submittedFrames; ++i) {
        const bool submitGpuBacked = target->wrapReadback || target->deckLinkGpuInput;
        if (!sink->submit(markerBusFrame(markerConfig, rate, i, submitGpuBacked))) {
            qWarning() << "sink rejected marker frame" << i;
            return 4;
        }
    }
    sink->stop();

    const std::optional<ContinuityReport> report = analyzeFrames(store.frames, markerConfig);
    if (!report) return 5;

    QTextStream(stdout) << "IOTARGET kind=" << target->cliKind
                        << " framesReceived=" << report->framesReceived
                        << " maxGapFrames=" << report->maxGapFrames << " drops=" << report->drops
                        << " avSyncMaxFrames=" << report->avSyncMaxFrames
                        << " cadence=" << target->cadence << Qt::endl;

    if (target->deckLinkGpuInput && deckLinkBackend.gpuFrames != frameCount) {
        qWarning() << "DeckLink GPU-native path did not receive every frame"
                   << deckLinkBackend.gpuFrames << frameCount;
        return 6;
    }
    if (!target->deckLinkGpuInput &&
        (target->outputKind == OutputTargetKind::DeckLinkSdiHdmi ||
         target->outputKind == OutputTargetKind::DeckLinkIpSt2110) &&
        deckLinkBackend.cpuFrames < frameCount) {
        qWarning() << "DeckLink CPU path did not receive every frame" << deckLinkBackend.cpuFrames
                   << frameCount;
        return 6;
    }
    if (target->outputKind == OutputTargetKind::DeckLinkIpSt2110 &&
        !validateSt2110Frames(store.frames, deckLinkBackend.st2110Frames, markerConfig,
                              target->deckLinkGpuInput)) {
        return 7;
    }

    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return runIoTargetMarkerSender(app.arguments());
}
