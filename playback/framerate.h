#ifndef FRAMERATE_H
#define FRAMERATE_H

#include <QtGlobal>

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

    int roundedFps(int fallback = 30) const {
        if (!isValid()) return fallback;
        return qMax(1, qRound(double(numerator) / double(denominator)));
    }

    qint64 frameIndexToMs(qint64 frameIndex) const {
        if (!isValid() || frameIndex <= 0) return 0;
        return (frameIndex * qint64(1000) * denominator) / numerator;
    }

    qint64 msToFrameIndex(qint64 ms) const {
        if (!isValid() || ms <= 0) return 0;
        return (ms * qint64(numerator)) / (qint64(1000) * denominator);
    }
};

#endif // FRAMERATE_H
