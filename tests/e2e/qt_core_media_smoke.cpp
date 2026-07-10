#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTextStream>
#include <QThread>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <cstring>
#include <memory>

#include "appenv.h"

namespace {

QString audioStateName(QtAudio::State state) {
    switch (state) {
    case QtAudio::ActiveState:
        return QStringLiteral("active");
    case QtAudio::SuspendedState:
        return QStringLiteral("suspended");
    case QtAudio::StoppedState:
        return QStringLiteral("stopped");
    case QtAudio::IdleState:
        return QStringLiteral("idle");
    }
    return QStringLiteral("unknown");
}

QString audioErrorName(QtAudio::Error error) {
    switch (error) {
    case QtAudio::NoError:
        return QStringLiteral("none");
    case QtAudio::OpenError:
        return QStringLiteral("open");
    case QtAudio::IOError:
        return QStringLiteral("io");
    case QtAudio::UnderrunError:
        return QStringLiteral("underrun");
    case QtAudio::FatalError:
        return QStringLiteral("fatal");
    }
    return QStringLiteral("unknown");
}

QVideoFrame solidFrame(uchar value, qint64 startTimeUs) {
    QVideoFrameFormat format(QSize(32, 18), QVideoFrameFormat::Format_BGRA8888);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly)) return {};
    std::memset(frame.bits(0), value, size_t(frame.mappedBytes(0)));
    frame.unmap();
    frame.setStartTime(startTimeUs);
    frame.setEndTime(startTimeUs + 40'000);
    return frame;
}

} // namespace

int main(int argc, char* argv[]) {
    appenv::configureQtMediaBackend();
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qt_core_media_smoke"));

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption allowNoAudio(QStringLiteral("allow-no-audio-device"));
    QCommandLineOption holdMs(QStringLiteral("hold-ms"),
                              QStringLiteral("Milliseconds to stay alive after JSON"),
                              QStringLiteral("milliseconds"), QStringLiteral("0"));
    parser.addOption(allowNoAudio);
    parser.addOption(holdMs);
    parser.process(application);

    bool holdOk = false;
    const int holdMilliseconds = parser.value(holdMs).toInt(&holdOk);
    if (!holdOk || holdMilliseconds < 0 || holdMilliseconds > 30'000) {
        QTextStream(stderr) << "invalid --hold-ms\n";
        return 2;
    }

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport QtMultimedia\nVideoOutput {}\n", QUrl());
    std::unique_ptr<QObject> videoOutput(component.create());
    if (!videoOutput) {
        QTextStream errorStream(stderr);
        for (const QQmlError& error : component.errors())
            errorStream << error.toString() << '\n';
        return 3;
    }
    QVideoSink* videoSink = videoOutput->property("videoSink").value<QVideoSink*>();
    if (!videoSink) {
        QTextStream(stderr) << "VideoOutput did not expose a QVideoSink\n";
        return 4;
    }

    int observedFrames = 0;
    QObject::connect(videoSink, &QVideoSink::videoFrameChanged, &application,
                     [&observedFrames](const QVideoFrame& frame) {
                         if (frame.isValid()) ++observedFrames;
                     });
    const QVideoFrame first = solidFrame(0x24, 0);
    const QVideoFrame second = solidFrame(0xc8, 40'000);
    if (!first.isValid() || !second.isValid()) {
        QTextStream(stderr) << "could not allocate writable video frames\n";
        return 5;
    }
    videoSink->setVideoFrame(first);
    application.processEvents(QEventLoop::AllEvents, 50);
    videoSink->setVideoFrame(second);
    application.processEvents(QEventLoop::AllEvents, 50);

    QString audioResult = QStringLiteral("failed");
    QString stateName = QStringLiteral("no-device");
    QString errorName = QStringLiteral("none");
    const QAudioDevice audioDevice = QMediaDevices::defaultAudioOutput();
    std::unique_ptr<QAudioSink> audioSink;
    QByteArray pcm;
    QBuffer audioBuffer;
    if (audioDevice.isNull()) {
        if (parser.isSet(allowNoAudio)) audioResult = QStringLiteral("no-device");
    } else {
        QAudioFormat format;
        format.setSampleRate(48'000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        if (audioDevice.isFormatSupported(format)) {
            pcm.fill('\0', 48'000 * 2 * int(sizeof(qint16)));
            audioBuffer.setBuffer(&pcm);
            audioBuffer.open(QIODevice::ReadOnly);
            audioSink = std::make_unique<QAudioSink>(audioDevice, format);
            audioSink->start(&audioBuffer);
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 500 && audioSink->state() == QtAudio::StoppedState &&
                   audioSink->error() == QtAudio::NoError) {
                application.processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            stateName = audioStateName(audioSink->state());
            errorName = audioErrorName(audioSink->error());
            if ((audioSink->state() == QtAudio::ActiveState ||
                 audioSink->state() == QtAudio::IdleState) &&
                audioSink->error() == QtAudio::NoError) {
                audioResult = QStringLiteral("started");
            }
        } else {
            errorName = QStringLiteral("unsupported-48000-stereo-s16");
        }
    }

    QString backend = QString::fromUtf8(qgetenv("QT_MEDIA_BACKEND"));
    if (backend.isEmpty()) backend = QStringLiteral("default");
    const QJsonObject result{
        {QStringLiteral("audio"), audioResult},
        {QStringLiteral("audioError"), errorName},
        {QStringLiteral("audioState"), stateName},
        {QStringLiteral("backend"), backend},
        {QStringLiteral("pid"), qint64(QCoreApplication::applicationPid())},
        {QStringLiteral("videoFramesObserved"), observedFrames},
    };
    QTextStream output(stdout);
    output << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    output.flush();

    if (holdMilliseconds > 0) QThread::msleep(static_cast<unsigned long>(holdMilliseconds));
    if (observedFrames < 2) return 6;
    if (audioResult != QStringLiteral("started") &&
        !(parser.isSet(allowNoAudio) && audioResult == QStringLiteral("no-device"))) {
        return 7;
    }
    return 0;
}
