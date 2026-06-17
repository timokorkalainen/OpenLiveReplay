#include "playback/output/outputtargetassignment.h"

QString outputTargetKindName(OutputTargetKind kind) {
    switch (kind) {
    case OutputTargetKind::QtPreview:
        return QStringLiteral("qt-preview");
    case OutputTargetKind::DeckLinkSdiHdmi:
        return QStringLiteral("decklink-sdi-hdmi");
    case OutputTargetKind::DeckLinkIpSt2110:
        return QStringLiteral("decklink-ip-st2110");
    case OutputTargetKind::Ndi:
        return QStringLiteral("ndi");
    case OutputTargetKind::Omt:
        return QStringLiteral("omt");
    case OutputTargetKind::Aja:
        return QStringLiteral("aja");
    }
    return QStringLiteral("unknown");
}
