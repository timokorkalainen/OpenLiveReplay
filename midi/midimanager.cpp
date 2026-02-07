#include "midimanager.h"

#include <cstdio>
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
    try {
        m_midiOut = std::make_unique<RtMidiOut>();
    } catch (...) {
        m_midiOut.reset();
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
        if (!m_callbackSet) {
            m_midiIn->setCallback(&MidiManager::midiCallback, this);
            m_callbackSet = true;
        }
        m_currentPort = index;
        m_currentPortName = m_ports.value(index);
        const QString portLower = m_currentPortName.toLower();
        m_isXTouch = portLower.contains("x-touch") || portLower.contains("mackie") || portLower.contains("xtouch");
        m_outputConnected = openOutputForPortName(m_currentPortName);
        std::fprintf(stderr,
                     "MIDI: Opened input %s isXTouch=%d outputConnected=%d outputPort=%d\n",
                     qPrintable(m_currentPortName),
                     m_isXTouch ? 1 : 0,
                     m_outputConnected ? 1 : 0,
                     m_outputPort);
        m_connected = true;
        emit currentPortChanged();
        emit connectedChanged();
    } catch (...) {
        m_currentPort = -1;
        m_currentPortName.clear();
        m_isXTouch = false;
        closeOutput();
        m_connected = false;
        std::fprintf(stderr,
                     "MIDI: Failed to open input port %d\n",
                     index);
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
    m_callbackSet = false;
    closeOutput();
    m_currentPort = -1;
    m_currentPortName.clear();
    m_isXTouch = false;
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
}

bool MidiManager::openOutputForPortName(const QString &name) {
    if (!m_midiOut) return false;

    closeOutput();

    try {
        const unsigned int count = m_midiOut->getPortCount();
        const QString matchName = name.trimmed();
        const QString matchLower = matchName.toLower();
        const bool matchXTouch = matchLower.contains("x-touch") || matchLower.contains("xtouch") || matchLower.contains("mackie");
        std::fprintf(stderr,
                     "MIDI: Searching output ports for %s\n",
                     qPrintable(name));
        for (unsigned int i = 0; i < count; ++i) {
            const QString portName = QString::fromStdString(m_midiOut->getPortName(i));
            const QString portLower = portName.toLower();
            std::fprintf(stderr,
                         "MIDI: Output port %u: %s\n",
                         i,
                         qPrintable(portName));
            if (matchXTouch && (portLower.contains("x-touch") || portLower.contains("xtouch") || portLower.contains("mackie"))) {
                m_midiOut->openPort(i);
                m_outputPort = static_cast<int>(i);
                m_outputConnected = true;
                std::fprintf(stderr,
                             "MIDI: Opened output (xtouch/mackie match) %s\n",
                             qPrintable(portName));
                return true;
            }
            if (!matchName.isEmpty() && portName.contains(matchName, Qt::CaseInsensitive)) {
                m_midiOut->openPort(i);
                m_outputPort = static_cast<int>(i);
                m_outputConnected = true;
                std::fprintf(stderr,
                             "MIDI: Opened output (name match) %s\n",
                             qPrintable(portName));
                return true;
            }
        }
        if (m_currentPort >= 0 && m_currentPort < static_cast<int>(count)) {
            m_midiOut->openPort(static_cast<unsigned int>(m_currentPort));
            m_outputPort = m_currentPort;
            m_outputConnected = true;
            std::fprintf(stderr,
                         "MIDI: Opened output (same index) %d\n",
                         m_currentPort);
            return true;
        }
    } catch (...) {
        std::fprintf(stderr,
                     "MIDI: Failed to open output for %s\n",
                     qPrintable(name));
    }

    m_outputPort = -1;
    m_outputConnected = false;
    return false;
}

void MidiManager::closeOutput() {
    if (!m_midiOut) return;
    if (m_midiOut->isPortOpen()) {
        try {
            m_midiOut->closePort();
        } catch (...) {
        }
    }
    m_outputPort = -1;
    m_outputConnected = false;
}

void MidiManager::sendXTouchTimecode(int hours, int minutes, int seconds, int frames, int fps) {
    if (!isXTouchConnected() || !m_midiOut) return;

    hours = qBound(0, hours, 23);
    minutes = qBound(0, minutes, 59);
    seconds = qBound(0, seconds, 59);

    int rateCode = 3; // 30 fps default
    if (fps <= 24) rateCode = 0;       // 24 fps
    else if (fps <= 25) rateCode = 1;  // 25 fps
    else if (fps < 30) rateCode = 2;   // 29.97 fps
    else rateCode = 3;                // 30 fps

    int maxFrames = (fps <= 0) ? 30 : fps;
    if (maxFrames > 30) maxFrames = 30;
    frames = qBound(0, frames, maxFrames - 1);

    // Send full-frame first (helps device lock), then quarter-frames for display updates.
    std::vector<unsigned char> msg;
    msg.reserve(10);
    msg.push_back(0xF0);
    msg.push_back(0x7F); // realtime
    msg.push_back(0x7F); // all devices
    msg.push_back(0x01); // MTC
    msg.push_back(0x01); // Full Frame
    msg.push_back(static_cast<unsigned char>((rateCode << 5) | (hours & 0x1F)));
    msg.push_back(static_cast<unsigned char>(minutes & 0x3F));
    msg.push_back(static_cast<unsigned char>(seconds & 0x3F));
    msg.push_back(static_cast<unsigned char>(frames & 0x1F));
    msg.push_back(0xF7);

    try {
        m_midiOut->sendMessage(&msg);
        std::fprintf(stderr,
                     "MIDI: Sent MTC Full Frame %02d:%02d:%02d:%02d fps=%d rateCode=%d\n",
                     hours, minutes, seconds, frames, fps, rateCode);
    } catch (...) {
        std::fprintf(stderr,
                     "MIDI: Failed to send MTC Full Frame\n");
    }

    sendMtcQuarterFrames(hours, minutes, seconds, frames, fps);
}

unsigned char MidiManager::segmentForChar(QChar ch) {
    if (ch.isDigit()) {
        switch (ch.toLatin1()) {
        case '0': return 0x3F; // a b c d e f
        case '1': return 0x06; // b c
        case '2': return 0x5B; // a b d e g
        case '3': return 0x4F; // a b c d g
        case '4': return 0x66; // b c f g
        case '5': return 0x6D; // a c d f g
        case '6': return 0x7D; // a c d e f g
        case '7': return 0x07; // a b c
        case '8': return 0x7F; // a b c d e f g
        case '9': return 0x6F; // a b c d f g
        default: break;
        }
    }
    if (ch == '-') {
        return 0x40; // g
    }
    return 0x00; // blank
}

void MidiManager::sendXTouchSegmentDisplay(const QString &digits, quint8 dots1, quint8 dots2) {
    if (!isXTouchConnected() || !m_midiOut) return;

    QString padded = digits.left(12);
    if (padded.size() < 12) {
        padded = padded.leftJustified(12, ' ');
    }

    std::vector<unsigned char> msg;
    msg.reserve(6 + 12 + 2 + 1);
    msg.push_back(0xF0);
    msg.push_back(0x00);
    msg.push_back(0x20);
    msg.push_back(0x32);
    msg.push_back(0x41);
    msg.push_back(0x37);

    for (int i = 0; i < 12; ++i) {
        msg.push_back(segmentForChar(padded.at(i)));
    }

    msg.push_back(dots1);
    msg.push_back(dots2);
    msg.push_back(0xF7);

    try {
        m_midiOut->sendMessage(&msg);
        std::fprintf(stderr,
                     "MIDI: Sent Segment Display '%s' dots1=0x%02X dots2=0x%02X\n",
                     qPrintable(padded),
                     dots1,
                     dots2);
        if (m_segmentDebugCount < 5) {
            m_segmentDebugCount++;
            std::fprintf(stderr, "MIDI: Segment SysEx bytes:");
            for (const auto &b : msg) {
                std::fprintf(stderr, " %02X", b);
            }
            std::fprintf(stderr, "\n");
        }
    } catch (...) {
        std::fprintf(stderr, "MIDI: Failed to send Segment Display\n");
    }
}

void MidiManager::sendMtcQuarterFrames(int hours, int minutes, int seconds, int frames, int fps) {
    if (!m_midiOut) return;

    int rateCode = 3; // 30 fps
    if (fps <= 24) rateCode = 0;
    else if (fps <= 25) rateCode = 1;
    else if (fps < 30) rateCode = 2;

    const int frameL = frames & 0x0F;
    const int frameH = (frames >> 4) & 0x01;
    const int secL = seconds & 0x0F;
    const int secH = (seconds >> 4) & 0x03;
    const int minL = minutes & 0x0F;
    const int minH = (minutes >> 4) & 0x03;
    const int hourL = hours & 0x0F;
    const int hourH = (hours >> 4) & 0x01;

    const unsigned char data[8] = {
        static_cast<unsigned char>((0x0 << 4) | frameL),
        static_cast<unsigned char>((0x1 << 4) | frameH),
        static_cast<unsigned char>((0x2 << 4) | secL),
        static_cast<unsigned char>((0x3 << 4) | secH),
        static_cast<unsigned char>((0x4 << 4) | minL),
        static_cast<unsigned char>((0x5 << 4) | minH),
        static_cast<unsigned char>((0x6 << 4) | hourL),
        static_cast<unsigned char>((0x7 << 4) | ((rateCode & 0x03) << 1) | hourH)
    };

    std::vector<unsigned char> msg(2);
    msg[0] = 0xF1;
    for (int i = 0; i < 8; ++i) {
        msg[1] = data[i];
        try {
            m_midiOut->sendMessage(&msg);
        } catch (...) {
            std::fprintf(stderr,
                         "MIDI: Failed to send MTC QF %d\n",
                         i);
        }
    }
}

void MidiManager::midiCallback(double, std::vector<unsigned char> *message, void *userData) {
    auto *self = static_cast<MidiManager *>(userData);
    if (!self || !message || message->empty()) return;
    int status = static_cast<int>((*message)[0]);
    int data1 = message->size() > 1 ? static_cast<int>((*message)[1]) : -1;
    int data2 = message->size() > 2 ? static_cast<int>((*message)[2]) : -1;
    self->emitTriggered();
    if (self->thread() == QThread::currentThread()) {
        emit self->midiMessage(status, data1, data2);
        return;
    }
    QMetaObject::invokeMethod(self, [self, status, data1, data2]() {
        emit self->midiMessage(status, data1, data2);
    }, Qt::QueuedConnection);
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