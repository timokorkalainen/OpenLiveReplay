#ifndef SHAREDCACHESLOT_H
#define SHAREDCACHESLOT_H

#include "playback/output/outputframecache.h"

#include <QMutex>
#include <memory>
#include <utility>

// Single-slot publication of an immutable OutputFrameCache. The worker
// publishes a fresh shared_ptr after each cache mutation; the output thread
// loads it lock-cheap and reads it immutably. The published object is const
// and only ever replaced, so a loaded snapshot is never written again.
class SharedCacheSlot {
public:
    std::shared_ptr<const OutputFrameCache> publish(std::shared_ptr<const OutputFrameCache> next) {
        QMutexLocker locker(&m_mutex);
        std::shared_ptr<const OutputFrameCache> previous = std::move(m_current);
        m_current = std::move(next);
        return previous;
    }
    std::shared_ptr<const OutputFrameCache> load() const {
        QMutexLocker locker(&m_mutex);
        return m_current;
    }

private:
    mutable QMutex m_mutex;
    std::shared_ptr<const OutputFrameCache> m_current;
};

#endif // SHAREDCACHESLOT_H
