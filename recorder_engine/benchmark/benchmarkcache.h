#ifndef OLR_BENCHMARKCACHE_H
#define OLR_BENCHMARKCACHE_H

#include "recorder_engine/benchmark/benchmarktypes.h"

#include <QString>

// Returns a stable device identifier string (e.g. "Apple M1 Pro arm64").
QString benchmarkDeviceLabel();

// Persist/restore a CodecBenchmarkResult as JSON.
// Returns false on I/O or format errors.
bool saveBenchmarkResult(const QString& path, const CodecBenchmarkResult& result);
bool loadBenchmarkResult(const QString& path, CodecBenchmarkResult& out);

// Returns true iff the cached result's deviceLabel and resolution match the given values.
// Used to invalidate the cache on device change or resolution change.
bool benchmarkResultMatches(const CodecBenchmarkResult& cached, const QString& deviceLabel,
                            const QString& resolution);

#endif // OLR_BENCHMARKCACHE_H
