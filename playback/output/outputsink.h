#ifndef OUTPUTSINK_H
#define OUTPUTSINK_H

#include "playback/output/outputbusengine.h"
#include "playback/output/outputtargetassignment.h"

class IOutputSink {
public:
    virtual ~IOutputSink() = default;

    virtual OutputTargetKind kind() const = 0;
    virtual bool start(const OutputTargetAssignment& assignment, FrameRate rate) = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;
    virtual bool submit(const OutputBusFrame& frame) = 0;
};

#endif // OUTPUTSINK_H
