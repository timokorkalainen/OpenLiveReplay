#ifndef STREAMDECKMANAGER_H
#define STREAMDECKMANAGER_H

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

// Bridges StreamDeckBridge.xcframework (Swift, iPadOS only) to the Qt app.
// Mirrors the MidiManager pattern: hardware events in -> action signals out,
// app state in -> deck rendering out. On non-iOS platforms a stub
// implementation (streamdeckmanager_stub.cpp) reports supported() == false.
//
// Action ids shared with DeckAction.swift and UIManager::dispatchControlAction:
//   0 play/pause, 1 rewind 5x (hold), 2 forward 5x (hold), 3 step fwd,
//   4 go live, 5 capture, 6 multiview, 7 step back, 8 jog (dial only),
//   9 record toggle, 10 shuttle (dial-turn), 20 timecode display,
//   21 speed display, -1 empty key.
class StreamDeckManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY connectedChanged)
    Q_PROPERTY(QString deviceModel READ deviceModel NOTIFY connectedChanged)
    Q_PROPERTY(int keyCount READ keyCount NOTIFY connectedChanged)
    Q_PROPERTY(int dialCount READ dialCount NOTIFY connectedChanged)
    Q_PROPERTY(bool driverAppInstalled READ driverAppInstalled NOTIFY driverAppInstalledChanged)
    Q_PROPERTY(bool simulatorAvailable READ simulatorAvailable CONSTANT)

public:
    explicit StreamDeckManager(QObject *parent = nullptr);
    ~StreamDeckManager() override;

    bool supported() const;
    bool connected() const { return m_connected; }
    QString deviceName() const { return m_deviceName; }
    QString deviceModel() const { return m_deviceModel; }
    int keyCount() const { return m_keyCount; }
    int dialCount() const { return m_dialCount; }
    bool driverAppInstalled() const;
    bool simulatorAvailable() const;

    Q_INVOKABLE void showSimulator();
    Q_INVOKABLE void closeSimulator();
    Q_INVOKABLE void setLearnMode(bool active);
    // Push the active model's maps to the deck (called by UIManager after edits
    // and on connect). Lists are index -> action id (-1 unbound).
    void pushKeyMap(const QString &model, const QList<int> &keyMap);
    void pushDialMaps(const QString &model,
                      const QList<int> &rotateMap, const QList<int> &pressMap);

    // Call once after signal connections are in place. No-op when unsupported.
    void start();

public slots:
    // App state pushed to the deck. Cheap no-ops when nothing is connected.
    void setRecording(bool recording, qint64 elapsedMs);
    void setTransport(bool playing, double speed, bool followLive);
    // timecodeText is the exact string the app's playback UI shows (the deck
    // never formats its own); fraction is the scrub-bar position (0..1).
    void setPosition(const QString &timecodeText, double fraction);

signals:
    void connectedChanged();
    void driverAppInstalledChanged();
    void actionTriggered(int actionId, bool pressed);
    void rotateTriggered(int actionId, int delta);
    void learnInput(int elementType, int index);
    void scrubTriggered(double fraction);

private:
    // Defined only in the iOS implementation (streamdeckmanager.mm).
    void handleDeviceConnected(const QString &name, const QString &model,
                               int keyCount, int dialCount);
    void handleDeviceDisconnected();
    void pushPendingPosition();
    static QString formatElapsed(qint64 ms);
    static QString formatSpeed(bool playing, double speed);

    bool m_connected = false;
    QString m_deviceName;
    QString m_deviceModel;
    int m_keyCount = 0;
    int m_dialCount = 0;

    // Position push throttle (<= 10 Hz, trailing-edge coalesced so the last
    // value after pause/step always lands).
    QElapsedTimer m_lastPositionPush;
    QTimer m_positionPushTimer;
    QString m_pendingTimecode;
    double m_pendingFraction = 0.0;
    QString m_lastPushedTimecode;
    bool m_lastRecording = false;
    QString m_lastElapsedText;
    static constexpr int kPositionPushIntervalMs = 100;
};

#endif // STREAMDECKMANAGER_H
