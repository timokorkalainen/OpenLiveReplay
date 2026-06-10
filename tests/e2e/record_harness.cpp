// Headless recording harness for end-to-end tests.
//
// Drives the real ReplayManager recording engine against a single source URL
// for a fixed wall-clock duration, then stops and prints the absolute path of
// the produced .mkv on stdout (last line). The surrounding driver script
// (run_record_e2e.sh) feeds it a synthetic FFmpeg stream and probes the output.
//
// Output lands in <Documents>/videos/<name>...mkv per Muxer::getVideoPath();
// the driver redirects $HOME to a temp dir to keep this hermetic.
//
// Usage:
//   record_harness --url <url> --name <base> --seconds <n>
//                  [--width W] [--height H] [--fps F]
#include <QCoreApplication>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <cstdio>

#include "recorder_engine/replaymanager.h"

namespace {
QString argValue(const QStringList& args, const QString& flag, const QString& fallback) {
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size()) return args.at(i + 1);
    return fallback;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QString url = argValue(args, QStringLiteral("--url"), QString());
    const QString name = argValue(args, QStringLiteral("--name"), QStringLiteral("olr_e2e"));
    const int seconds = argValue(args, QStringLiteral("--seconds"), QStringLiteral("6")).toInt();
    const int width = argValue(args, QStringLiteral("--width"), QStringLiteral("640")).toInt();
    const int height = argValue(args, QStringLiteral("--height"), QStringLiteral("480")).toInt();
    const int fps = argValue(args, QStringLiteral("--fps"), QStringLiteral("30")).toInt();

    if (url.isEmpty()) {
        fprintf(stderr, "record_harness: --url is required\n");
        return 2;
    }

    ReplayManager rm;
    rm.setSourceUrls({url});
    rm.setSourceNames({QStringLiteral("E2E")});
    rm.setViewCount(1);
    rm.setViewNames({QStringLiteral("E2E")});
    rm.setOutputDirectory(QStringLiteral("/tmp"));
    rm.setBaseFileName(name);
    rm.setVideoWidth(width);
    rm.setVideoHeight(height);
    rm.setFps(fps);

    rm.startRecording();
    if (!rm.isRecording()) {
        fprintf(stderr, "record_harness: startRecording() failed (engine not recording)\n");
        return 4;
    }
    // Map view 0 -> source 0 (must happen after the workers exist).
    rm.updateViewMapping({0});

    const QString outPath = rm.getVideoPath();

    int exitCode = 0;
    QTimer::singleShot(seconds * 1000, &app, [&]() {
        rm.stopRecording();
        // Give worker threads / muxer trailer a moment to flush, then report.
        QTimer::singleShot(700, &app, [&]() {
            if (outPath.isEmpty()) {
                fprintf(stderr, "record_harness: engine reported no output path\n");
                exitCode = 3;
            } else {
                // Last stdout line is the artifact path the driver probes.
                printf("%s\n", qPrintable(outPath));
                fflush(stdout);
            }
            app.quit();
        });
    });

    app.exec();
    return exitCode;
}
