#ifndef CUTSCHEDULE_H
#define CUTSCHEDULE_H

#include <QtGlobal>

// Pure boundary math for the atomic cut: maps a lead to the output frame index
// at which the staging cache should be promoted to active.
struct CutSchedule {
    // The output frame index that the dispatcher has not yet emitted, offset by lead.
    static qint64 outputFrameForCut(qint64 nextOutputFrameIndex, int leadFrames);

    // Convert a lead in wall-clock ms to whole output frames (ceil, fps-relative).
    static qint64 framesForLeadMs(qint64 leadMs, double fps);

    // Fire when the dispatcher's next index has reached (or overshot) the schedule.
    static bool shouldFireAt(qint64 nextOutputFrameIndex, qint64 scheduledFrame);

    // Playhead position once the cut fires, accounting for any dispatcher overshoot.
    static qint64 playheadAfterCut(qint64 targetMs, qint64 firedFrameIndex,
                                   qint64 scheduledFrameIndex, double fps);
};

#endif // CUTSCHEDULE_H
