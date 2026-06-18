#include "nativesrturloptions.h"

#include <QByteArray>

namespace {
QString percentDecoded(const QString& value) {
    return QString::fromUtf8(QByteArray::fromPercentEncoding(value.toUtf8()));
}
} // namespace

bool nativeSrtPassphraseIsValid(const QString& passphrase) {
    const int length = int(passphrase.toUtf8().size());
    return length >= kNativeSrtMinPassphraseLen && length <= kNativeSrtMaxPassphraseLen;
}

bool nativeSrtPbKeyLenIsValid(int pbKeyLen) {
    return pbKeyLen == 16 || pbKeyLen == 24 || pbKeyLen == 32;
}

NativeSrtUrlOptions nativeSrtUrlOptionsFromUrl(const QUrl& url) {
    NativeSrtUrlOptions options;
    const QString encodedUrl = url.toString(QUrl::FullyEncoded);
    const int queryStart = encodedUrl.indexOf(QLatin1Char('?'));
    if (queryStart < 0) {
        return options;
    }

    bool sawPassphrase = false;
    bool sawStreamId = false;
    const QString rawQuery = encodedUrl.mid(queryStart + 1);
    qsizetype itemStart = 0;
    while (itemStart < rawQuery.size()) {
        qsizetype itemEnd = rawQuery.indexOf(QLatin1Char('&'), itemStart);
        if (itemEnd < 0) {
            itemEnd = rawQuery.size();
        }
        const qsizetype equals = rawQuery.indexOf(QLatin1Char('='), itemStart);
        if (equals >= itemStart && equals < itemEnd) {
            const QString key = percentDecoded(rawQuery.mid(itemStart, equals - itemStart));
            const QString value = percentDecoded(rawQuery.mid(equals + 1, itemEnd - equals - 1));
            // streamid keeps its original raw-'#!::...' handling: its value is
            // captured up to the next '&' (an unescaped '#' inside a stream ID must
            // not be treated as a fragment). First streamid wins; keep scanning so a
            // passphrase/pbkeylen on either side of it is still picked up.
            if (key == QStringLiteral("streamid")) {
                if (!sawStreamId) {
                    options.streamId = value;
                    sawStreamId = true;
                }
            } else if (key == QStringLiteral("passphrase")) {
                options.passphrase = value;
                sawPassphrase = true;
            } else if (key == QStringLiteral("pbkeylen")) {
                bool ok = false;
                const int parsed = value.toInt(&ok);
                if (ok) {
                    options.pbKeyLen = parsed;
                }
            }
        }
        itemStart = itemEnd + 1;
    }

    // A passphrase with no explicit pbkeylen defaults to AES-128 (16-byte key),
    // matching libsrt's default. pbkeylen alone (no passphrase) stays as parsed so
    // callers can detect the meaningless "key length but nothing to encrypt" case.
    if (sawPassphrase && options.pbKeyLen == 0) {
        options.pbKeyLen = 16;
    }

    return options;
}
