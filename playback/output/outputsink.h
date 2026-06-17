#ifndef OUTPUTSINK_H
#define OUTPUTSINK_H

#include "playback/output/outputbusengine.h"
#include "playback/output/outputtargetassignment.h"

struct OutputSinkStatus {
    qint64 acceptedFrames = 0;
    qint64 failedFrames = 0;
    qint64 droppedFrames = 0;
    qint64 currentQueueDepth = 0;
    qint64 maxQueueDepth = 0;
    qint64 deliveryGaps = 0;
    qint64 lastQueuedFrameIndex = -1;
    qint64 lastDeliveredFrameIndex = -1;
    qint64 lastSubmitDurationNs = 0;
    bool hasLastResult = false;
    bool lastResultSucceeded = true;
    bool hasLastQueuedFrameIndex = false;
    bool hasLastDeliveredFrameIndex = false;
    QString state;
    QString message;
};

class IOutputSink {
public:
    virtual ~IOutputSink() = default;

    virtual OutputTargetKind kind() const = 0;
    virtual bool start(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;
    virtual bool submit(const OutputBusFrame& frame) = 0;
    virtual OutputSinkStatus outputStatus() const { return OutputSinkStatus{}; }
};

#endif // OUTPUTSINK_H
