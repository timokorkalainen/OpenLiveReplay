#ifndef NATIVESRTURLOPTIONS_H
#define NATIVESRTURLOPTIONS_H

#include <QString>
#include <QUrl>

struct NativeSrtUrlOptions {
    QString streamId;
};

NativeSrtUrlOptions nativeSrtUrlOptionsFromUrl(const QUrl& url);

#endif // NATIVESRTURLOPTIONS_H
