#ifndef MIDIMANAGER_H
#define MIDIMANAGER_H

#include <QObject>
#include <QStringList>
#include <memory>

class RtMidiIn;
class RtMidiOut;

class MidiManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList ports READ ports NOTIFY portsChanged)
    Q_PROPERTY(int currentPort READ currentPort NOTIFY currentPortChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit MidiManager(QObject *parent = nullptr);
    ~MidiManager();

    QStringList ports() const { return m_ports; }
    int currentPort() const { return m_currentPort; }
    bool connected() const { return m_connected; }

    Q_INVOKABLE void refreshPorts();
    Q_INVOKABLE void openPort(int index);
    Q_INVOKABLE void closePort();
    Q_INVOKABLE bool isXTouchConnected() const { return m_isXTouch && m_connected && m_outputConnected; }
    Q_INVOKABLE void sendXTouchTimecode(int hours, int minutes, int seconds, int frames, int fps);
    Q_INVOKABLE void sendXTouchSegmentDisplay(const QString &digits, quint8 dots1, quint8 dots2);

signals:
    void portsChanged();
    void currentPortChanged();
    void connectedChanged();
    void midiTriggered();
    void midiMessage(int status, int data1, int data2);

private:
    static void midiCallback(double deltaTime, std::vector<unsigned char> *message, void *userData);
    void emitTriggered();
    bool openOutputForPortName(const QString &name);
    void closeOutput();
    void sendMtcQuarterFrames(int hours, int minutes, int seconds, int frames, int fps);
    static unsigned char segmentForChar(QChar ch);

    std::unique_ptr<RtMidiIn> m_midiIn;
    std::unique_ptr<RtMidiOut> m_midiOut;
    QStringList m_ports;
    QString m_currentPortName;
    int m_currentPort = -1;
    bool m_connected = false;
    bool m_callbackSet = false;
    int m_outputPort = -1;
    bool m_outputConnected = false;
    bool m_isXTouch = false;
    int m_segmentDebugCount = 0;
};

#endif // MIDIMANAGER_H