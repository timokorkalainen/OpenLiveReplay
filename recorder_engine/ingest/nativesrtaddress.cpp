#include "nativesrtaddress.h"

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

#include <cstring>

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
