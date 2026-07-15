#include <QtTest>

#include "recorder_engine/ingest/decodedframeevidencequeue.h"

#include <type_traits>

extern "C" {
#include <libavutil/avutil.h>
}

class TestDecodedFrameEvidenceQueue : public QObject {
    Q_OBJECT

private slots:
    void multipleNoOutputSubmissionsBindByOutputPts();
    void carrierIdentityAndGenerationStayDistinct();
    void reorderedOutputsMatchTheirOwnInputs();
    void duplicatePtsAreConsumedFifo();
    void invalidAndMissingPtsDoNotConsumeNewerEvidence();
    void oldestEntriesArePrunedAtTheBound();
    void clearDropsEveryPendingSubmission();
};

namespace {

DecodedFrameEvidence evidence(qint64 pts90k, int64_t sourcePtsMs, int64_t timecode100ns,
                              uint64_t generation) {
    TimecodeEvidence timing;
    timing.sourceGeneration = generation;
    return DecodedFrameEvidence{pts90k,
                                sourcePtsMs,
                                timecode100ns,
                                timing,
                                DecodedFrameEvidence::CarrierSessionIdentity{},
                                DecodedFrameEvidence::CarrierGeneration{generation}};
}

static_assert(!std::is_constructible_v<DecodedFrameEvidence, qint64, int64_t, int64_t,
                                       std::optional<TimecodeEvidence>, uint64_t>);

} // namespace

void TestDecodedFrameEvidenceQueue::multipleNoOutputSubmissionsBindByOutputPts() {
    DecodedFrameEvidenceQueue queue(8);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(93'000, 1033, 20, 2));
    queue.enqueue(evidence(96'000, 1067, 30, 3));

    const auto output = queue.takeForOutputPts(90'000);
    QVERIFY(output.has_value());
    QCOMPARE(output->sourcePtsMs, int64_t(1000));
    QCOMPARE(output->sourceTimecode100ns, int64_t(10));
    QVERIFY(output->timecodeEvidence.has_value());
    QCOMPARE(output->timecodeEvidence->sourceGeneration, uint64_t(1));
    QCOMPARE(output->carrierGeneration, uint64_t(1));
    QCOMPARE(queue.size(), qsizetype(2));
}

void TestDecodedFrameEvidenceQueue::carrierIdentityAndGenerationStayDistinct() {
    const DecodedFrameEvidence value{90'000,
                                     1000,
                                     10,
                                     std::nullopt,
                                     DecodedFrameEvidence::CarrierSessionIdentity{7},
                                     DecodedFrameEvidence::CarrierGeneration{11}};

    QCOMPARE(value.carrierSessionIdentity, uint64_t(7));
    QCOMPARE(value.carrierGeneration, uint64_t(11));
}

void TestDecodedFrameEvidenceQueue::reorderedOutputsMatchTheirOwnInputs() {
    DecodedFrameEvidenceQueue queue(8);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(93'000, 1033, 20, 2));

    const auto second = queue.takeForOutputPts(93'000);
    const auto first = queue.takeForOutputPts(90'000);
    QVERIFY(second.has_value());
    QVERIFY(first.has_value());
    QCOMPARE(second->sourcePtsMs, int64_t(1033));
    QCOMPARE(first->sourcePtsMs, int64_t(1000));
}

void TestDecodedFrameEvidenceQueue::duplicatePtsAreConsumedFifo() {
    DecodedFrameEvidenceQueue queue(8);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(90'000, 1001, 20, 2));

    const auto first = queue.takeForOutputPts(90'000);
    const auto second = queue.takeForOutputPts(90'000);
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QCOMPARE(first->sourcePtsMs, int64_t(1000));
    QCOMPARE(second->sourcePtsMs, int64_t(1001));
}

void TestDecodedFrameEvidenceQueue::invalidAndMissingPtsDoNotConsumeNewerEvidence() {
    DecodedFrameEvidenceQueue queue(8);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(93'000, 1033, 20, 2));

    QVERIFY(!queue.takeForOutputPts(AV_NOPTS_VALUE).has_value());
    QVERIFY(!queue.takeForOutputPts(91'000).has_value());
    QCOMPARE(queue.size(), qsizetype(2));

    const auto first = queue.takeForOutputPts(90'000);
    QVERIFY(first.has_value());
    QCOMPARE(first->sourcePtsMs, int64_t(1000));
}

void TestDecodedFrameEvidenceQueue::oldestEntriesArePrunedAtTheBound() {
    DecodedFrameEvidenceQueue queue(2);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(93'000, 1033, 20, 2));
    queue.enqueue(evidence(96'000, 1067, 30, 3));

    QCOMPARE(queue.size(), qsizetype(2));
    QVERIFY(!queue.takeForOutputPts(90'000).has_value());
    QVERIFY(queue.takeForOutputPts(93'000).has_value());
    QVERIFY(queue.takeForOutputPts(96'000).has_value());
}

void TestDecodedFrameEvidenceQueue::clearDropsEveryPendingSubmission() {
    DecodedFrameEvidenceQueue queue(8);
    queue.enqueue(evidence(90'000, 1000, 10, 1));
    queue.enqueue(evidence(93'000, 1033, 20, 2));
    queue.clear();

    QCOMPARE(queue.size(), qsizetype(0));
    QVERIFY(!queue.takeForOutputPts(90'000).has_value());
    QVERIFY(!queue.takeForOutputPts(93'000).has_value());
}

QTEST_GUILESS_MAIN(TestDecodedFrameEvidenceQueue)
#include "tst_decodedframeevidencequeue.moc"
