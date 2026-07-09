#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/output/colormetadatapolicy.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputframecache.h"
#include "playback/output/qtpreviewsink.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <QElapsedTimer>
#include <QThread>

class TestQtPreviewSink : public QObject {
    Q_OBJECT
private slots:
    void deliverMediaFrameUpdatesProviderLatestImage();
    void frameProviderDeliverHandleUpdatesLatestImage();
    void frameProviderUsesMonotonicPresentationTimesForBackwardPlayback();
    void frameProviderPresentationTimesTrackDeliveryClock();
    void frameProviderEmitsFrameChangedForDirectPreview();
    void frameProviderCoalescesQueuedVideoSinkUpdatesToLatestFrame();
    void frameProviderFlushWaitsForSubmittedSerial();
    void frameProviderFlushWaitsForDirectPreviewConsumerSerial();
    void outputSinkFlushDoesNotBlockOnDirectPreviewConsumer();
    void deliverBusEngineFrameUpdatesProviderLatestImage();
    void outputSinkEndpointDeliversOnlyWhenStarted();
    void outputSinkIsInactiveUntilProviderHasConsumer();
    void outputSinkFlushTimesOutWhenVideoSinkCannotDrain();
    void qVideoFrameCarriesPresentationTimeFromFrameMetadata();
    void colorMetadataRoundTripsDecodeToSink();
    void taggedBt601FrameMapsToBt601();
    void defaultTaggingReproducesLegacyHeightHeuristic();
};

void TestQtPreviewSink::deliverMediaFrameUpdatesProviderLatestImage() {
    FrameProvider provider;
    QtPreviewSink sink(&provider);

    FrameHandle frame = solidYuv420pHandle(4, 4, 80, 128, 128);
    frame.metadata().key.ptsMs = 123;
    frame.metadata().outputFrameIndex = 9;
    QVERIFY(sink.deliver(frame));

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
}

void TestQtPreviewSink::frameProviderDeliverHandleUpdatesLatestImage() {
    FrameProvider provider;

    FrameHandle frame = solidYuv420pHandle(4, 4, 80, 128, 128);
    frame.metadata().key.ptsMs = 123;
    provider.deliverHandle(frame);

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
}

void TestQtPreviewSink::frameProviderUsesMonotonicPresentationTimesForBackwardPlayback() {
    FrameProvider provider;
    QVideoSink sink;
    provider.addVideoSink(&sink);

    FrameHandle later = solidYuv420pHandle(4, 4, 90, 128, 128);
    later.metadata().key.ptsMs = 1000;
    const quint64 laterSerial = provider.deliverHandle(later);
    const qint64 laterStartUs = sink.videoFrame().startTime();

    FrameHandle earlier = solidYuv420pHandle(4, 4, 110, 128, 128);
    earlier.metadata().key.ptsMs = 900;
    const quint64 earlierSerial = provider.deliverHandle(earlier);
    const qint64 earlierStartUs = sink.videoFrame().startTime();

    QVERIFY(earlierSerial > laterSerial);
    QVERIFY2(earlierStartUs > laterStartUs,
             qPrintable(QStringLiteral("laterStartUs=%1 earlierStartUs=%2")
                            .arg(laterStartUs)
                            .arg(earlierStartUs)));
}

void TestQtPreviewSink::frameProviderPresentationTimesTrackDeliveryClock() {
    FrameProvider provider;
    QVideoSink sink;
    provider.addVideoSink(&sink);

    FrameHandle first = solidYuv420pHandle(4, 4, 90, 128, 128);
    first.metadata().key.ptsMs = 1000;
    provider.deliverHandle(first);
    const qint64 firstStartUs = sink.videoFrame().startTime();

    QTest::qWait(20);

    FrameHandle second = solidYuv420pHandle(4, 4, 100, 128, 128);
    second.metadata().key.ptsMs = 900;
    provider.deliverHandle(second);
    const QVideoFrame displayed = sink.videoFrame();
    const qint64 secondStartUs = displayed.startTime();

    QVERIFY2(secondStartUs - firstStartUs >= 10000,
             qPrintable(QStringLiteral("firstStartUs=%1 secondStartUs=%2")
                            .arg(firstStartUs)
                            .arg(secondStartUs)));
    QCOMPARE(displayed.endTime(), qint64(-1));
}

void TestQtPreviewSink::frameProviderEmitsFrameChangedForDirectPreview() {
    QVERIFY(FrameProvider::staticMetaObject.indexOfSignal("frameChanged(qulonglong)") >= 0);

    FrameProvider provider;
    QSignalSpy spy(&provider, SIGNAL(frameChanged(qulonglong)));
    QVERIFY(spy.isValid());

    FrameHandle frame = solidYuv420pHandle(4, 4, 120, 128, 128);
    frame.metadata().key.ptsMs = 240;
    const quint64 serial = provider.deliverHandle(frame);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toULongLong(), serial);
}

void TestQtPreviewSink::frameProviderCoalescesQueuedVideoSinkUpdatesToLatestFrame() {
    FrameProvider provider;
    QThread sinkThread;
    auto sink = std::make_unique<QVideoSink>();
    sink->moveToThread(&sinkThread);

    std::atomic<int> visibleFrameChanges{0};
    QObject::connect(
        sink.get(), &QVideoSink::videoFrameChanged, &provider,
        [&visibleFrameChanges](const QVideoFrame&) {
            visibleFrameChanges.fetch_add(1, std::memory_order_relaxed);
        },
        Qt::DirectConnection);

    provider.addVideoSink(sink.get());

    for (int i = 0; i < 10; ++i) {
        FrameHandle frame = solidYuv420pHandle(4, 4, quint8(30 + i * 10), 128, 128);
        frame.metadata().key.ptsMs = i * 40;
        provider.deliverHandle(frame);
    }
    QCOMPARE(visibleFrameChanges.load(std::memory_order_relaxed), 0);

    sinkThread.start();
    QVERIFY(QMetaObject::invokeMethod(sink.get(), []() {}, Qt::BlockingQueuedConnection));

    const int observedFrameChanges = visibleFrameChanges.load(std::memory_order_relaxed);

    QImage visibleImage;
    QVERIFY(QMetaObject::invokeMethod(
        sink.get(), [&sink, &visibleImage]() { visibleImage = sink->videoFrame().toImage(); },
        Qt::BlockingQueuedConnection));

    const QImage latestImage = provider.latestImage();
    QVERIFY(!latestImage.isNull());
    QVERIFY(!visibleImage.isNull());
    QCOMPARE(visibleImage.pixelColor(0, 0), latestImage.pixelColor(0, 0));

    provider.removeVideoSink(sink.get());
    QVideoSink* rawSink = sink.release();
    QVERIFY(QMetaObject::invokeMethod(
        rawSink, [rawSink]() { delete rawSink; }, Qt::BlockingQueuedConnection));
    sinkThread.quit();
    QVERIFY(sinkThread.wait(1000));

    QCOMPARE(observedFrameChanges, 1);
}

void TestQtPreviewSink::frameProviderFlushWaitsForSubmittedSerial() {
    FrameProvider provider;
    QThread sinkThread;
    auto sink = std::make_unique<QVideoSink>();
    sink->moveToThread(&sinkThread);
    sinkThread.start();

    provider.addVideoSink(sink.get());

    FrameHandle frame = solidYuv420pHandle(4, 4, 120, 128, 128);
    frame.metadata().key.ptsMs = 240;
    const quint64 serial = provider.deliverHandle(frame);

    QVERIFY(provider.flushVideoSinks(500, serial));

    QImage visibleImage;
    QVERIFY(QMetaObject::invokeMethod(
        sink.get(), [&sink, &visibleImage]() { visibleImage = sink->videoFrame().toImage(); },
        Qt::BlockingQueuedConnection));

    QVERIFY(!visibleImage.isNull());
    QCOMPARE(visibleImage.pixelColor(0, 0), provider.latestImage().pixelColor(0, 0));

    provider.removeVideoSink(sink.get());
    QVideoSink* rawSink = sink.release();
    QVERIFY(QMetaObject::invokeMethod(
        rawSink, [rawSink]() { delete rawSink; }, Qt::BlockingQueuedConnection));
    sinkThread.quit();
    QVERIFY(sinkThread.wait(1000));
}

void TestQtPreviewSink::frameProviderFlushWaitsForDirectPreviewConsumerSerial() {
    FrameProvider provider;
    QObject consumer;
    provider.addDirectPreviewConsumer(&consumer);

    FrameHandle frame = solidYuv420pHandle(4, 4, 120, 128, 128);
    frame.metadata().key.ptsMs = 240;
    const quint64 serial = provider.deliverHandle(frame);

    std::thread painter([&provider, &consumer, serial]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        provider.markDirectPreviewConsumerPainted(&consumer, serial);
    });

    QElapsedTimer timer;
    timer.start();
    QVERIFY(provider.flushDirectPreviewConsumers(500, serial));
    const qint64 elapsedMs = timer.elapsed();
    painter.join();

    QVERIFY2(elapsedMs >= 10,
             qPrintable(QStringLiteral("flush returned after %1ms").arg(elapsedMs)));
    provider.removeDirectPreviewConsumer(&consumer);
}

void TestQtPreviewSink::outputSinkFlushDoesNotBlockOnDirectPreviewConsumer() {
    OutputFrameCache cache(1, 4, 4);
    FrameHandle source = solidYuv420pHandle(4, 4, 70, 128, 128);
    source.metadata().key.feedIndex = 0;
    source.metadata().key.ptsMs = 100;
    cache.insertVideoFrame(source);

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    OutputBusFrame busFrame = engine.renderFeed(0, 3, state, cache);

    FrameProvider provider;
    QObject consumer;
    provider.addDirectPreviewConsumer(&consumer);

    QtPreviewOutputSink output(&provider);
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    QVERIFY(output.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(output.submit(busFrame));

    QElapsedTimer timer;
    timer.start();
    QVERIFY(output.flush(500));
    const qint64 elapsedMs = timer.elapsed();

    QVERIFY2(elapsedMs < 50,
             qPrintable(QStringLiteral("flush returned after %1ms").arg(elapsedMs)));
    provider.removeDirectPreviewConsumer(&consumer);
}

void TestQtPreviewSink::deliverBusEngineFrameUpdatesProviderLatestImage() {
    OutputFrameCache cache(1, 4, 4);
    FrameHandle source = solidYuv420pHandle(4, 4, 90, 128, 128);
    source.metadata().key.feedIndex = 0;
    source.metadata().key.ptsMs = 100;
    cache.insertVideoFrame(source);

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    state.playing = false;

    OutputBusFrame busFrame = engine.renderFeed(0, 25, state, cache);

    FrameProvider provider;
    QtPreviewSink sink(&provider);
    QVERIFY(sink.deliver(busFrame.video));

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
}

void TestQtPreviewSink::outputSinkEndpointDeliversOnlyWhenStarted() {
    OutputFrameCache cache(1, 4, 4);
    FrameHandle source = solidYuv420pHandle(4, 4, 70, 128, 128);
    source.metadata().key.feedIndex = 0;
    source.metadata().key.ptsMs = 100;
    cache.insertVideoFrame(source);

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    OutputBusFrame busFrame = engine.renderFeed(0, 3, state, cache);

    FrameProvider provider;
    QtPreviewOutputSink sink(&provider);
    QVERIFY(!sink.submit(busFrame));

    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    QVERIFY(sink.start(assignment, FrameRate::fromFraction(25, 1)));
    QObject consumer;
    provider.addDirectPreviewConsumer(&consumer);
    QVERIFY(sink.submit(busFrame));
    QVERIFY(sink.isActive());

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
    provider.removeDirectPreviewConsumer(&consumer);
}

void TestQtPreviewSink::outputSinkIsInactiveUntilProviderHasConsumer() {
    FrameProvider provider;
    QtPreviewOutputSink output(&provider);
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.sourceBus = OutputBusId::pgm();
    assignment.enabled = true;

    QVERIFY(output.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(!output.isActive());

    QObject consumer;
    provider.addDirectPreviewConsumer(&consumer);
    QVERIFY(output.isActive());

    provider.removeDirectPreviewConsumer(&consumer);
    QVERIFY(!output.isActive());

    QVideoSink videoSink;
    provider.addVideoSink(&videoSink);
    QVERIFY(output.isActive());

    provider.removeVideoSink(&videoSink);
    QVERIFY(!output.isActive());
}

void TestQtPreviewSink::outputSinkFlushTimesOutWhenVideoSinkCannotDrain() {
    FrameProvider provider;
    QThread sinkThread;
    auto sink = std::make_unique<QVideoSink>();
    sink->moveToThread(&sinkThread);
    provider.addVideoSink(sink.get());

    OutputFrameCache cache(1, 4, 4);
    FrameHandle source = solidYuv420pHandle(4, 4, 70, 128, 128);
    source.metadata().key.feedIndex = 0;
    source.metadata().key.ptsMs = 100;
    cache.insertVideoFrame(source);

    OutputBusEngine engine(FrameRate::fromFraction(25, 1), 1, 4, 4);
    PlaybackStateSnapshot state;
    state.playheadMs = 100;
    OutputBusFrame busFrame = engine.renderFeed(0, 3, state, cache);

    QtPreviewOutputSink output(&provider);
    OutputTargetAssignment assignment;
    assignment.kind = OutputTargetKind::QtPreview;
    assignment.sourceBus = OutputBusId::feed(0);
    assignment.enabled = true;
    QVERIFY(output.start(assignment, FrameRate::fromFraction(25, 1)));
    QVERIFY(output.submit(busFrame));

    QElapsedTimer timer;
    timer.start();
    const bool flushed = output.flush(50);
    const qint64 elapsedMs = timer.elapsed();

    sinkThread.start();
    QVERIFY(QMetaObject::invokeMethod(sink.get(), []() {}, Qt::BlockingQueuedConnection));
    provider.removeVideoSink(sink.get());
    QVideoSink* rawSink = sink.release();
    QVERIFY(QMetaObject::invokeMethod(
        rawSink, [rawSink]() { delete rawSink; }, Qt::BlockingQueuedConnection));
    sinkThread.quit();
    QVERIFY(sinkThread.wait(1000));

    QVERIFY(!flushed);
    QVERIFY2(elapsedMs >= 40,
             qPrintable(QStringLiteral("flush returned after %1ms").arg(elapsedMs)));
}

void TestQtPreviewSink::qVideoFrameCarriesPresentationTimeFromFrameMetadata() {
    FrameHandle frame = solidYuv420pHandle(4, 4, 80, 128, 128);
    frame.metadata().key.ptsMs = 28066;
    frame.metadata().outputFrameIndex = 3030;

    const QVideoFrame qFrame = QtPreviewSink::toQVideoFrame(frame);

    QVERIFY(qFrame.isValid());
    QCOMPARE(qFrame.startTime(), qint64(28066000));
    QVERIFY2(qFrame.endTime() > qFrame.startTime(),
             qPrintable(QStringLiteral("endTime=%1 startTime=%2")
                            .arg(qFrame.endTime())
                            .arg(qFrame.startTime())));
}

void TestQtPreviewSink::colorMetadataRoundTripsDecodeToSink() {
    FrameHandle handle = solidYuv420pHandle(1920, 1080, 80, 128, 128);
    handle.metadata().color = defaultColorMetadataForHeight(1080);

    const QVideoFrame qFrame = QtPreviewSink::toQVideoFrame(handle);
    QVERIFY(qFrame.isValid());
    QCOMPARE(qFrame.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT709);
    QCOMPARE(qFrame.surfaceFormat().colorRange(), QVideoFrameFormat::ColorRange_Video);
}

void TestQtPreviewSink::taggedBt601FrameMapsToBt601() {
    FrameHandle handle = solidYuv420pHandle(1920, 1080, 80, 128, 128);
    ColorMetadata color;
    color.matrix = ColorMatrix::Bt601;
    color.range = ColorRange::Video;
    handle.metadata().color = color;

    const QVideoFrame qFrame = QtPreviewSink::toQVideoFrame(handle);
    QVERIFY(qFrame.isValid());
    QCOMPARE(qFrame.surfaceFormat().colorSpace(), QVideoFrameFormat::ColorSpace_BT601);
}

void TestQtPreviewSink::defaultTaggingReproducesLegacyHeightHeuristic() {
    FrameHandle tall = solidYuv420pHandle(1280, 720, 80, 128, 128);
    tall.metadata().color = defaultColorMetadataForHeight(720);
    QCOMPARE(QtPreviewSink::toQVideoFrame(tall).surfaceFormat().colorSpace(),
             QVideoFrameFormat::ColorSpace_BT709);

    FrameHandle shortFrame = solidYuv420pHandle(720, 480, 80, 128, 128);
    shortFrame.metadata().color = defaultColorMetadataForHeight(480);
    QCOMPARE(QtPreviewSink::toQVideoFrame(shortFrame).surfaceFormat().colorSpace(),
             QVideoFrameFormat::ColorSpace_BT601);
}

QTEST_MAIN(TestQtPreviewSink)
#include "tst_qtpreviewsink.moc"
