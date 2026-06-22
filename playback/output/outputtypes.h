#ifndef OUTPUTTYPES_H
#define OUTPUTTYPES_H

#include <QString>
#include <QtGlobal>

#include <cstdint>

#include "playback/framerate.h"

enum class OutputBusKind {
    Feed,
    Multiview,
    Pgm,
};

struct OutputBusId {
    OutputBusKind kind = OutputBusKind::Feed;
    int index = 0;

    static OutputBusId feed(int feedIndex) { return {OutputBusKind::Feed, feedIndex}; }
    static OutputBusId multiview() { return {OutputBusKind::Multiview, 0}; }
    static OutputBusId pgm() { return {OutputBusKind::Pgm, 0}; }

    bool operator==(const OutputBusId& other) const {
        return kind == other.kind && index == other.index;
    }
};

inline uint qHash(const OutputBusId& id, uint seed = 0) {
    return seed ^ uint(int(id.kind) * 1000003 + id.index);
}

enum class OutputTargetKind {
    QtPreview,
    DeckLinkSdiHdmi,
    DeckLinkIpSt2110,
    Ndi,
    Omt,
    Aja,
};

enum class MediaPixelFormat {
    Invalid,
    Yuv420p,
};

enum class MediaSampleFormat {
    Invalid,
    S16Interleaved,
};

struct PlaybackStateSnapshot {
    qint64 playheadMs = 0;
    bool playing = false;
    double speed = 1.0;
    qint64 playStartedAtOutputFrame = 0;
    qint64 playStartedAtPlayheadMs = 0;
    int selectedFeedIndex = -1;
    uint64_t gpuGeneration = 0;
    bool forcePlayEpochReset = false;
};

#endif // OUTPUTTYPES_H
