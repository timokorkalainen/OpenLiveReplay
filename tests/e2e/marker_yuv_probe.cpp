// Reads raw single-plane luma frames (width*height bytes each) from stdin, decodes the
// per-frame marker counter, and reports continuity of the recorded marker sequence. Tier (c)
// uses it to validate the NDI ingest -> record segment, fed by ffmpeg decoding the recorded
// MKV's luma:  ffmpeg -i marker.mkv -map 0:v:0 -f rawvideo -pix_fmt gray - | marker_yuv_probe 256
// 144 Pure (no NDI). The marker geometry is tied to its config, so only 256x144 is accepted.
#include <QByteArray>
#include <QString>

#include <cstdio>
#include <vector>

#include "tests/e2e/ndi_output_marker.h"
#include "tests/e2e/ndi_recv_analysis.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: marker_yuv_probe <width> <height>  (raw gray frames on stdin)\n");
        return 2;
    }
    const int width = QString::fromUtf8(argv[1]).toInt();
    const int height = QString::fromUtf8(argv[2]).toInt();

    NdiOutputMarkerConfig mk; // marker cell geometry is tied to the config (256x144)
    if (width != mk.width || height != mk.height) {
        fprintf(stderr, "[marker_yuv_probe] marker requires %dx%d, got %dx%d\n", mk.width,
                mk.height, width, height);
        return 1;
    }

    const size_t frameBytes = size_t(width) * size_t(height);
    QByteArray buf(int(frameBytes), '\0');
    auto* ptr = reinterpret_cast<uchar*>(buf.data());

    std::vector<qint64> indices;
    qint64 maxGap = 0;
    qint64 prev = -1;
    while (true) {
        const size_t got = std::fread(ptr, 1, frameBytes, stdin);
        if (got < frameBytes) break; // EOF or short final read
        const qint64 idx = ndiMarkerDecodeIndex(mk, ptr, width);
        if (idx < 0) continue; // undecodable frame - skip
        if (prev >= 0 && idx > prev) maxGap = qMax(maxGap, idx - prev);
        prev = idx;
        indices.push_back(idx);
    }

    if (indices.empty()) {
        fprintf(stderr, "[marker_yuv_probe] no marker frames decoded\n");
        return 1;
    }
    const NdiContinuity cont = ndiAnalyzeContinuity(indices);
    printf("MKVMARK framesDecoded=%lld drops=%lld dupes=%lld reorders=%lld maxGapFrames=%lld "
           "firstIndex=%lld lastIndex=%lld\n",
           (long long) cont.framesReceived, (long long) cont.drops, (long long) cont.dupes,
           (long long) cont.reorders, (long long) maxGap, (long long) indices.front(),
           (long long) indices.back());
    fflush(stdout);
    return 0;
}
