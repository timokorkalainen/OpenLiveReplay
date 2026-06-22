#ifndef OLR_MEDIAFOUNDATIONRUNTIME_H
#define OLR_MEDIAFOUNDATIONRUNTIME_H

#include <QString>

// Starts the process-wide Media Foundation runtime on Windows.
//
// Media Foundation hardware MFTs can outlive the short C++ wrapper call stack on
// driver-owned worker threads. Starting/stopping MF around every codec probe or
// benchmark pipeline is brittle, so Windows callers keep MFStartup process-wide
// and only balance COM apartments per thread.
bool ensureMediaFoundationRuntime(QString* error);

#endif // OLR_MEDIAFOUNDATIONRUNTIME_H
