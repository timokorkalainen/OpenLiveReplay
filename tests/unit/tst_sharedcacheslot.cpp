#include <QtTest>
#include <atomic>
#include <memory>
#include <thread>
#include "playback/output/framehandle.h"
#include "playback/output/sharedcacheslot.h"

static std::shared_ptr<const OutputFrameCache> makeCache(uchar y) {
    auto c = std::make_shared<OutputFrameCache>(1, 4, 4);
    FrameHandle f = solidYuv420pHandle(4, 4, y, 128, 128);
    f.metadata().key.feedIndex = 0;
    f.metadata().key.ptsMs = 100;
    c->insertVideoFrame(f);
    return std::const_pointer_cast<const OutputFrameCache>(c);
}

class TestSharedCacheSlot : public QObject {
    Q_OBJECT
private slots:
    void loadReturnsLastPublished();
    void concurrentPublishYieldsConsistentSnapshot();
};

void TestSharedCacheSlot::loadReturnsLastPublished() {
    SharedCacheSlot slot;
    slot.publish(makeCache(11));
    slot.publish(makeCache(22));
    auto got = slot.load();
    QVERIFY(got != nullptr);
    auto frame = got->videoFrameAt(0, 100);
    QVERIFY(frame.has_value());
    QCOMPARE(int(uchar(MediaVideoFrameView(*frame).planeY.at(0))), 22);
}

void TestSharedCacheSlot::concurrentPublishYieldsConsistentSnapshot() {
    SharedCacheSlot slot;
    slot.publish(makeCache(1));
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 5000 && !stop.load(); ++i)
            slot.publish(makeCache(uchar(50 + (i % 3))));
    });
    for (int i = 0; i < 5000; ++i) {
        auto snap = slot.load(); // never null after first publish
        QVERIFY(snap != nullptr);
        auto frame = snap->videoFrameAt(0, 100);
        QVERIFY(frame.has_value()); // immutable: never torn
        const int v = int(uchar(MediaVideoFrameView(*frame).planeY.at(0)));
        QVERIFY(v == 1 || (v >= 50 && v <= 52));
    }
    stop.store(true);
    writer.join();
}

QTEST_MAIN(TestSharedCacheSlot)
#include "tst_sharedcacheslot.moc"
