#include "tests/gpu_fault/win_gpu_fault_protocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QUuid>

namespace {

void report(const QString& message) {
    QTextStream(stderr) << message << Qt::endl;
}

bool runChild(const QString& child, const QString& mode, int timeoutMs, QJsonObject* evidence,
              QString* error, const QJsonObject* admission = nullptr) {
    QProcess process;
    process.setProgram(child);
    QStringList arguments{mode};
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("OLR_GPU_FAULT_EXPECTED_LUID_HIGH"));
    environment.remove(QStringLiteral("OLR_GPU_FAULT_EXPECTED_LUID_LOW"));
    if (admission) {
        environment.insert(QStringLiteral("OLR_GPU_FAULT_EXPECTED_LUID_HIGH"),
                           QString::number(admission->value(QStringLiteral("luidHigh")).toInt()));
        environment.insert(
            QStringLiteral("OLR_GPU_FAULT_EXPECTED_LUID_LOW"),
            QString::number(quint64(admission->value(QStringLiteral("luidLow")).toDouble())));
    }
    if (mode == QStringLiteral("--trigger-tdr")) {
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        environment.insert(QStringLiteral("OLR_GPU_FAULT_CHILD_TOKEN"), token);
        arguments.append(QStringLiteral("--parent-token=") + token);
    }
    process.setProcessEnvironment(environment);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        *error = QStringLiteral("child failed to start: %1").arg(process.errorString());
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        *error = QStringLiteral("child watchdog expired in %1").arg(mode);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QStringLiteral("child %1 failed: exit=%2 stderr=%3")
                     .arg(mode)
                     .arg(process.exitCode())
                     .arg(QString::fromUtf8(process.readAllStandardError()));
        return false;
    }
    return winGpuFault::decodeSingle(process.readAllStandardOutput(), evidence, error);
}

bool appendEvidence(const QJsonObject& evidence, QString* error) {
    const QString path = qEnvironmentVariable("OLR_GPU_FAULT_EVIDENCE",
                                              QStringLiteral("windows-gpu-fault-evidence.jsonl"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        *error = QStringLiteral("cannot append evidence file %1").arg(path);
        return false;
    }
    return file.write(winGpuFault::encode(evidence)) > 0;
}

int selfTest() {
    QString error;
    QJsonObject decoded;
    if (winGpuFault::decodeSingle("not-json", &decoded, &error)) return 1;

    const QJsonObject fence{{QStringLiteral("mode"), QStringLiteral("probe-fence")},
                            {QStringLiteral("signalCount"), 1},
                            {QStringLiteral("signalValue"), 5},
                            {QStringLiteral("initialCompleted"), 4},
                            {QStringLiteral("pendingInitially"), 1},
                            {QStringLiteral("finalCompleted"), 5},
                            {QStringLiteral("pendingFinally"), 0},
                            {QStringLiteral("waitCount"), 1}};
    if (!winGpuFault::validateFenceEvidence(fence, &error)) return 2;
    QJsonObject badFence = fence;
    badFence.insert(QStringLiteral("signalCount"), 2);
    if (winGpuFault::validateFenceEvidence(badFence, &error)) return 3;

    const QJsonObject tdr{{QStringLiteral("mode"), QStringLiteral("trigger-tdr")},
                          {QStringLiteral("destructiveStarted"), true},
                          {QStringLiteral("removedHresult"), -2005270523},
                          {QStringLiteral("generationBefore"), 7},
                          {QStringLiteral("generationAfter"), 8},
                          {QStringLiteral("realLossToken"), true},
                          {QStringLiteral("tokenGeneration"), 8},
                          {QStringLiteral("staleFrameRejected"), true},
                          {QStringLiteral("signalCount"), 1},
                          {QStringLiteral("pendingInitially"), 1},
                          {QStringLiteral("releasedRetains"), 1},
                          {QStringLiteral("deadFenceWaits"), 0},
                          {QStringLiteral("pendingFinally"), 0}};
    if (!winGpuFault::validateTdrEvidence(tdr, &error)) return 4;
    QJsonObject badTdr = tdr;
    badTdr.insert(QStringLiteral("removedHresult"), 0);
    if (winGpuFault::validateTdrEvidence(badTdr, &error)) return 5;

    QTextStream(stdout) << "GPU fault protocol self-test passed" << Qt::endl;
    return 0;
}

int runLane(const QString& child) {
    if (qEnvironmentVariableIntValue("OLR_GPU_FAULT_LANE") != 1) {
        QTextStream(stdout) << "SKIP: OLR_GPU_FAULT_LANE=1 is required" << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }

    QString error;
    QJsonObject capability;
    if (!runChild(child, QStringLiteral("--capability"), 15000, &capability, &error)) {
        report(error);
        return 10;
    }
    if (!appendEvidence(capability, &error)) {
        report(error);
        return 11;
    }
    if (!capability.value(QStringLiteral("supported")).toBool() ||
        !capability.value(QStringLiteral("destructiveAllowed")).toBool()) {
        QTextStream(stdout) << "SKIP: "
                            << capability.value(QStringLiteral("reason"))
                                   .toString(QStringLiteral("unsupported hardware/session"))
                            << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }

    QJsonObject fence;
    if (!runChild(child, QStringLiteral("--probe-fence"), 30000, &fence, &error, &capability) ||
        !winGpuFault::validateFenceEvidence(fence, &error) || !appendEvidence(fence, &error)) {
        report(error);
        return 12;
    }

    QJsonObject tdr;
    if (!runChild(child, QStringLiteral("--trigger-tdr"), winGpuFault::kChildTimeoutMs, &tdr,
                  &error, &capability) ||
        !winGpuFault::validateTdrEvidence(tdr, &error) || !appendEvidence(tdr, &error)) {
        report(error);
        return 13;
    }

    QTextStream(stdout) << "Windows real GPU fault lane passed" << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--self-test"))) return selfTest();
    const int childIndex = args.indexOf(QStringLiteral("--run")) + 1;
    if (childIndex <= 0 || childIndex >= args.size()) return 64;
    return runLane(args.at(childIndex));
}
