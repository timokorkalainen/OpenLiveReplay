#ifndef REPOSITIONFILLPOLICY_H
#define REPOSITIONFILLPOLICY_H

#include <QtGlobal>

inline int repositionReferenceFeed(int feedCount, int selectedFeed) {
    if (feedCount <= 0) return -1;
    return selectedFeed >= 0 && selectedFeed < feedCount ? selectedFeed : 0;
}

inline int playbackViewReferenceFeed(bool singleView, int selectedFeed) {
    return singleView ? selectedFeed : -1;
}

template <typename NewestPtsAt>
bool repositionFillReady(int feedCount, int selectedFeed, bool requireAllFeeds, qint64 fillToMs,
                         NewestPtsAt&& newestPtsAt) {
    if (feedCount <= 0) return false;

    if (requireAllFeeds) {
        for (int feed = 0; feed < feedCount; ++feed) {
            if (newestPtsAt(feed) < fillToMs) return false;
        }
        return true;
    }

    const int referenceFeed = repositionReferenceFeed(feedCount, selectedFeed);
    return newestPtsAt(referenceFeed) >= fillToMs;
}

#endif // REPOSITIONFILLPOLICY_H
