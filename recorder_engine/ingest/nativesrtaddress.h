#ifndef NATIVESRTADDRESS_H
#define NATIVESRTADDRESS_H

#include <QList>
#include <QString>

#include <cstddef>
#include <functional>

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

using NativeSrtAddressResolver = std::function<QList<NativeSrtSockaddr>()>;
using NativeSrtShouldStop = std::function<bool()>;
using NativeSrtAddressConnector =
    std::function<bool(const NativeSrtSockaddr& address, QString* error)>;

QList<NativeSrtSockaddr> nativeSrtResolveSockaddrs(const QString& host, quint16 port);
QList<NativeSrtSockaddr>
nativeSrtResolveSockaddrsWithTimeout(const NativeSrtAddressResolver& resolver, int timeoutMs,
                                     const NativeSrtShouldStop& shouldStop);
QList<NativeSrtSockaddr>
nativeSrtResolveSockaddrsWithTimeout(const QString& host, quint16 port, int timeoutMs,
                                     const NativeSrtShouldStop& shouldStop);
bool nativeSrtTrySockaddrs(const QList<NativeSrtSockaddr>& addresses,
                           const NativeSrtAddressConnector& connector, QString* error,
                           const NativeSrtShouldStop& shouldStop = NativeSrtShouldStop());

#endif // NATIVESRTADDRESS_H
