#include <QtTest>

#include "recorder_engine/codec/mediafoundationh264policy.h"

class TestMediaFoundationH264Policy : public QObject {
    Q_OBJECT

private slots:
    void exactKnownDriverIsRejected();
    void differentIdentityIsAllowed();
    void overrideAllowsKnownDriver();
    void selectorSkipsKnownDriverAndChoosesNext();
};

namespace {

MfH264TransformIdentity knownUnstableIdentity() {
    return {QStringLiteral("{60F44560-5A20-4857-BFEF-D29773CB8040}"),
            QStringLiteral("NVIDIA H.264 Encoder MFT"), QStringLiteral("nvEncMFTH264x.dll"),
            QStringLiteral("32.0.15.6094")};
}

} // namespace

void TestMediaFoundationH264Policy::exactKnownDriverIsRejected() {
    QVERIFY(isKnownUnstableMfH264Transform(knownUnstableIdentity()));
    QVERIFY(!allowMfH264Transform(knownUnstableIdentity(), false));
}

void TestMediaFoundationH264Policy::differentIdentityIsAllowed() {
    auto identity = knownUnstableIdentity();

    identity.clsid = QStringLiteral("{6CA50344-051A-4DED-9779-A43305165E35}");
    QVERIFY(allowMfH264Transform(identity, false));

    identity = knownUnstableIdentity();
    identity.moduleFileName = QStringLiteral("another-encoder.dll");
    QVERIFY(allowMfH264Transform(identity, false));

    identity = knownUnstableIdentity();
    identity.moduleVersion = QStringLiteral("32.0.15.6095");
    QVERIFY(allowMfH264Transform(identity, false));
}

void TestMediaFoundationH264Policy::overrideAllowsKnownDriver() {
    QVERIFY(allowMfH264Transform(knownUnstableIdentity(), true));
}

void TestMediaFoundationH264Policy::selectorSkipsKnownDriverAndChoosesNext() {
    const QList<MfH264TransformIdentity> identities = {
        knownUnstableIdentity(),
        {QStringLiteral("{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"),
         QStringLiteral("Stable hardware encoder"), QStringLiteral("stable.dll"),
         QStringLiteral("1.0.0.0")}};

    QCOMPARE(selectMfH264TransformCandidate(identities, false), 1);
    QCOMPARE(selectMfH264TransformCandidate(identities, true), 0);
    QCOMPARE(selectMfH264TransformCandidate({knownUnstableIdentity()}, false), -1);
}

QTEST_GUILESS_MAIN(TestMediaFoundationH264Policy)
#include "tst_mediafoundationh264policy.moc"
