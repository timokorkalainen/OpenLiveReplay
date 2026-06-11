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
    bridge.onJog = nil;
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
    return true;
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

    bridge.onJog = ^(NSInteger delta) {
        StreamDeckManager *s = self.data();
        if (!s) return;
        QMetaObject::invokeMethod(s, [s, delta]() {
            emit s->jogTriggered(int(delta));
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
        Q_UNUSED(keyCount)
        Q_UNUSED(dialCount)
        StreamDeckManager *s = self.data();
        if (!s) return;
        const QString qname = QString::fromNSString(name);
        const QString qmodel = QString::fromNSString(model);
        QMetaObject::invokeMethod(s, [s, qname, qmodel]() {
            s->handleDeviceConnected(qname, qmodel);
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

void StreamDeckManager::handleDeviceConnected(const QString &name, const QString &model)
{
    m_deviceName = name;
    m_deviceModel = model;
    m_connected = true;
    emit connectedChanged();
}

void StreamDeckManager::handleDeviceDisconnected()
{
    if (!m_connected) return;
    m_connected = false;
    m_deviceName.clear();
    m_deviceModel.clear();
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

void StreamDeckManager::setPosition(qint64 posMs, qint64 durationMs, int fps)
{
    if (!m_connected) return;

    m_pendingTimecode = formatTimecode(posMs, fps);
    // durationMs is -1 before the recording clock exists; treat as no extent.
    m_pendingFraction = durationMs > 0
        ? qBound(0.0, double(posMs) / double(durationMs), 1.0)
        : 0.0;
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

QString StreamDeckManager::formatTimecode(qint64 ms, int fps)
{
    if (ms < 0) ms = 0;
    if (fps <= 0) fps = 30;
    const qint64 totalSeconds = ms / 1000;
    const int frames = int((ms % 1000) * fps / 1000);
    return QString("%1:%2:%3:%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
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
