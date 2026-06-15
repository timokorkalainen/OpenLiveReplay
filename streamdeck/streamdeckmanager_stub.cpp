#include "streamdeckmanager.h"

// Non-iOS stub: same interface, no hardware. supported() lets QML hide the
// whole Stream Deck section without #ifdefs anywhere else.

StreamDeckManager::StreamDeckManager(QObject *parent)
    : QObject(parent)
{
}

StreamDeckManager::~StreamDeckManager() = default;

bool StreamDeckManager::supported() const { return false; }
bool StreamDeckManager::driverAppInstalled() const { return false; }
bool StreamDeckManager::simulatorAvailable() const { return false; }

void StreamDeckManager::showSimulator() {}
void StreamDeckManager::closeSimulator() {}
void StreamDeckManager::start() {}

void StreamDeckManager::setRecording(bool, qint64) {}
void StreamDeckManager::setTransport(bool, double, bool) {}
void StreamDeckManager::setPosition(const QString &, double) {}

void StreamDeckManager::setLearnMode(bool) {}
void StreamDeckManager::pushKeyMap(const QString &, const QList<int> &) {}
void StreamDeckManager::pushDialMaps(const QString &, const QList<int> &, const QList<int> &) {}
