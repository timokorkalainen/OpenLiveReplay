// Headless multi-source sync-measurement harness.
//
// Records N source URLs into an N-view MKV (view i <- source i, 1:1) using the
// real ReplayManager, for a fixed wall-clock duration, then prints the absolute
// path of the produced .mkv on stdout (last line). The driver (run_sync_e2e.sh)
// feeds it synchronized synthetic FFmpeg streams and measures marker timing in
// the output. No measurement or assertions live here.
//
// Usage:
//   sync_harness --url <U1> [--url <U2> ...] --outdir <dir> --seconds <n>
//                [--name <base>] [--width W] [--height H] [--fps F]
#include <QCoreApplication>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QtGlobal>
#include <cstdio>

#include "recorder_engine/replaymanager.h"

namespace {
QString argValue(const QStringList& args, const QString& flag, const QString& fallback) {
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size()) return args.at(i + 1);
    return fallback;
}
// All values following each occurrence of --url, in order.
QStringList allUrls(const QStringList& args) {
    QStringList urls;
    for (int i = 0; i + 1 < args.size(); ++i)
        if (args.at(i) == QStringLiteral("--url")) urls << args.at(i + 1);
    return urls;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    const QStringList urls = allUrls(args);
    const QString name = argValue(args, QStringLiteral("--name"), QStringLiteral("olr_sync"));
    const int seconds = argValue(args, QStringLiteral("--seconds"), QStringLiteral("8")).toInt();
    const int width = argValue(args, QStringLiteral("--width"), QStringLiteral("320")).toInt();
    const int height = argValue(args, QStringLiteral("--height"), QStringLiteral("240")).toInt();
    const int fps = argValue(args, QStringLiteral("--fps"), QStringLiteral("30")).toInt();
    // Where recordings land; the driver points this at a temp dir so the test
    // is hermetic. Empty -> engine default (~/Documents/videos).
    const QString outdir = argValue(args, QStringLiteral("--outdir"), QString());
    const int trimMs = argValue(args, QStringLiteral("--trim"), QStringLiteral("0")).toInt();
    const bool reportConnections = args.contains(QStringLiteral("--report-connections"));

    if (urls.isEmpty()) {
        fprintf(stderr, "sync_harness: at least one --url is required\n");
        return 2;
    }
    if (seconds <= 0) {
        fprintf(stderr, "sync_harness: --seconds must be a positive integer\n");
        return 2;
    }

    const int n = urls.size();

    // One view per source, mapped 1:1. View names: SRC0, SRC1, ...
    QStringList sourceNames, viewNames;
    QList<int> viewSlotMap;
    for (int i = 0; i < n; ++i) {
        sourceNames << QStringLiteral("SRC%1").arg(i);
        viewNames << QStringLiteral("SRC%1").arg(i);
        viewSlotMap << i;
    }

    ReplayManager rm;
    rm.setSourceUrls(urls);
    rm.setSourceNames(sourceNames);
    rm.setViewCount(n);
    rm.setViewNames(viewNames);
    if (!outdir.isEmpty()) rm.setOutputDirectory(outdir);
    rm.setBaseFileName(name);
    rm.setVideoWidth(width);
    rm.setVideoHeight(height);
    rm.setFps(fps);

    // Count distinct sources that reach the connected state. Queued to the app
    // (main) thread — the signal is emitted from a worker thread. Connected
    // before startRecording() so no early connect is missed.
    QSet<int> connectedSources;
    QObject::connect(&rm, &ReplayManager::sourceConnectionChanged, &app,
                     [&connectedSources](int sourceIndex, bool connected) {
                         if (connected) connectedSources.insert(sourceIndex);
                     });

    rm.startRecording();
    if (!rm.isRecording()) {
        fprintf(stderr, "sync_harness: startRecording() failed (engine not recording)\n");
        return 4;
    }
    rm.updateViewMapping(viewSlotMap);

    if (trimMs != 0 && n >= 1) {
        rm.updateSourceTrim(n - 1, trimMs);
    }

    const QString outPath = rm.getVideoPath();

    int exitCode = 0;
    QTimer::singleShot(seconds * 1000, &app, [&]() {
        rm.stopRecording();
        // Give worker threads / muxer trailer a moment to flush, then report.
        QTimer::singleShot(700, &app, [&]() {
            if (outPath.isEmpty()) {
                fprintf(stderr, "sync_harness: engine reported no output path\n");
                exitCode = 3;
            } else {
                printf("%s\n", qPrintable(outPath));
                fflush(stdout);
            }
            if (reportConnections) {
                fprintf(stderr, "connected=%d\n", int(connectedSources.size()));
                fflush(stderr);
            }
            app.quit();
        });
    });

    app.exec();
    return exitCode;
}
