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
#include <cstdlib>

#include "recorder_engine/replaymanager.h"

namespace {
QString argValue(const QStringList& args, const QString& flag, const QString& fallback) {
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size()) return args.at(i + 1);
    return fallback;
}

void stderrMessageHandler(QtMsgType, const QMessageLogContext&, const QString& message) {
    fprintf(stderr, "%s\n", qPrintable(message));
    fflush(stderr);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.contains(QStringLiteral("--qt-log-stderr"))) {
        qInstallMessageHandler(stderrMessageHandler);
    }

    const QString url = argValue(args, QStringLiteral("--url"), QString());
    const QString name = argValue(args, QStringLiteral("--name"), QStringLiteral("olr_e2e"));
    const int seconds = argValue(args, QStringLiteral("--seconds"), QStringLiteral("6")).toInt();
    const int width = argValue(args, QStringLiteral("--width"), QStringLiteral("640")).toInt();
    const int height = argValue(args, QStringLiteral("--height"), QStringLiteral("480")).toInt();
    const int fps = argValue(args, QStringLiteral("--fps"), QStringLiteral("30")).toInt();
    // Where recordings land. The engine honors this (ReplayManager ->
    // Muxer::setOutputDirectory); the driver points it at a temp dir so the
    // test is hermetic. Empty -> engine default (~/Documents/videos).
    const QString outdir = argValue(args, QStringLiteral("--outdir"), QString());

    if (url.isEmpty()) {
        fprintf(stderr, "record_harness: --url is required\n");
        return 2;
    }

    // Multi-track fixtures for playback tests: OLR_VIEWS (default 2) controls
    // how many view-tracks the recording carries. View 0 maps to the single
    // real source; the remaining views are unmapped (blue fill). The resulting
    // .mkv has <vc> video tracks (1 real + vc-1 blue), <vc> audio, and <vc>
    // subtitle tracks — what the playback worker decodes against.
    int vc = 2;
    if (const char* env = std::getenv("OLR_VIEWS")) {
        const int parsed = atoi(env);
        if (parsed >= 1) vc = parsed;
    }

    // viewNames: first = "TEST" (the real source), rest empty (blue fill).
    QStringList viewNames;
    viewNames.reserve(vc);
    viewNames << QStringLiteral("TEST");
    for (int v = 1; v < vc; ++v)
        viewNames << QString();

    // viewSlotMap: view 0 -> source 0; views 1..vc-1 -> -1 (unmapped/blue).
    QList<int> viewSlotMap;
    viewSlotMap.reserve(vc);
    viewSlotMap << 0;
    for (int v = 1; v < vc; ++v)
        viewSlotMap << -1;

    ReplayManager rm;
    rm.setSourceUrls({url});
    rm.setSourceNames({QStringLiteral("E2E")});
    rm.setViewCount(vc);
    rm.setViewNames(viewNames);
    if (!outdir.isEmpty()) rm.setOutputDirectory(outdir);
    rm.setBaseFileName(name);
    rm.setVideoWidth(width);
    rm.setVideoHeight(height);
    rm.setFps(fps);

    rm.startRecording();
    if (!rm.isRecording()) {
        fprintf(stderr, "record_harness: startRecording() failed (engine not recording)\n");
        return 4;
    }
    // Apply the view mapping (must happen after the workers exist).
    rm.updateViewMapping(viewSlotMap);

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
