#include <QtTest>

#include "playback/output/colormetadatapolicy.h"
#include "playback/playbackworker.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

class TestColorMetadataPlumb : public QObject {
    Q_OBJECT
private slots:
    void untaggedAvFrameResolvesToHeightDefault();
    void taggedAvFrameIsHonoured();

private:
    static AVFrame* makeFrame(int width, int height);
};

AVFrame* TestColorMetadataPlumb::makeFrame(int width, int height) {
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    return frame;
}

void TestColorMetadataPlumb::untaggedAvFrameResolvesToHeightDefault() {
    AVFrame* frame = makeFrame(1920, 1080);
    const ColorMetadata metadata = colorMetadataForAvFrame(frame);
    av_frame_free(&frame);
    QVERIFY(metadata == defaultColorMetadataForHeight(1080));
}

void TestColorMetadataPlumb::taggedAvFrameIsHonoured() {
    AVFrame* frame = makeFrame(720, 480);
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->color_range = AVCOL_RANGE_JPEG;

    const ColorMetadata metadata = colorMetadataForAvFrame(frame);
    av_frame_free(&frame);

    QCOMPARE(int(metadata.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(metadata.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(metadata.range), int(ColorRange::Full));
}

QTEST_GUILESS_MAIN(TestColorMetadataPlumb)
#include "tst_colormetadataplumb.moc"
