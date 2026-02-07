#include "midimanager.h"

#include <QMetaObject>
#include <QThread>

#include <RtMidi.h>

MidiManager::MidiManager(QObject *parent)
    : QObject(parent) {
    try {
        m_midiIn = std::make_unique<RtMidiIn>();
        m_midiIn->ignoreTypes(false, false, false);
    } catch (...) {
        m_midiIn.reset();
    }
    refreshPorts();
}

MidiManager::~MidiManager() {
    closePort();
}

void MidiManager::refreshPorts() {
    if (!m_midiIn) return;
    QStringList newPorts;
    try {
        unsigned int count = m_midiIn->getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            newPorts.append(QString::fromStdString(m_midiIn->getPortName(i)));
        }
    } catch (...) {
        return;
    }
    if (newPorts != m_ports) {
        m_ports = newPorts;
        emit portsChanged();
    }
    if (m_currentPort >= m_ports.size()) {
        m_currentPort = -1;
        emit currentPortChanged();
    }
}

void MidiManager::openPort(int index) {
    if (!m_midiIn) return;
    if (index < 0 || index >= m_ports.size()) return;

    closePort();

    try {
        m_midiIn->openPort(static_cast<unsigned int>(index));
        m_midiIn->setCallback(&MidiManager::midiCallback, this);
        m_currentPort = index;
        m_connected = true;
        emit currentPortChanged();
        emit connectedChanged();
    } catch (...) {
        m_currentPort = -1;
        m_connected = false;
        emit currentPortChanged();
        emit connectedChanged();
    }
}

void MidiManager::closePort() {
    if (!m_midiIn) return;
    if (m_midiIn->isPortOpen()) {
        try {
            m_midiIn->closePort();
        } catch (...) {
        }
    }
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
}

void MidiManager::midiCallback(double, std::vector<unsigned char> *message, void *userData) {
    auto *self = static_cast<MidiManager *>(userData);
    if (!self || !message || message->empty()) return;
    self->emitTriggered();
}

void MidiManager::emitTriggered() {
    if (thread() == QThread::currentThread()) {
        emit midiTriggered();
        return;
    }
    QMetaObject::invokeMethod(this, [this]() {
        emit midiTriggered();
    }, Qt::QueuedConnection);
}