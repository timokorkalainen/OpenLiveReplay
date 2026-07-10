#ifndef APPENV_H
#define APPENV_H

#include <QString>

namespace appenv {

void configureQtMediaBackend();
quint16 controlPort();
QString documentsRoot();
QString documentsPath(const QString& relativePath);

} // namespace appenv

#endif // APPENV_H
