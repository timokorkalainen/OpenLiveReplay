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

    rm.startRecording();
    if (!rm.isRecording()) {
        fprintf(stderr, "sync_harness: startRecording() failed (engine not recording)\n");
        return 4;
    }
    rm.updateViewMapping(viewSlotMap);

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
            app.quit();
        });
    });

    app.exec();
    return exitCode;
}
