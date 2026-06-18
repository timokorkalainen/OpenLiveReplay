// Writes the foundation NDI marker as raw planar frames for muxing into a test MKV:
//   <out>.yuv : YUV420P (256x144), luma = marker (counter + flash), chroma = neutral 128.
//   <out>.pcm : S16LE interleaved stereo @ 48 kHz (marker beep on flash frames).
// Pure (no NDI). usage: ndi_marker_mkv_source <out-prefix> <seconds>
#include <QByteArray>
#include <QFile>

#include <cstdio>

#include "tests/e2e/ndi_output_marker.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_marker_mkv_source <out-prefix> <seconds>\n");
        return 2;
    }
    const QString prefix = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();

    NdiOutputMarkerConfig mk; // defaults must match the probe/sender
    const qint64 frames = qint64(seconds * mk.fpsNum / mk.fpsDen);
    const int chromaW = mk.width / 2;
    const int chromaH = mk.height / 2;
    const QByteArray neutralU(chromaW * chromaH, char(128));

    QFile yuv(prefix + ".yuv");
    QFile pcm(prefix + ".pcm");
    if (!yuv.open(QIODevice::WriteOnly) || !pcm.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "[ndi_marker_mkv_source] cannot open output files\n");
        return 1;
    }
    for (qint64 i = 0; i < frames; ++i) {
        const QByteArray y = ndiMarkerLumaPlane(mk, i); // width*height
        yuv.write(y);
        yuv.write(neutralU); // U
        yuv.write(neutralU); // V
        pcm.write(ndiMarkerAudioS16(mk, i));
    }
    yuv.close();
    pcm.close();
    fprintf(stderr, "[ndi_marker_mkv_source] wrote %lld frames (%dx%d @ %d/%d)\n",
            (long long) frames, mk.width, mk.height, mk.fpsNum, mk.fpsDen);
    return 0;
}
