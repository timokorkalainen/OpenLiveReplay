#include "streamdeck/streamdeckmappingstore.h"

#include <array>
#include <cmath>

namespace {

constexpr int kUnbound = -1;

const QList<int> kKeyPriority = {9, 0, 4, 5, 7, 3, 1, 2, 6, 20, 21};

const std::array<double, 7> kShuttleLadder = {-5.0, -2.0, -1.0, 0.0, 1.0, 2.0, 5.0};

QList<int> defaultKeyRow(int keyCount) {
    QList<int> row;
    row.reserve(keyCount);
    for (int i = 0; i < keyCount; ++i)
        row.append(i < static_cast<int>(kKeyPriority.size()) ? kKeyPriority.at(i) : kUnbound);
    return row;
}

QList<int> blankRow(int n) { return QList<int>(n, kUnbound); }

void removeAction(QList<int> &row, int action) {
    for (int &v : row)
        if (v == action) v = kUnbound;
}

QList<int> fit(const QList<int> &saved, int n) {
    if (n <= 0) return {};
    QList<int> row = saved.mid(0, n);
    while (row.size() < n) row.append(kUnbound);
    return row;
}

} // namespace

bool StreamDeckMappingStore::isValidBinding(int action, ElementType type) {
    const bool rotateOnly = (action == 8 || action == 10);
    const bool keyOnly = (action == 20 || action == 21);
    switch (type) {
    case DialTurn:  return rotateOnly;
    case Key:       return !rotateOnly;
    case DialPress: return !rotateOnly && !keyOnly;
    }
    return false;
}

void StreamDeckMappingStore::resetToDefault(const QString &model, int keyCount, int dialCount) {
    if (model == QLatin1String("pedal")) {
        // Foot switches, no displays: play/pause, step back, step forward.
        QList<int> row;
        const int pedal[3] = {0, 7, 3};
        for (int i = 0; i < keyCount; ++i)
            row.append(i < 3 ? pedal[i] : -1);
        m_keyMaps[model] = row;
    } else {
        m_keyMaps[model] = defaultKeyRow(keyCount);
    }
    QList<int> rotate = blankRow(dialCount);
    QList<int> press = blankRow(dialCount);
    if (dialCount > 0) {
        rotate[0] = 8;   // jog wheel on dial 0
        // Dial presses default unbound: play/pause lives on a key, and the
        // move/displace model forbids one action on two controls. Users can
        // learn a dial press explicitly.
    }
    m_dialRotateMaps[model] = rotate;
    m_dialPressMaps[model] = press;
}

void StreamDeckMappingStore::ensureModel(const QString &model, int keyCount, int dialCount) {
    if (!m_keyMaps.contains(model)) {
        resetToDefault(model, keyCount, dialCount);
        return;
    }
    m_keyMaps[model] = fit(m_keyMaps.value(model), keyCount);
    m_dialRotateMaps[model] = fit(m_dialRotateMaps.value(model), dialCount);
    m_dialPressMaps[model] = fit(m_dialPressMaps.value(model), dialCount);
}

QList<int> &StreamDeckMappingStore::rowFor(ElementType type, const QString &model) {
    switch (type) {
    case Key:       return m_keyMaps[model];
    case DialPress: return m_dialPressMaps[model];
    case DialTurn:  return m_dialRotateMaps[model];
    }
    return m_keyMaps[model];
}

bool StreamDeckMappingStore::bind(const QString &model, int action, ElementType type,
                                  int index, int keyCount, int dialCount) {
    if (!isValidBinding(action, type)) return false;
    ensureModel(model, keyCount, dialCount);
    QList<int> &target = rowFor(type, model);
    if (index < 0 || index >= target.size()) return false;
    removeAction(m_keyMaps[model], action);
    removeAction(m_dialPressMaps[model], action);
    removeAction(m_dialRotateMaps[model], action);
    target[index] = action;
    return true;
}

void StreamDeckMappingStore::clear(const QString &model, int action) {
    if (!m_keyMaps.contains(model)) return;
    removeAction(m_keyMaps[model], action);
    removeAction(m_dialPressMaps[model], action);
    removeAction(m_dialRotateMaps[model], action);
}

QString StreamDeckMappingStore::bindingLabel(const QString &model, int action) const {
    const int k = m_keyMaps.value(model).indexOf(action);
    if (k >= 0) return QStringLiteral("Key %1").arg(k);
    const int p = m_dialPressMaps.value(model).indexOf(action);
    if (p >= 0) return QStringLiteral("Dial %1 press").arg(p);
    const int r = m_dialRotateMaps.value(model).indexOf(action);
    if (r >= 0) return QStringLiteral("Dial %1 turn").arg(r);
    return QStringLiteral("Unassigned");
}

void StreamDeckMappingStore::loadFrom(const AppSettings &settings) {
    m_keyMaps = settings.streamDeckKeyMaps;
    m_dialPressMaps = settings.streamDeckDialPressMaps;
    m_dialRotateMaps = settings.streamDeckDialRotateMaps;
}

void StreamDeckMappingStore::writeTo(AppSettings &settings) const {
    settings.streamDeckKeyMaps = m_keyMaps;
    settings.streamDeckDialPressMaps = m_dialPressMaps;
    settings.streamDeckDialRotateMaps = m_dialRotateMaps;
}

ShuttleResult shuttleLadderStep(double currentSpeed, int delta) {
    int idx = 0;
    double best = std::abs(currentSpeed - kShuttleLadder[0]);
    for (int i = 1; i < static_cast<int>(kShuttleLadder.size()); ++i) {
        const double d = std::abs(currentSpeed - kShuttleLadder[static_cast<size_t>(i)]);
        if (d < best) { best = d; idx = i; }
    }
    idx += delta;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(kShuttleLadder.size()))
        idx = static_cast<int>(kShuttleLadder.size()) - 1;
    const double speed = kShuttleLadder[static_cast<size_t>(idx)];
    return ShuttleResult{speed, speed != 0.0};
}
