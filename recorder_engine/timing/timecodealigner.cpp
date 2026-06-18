#include "timecodealigner.h"

// Convert a 100 ns timecode (since midnight) to an absolute TC frame count at
// the nominal integer fps. Uses the same rounding as Smpte12m::from100ns so a
// 30/60 fps TC round-trips to the correct frame for NTSC-derived rates.
static int64_t tcFramesFrom100ns(int64_t timecode100ns, int nominalFps)
{
    const int fps = nominalFps > 0 ? nominalFps : 30;
    return (timecode100ns * fps + 5'000'000) / 10'000'000;
}

TimecodeAligner::TimecodeAligner(int nominalFps)
    : m_nominalFps(nominalFps > 0 ? nominalFps : 30)
{
}

void TimecodeAligner::observe(int sourceIndex, int64_t sourceTimecode100ns,
                              int64_t sessionFrameIndex)
{
    if (sourceIndex < 0 || sourceIndex >= kMaxSources)
        return;
    if (sourceTimecode100ns < 0)
        return;

    Anchor& anchor = m_anchors[sourceIndex];
    if (anchor.set)
        return; // keep the FIRST observation as the anchor

    anchor.set = true;
    anchor.tcFrames = tcFramesFrom100ns(sourceTimecode100ns, m_nominalFps);
    anchor.sessionFrame = sessionFrameIndex;
}

bool TimecodeAligner::hasTimecode(int sourceIndex) const
{
    if (sourceIndex < 0 || sourceIndex >= kMaxSources)
        return false;
    return m_anchors[sourceIndex].set;
}

int64_t TimecodeAligner::toSessionFrameIndex(int sourceIndex,
                                             int64_t mediaTimecode100ns) const
{
    if (sourceIndex < 0 || sourceIndex >= kMaxSources)
        return -1;
    const Anchor& anchor = m_anchors[sourceIndex];
    if (!anchor.set)
        return -1;
    const int64_t tcFrames = tcFramesFrom100ns(mediaTimecode100ns, m_nominalFps);
    return anchor.sessionFrame + (tcFrames - anchor.tcFrames);
}

int64_t TimecodeAligner::frameOffset(int sourceIndexA, int sourceIndexB) const
{
    if (sourceIndexA < 0 || sourceIndexA >= kMaxSources)
        return 0;
    if (sourceIndexB < 0 || sourceIndexB >= kMaxSources)
        return 0;
    const Anchor& a = m_anchors[sourceIndexA];
    const Anchor& b = m_anchors[sourceIndexB];
    if (!a.set || !b.set)
        return 0;

    // delta = sessionFrame - tcFrames is each source's TC->frame offset. The
    // residual between B and A is (deltaB - deltaA); negated, it is the number
    // of frames to ADD to source B's mapping so equal-TC frames coincide with A.
    const int64_t deltaA = a.sessionFrame - a.tcFrames;
    const int64_t deltaB = b.sessionFrame - b.tcFrames;
    return -(deltaB - deltaA);
}

bool TimecodeAligner::sourcesAligned(int sourceIndexA, int sourceIndexB,
                                     int toleranceFrames) const
{
    if (!hasTimecode(sourceIndexA) || !hasTimecode(sourceIndexB))
        return false;
    const int64_t off = frameOffset(sourceIndexA, sourceIndexB);
    const int64_t absOff = off < 0 ? -off : off;
    return absOff <= toleranceFrames;
}

bool TimecodeAligner::firstTimecode100ns(int64_t& out) const
{
    bool found = false;
    int64_t earliestFrames = 0;
    for (int i = 0; i < kMaxSources; ++i) {
        if (!m_anchors[i].set)
            continue;
        if (!found || m_anchors[i].tcFrames < earliestFrames) {
            earliestFrames = m_anchors[i].tcFrames;
            found = true;
        }
    }
    if (!found)
        return false;
    // Inverse of tcFramesFrom100ns: frames * 1e7 / fps.
    out = earliestFrames * 10'000'000 / m_nominalFps;
    return true;
}

void TimecodeAligner::reset()
{
    for (int i = 0; i < kMaxSources; ++i)
        m_anchors[i] = Anchor{};
}
