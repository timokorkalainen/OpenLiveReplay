#include <QByteArray>
#include <QCoreApplication>
#include <QThread>
#include <QUrl>

#include "recorder_engine/ingest/nativesrtingestsession.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <srt/srt.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace {
struct ListenerResult {
    QString streamId;
    QString error;
};

QString srtLastError() {
    return QString::fromUtf8(srt_getlasterror_str());
}

bool setSrtOption(SRTSOCKET socket, SRT_SOCKOPT option, const void* value, int size, QString* error,
                  const QString& name) {
    if (srt_setsockopt(socket, 0, option, value, size) == SRT_ERROR) {
        if (error) *error = QStringLiteral("%1 failed: %2").arg(name, srtLastError());
        return false;
    }
    return true;
}

sockaddr_in loopbackAddress(quint16 port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

QString readAcceptedStreamId(SRTSOCKET accepted) {
    char buffer[512]{};
    int len = int(sizeof(buffer));
    if (srt_getsockopt(accepted, 0, SRTO_STREAMID, buffer, &len) == SRT_ERROR) {
        return QString();
    }
    QByteArray bytes(buffer, qMax(0, len));
    const int nul = bytes.indexOf('\0');
    if (nul >= 0) {
        bytes.truncate(nul);
    }
    return QString::fromUtf8(bytes);
}

void acceptOneStreamId(SRTSOCKET listener, std::atomic<bool>* done, ListenerResult* result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_storage peer{};
        int peerLen = int(sizeof(peer));
        const SRTSOCKET accepted =
            srt_accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (accepted != SRT_INVALID_SOCK) {
            result->streamId = readAcceptedStreamId(accepted);
            if (result->streamId.isEmpty()) {
                result->error =
                    QStringLiteral("accepted socket had no SRTO_STREAMID: %1").arg(srtLastError());
            }
            done->store(true, std::memory_order_release);
            QThread::msleep(1000);
            srt_close(accepted);
            return;
        }

        int osError = 0;
        const int code = srt_getlasterror(&osError);
        Q_UNUSED(osError);
        if (code != SRT_EASYNCRCV && code != SRT_ETIMEOUT) {
            result->error = QStringLiteral("srt_accept failed: %1").arg(srtLastError());
            done->store(true, std::memory_order_release);
            return;
        }
        QThread::msleep(20);
    }
    result->error = QStringLiteral("timed out waiting for SRT caller");
    done->store(true, std::memory_order_release);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const quint16 port = quint16(args.size() > 1 ? args.at(1).toInt() : 23650);
    const QString expectedStreamId = QStringLiteral("#!::r=olr-streamid,m=request");

    if (srt_startup() == SRT_ERROR) {
        fprintf(stderr, "srt_streamid_harness: srt_startup failed: %s\n", srt_getlasterror_str());
        return 2;
    }

    SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        fprintf(stderr, "srt_streamid_harness: listener socket failed: %s\n",
                srt_getlasterror_str());
        srt_cleanup();
        return 2;
    }

    QString setupError;
    const int yes = 1;
    const int timeoutMs = 200;
    const SRT_TRANSTYPE transType = SRTT_LIVE;
    if (!setSrtOption(listener, SRTO_TRANSTYPE, &transType, sizeof(transType), &setupError,
                      QStringLiteral("SRTO_TRANSTYPE")) ||
        !setSrtOption(listener, SRTO_REUSEADDR, &yes, sizeof(yes), &setupError,
                      QStringLiteral("SRTO_REUSEADDR")) ||
        !setSrtOption(listener, SRTO_RCVTIMEO, &timeoutMs, sizeof(timeoutMs), &setupError,
                      QStringLiteral("SRTO_RCVTIMEO"))) {
        fprintf(stderr, "srt_streamid_harness: %s\n", qPrintable(setupError));
        srt_close(listener);
        srt_cleanup();
        return 2;
    }

    sockaddr_in bindAddress = loopbackAddress(port);
    if (srt_bind(listener, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) ==
        SRT_ERROR) {
        fprintf(stderr, "srt_streamid_harness: bind 127.0.0.1:%u failed: %s\n", port,
                srt_getlasterror_str());
        srt_close(listener);
        srt_cleanup();
        return 2;
    }
    if (srt_listen(listener, 1) == SRT_ERROR) {
        fprintf(stderr, "srt_streamid_harness: listen failed: %s\n", srt_getlasterror_str());
        srt_close(listener);
        srt_cleanup();
        return 2;
    }

    std::atomic<bool> done{false};
    ListenerResult result;
    std::thread listenerThread(acceptOneStreamId, listener, &done, &result);

    NativeSrtIngestSession session(0, 320, 240, nullptr);
    IngestCallbacks callbacks;
    callbacks.shouldStop = []() { return false; };
    callbacks.logInfo = [](const QString& message) {
        fprintf(stderr, "%s\n", qPrintable(message));
    };
    const QString encodedStreamId = QString::fromLatin1(QUrl::toPercentEncoding(expectedStreamId));
    const QUrl url(QStringLiteral("srt://127.0.0.1:%1?streamid=%2&transtype=live")
                       .arg(port)
                       .arg(encodedStreamId));
    const bool opened = session.open(url, callbacks);
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < waitDeadline) {
        QThread::msleep(20);
    }

    srt_close(listener);
    if (listenerThread.joinable()) {
        listenerThread.join();
    }
    session.requestStop();
    srt_cleanup();

    if (!opened) {
        fprintf(stderr, "srt_streamid_harness: NativeSrtIngestSession did not connect\n");
        return 1;
    }
    if (!result.error.isEmpty()) {
        fprintf(stderr, "srt_streamid_harness: %s\n", qPrintable(result.error));
        return 1;
    }
    if (result.streamId != expectedStreamId) {
        fprintf(stderr, "srt_streamid_harness: streamid mismatch: got '%s', expected '%s'\n",
                qPrintable(result.streamId), qPrintable(expectedStreamId));
        return 1;
    }

    printf("PASS: native SRT caller streamid reached listener: %s\n", qPrintable(result.streamId));
    return 0;
}
