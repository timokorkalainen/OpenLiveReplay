#include "recorder_engine/ingest/spsframerate.h"

#include "recorder_engine/ingest/h26xtimingcontext.h"

SpsFrameRate parseSpsFrameRate(NativeVideoCodec codec, const QByteArray& nal) {
    H26xTimingContext context;
    if (!context.updateParameterSets(codec, {}, {nal})) return {};
    const FrameRateQ rate = context.constantFrameRate();
    if (!rate.valid()) return {};
    return SpsFrameRate{rate.num, rate.den};
}
