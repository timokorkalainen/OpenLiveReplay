#include "streamdeckmanager.h"

#import <Foundation/Foundation.h>
#import <StreamDeckBridge/StreamDeckBridge-Swift.h>

#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>

// All bridge callbacks arrive on the platform main thread, which is also
// Qt's main thread on iOS. Queued invocation still bounces them through the
// Qt event loop so handler code never re-enters mid-render, and the QPointer
// context cancels delivery if the manager is destroyed first.

StreamDeckManager::StreamDeckManager(QObject *parent)
    : QObject(parent)
{
    m_positionPushTimer.setSingleShot(true);
    connect(&m_positionPushTimer, &QTimer::timeout,
            this, &StreamDeckManager::pushPendingPosition);
    m_lastPositionPush.start();
}

StreamDeckManager::~StreamDeckManager()
{
    OLRStreamDeckBridge *bridge = [OLRStreamDeckBridge shared];
    bridge.onAction = nil;
    bridge.onRotate = nil;
    bridge.onLearnInput = nil;
    bridge.onScrub = nil;
    bridge.onDeviceConnected = nil;
    bridge.onDeviceDisconnected = nil;
}

bool StreamDeckManager::supported() const { return true; }

bool StreamDeckManager::driverAppInstalled() const
{
    return [OLRStreamDeckBridge shared].driverAppInstalled;
}

bool StreamDeckManager::simulatorAvailable() const
{
#ifdef QT_DEBUG
    // Only when the simulator is actually compiled into the bridge.
    return [OLRStreamDeckBridge shared].simulatorSupported;
#else
    return false;
#endif
}

void StreamDeckManager::showSimulator()
{
    [[OLRStreamDeckBridge shared] showSimulator];
}

void StreamDeckManager::closeSimulator()
{
    [[OLRStreamDeckBridge shared] closeSimulator];
}

void StreamDeckManager::start()
{
    OLRStreamDeckBridge *bridge = [OLRStreamDeckBridge shared];
    QPointer<StreamDeckManager> self(this);

    bridge.onAction = ^(NSInteger actionId, BOOL pressed) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, actionId, pressed]() {
            emit s->actionTriggered(int(actionId), pressed);
        }, Qt::QueuedConnection);
    };

    bridge.onRotate = ^(NSInteger actionId, NSInteger delta) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, actionId, delta]() {
            emit s->rotateTriggered(int(actionId), int(delta));
        }, Qt::QueuedConnection);
    };

    bridge.onLearnInput = ^(NSInteger elementType, NSInteger index) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, elementType, index]() {
            emit s->learnInput(int(elementType), int(index));
        }, Qt::QueuedConnection);
    };

    bridge.onScrub = ^(double fraction) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, fraction]() {
            emit s->scrubTriggered(fraction);
        }, Qt::QueuedConnection);
    };

    bridge.onDeviceConnected = ^(NSString *name, NSString *model,
                                 NSInteger keyCount, NSInteger dialCount) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        const QString qname = QString::fromNSString(name);
        const QString qmodel = QString::fromNSString(model);
        const int kc = int(keyCount);
        const int dc = int(dialCount);
        QMetaObject::invokeMethod(s, [s, qname, qmodel, kc, dc]() {
            s->handleDeviceConnected(qname, qmodel, kc, dc);
        }, Qt::QueuedConnection);
    };

    bridge.onDeviceDisconnected = ^{
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s]() {
            s->handleDeviceDisconnected();
        }, Qt::QueuedConnection);
    };

    [bridge start];

    // canOpenURL can change while we're backgrounded (user installs the
    // driver app); refresh the QML binding whenever we become active.
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) emit driverAppInstalledChanged();
        });
    }
}

void StreamDeckManager::handleDeviceConnected(const QString &name, const QString &model,
                                              int keyCount, int dialCount)
{
    m_deviceName = name;
    m_deviceModel = model;
    m_keyCount = keyCount;
    m_dialCount = dialCount;
    m_connected = true;
    emit connectedChanged();
}

void StreamDeckManager::handleDeviceDisconnected()
{
    if (!m_connected) return;
    m_connected = false;
    m_deviceName.clear();
    m_deviceModel.clear();
    m_keyCount = 0;
    m_dialCount = 0;
    // A hold action (rewind/forward 5x) never gets its release once the deck
    // is gone — synthesize releases so the transport can't stay stuck at 5x.
    emit actionTriggered(1, false);
    emit actionTriggered(2, false);
    emit connectedChanged();
}

void StreamDeckManager::setRecording(bool recording, qint64 elapsedMs)
{
    const QString elapsed = recording ? formatElapsed(elapsedMs) : QString();
    if (recording == m_lastRecording && elapsed == m_lastElapsedText) return;
    m_lastRecording = recording;
    m_lastElapsedText = elapsed;
    [[OLRStreamDeckBridge shared] setRecording:recording
                                   elapsedText:elapsed.toNSString()];
}

void StreamDeckManager::setTransport(bool playing, double speed, bool followLive)
{
    [[OLRStreamDeckBridge shared] setTransportWithPlaying:playing
                                                speedText:formatSpeed(playing, speed).toNSString()
                                               followLive:followLive];
}

void StreamDeckManager::setPosition(const QString &timecodeText, double fraction)
{
    if (!m_connected) return;

    // The app already formatted this — the deck mirrors it verbatim.
    m_pendingTimecode = timecodeText;
    m_pendingFraction = qBound(0.0, fraction, 1.0);
    if (m_pendingTimecode == m_lastPushedTimecode) return;

    const qint64 sinceLast = m_lastPositionPush.elapsed();
    if (sinceLast >= kPositionPushIntervalMs) {
        pushPendingPosition();
    } else if (!m_positionPushTimer.isActive()) {
        m_positionPushTimer.start(int(kPositionPushIntervalMs - sinceLast));
    }
}

void StreamDeckManager::pushPendingPosition()
{
    m_lastPositionPush.restart();
    m_lastPushedTimecode = m_pendingTimecode;
    [[OLRStreamDeckBridge shared] setPositionWithTimecodeText:m_pendingTimecode.toNSString()
                                             positionFraction:m_pendingFraction];
}

QString StreamDeckManager::formatElapsed(qint64 ms)
{
    if (ms < 0) ms = 0;
    const qint64 totalSeconds = ms / 1000;
    return QString("%1:%2:%3")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

QString StreamDeckManager::formatSpeed(bool playing, double speed)
{
    if (!playing) return QStringLiteral("Paused");
    return QString::number(speed, 'f', 1) + QStringLiteral("×");
}

void StreamDeckManager::setLearnMode(bool active)
{
    [[OLRStreamDeckBridge shared] setLearnMode:active];
}

static NSArray<NSNumber *> *toNSNumbers(const QList<int> &list)
{
    NSMutableArray<NSNumber *> *arr = [NSMutableArray arrayWithCapacity:list.size()];
    for (int v : list) [arr addObject:@(v)];
    return arr;
}

void StreamDeckManager::pushKeyMap(const QString &model, const QList<int> &keyMap)
{
    [[OLRStreamDeckBridge shared] setKeyMapping:toNSNumbers(keyMap)
                                       forModel:model.toNSString()];
}

void StreamDeckManager::pushDialMaps(const QString &model,
                                     const QList<int> &rotateMap, const QList<int> &pressMap)
{
    [[OLRStreamDeckBridge shared] setDialMappingWithRotate:toNSNumbers(rotateMap)
                                                     press:toNSNumbers(pressMap)
                                                  forModel:model.toNSString()];
}
