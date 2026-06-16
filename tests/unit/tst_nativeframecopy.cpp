#include <QtTest>

#include "recorder_engine/ingest/nativeframecopy.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class TestNativeFrameCopy : public QObject {
    Q_OBJECT
private slots:
    void nv12CopiesToYuv420pWithStride();
    void rejectsInvalidInputs_data();
    void rejectsInvalidInputs();
};

void TestNativeFrameCopy::nv12CopiesToYuv420pWithStride() {
    const int width = 4;
    const int height = 4;
    const int yStride = 6;
    const int uvStride = 6;

    QByteArray y(yStride * height, char(0));
    QByteArray uv(uvStride * (height / 2), char(0));

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            y[row * yStride + col] = char(10 + row * width + col);
        }
    }
    for (int row = 0; row < height / 2; ++row) {
        for (int col = 0; col < width / 2; ++col) {
            uv[row * uvStride + col * 2] = char(80 + row * 2 + col);
            uv[row * uvStride + col * 2 + 1] = char(120 + row * 2 + col);
        }
    }

    AVFrame* frame = nativeCopyNv12ToYuv420p(
        reinterpret_cast<const uint8_t*>(y.constData()), yStride,
        reinterpret_cast<const uint8_t*>(uv.constData()), uvStride,
        width, height);

    QVERIFY(frame);
    QCOMPARE(frame->format, int(AV_PIX_FMT_YUV420P));
    QCOMPARE(frame->width, width);
    QCOMPARE(frame->height, height);
    QCOMPARE(int(frame->data[0][0]), 10);
    QCOMPARE(int(frame->data[0][3]), 13);
    QCOMPARE(int(frame->data[0][frame->linesize[0] + 0]), 14);
    QCOMPARE(int(frame->data[1][0]), 80);
    QCOMPARE(int(frame->data[1][1]), 81);
    QCOMPARE(int(frame->data[2][0]), 120);
    QCOMPARE(int(frame->data[2][1]), 121);

    av_frame_free(&frame);
}

void TestNativeFrameCopy::rejectsInvalidInputs_data() {
    QTest::addColumn<bool>("nullYPlane");
    QTest::addColumn<bool>("nullUvPlane");
    QTest::addColumn<int>("yStride");
    QTest::addColumn<int>("uvStride");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");

    QTest::newRow("null yPlane") << true << false << 4 << 4 << 4 << 4;
    QTest::newRow("null uvPlane") << false << true << 4 << 4 << 4 << 4;
    QTest::newRow("zero width") << false << false << 4 << 4 << 0 << 4;
    QTest::newRow("zero height") << false << false << 4 << 4 << 4 << 0;
    QTest::newRow("negative width") << false << false << 4 << 4 << -4 << 4;
    QTest::newRow("negative height") << false << false << 4 << 4 << 4 << -4;
    QTest::newRow("odd width") << false << false << 5 << 5 << 5 << 4;
    QTest::newRow("odd height") << false << false << 4 << 4 << 4 << 5;
    QTest::newRow("short y stride") << false << false << 3 << 4 << 4 << 4;
    QTest::newRow("short uv stride") << false << false << 4 << 3 << 4 << 4;
    QTest::newRow("zero y stride") << false << false << 0 << 4 << 4 << 4;
    QTest::newRow("zero uv stride") << false << false << 4 << 0 << 4 << 4;
    QTest::newRow("negative y stride") << false << false << -1 << 4 << 4 << 4;
    QTest::newRow("negative uv stride") << false << false << 4 << -1 << 4 << 4;
}

void TestNativeFrameCopy::rejectsInvalidInputs() {
    QFETCH(bool, nullYPlane);
    QFETCH(bool, nullUvPlane);
    QFETCH(int, yStride);
    QFETCH(int, uvStride);
    QFETCH(int, width);
    QFETCH(int, height);

    const QByteArray y(32, char(0));
    const QByteArray uv(32, char(0));
    const uint8_t* yPlane = nullYPlane ? nullptr : reinterpret_cast<const uint8_t*>(y.constData());
    const uint8_t* uvPlane = nullUvPlane ? nullptr : reinterpret_cast<const uint8_t*>(uv.constData());

    AVFrame* frame = nativeCopyNv12ToYuv420p(yPlane, yStride, uvPlane, uvStride, width, height);

    QVERIFY(!frame);
}

QTEST_GUILESS_MAIN(TestNativeFrameCopy)
#include "tst_nativeframecopy.moc"
