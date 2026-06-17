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
#include <QHash>
#include <QVector>
#include <QPair>
#include <QList>
#include <QElapsedTimer>
#include <algorithm>
#include <QtGlobal>
#include <cstdio>

#include "recorder_engine/framerate.h"
#include "recorder_engine/replaymanager.h"
#include "recorder_engine/ingest/ingestsession.h"

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
    const FrameRate frameRate =
        parseFrameRate(argValue(args, QStringLiteral("--fps"), QStringLiteral("30")));
    // Where recordings land; the driver points this at a temp dir so the test
    // is hermetic. Empty -> engine default (~/Documents/videos).
    const QString outdir = argValue(args, QStringLiteral("--outdir"), QString());
    const int trimMs = argValue(args, QStringLiteral("--trim"), QStringLiteral("0")).toInt();
    const bool reportConnections = args.contains(QStringLiteral("--report-connections"));
    const bool reportConnectionEvents = args.contains(QStringLiteral("--report-connection-events"));
    const bool reportStats = args.contains(QStringLiteral("--report-stats"));

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
    rm.setFrameRate(frameRate);

    // Per-source connection observability. connectionChanged fires on EVERY real
    // transition (StreamWorker::setConnected emits only on change), queued to this
    // (main) thread, so the captures below are mutated single-threaded. Wired before
    // startRecording() so no early connect is missed.
    //  - connectedSources: distinct sources ever connected (for --report-connections).
    //  - connEvents: per source, a chronological (elapsedMs, connected) timeline
    //    (for --report-connection-events), used by the reconnect e2e to verify a
    //    real up->down->up sequence.
    QSet<int> connectedSources;
    QHash<int, QVector<QPair<qint64, bool>>> connEvents;
    QHash<int, SrtStats> latestStats;
    QElapsedTimer connTimer;
    connTimer.start();
    QObject::connect(&rm, &ReplayManager::sourceConnectionChanged, &app,
                     [&connectedSources, &connEvents, &connTimer](int sourceIndex, bool connected) {
                         if (connected) connectedSources.insert(sourceIndex);
                         connEvents[sourceIndex].append(qMakePair(connTimer.elapsed(), connected));
                     });
    QObject::connect(&rm, &ReplayManager::sourceStatsUpdated, &app,
                     [&latestStats](int sourceIndex, SrtStats stats) {
                         latestStats.insert(sourceIndex, stats);
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
        // Emit connection telemetry BEFORE stopRecording(). The connected-source set
        // and per-source event timeline are final at the record's end, and a dead or
        // closing SRT source can stall stopRecording() teardown for minutes (SRT's
        // global-lock contention under load). Printing here guarantees the count and
        // timeline reach the driver even when teardown hangs; the driver bounds the
        // harness with a watchdog and reads these lines. The MKV path still prints
        // post-flush below (it needs the muxer trailer written).
        if (reportConnections) {
            fprintf(stderr, "connected=%d\n", int(connectedSources.size()));
            fflush(stderr);
        }
        if (reportConnectionEvents) {
            QList<int> srcs = connEvents.keys();
            std::sort(srcs.begin(), srcs.end());
            for (int src : srcs) {
                QString line = QStringLiteral("conn_events src=%1").arg(src);
                const QVector<QPair<qint64, bool>>& evs = connEvents.value(src);
                for (const QPair<qint64, bool>& ev : evs)
                    line += QStringLiteral(" %1:%2").arg(ev.first).arg(
                        ev.second ? QStringLiteral("up") : QStringLiteral("down"));
                fprintf(stderr, "%s\n", qPrintable(line));
            }
            fflush(stderr);
        }
        if (reportStats) {
            QList<int> srcs = latestStats.keys();
            std::sort(srcs.begin(), srcs.end());
            for (int src : srcs) {
                const SrtStats& s = latestStats.value(src);
                fprintf(stderr, "stats src=%d recv=%lld retrans=%lld loss=%lld drop=%lld\n", src,
                        (long long) s.recvTotal, (long long) s.retransTotal,
                        (long long) s.lossTotal, (long long) s.dropTotal);
            }
            fflush(stderr);
        }
        rm.stopRecording();
        // Give worker threads / muxer trailer a moment to flush, then report the path.
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
