#ifndef OUTPUTTYPES_H
#define OUTPUTTYPES_H

#include <QString>
#include <QtGlobal>

#include <cstdint>

struct FrameRate {
    int numerator = 0;
    int denominator = 1;

    static FrameRate fromFraction(int num, int den) {
        FrameRate r;
        r.numerator = num;
        r.denominator = den;
        return r;
    }

    bool isValid() const { return numerator > 0 && denominator > 0; }

    qint64 frameIndexToMs(qint64 frameIndex) const {
        if (!isValid() || frameIndex <= 0) return 0;
        return (frameIndex * qint64(1000) * denominator) / numerator;
    }

    qint64 msToFrameIndex(qint64 ms) const {
        if (!isValid() || ms <= 0) return 0;
        return (ms * qint64(numerator)) / (qint64(1000) * denominator);
    }
};

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
};

#endif // OUTPUTTYPES_H
