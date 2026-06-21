#include <QtTest>

#include "playback/frameprovider.h"
#include "playback/output/colormetadatapolicy.h"
#include "playback/output/outputbusengine.h"
#include "playback/output/outputframecache.h"
#include "playback/output/qtpreviewsink.h"

class TestQtPreviewSink : public QObject {
    Q_OBJECT
private slots:
    void deliverMediaFrameUpdatesProviderLatestImage();
    void deliverBusEngineFrameUpdatesProviderLatestImage();
    void outputSinkEndpointDeliversOnlyWhenStarted();
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
    QVERIFY(sink.submit(busFrame));
    QVERIFY(sink.isActive());

    QImage image = provider.latestImage();
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 4);
    QCOMPARE(image.height(), 4);
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
