#ifndef STREAMDECKMAPPINGSTORE_H
#define STREAMDECKMAPPINGSTORE_H

#include <QList>
#include <QMap>
#include <QString>
#include "settingsmanager.h"

// Cross-platform owner of the per-model Stream Deck mappings. No Qt signals;
// UIManager emits the QML-facing change notifications. Action ids match
// DeckAction.swift / UIManager::dispatchControlAction (see the plan header).
//
// Element types: 0 = key, 1 = dial-press, 2 = dial-turn.
class StreamDeckMappingStore {
public:
    enum ElementType { Key = 0, DialPress = 1, DialTurn = 2 };

    // Built-in fallback layout for a model+geometry. Mirrors the Swift
    // DeckAction.defaultMapping for keys; dials default to dial 0 rotate=jog,
    // press=play/pause; all other dials unbound.
    void resetToDefault(const QString &model, int keyCount, int dialCount);

    // Apply a learned binding with move/displace semantics. Returns true if
    // applied, false if the (action, element) pairing is invalid (caller keeps
    // listening). Validates: jog(8)/shuttle(10) -> DialTurn only;
    // timecode(20)/speed(21) -> Key only; everything else -> Key or DialPress.
    bool bind(const QString &model, int action, ElementType type, int index,
              int keyCount, int dialCount);

    // Remove an action from whichever control holds it (no-op if unbound).
    void clear(const QString &model, int action);

    // Ensure the model's rows exist and fit the live geometry (creates the
    // default layout if the model is new; clamps saved rows otherwise).
    // Called on connect before pushing maps to the deck.
    void clampToGeometry(const QString &model, int keyCount, int dialCount) {
        ensureModel(model, keyCount, dialCount);
    }

    // Human-readable current binding, e.g. "Key 5", "Dial 2 turn",
    // "Dial 0 press", or "Unassigned".
    QString bindingLabel(const QString &model, int action) const;

    // Accessors (also used to push to the bridge). Absent model -> empty list.
    QList<int> keyMap(const QString &model) const { return m_keyMaps.value(model); }
    QList<int> dialPressMap(const QString &model) const { return m_dialPressMaps.value(model); }
    QList<int> dialRotateMap(const QString &model) const { return m_dialRotateMaps.value(model); }
    bool hasModel(const QString &model) const { return m_keyMaps.contains(model); }

    // AppSettings <-> store. loadFrom prunes indices that exceed the live
    // geometry for the connected model (see ensureModel).
    void loadFrom(const AppSettings &settings);
    void writeTo(AppSettings &settings) const;

    // Validity helper exposed for tests and the editor.
    static bool isValidBinding(int action, ElementType type);

private:
    // Guarantee the three rows for `model` exist and are sized to the geometry,
    // creating the default layout if the model is new.
    void ensureModel(const QString &model, int keyCount, int dialCount);
    QList<int> &rowFor(ElementType type, const QString &model);

    QMap<QString, QList<int>> m_keyMaps;
    QMap<QString, QList<int>> m_dialPressMaps;
    QMap<QString, QList<int>> m_dialRotateMaps;
};

// Pure shuttle-ladder step. Snaps `currentSpeed` to the nearest rung of
// {-5,-2,-1,0,1,2,5}, moves `delta` rungs (clamped), and returns the new
// speed + whether playback should run (false only at rung 0).
struct ShuttleResult { double speed; bool playing; };
ShuttleResult shuttleLadderStep(double currentSpeed, int delta);

#endif // STREAMDECKMAPPINGSTORE_H
