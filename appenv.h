#ifndef APPENV_H
#define APPENV_H

#include <QString>

namespace appenv {

quint16 controlPort();
QString documentsRoot();
QString documentsPath(const QString& relativePath);

} // namespace appenv

#endif // APPENV_H
