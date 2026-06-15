#include "playback/telemetrytimelinereader.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace {

QString avErrorString(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

} // namespace

bool TelemetryTimelineReader::load(const QString &filePath) {
    m_lastError.clear();
    m_feedIds.clear();
    m_events.clear();

    AVFormatContext *formatContext = nullptr;
    const QByteArray encodedPath = filePath.toUtf8();
    int ret = avformat_open_input(&formatContext, encodedPath.constData(), nullptr, nullptr);
    if (ret < 0) {
        m_lastError = QStringLiteral("Failed to open telemetry file: ") + avErrorString(ret);
        return false;
    }

    ret = avformat_find_stream_info(formatContext, nullptr);
    if (ret < 0) {
        avformat_close_input(&formatContext);
        m_lastError = QStringLiteral("Failed to read telemetry stream info: ") + avErrorString(ret);
        return false;
    }

    QMap<int, QString> streamToFeedId;
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
        AVStream *stream = formatContext->streams[i];
        if (!stream) {
            continue;
        }

        const AVDictionaryEntry *trackType = av_dict_get(stream->metadata, "olr_track_type", nullptr, 0);
        const AVDictionaryEntry *feedIdEntry = av_dict_get(stream->metadata, "olr_feed_id", nullptr, 0);
        if (!trackType || !feedIdEntry) {
            continue;
        }

        const QString trackTypeValue = QString::fromUtf8(trackType->value);
        const QString feedId = QString::fromUtf8(feedIdEntry->value);
        if (trackTypeValue != QStringLiteral("feed_telemetry") || feedId.isEmpty()) {
            continue;
        }

        streamToFeedId.insert(static_cast<int>(i), feedId);
        if (!m_feedIds.contains(feedId)) {
            m_feedIds.append(feedId);
        }
        if (!m_events.contains(feedId)) {
            m_events.insert(feedId, {});
        }
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        avformat_close_input(&formatContext);
        m_lastError = QStringLiteral("Failed to allocate telemetry packet");
        return false;
    }

    while ((ret = av_read_frame(formatContext, packet)) >= 0) {
        const QString feedId = streamToFeedId.value(packet->stream_index);
        if (!feedId.isEmpty() && packet->data && packet->size > 0 && packet->pts != AV_NOPTS_VALUE) {
            const QByteArray payloadBytes(reinterpret_cast<const char *>(packet->data), packet->size);
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(payloadBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                AVStream *stream = formatContext->streams[packet->stream_index];
                Entry entry;
                entry.ptsMs = av_rescale_q(packet->pts, stream->time_base, AVRational{1, 1000});
                entry.payload = document.object();
                m_events[feedId].append(entry);
            }
        }
        av_packet_unref(packet);
    }

    const int readResult = ret;

    av_packet_free(&packet);
    avformat_close_input(&formatContext);

    if (readResult != AVERROR_EOF) {
        m_feedIds.clear();
        m_events.clear();
        m_lastError = QStringLiteral("Failed to read telemetry packet: ") + avErrorString(readResult);
        return false;
    }

    for (auto it = m_events.begin(); it != m_events.end(); ++it) {
        std::stable_sort(it.value().begin(), it.value().end(), [](const Entry &a, const Entry &b) {
            return a.ptsMs < b.ptsMs;
        });
    }

    return true;
}

QVariantMap TelemetryTimelineReader::stateAt(qint64 playheadMs) const {
    QVariantMap state;
    for (auto it = m_events.constBegin(); it != m_events.constEnd(); ++it) {
        const QList<Entry> &entries = it.value();
        const Entry *latest = nullptr;
        for (const Entry &entry : entries) {
            if (entry.ptsMs > playheadMs) {
                break;
            }
            latest = &entry;
        }
        if (latest) {
            state.insert(it.key(), latest->payload.toVariantMap());
        }
    }
    return state;
}
