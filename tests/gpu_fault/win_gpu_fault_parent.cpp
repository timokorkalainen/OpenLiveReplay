#include "tests/gpu_fault/win_gpu_fault_protocol.h"
#include "tests/gpu_fault/win_gpu_fault_safety.h"
#include "tests/gpu_fault/win_gpu_fault_worker_oracle.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QUuid>

#include <windows.h>

namespace {

void report(const QString& message) {
    QTextStream(stderr) << message << Qt::endl;
}

class ChildJob final {
public:
    ~ChildJob() {
        if (m_handle) CloseHandle(m_handle);
    }

    bool create(const QString& name, QString* error) {
        m_handle = CreateJobObjectW(nullptr, reinterpret_cast<const wchar_t*>(name.utf16()));
        if (!m_handle || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (error)
                *error = QStringLiteral("cannot create unique child Job Object (Win32 %1)")
                             .arg(GetLastError());
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_handle, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            if (error)
                *error = QStringLiteral("cannot set Job Object kill-on-close (Win32 %1)")
                             .arg(GetLastError());
            return false;
        }
        return true;
    }

    bool assign(qint64 processId, QString* error) const {
        HANDLE process =
            OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, DWORD(processId));
        if (!process) {
            if (error)
                *error = QStringLiteral("cannot open child for Job Object assignment (Win32 %1)")
                             .arg(GetLastError());
            return false;
        }
        const bool assigned = AssignProcessToJobObject(m_handle, process) != FALSE;
        const DWORD assignmentError = assigned ? ERROR_SUCCESS : GetLastError();
        CloseHandle(process);
        if (!assigned && error)
            *error = QStringLiteral("cannot contain child in Job Object (Win32 %1)")
                         .arg(assignmentError);
        return assigned;
    }

    void terminate() const {
        if (m_handle) TerminateJobObject(m_handle, 0xEE);
    }

private:
    HANDLE m_handle = nullptr;
};

void mergeObject(QJsonObject* target, const QJsonObject& values) {
    for (auto it = values.begin(); it != values.end(); ++it)
        target->insert(it.key(), it.value());
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
    const bool requiresJob =
        mode == QStringLiteral("--trigger-tdr") || mode == QStringLiteral("--verify-job");
    ChildJob job;
    QString jobName;
    if (requiresJob) {
        jobName = QStringLiteral("Local\\OpenLiveReplayGpuFault-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!job.create(jobName, error)) return false;
        environment.insert(QStringLiteral("OLR_GPU_FAULT_JOB_NAME"), jobName);
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
    if (requiresJob && !job.assign(process.processId(), error)) {
        job.terminate();
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        job.terminate();
        process.kill();
        process.waitForFinished(5000);
        const QByteArray stdoutBytes = process.readAllStandardOutput().left(65536);
        const QByteArray stderrBytes = process.readAllStandardError().left(65536);
        *evidence = {{QStringLiteral("mode"), QStringLiteral("child-failure")},
                     {QStringLiteral("childMode"), mode},
                     {QStringLiteral("reason"), QStringLiteral("watchdog-timeout")},
                     {QStringLiteral("stdout"), QString::fromUtf8(stdoutBytes)},
                     {QStringLiteral("stderr"), QString::fromUtf8(stderrBytes)}};
        QJsonObject partial;
        QString parseError;
        if (winGpuFault::decodeAvailable(stdoutBytes, &partial, &parseError))
            evidence->insert(QStringLiteral("destructiveEvidence"), partial);
        *error = QStringLiteral("child watchdog expired in %1").arg(mode);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QByteArray stdoutBytes = process.readAllStandardOutput().left(65536);
        const QByteArray stderrBytes = process.readAllStandardError().left(65536);
        *evidence = {{QStringLiteral("mode"), QStringLiteral("child-failure")},
                     {QStringLiteral("childMode"), mode},
                     {QStringLiteral("reason"), QStringLiteral("abnormal-exit")},
                     {QStringLiteral("exitCode"), process.exitCode()},
                     {QStringLiteral("stdout"), QString::fromUtf8(stdoutBytes)},
                     {QStringLiteral("stderr"), QString::fromUtf8(stderrBytes)}};
        QJsonObject partial;
        QString parseError;
        if (winGpuFault::decodeAvailable(stdoutBytes, &partial, &parseError))
            evidence->insert(QStringLiteral("destructiveEvidence"), partial);
        *error = QStringLiteral("child %1 failed: exit=%2 stderr=%3")
                     .arg(mode)
                     .arg(process.exitCode())
                     .arg(QString::fromUtf8(stderrBytes));
        return false;
    }
    return winGpuFault::decodeMerged(process.readAllStandardOutput(), evidence, error);
}

bool appendEvidence(const QJsonObject& evidence, QString* error) {
    const QString path = qEnvironmentVariable("OLR_GPU_FAULT_EVIDENCE",
                                              QStringLiteral("windows-gpu-fault-evidence.jsonl"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        *error = QStringLiteral("cannot append evidence file %1").arg(path);
        return false;
    }
    const QByteArray record = winGpuFault::encode(evidence);
    return file.write(record) == record.size() && file.flush();
}

int selfTest(const QString& child) {
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
                            {QStringLiteral("waitCount"), 1},
                            {QStringLiteral("waited"), true}};
    if (!winGpuFault::validateFenceEvidence(fence, &error)) return 2;
    QJsonObject badFence = fence;
    badFence.insert(QStringLiteral("signalCount"), 2);
    if (winGpuFault::validateFenceEvidence(badFence, &error)) return 3;

    const QJsonObject tdr{{QStringLiteral("mode"), QStringLiteral("trigger-tdr")},
                          {QStringLiteral("destructiveStarted"), true},
                          {QStringLiteral("jobContained"), true},
                          {QStringLiteral("tdrPolicySafe"), true},
                          {QStringLiteral("dedicatedRunner"), true},
                          {QStringLiteral("removedHresult"), -2005270523},
                          {QStringLiteral("generationBefore"), 7},
                          {QStringLiteral("generationAfter"), 8},
                          {QStringLiteral("realLossToken"), true},
                          {QStringLiteral("tokenlessBeforeRemoval"), true},
                          {QStringLiteral("faultSurfaceBound"), true},
                          {QStringLiteral("tokenGeneration"), 8},
                          {QStringLiteral("staleFrameRejected"), true},
                          {QStringLiteral("signalCount"), 1},
                          {QStringLiteral("pendingInitially"), 1},
                          {QStringLiteral("releasedRetains"), 1},
                          {QStringLiteral("abandonAttempted"), true},
                          {QStringLiteral("retainedSurfaceReleased"), true},
                          {QStringLiteral("deadFenceWaits"), 0},
                          {QStringLiteral("pendingFinally"), 0},
                          {QStringLiteral("workerRecoveryExercised"), true},
                          {QStringLiteral("workerRealLossToken"), true},
                          {QStringLiteral("workerGenerationBefore"), 7},
                          {QStringLiteral("workerGenerationAfter"), 8},
                          {QStringLiteral("workerStaleFrameRejected"), true},
                          {QStringLiteral("workerCacheRecoveredToCpu"), true},
                          {QStringLiteral("workerOutputResumed"), true},
                          {QStringLiteral("workerCoherentState"), true},
                          {QStringLiteral("recoveryComplete"), true},
                          {QStringLiteral("workerAbandonedRetains"), 1},
                          {QStringLiteral("workerRetainedSurfaceReleased"), true},
                          {QStringLiteral("workerRecoveryMs"), 250}};
    if (!winGpuFault::validateTdrEvidence(tdr, &error)) return 4;
    QJsonObject badTdr = tdr;
    badTdr.insert(QStringLiteral("removedHresult"), 0);
    if (winGpuFault::validateTdrEvidence(badTdr, &error)) return 5;

    winGpuFault::TdrPolicySnapshot policy;
    if (!winGpuFault::validateTdrPolicy(policy, &error)) return 6;
    policy.level = 1;
    if (winGpuFault::validateTdrPolicy(policy, &error)) return 7;
    policy = {};
    policy.delaySeconds = 11;
    if (winGpuFault::validateTdrPolicy(policy, &error)) return 8;
    policy = {};
    policy.debugMode = 1;
    if (winGpuFault::validateTdrPolicy(policy, &error)) return 9;
    policy = {};
    policy.limitCount = 1;
    if (winGpuFault::validateTdrPolicy(policy, &error)) return 10;
    policy = {};
    policy.testModePresent = true;
    if (winGpuFault::validateTdrPolicy(policy, &error)) return 11;

    QJsonObject jobEvidence;
    if (child.isEmpty() ||
        !runChild(child, QStringLiteral("--verify-job"), 10000, &jobEvidence, &error) ||
        !jobEvidence.value(QStringLiteral("jobContained")).toBool())
        return 12;

    QJsonObject mergedEvidence;
    const QByteArray checkpoints =
        winGpuFault::encode({{QStringLiteral("phase"), QStringLiteral("removal")},
                             {QStringLiteral("generationAfter"), 8}}) +
        winGpuFault::encode({{QStringLiteral("phase"), QStringLiteral("recovery")},
                             {QStringLiteral("recoveryComplete"), true}});
    if (!winGpuFault::decodeMerged(checkpoints, &mergedEvidence, &error) ||
        mergedEvidence.value(QStringLiteral("phase")) != QStringLiteral("recovery") ||
        mergedEvidence.value(QStringLiteral("generationAfter")).toInt() != 8 ||
        !mergedEvidence.value(QStringLiteral("recoveryComplete")).toBool())
        return 13;
    const QByteArray truncatedCheckpoints = checkpoints + QByteArrayLiteral("{\"phase\":");
    if (!winGpuFault::decodeAvailable(truncatedCheckpoints, &mergedEvidence, &error) ||
        mergedEvidence.value(QStringLiteral("phase")) != QStringLiteral("recovery") ||
        !mergedEvidence.value(QStringLiteral("recoveryComplete")).toBool())
        return 14;

    QTextStream(stdout) << "GPU fault protocol self-test passed" << Qt::endl;
    return 0;
}

int runLane(const QString& child) {
    if (qEnvironmentVariableIntValue("OLR_GPU_FAULT_LANE") != 1) {
        QTextStream(stdout) << "SKIP: OLR_GPU_FAULT_LANE=1 is required" << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }
    if (qEnvironmentVariableIntValue("OLR_GPU_FAULT_DEDICATED_RUNNER") != 1) {
        QTextStream(stdout) << "SKIP: OLR_GPU_FAULT_DEDICATED_RUNNER=1 is required" << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }

    QString error;
    winGpuFault::TdrPolicySnapshot policy;
    const bool policySafe = winGpuFault::readAndValidateTdrPolicy(&policy, &error);
    QJsonObject safety{{QStringLiteral("mode"), QStringLiteral("host-safety")},
                       {QStringLiteral("dedicatedRunner"), true},
                       {QStringLiteral("tdrPolicySafe"), policySafe},
                       {QStringLiteral("reason"), error}};
    mergeObject(&safety, winGpuFault::tdrPolicyEvidence(policy));
    if (!appendEvidence(safety, &error)) {
        report(error);
        return 9;
    }
    if (!policySafe) {
        QTextStream(stdout) << "SKIP: unsafe TDR policy: " << safety.value("reason").toString()
                            << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }

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
                  &error, &capability)) {
        if (!tdr.isEmpty()) {
            QString appendError;
            if (!appendEvidence(tdr, &appendError)) report(appendError);
        }
        report(error);
        return 13;
    }
    // Preserve destructive evidence even when an oracle fails. Losing the raw
    // observation makes a hardware fault lane impossible to diagnose safely.
    if (!appendEvidence(tdr, &error)) {
        report(error);
        return 14;
    }
    if (!winGpuFault::validateTdrEvidence(tdr, &error)) {
        report(error + QStringLiteral(": ") +
               QString::fromUtf8(winGpuFault::encode(tdr)).trimmed());
        return 14;
    }

    QTextStream(stdout) << "Windows real GPU fault lane passed" << Qt::endl;
    return 0;
}

int probeWorkerOracle(const QString& child) {
    QString error;
    QJsonObject capability;
    if (child.isEmpty() ||
        !runChild(child, QStringLiteral("--capability"), 15000, &capability, &error)) {
        report(error);
        return 20;
    }
    if (!capability.value(QStringLiteral("supported")).toBool()) {
        QTextStream(stdout) << "SKIP: " << capability.value(QStringLiteral("reason")).toString()
                            << Qt::endl;
        return winGpuFault::kSkipExitCode;
    }
    WinGpuFaultWorkerOracle oracle;
    if (!oracle.prepare(capability, &error)) {
        report(error);
        return 21;
    }
    QTextStream(stdout) << "PlaybackWorker fault oracle prepared" << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--self-test"))) {
        const int childIndex = args.indexOf(QStringLiteral("--self-test")) + 1;
        return selfTest(childIndex > 0 && childIndex < args.size() ? args.at(childIndex)
                                                                   : QString{});
    }
    if (args.contains(QStringLiteral("--probe-worker"))) {
        const int childIndex = args.indexOf(QStringLiteral("--probe-worker")) + 1;
        return probeWorkerOracle(childIndex > 0 && childIndex < args.size() ? args.at(childIndex)
                                                                            : QString{});
    }
    const int childIndex = args.indexOf(QStringLiteral("--run")) + 1;
    if (childIndex <= 0 || childIndex >= args.size()) return 64;
    return runLane(args.at(childIndex));
}
