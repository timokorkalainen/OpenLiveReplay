#include <QtTest>

#ifdef __APPLE__

#include "recorder_engine/ingest/colortags_apple.h"

#include <CoreMedia/CoreMedia.h>

class TestColorTagsApple : public QObject {
    Q_OBJECT
private slots:
    void bt709FullRangeAttachmentsAreRead();
    void nullFormatIsAbsent();

private:
    static CMVideoFormatDescriptionRef makeFmt(CFStringRef matrix, CFStringRef primaries,
                                               CFStringRef transfer, bool fullRange);
};

CMVideoFormatDescriptionRef TestColorTagsApple::makeFmt(CFStringRef matrix, CFStringRef primaries,
                                                        CFStringRef transfer, bool fullRange) {
    const void* keys[] = {kCMFormatDescriptionExtension_YCbCrMatrix,
                          kCMFormatDescriptionExtension_ColorPrimaries,
                          kCMFormatDescriptionExtension_TransferFunction,
                          kCMFormatDescriptionExtension_FullRangeVideo};
    const void* values[] = {matrix, primaries, transfer,
                            fullRange ? kCFBooleanTrue : kCFBooleanFalse};
    CFDictionaryRef extensions =
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CMVideoFormatDescriptionRef fmt = nullptr;
    CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_H264, 1920, 1080,
                                   extensions, &fmt);
    if (extensions) CFRelease(extensions);
    return fmt;
}

void TestColorTagsApple::bt709FullRangeAttachmentsAreRead() {
    CMVideoFormatDescriptionRef fmt = makeFmt(kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2,
                                              kCMFormatDescriptionColorPrimaries_ITU_R_709_2,
                                              kCMFormatDescriptionTransferFunction_ITU_R_709_2,
                                              true);
    QVERIFY(fmt != nullptr);
    const VuiColorInfo info = colorVuiFromFormatDescription(fmt);
    CFRelease(fmt);

    QVERIFY(info.present);
    QCOMPARE(int(info.matrix), int(ColorMatrix::Bt709));
    QCOMPARE(int(info.primaries), int(ColorPrimaries::Bt709));
    QCOMPARE(int(info.transfer), int(ColorTransfer::Bt709));
    QCOMPARE(int(info.range), int(ColorRange::Full));
}

void TestColorTagsApple::nullFormatIsAbsent() {
    const VuiColorInfo info = colorVuiFromFormatDescription(nullptr);
    QVERIFY(!info.present);
}

QTEST_GUILESS_MAIN(TestColorTagsApple)
#include "tst_colortags_apple.moc"

#endif
