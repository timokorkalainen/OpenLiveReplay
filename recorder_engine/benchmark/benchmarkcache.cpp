#include "recorder_engine/benchmark/benchmarkcache.h"
#include "recorder_engine/codec/videocodecchoice.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSysInfo>

QString benchmarkDeviceLabel()
{
    return QSysInfo::prettyProductName() + QStringLiteral(" ") +
           QSysInfo::currentCpuArchitecture();
}

bool saveBenchmarkResult(const QString& path, const CodecBenchmarkResult& result)
{
    QJsonObject root;
    root[QStringLiteral("h264Available")]  = result.h264Available;
    root[QStringLiteral("h264SafeFeeds")]  = result.h264SafeFeeds;
    root[QStringLiteral("mpeg2SafeFeeds")] = result.mpeg2SafeFeeds;
    root[QStringLiteral("h264EncodeMs")]   = result.h264EncodeMs;
    root[QStringLiteral("h264DecodeMs")]   = result.h264DecodeMs;
    root[QStringLiteral("mpeg2EncodeMs")]  = result.mpeg2EncodeMs;
    root[QStringLiteral("mpeg2DecodeMs")]  = result.mpeg2DecodeMs;
    root[QStringLiteral("recommended")]    = videoCodecToString(result.recommended);
    root[QStringLiteral("deviceLabel")]    = result.deviceLabel;
    root[QStringLiteral("resolution")]     = result.resolution;
    root[QStringLiteral("timestamp")]      = result.timestamp;
    root[QStringLiteral("ceilingReached")] = result.ceilingReached;

    QJsonDocument doc(root);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "benchmarkcache: failed to open for writing:" << path;
        return false;
    }
    const QByteArray bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) < bytes.size()) {
        qWarning() << "benchmarkcache: failed to write:" << path;
        file.close();
        return false;
    }
    file.close();
    return true;
}

bool loadBenchmarkResult(const QString& path, CodecBenchmarkResult& out)
{
    QFile file(path);
    if (!file.exists()) {
        qDebug() << "benchmarkcache: file does not exist:" << path;
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "benchmarkcache: failed to open for reading:" << path;
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (doc.isNull()) {
        qDebug() << "benchmarkcache: JSON parse error:" << error.errorString();
        return false;
    }
    if (!doc.isObject()) {
        qDebug() << "benchmarkcache: JSON document is not an object";
        return false;
    }

    const QJsonObject root = doc.object();
    out.h264Available  = root[QStringLiteral("h264Available")].toBool();
    out.h264SafeFeeds  = root[QStringLiteral("h264SafeFeeds")].toInt(-1);
    out.mpeg2SafeFeeds = root[QStringLiteral("mpeg2SafeFeeds")].toInt(-1);
    out.h264EncodeMs   = root[QStringLiteral("h264EncodeMs")].toDouble();
    out.h264DecodeMs   = root[QStringLiteral("h264DecodeMs")].toDouble();
    out.mpeg2EncodeMs  = root[QStringLiteral("mpeg2EncodeMs")].toDouble();
    out.mpeg2DecodeMs  = root[QStringLiteral("mpeg2DecodeMs")].toDouble();
    out.recommended    = videoCodecFromString(root[QStringLiteral("recommended")].toString(),
                                              VideoCodecChoice::Mpeg2Software);
    out.deviceLabel    = root[QStringLiteral("deviceLabel")].toString();
    out.resolution     = root[QStringLiteral("resolution")].toString();
    out.timestamp      = root[QStringLiteral("timestamp")].toString();
    out.ceilingReached = root[QStringLiteral("ceilingReached")].toBool();
    return true;
}

bool benchmarkResultMatches(const CodecBenchmarkResult& cached,
                            const QString& deviceLabel,
                            const QString& resolution)
{
    return cached.deviceLabel == deviceLabel && cached.resolution == resolution;
}
