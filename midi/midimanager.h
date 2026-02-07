#ifndef MIDIMANAGER_H
#define MIDIMANAGER_H

#include <QObject>
#include <QStringList>
#include <memory>

class RtMidiIn;

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

signals:
    void portsChanged();
    void currentPortChanged();
    void connectedChanged();
    void midiTriggered();
    void midiMessage(int status, int data1, int data2);

private:
    static void midiCallback(double deltaTime, std::vector<unsigned char> *message, void *userData);
    void emitTriggered();

    std::unique_ptr<RtMidiIn> m_midiIn;
    QStringList m_ports;
    int m_currentPort = -1;
    bool m_connected = false;
};

#endif // MIDIMANAGER_H