#ifndef OUTPUTBUSENGINE_H
#define OUTPUTBUSENGINE_H

#include "playback/output/outputframecache.h"
#include "playback/output/outputframeclock.h"

#include <QDebug>

struct OutputFrameIdentity {
    OutputBusId bus = OutputBusId::pgm();
    qint64 outputFrameIndex = -1;
    qint64 sampledPlayheadMs = 0;
    int sourceFeedIndex = -1;
    qint64 sourcePtsMs = 0;
    bool videoPlaceholder = false;
    bool audioSilent = true;
    quint32 videoHash = 0;
    quint32 audioHash = 0;

    bool samePayloadAs(const OutputFrameIdentity& other) const {
        return bus == other.bus && sourceFeedIndex == other.sourceFeedIndex &&
               sourcePtsMs == other.sourcePtsMs && videoPlaceholder == other.videoPlaceholder &&
               audioSilent == other.audioSilent && videoHash == other.videoHash &&
               audioHash == other.audioHash;
    }

    bool operator==(const OutputFrameIdentity& other) const {
        return bus == other.bus && outputFrameIndex == other.outputFrameIndex &&
               sampledPlayheadMs == other.sampledPlayheadMs &&
               sourceFeedIndex == other.sourceFeedIndex && sourcePtsMs == other.sourcePtsMs &&
               videoPlaceholder == other.videoPlaceholder && audioSilent == other.audioSilent &&
               videoHash == other.videoHash && audioHash == other.audioHash;
    }
};

QDebug operator<<(QDebug debug, const OutputFrameIdentity& identity);

struct OutputBusFrame {
    OutputBusId bus;
    qint64 outputFrameIndex = 0;
    qint64 sampledPlayheadMs = 0;
    MediaVideoFrame video;
    MediaAudioFrame audio;
    OutputFrameIdentity identity;
};

OutputFrameIdentity outputFrameIdentityFor(const OutputBusFrame& frame);

// Persistent (caller-owned) memo for the multiview compositor. The composited grid depends
// only on the source frames selected for a tick, so when the exact per-feed source
// descriptor is unchanged the full-resolution scale can be skipped and the prior planes
// reused. The descriptor (not a hash) gates reuse so memoized output is byte-identical to a
// fresh composite — a hash key could collide and emit a stale frame.
struct MultiviewComposite {
    bool valid = false;
    QVector<qint64> sourceKeys; // 2 entries per feed: present flag, then selected pts
    MediaVideoFrame video;
};

class OutputBusEngine {
public:
    OutputBusEngine(FrameRate rate, int feedCount, int width, int height);

    OutputBusFrame renderFeed(int feedIndex, qint64 outputFrameIndex,
                              const PlaybackStateSnapshot& state,
                              const OutputFrameCache& cache) const;
    OutputBusFrame renderPgm(qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
                             const OutputFrameCache& cache) const;
    OutputBusFrame renderMultiview(qint64 outputFrameIndex, const PlaybackStateSnapshot& state,
                                   const OutputFrameCache& cache,
                                   MultiviewComposite* memo = nullptr) const;

    int audioSamplesPerFrame() const;

private:
    OutputBusFrame renderSingleSource(OutputBusId bus, int feedIndex, qint64 outputFrameIndex,
                                      const PlaybackStateSnapshot& state,
                                      const OutputFrameCache& cache, bool allowAudio) const;
    MediaAudioFrame renderAudioForFeed(int feedIndex, qint64 outputFrameIndex,
                                       const PlaybackStateSnapshot& state,
                                       const OutputFrameCache& cache, bool allowAudio) const;
    qint64 audioBoundarySampleForFrame(qint64 outputFrameIndex) const;
    qint64 sourceAudioStartSample(qint64 outputFrameIndex,
                                  const PlaybackStateSnapshot& state) const;
    int audioSamplesForOutputFrame(qint64 outputFrameIndex) const;

    OutputFrameClock m_clock;
    int m_feedCount = 0;
    int m_width = 1920;
    int m_height = 1080;
};

#endif // OUTPUTBUSENGINE_H
