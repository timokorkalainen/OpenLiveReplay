#include "nativesrtaddress.h"

#include <QUrl>

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

bool nativeSrtResolveSockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address) {
    if (!address || host.isEmpty()) {
        return false;
    }

#ifdef _WIN32
    ScopedWinsockStartup winsock;
    if (!winsock.ok()) {
        return false;
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
        return false;
    }

    bool copied = false;
    for (const addrinfo* current = results; current; current = current->ai_next) {
        if (!current->ai_addr || current->ai_addrlen == 0 ||
            size_t(current->ai_addrlen) > sizeof(address->storage)) {
            continue;
        }
        if (current->ai_family != AF_INET && current->ai_family != AF_INET6) {
            continue;
        }

        memset(address->storage, 0, sizeof(address->storage));
        memcpy(address->storage, current->ai_addr, size_t(current->ai_addrlen));
        address->size = int(current->ai_addrlen);
        copied = true;
        break;
    }

    freeaddrinfo(results);
    return copied;
}
