#include "nativesrtaddress.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QUrl>
#include <QWaitCondition>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstring>
#include <memory>
#include <thread>
#include <utility>

namespace {
#ifdef _WIN32
class ScopedWinsockStartup {
public:
    ScopedWinsockStartup() {
        WSADATA data;
        m_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~ScopedWinsockStartup() {
        if (m_ok) {
            WSACleanup();
        }
    }
    bool ok() const { return m_ok; }

private:
    bool m_ok = false;
};
#endif

struct ResolveState {
    QMutex mutex;
    QWaitCondition finishedCondition;
    QList<NativeSrtSockaddr> addresses;
    bool finished = false;
};
} // namespace

sockaddr* NativeSrtSockaddr::sockaddrPtr() {
    return reinterpret_cast<sockaddr*>(storage);
}

const sockaddr* NativeSrtSockaddr::sockaddrPtr() const {
    return reinterpret_cast<const sockaddr*>(storage);
}

bool nativeSrtIsNumericIpv4Host(const QString& host) {
    if (host.isEmpty()) {
        return false;
    }

    sockaddr_in address {};
    return inet_pton(AF_INET, host.toUtf8().constData(), &address.sin_addr) == 1;
}

bool nativeSrtMakeIpv4Sockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address) {
    if (!address || host.isEmpty()) {
        return false;
    }

    sockaddr_in ipv4 {};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);
    if (inet_pton(AF_INET, host.toUtf8().constData(), &ipv4.sin_addr) != 1) {
        return false;
    }
    if (sizeof(ipv4) > sizeof(address->storage)) {
        return false;
    }

    memset(address->storage, 0, sizeof(address->storage));
    memcpy(address->storage, &ipv4, sizeof(ipv4));
    address->size = int(sizeof(ipv4));
    return true;
}

QList<NativeSrtSockaddr> nativeSrtResolveSockaddrs(const QString& host, quint16 port) {
    QList<NativeSrtSockaddr> addresses;
    if (host.isEmpty()) {
        return addresses;
    }
#ifdef _WIN32
    ScopedWinsockStartup winsock;
    if (!winsock.ok()) {
        return addresses;
    }
#endif

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    const QByteArray hostBytes = QUrl::toAce(host);
    const QByteArray portBytes = QByteArray::number(port);
    addrinfo* results = nullptr;
    if (getaddrinfo(hostBytes.constData(), portBytes.constData(), &hints, &results) != 0) {
        return addresses;
    }

    for (const addrinfo* current = results; current; current = current->ai_next) {
        if (!current->ai_addr || current->ai_addrlen == 0 ||
            size_t(current->ai_addrlen) > sizeof(NativeSrtSockaddr::storage)) {
            continue;
        }
        if (current->ai_family != AF_INET && current->ai_family != AF_INET6) {
            continue;
        }

        NativeSrtSockaddr address;
        memset(address.storage, 0, sizeof(address.storage));
        memcpy(address.storage, current->ai_addr, size_t(current->ai_addrlen));
        address.size = int(current->ai_addrlen);
        addresses.append(address);
    }

    freeaddrinfo(results);
    return addresses;
}

bool nativeSrtResolveSockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address) {
    if (!address) {
        return false;
    }

    const QList<NativeSrtSockaddr> addresses = nativeSrtResolveSockaddrs(host, port);
    if (addresses.isEmpty()) {
        return false;
    }
    *address = addresses.constFirst();
    return true;
}

QList<NativeSrtSockaddr>
nativeSrtResolveSockaddrsWithTimeout(const NativeSrtAddressResolver& resolver, int timeoutMs,
                                     const NativeSrtShouldStop& shouldStop) {
    if (!resolver) {
        return {};
    }

    const auto state = std::make_shared<ResolveState>();
    std::thread([state, resolver]() {
        QList<NativeSrtSockaddr> resolved;
        try {
            resolved = resolver();
        } catch (...) {
            resolved.clear();
        }

        QMutexLocker locker(&state->mutex);
        state->addresses = std::move(resolved);
        state->finished = true;
        state->finishedCondition.wakeAll();
    }).detach();

    QElapsedTimer elapsed;
    elapsed.start();
    QMutexLocker locker(&state->mutex);
    while (!state->finished) {
        if (shouldStop && shouldStop()) {
            return {};
        }
        if (timeoutMs >= 0 && elapsed.elapsed() >= timeoutMs) {
            return {};
        }

        const int waitMs =
            timeoutMs < 0 ? 50 : qMax(1, qMin<qint64>(50, timeoutMs - elapsed.elapsed()));
        state->finishedCondition.wait(&state->mutex, waitMs);
    }

    return state->addresses;
}

QList<NativeSrtSockaddr>
nativeSrtResolveSockaddrsWithTimeout(const QString& host, quint16 port, int timeoutMs,
                                     const NativeSrtShouldStop& shouldStop) {
    return nativeSrtResolveSockaddrsWithTimeout(
        [host, port]() { return nativeSrtResolveSockaddrs(host, port); }, timeoutMs, shouldStop);
}

bool nativeSrtTrySockaddrs(const QList<NativeSrtSockaddr>& addresses,
                           const NativeSrtAddressConnector& connector, QString* error,
                           const NativeSrtShouldStop& shouldStop) {
    if (!connector || addresses.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Native SRT host lookup returned no usable addresses.");
        }
        return false;
    }

    QString lastError;
    for (const NativeSrtSockaddr& address : addresses) {
        if (shouldStop && shouldStop()) {
            if (error) {
                *error = lastError.isEmpty() ? QStringLiteral("Native SRT connect cancelled.")
                                             : lastError;
            }
            return false;
        }

        QString attemptError;
        if (connector(address, &attemptError)) {
            if (error) {
                error->clear();
            }
            return true;
        }
        if (!attemptError.isEmpty()) {
            lastError = attemptError;
        }
        if (shouldStop && shouldStop()) {
            if (error) {
                *error = lastError.isEmpty() ? QStringLiteral("Native SRT connect cancelled.")
                                             : lastError;
            }
            return false;
        }
    }

    if (error) {
        *error = lastError.isEmpty()
                     ? QStringLiteral("Native SRT connect failed for all resolved addresses.")
                     : lastError;
    }
    return false;
}
