#ifndef OUTPUTTARGETASSIGNMENT_H
#define OUTPUTTARGETASSIGNMENT_H

#include "playback/output/outputtypes.h"

#include <QVariantMap>

struct OutputTargetAssignment {
    QString id;
    OutputBusId sourceBus = OutputBusId::pgm();
    OutputTargetKind kind = OutputTargetKind::QtPreview;
    bool enabled = false;
    QVariantMap settings;

    void setEnabled(bool on) { enabled = on; }
};

QString outputTargetKindName(OutputTargetKind kind);

#endif // OUTPUTTARGETASSIGNMENT_H
