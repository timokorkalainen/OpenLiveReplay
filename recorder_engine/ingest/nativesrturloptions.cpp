#include "nativesrturloptions.h"

#include <QByteArray>

namespace {
QString percentDecoded(const QString& value) {
    return QString::fromUtf8(QByteArray::fromPercentEncoding(value.toUtf8()));
}
} // namespace

NativeSrtUrlOptions nativeSrtUrlOptionsFromUrl(const QUrl& url) {
    NativeSrtUrlOptions options;
    const QString encodedUrl = url.toString(QUrl::FullyEncoded);
    const int queryStart = encodedUrl.indexOf(QLatin1Char('?'));
    if (queryStart < 0) {
        return options;
    }

    const QString rawQuery = encodedUrl.mid(queryStart + 1);
    qsizetype itemStart = 0;
    while (itemStart < rawQuery.size()) {
        qsizetype itemEnd = rawQuery.indexOf(QLatin1Char('&'), itemStart);
        if (itemEnd < 0) {
            itemEnd = rawQuery.size();
        }
        const qsizetype equals = rawQuery.indexOf(QLatin1Char('='), itemStart);
        if (equals >= itemStart && equals < itemEnd &&
            percentDecoded(rawQuery.mid(itemStart, equals - itemStart)) ==
                QStringLiteral("streamid")) {
            options.streamId = percentDecoded(rawQuery.mid(equals + 1, itemEnd - equals - 1));
            return options;
        }
        itemStart = itemEnd + 1;
    }

    return options;
}
