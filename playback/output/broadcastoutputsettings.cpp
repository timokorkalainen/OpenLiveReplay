#include "playback/output/broadcastoutputsettings.h"

#include <QVariantMap>

namespace {
constexpr auto kSenderName = "senderName";

QString senderNameKey() {
    return QString::fromLatin1(kSenderName);
}

QList<OutputBusId> orderedBuses(int feedCount) {
    QList<OutputBusId> buses;
    for (int feed = 0; feed < qMax(0, feedCount); ++feed) {
        buses.append(OutputBusId::feed(feed));
    }
    buses.append(OutputBusId::multiview());
    buses.append(OutputBusId::pgm());
    return buses;
}

const OutputTargetAssignment* findAssignment(const QList<OutputTargetAssignment>& outputs,
                                             OutputTargetKind kind, OutputBusId bus) {
    for (const OutputTargetAssignment& assignment : outputs) {
        if (assignment.kind == kind && assignment.sourceBus == bus) return &assignment;
    }
    return nullptr;
}
} // namespace

namespace BroadcastOutputSettings {

QList<OutputTargetAssignment> ensureTargets(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind) {
    QList<OutputTargetAssignment> result;
    for (const OutputTargetAssignment& assignment : outputs) {
        if (assignment.kind != kind) result.append(assignment);
    }

    for (OutputBusId bus : orderedBuses(feedCount)) {
        OutputTargetAssignment assignment;
        if (const OutputTargetAssignment* existing = findAssignment(outputs, kind, bus)) {
            assignment = *existing;
        }
        assignment.id = targetId(bus, kind);
        assignment.sourceBus = bus;
        assignment.kind = kind;
        if (!assignment.settings.contains(senderNameKey()) ||
            assignment.settings.value(senderNameKey()).toString().trimmed().isEmpty()) {
            assignment.settings.insert(senderNameKey(), defaultSenderName(bus));
        }
        result.append(assignment);
    }
    return result;
}

QList<OutputTargetAssignment> setEnabled(const QList<OutputTargetAssignment>& outputs,
                                         int feedCount, OutputTargetKind kind, OutputBusId bus,
                                         bool enabled) {
    QList<OutputTargetAssignment> result = ensureTargets(outputs, feedCount, kind);
    for (OutputTargetAssignment& assignment : result) {
        if (assignment.kind == kind && assignment.sourceBus == bus) {
            assignment.enabled = enabled;
            break;
        }
    }
    return result;
}

QList<OutputTargetAssignment> setSenderName(const QList<OutputTargetAssignment>& outputs,
                                            int feedCount, OutputTargetKind kind, OutputBusId bus,
                                            const QString& senderName) {
    QList<OutputTargetAssignment> result = ensureTargets(outputs, feedCount, kind);
    for (OutputTargetAssignment& assignment : result) {
        if (assignment.kind == kind && assignment.sourceBus == bus) {
            const QString normalized = senderName.trimmed();
            assignment.settings.insert(senderNameKey(),
                                       normalized.isEmpty() ? defaultSenderName(bus) : normalized);
            break;
        }
    }
    return result;
}

bool isEnabled(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
               OutputBusId bus) {
    const OutputTargetAssignment* assignment = findAssignment(outputs, kind, bus);
    return assignment && assignment->enabled;
}

QString senderName(const QList<OutputTargetAssignment>& outputs, OutputTargetKind kind,
                   OutputBusId bus) {
    const OutputTargetAssignment* assignment = findAssignment(outputs, kind, bus);
    if (!assignment) return defaultSenderName(bus);
    const QString name = assignment->settings.value(senderNameKey()).toString().trimmed();
    return name.isEmpty() ? defaultSenderName(bus) : name;
}

QVariantList rows(const QList<OutputTargetAssignment>& outputs, int feedCount,
                  OutputTargetKind kind) {
    const QList<OutputTargetAssignment> normalized = ensureTargets(outputs, feedCount, kind);

    QVariantList list;
    for (OutputBusId bus : orderedBuses(feedCount)) {
        const OutputTargetAssignment* assignment = findAssignment(normalized, kind, bus);
        if (!assignment) continue;

        QVariantMap row;
        row.insert(QStringLiteral("id"), assignment->id);
        row.insert(QStringLiteral("kind"), outputTargetKindName(kind));
        row.insert(QStringLiteral("busKind"), busKindName(bus));
        row.insert(QStringLiteral("feedIndex"),
                   bus.kind == OutputBusKind::Feed ? bus.index : -1);
        row.insert(QStringLiteral("label"), label(bus));
        row.insert(QStringLiteral("enabled"), assignment->enabled);
        row.insert(QStringLiteral("senderName"), senderName(normalized, kind, bus));
        list.append(row);
    }
    return list;
}

OutputBusId busFromUiKey(const QString& busKind, int feedIndex) {
    if (busKind == QStringLiteral("feed")) return OutputBusId::feed(qMax(0, feedIndex));
    if (busKind == QStringLiteral("multiview")) return OutputBusId::multiview();
    return OutputBusId::pgm();
}

QString busKindName(OutputBusId bus) {
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("feed");
    case OutputBusKind::Multiview:
        return QStringLiteral("multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("pgm");
    }
    return QStringLiteral("pgm");
}

QString label(OutputBusId bus) {
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("Feed %1").arg(bus.index + 1);
    case OutputBusKind::Multiview:
        return QStringLiteral("Multiview");
    case OutputBusKind::Pgm:
        return QStringLiteral("PGM");
    }
    return QStringLiteral("PGM");
}

QString defaultSenderName(OutputBusId bus) {
    return QStringLiteral("OpenLiveReplay %1").arg(label(bus));
}

QString targetId(OutputBusId bus, OutputTargetKind kind) {
    const QString suffix = outputTargetKindName(kind);
    switch (bus.kind) {
    case OutputBusKind::Feed:
        return QStringLiteral("feed%1-%2").arg(bus.index).arg(suffix);
    case OutputBusKind::Multiview:
        return QStringLiteral("multiview-%1").arg(suffix);
    case OutputBusKind::Pgm:
        return QStringLiteral("pgm-%1").arg(suffix);
    }
    return QStringLiteral("pgm-%1").arg(suffix);
}

} // namespace BroadcastOutputSettings
