#ifndef NATIVESRTADDRESS_H
#define NATIVESRTADDRESS_H

#include <QString>

#include <cstddef>

struct sockaddr;

struct NativeSrtSockaddr {
    alignas(std::max_align_t) unsigned char storage[128] {};
    int size = 0;

    sockaddr* sockaddrPtr();
    const sockaddr* sockaddrPtr() const;
};

bool nativeSrtIsNumericIpv4Host(const QString& host);
bool nativeSrtMakeIpv4Sockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address);
bool nativeSrtResolveSockaddr(const QString& host, quint16 port, NativeSrtSockaddr* address);

#endif // NATIVESRTADDRESS_H
