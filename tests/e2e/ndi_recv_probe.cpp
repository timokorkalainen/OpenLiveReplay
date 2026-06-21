// Headless NDI receiver probe: discovers an OLR NDI source, captures video+audio for a
// bounded window, decodes the marker, and prints continuity / A-V sync / cadence metrics.
// Runtime-loaded (no NDI SDK at build time). Exits 77 (SKIP) if the runtime is absent or no
// source appears; 1 on a hard capture error; 0 otherwise (the driver decides pass/fail).
//
// usage: ndi_recv_probe <source-name-substring> <capture-seconds>
// env: OLR_NDI_RUNTIME_LIBRARY (override), OLR_NDI_FIND_TIMEOUT_MS (default 5000)
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLibrary>
#include <QString>

#include <cstdio>
#include <vector>

#include "playback/output/ndiabi.h"
#include "playback/output/ndiruntimepaths.h"
#include "tests/e2e/ndi_output_marker.h"
#include "tests/e2e/ndi_recv_analysis.h"

using namespace olr::ndi;

namespace {
constexpr int kSkip = 77;

struct Recv {
    QLibrary lib;
    NDIlib_initialize_fn init = nullptr;
    NDIlib_destroy_fn destroy = nullptr;
    NDIlib_find_create_v2_fn findCreate = nullptr;
    NDIlib_find_destroy_fn findDestroy = nullptr;
    NDIlib_find_wait_for_sources_fn findWait = nullptr;
    NDIlib_find_get_current_sources_fn findSources = nullptr;
    NDIlib_recv_create_v3_fn recvCreate = nullptr;
    NDIlib_recv_destroy_fn recvDestroy = nullptr;
    NDIlib_recv_capture_v3_fn recvCapture = nullptr;
    NDIlib_recv_free_video_v2_fn freeVideo = nullptr;
    NDIlib_recv_free_audio_v3_fn freeAudio = nullptr;

    bool load() {
        for (const QString& candidate : runtimeLibraryCandidates()) {
            if (candidate.isEmpty()) continue;
            lib.setFileName(candidate);
            if (!lib.load()) continue;
            init = reinterpret_cast<NDIlib_initialize_fn>(lib.resolve("NDIlib_initialize"));
            destroy = reinterpret_cast<NDIlib_destroy_fn>(lib.resolve("NDIlib_destroy"));
            findCreate =
                reinterpret_cast<NDIlib_find_create_v2_fn>(lib.resolve("NDIlib_find_create_v2"));
            findDestroy =
                reinterpret_cast<NDIlib_find_destroy_fn>(lib.resolve("NDIlib_find_destroy"));
            findWait = reinterpret_cast<NDIlib_find_wait_for_sources_fn>(
                lib.resolve("NDIlib_find_wait_for_sources"));
            findSources = reinterpret_cast<NDIlib_find_get_current_sources_fn>(
                lib.resolve("NDIlib_find_get_current_sources"));
            recvCreate =
                reinterpret_cast<NDIlib_recv_create_v3_fn>(lib.resolve("NDIlib_recv_create_v3"));
            recvDestroy =
                reinterpret_cast<NDIlib_recv_destroy_fn>(lib.resolve("NDIlib_recv_destroy"));
            recvCapture =
                reinterpret_cast<NDIlib_recv_capture_v3_fn>(lib.resolve("NDIlib_recv_capture_v3"));
            freeVideo = reinterpret_cast<NDIlib_recv_free_video_v2_fn>(
                lib.resolve("NDIlib_recv_free_video_v2"));
            freeAudio = reinterpret_cast<NDIlib_recv_free_audio_v3_fn>(
                lib.resolve("NDIlib_recv_free_audio_v3"));
            if (findCreate && findSources && recvCreate && recvDestroy && recvCapture &&
                freeVideo && freeAudio) {
                if (init) init();
                return true;
            }
            lib.unload();
        }
        return false;
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        fprintf(stderr, "usage: ndi_recv_probe <source-substring> <capture-seconds>\n");
        return 2;
    }
    const QString want = QString::fromUtf8(argv[1]);
    const double seconds = QString::fromUtf8(argv[2]).toDouble();
    const int findTimeoutMs = qEnvironmentVariableIntValue("OLR_NDI_FIND_TIMEOUT_MS") > 0
                                  ? qEnvironmentVariableIntValue("OLR_NDI_FIND_TIMEOUT_MS")
                                  : 5000;

    Recv ndi;
    if (!ndi.load()) {
        fprintf(stderr, "[ndi_recv_probe] NDI runtime not available - SKIP\n");
        return kSkip;
    }

    NDIlib_find_create_t findCfg;
    NDIlib_find_instance_t finder = ndi.findCreate(&findCfg);
    if (!finder) {
        fprintf(stderr, "[ndi_recv_probe] find_create failed - SKIP\n");
        return kSkip;
    }

    NDIlib_source_t chosen;
    bool found = false;
    QElapsedTimer findTimer;
    findTimer.start();
    while (findTimer.elapsed() < findTimeoutMs && !found) {
        if (ndi.findWait) ndi.findWait(finder, 1000);
        quint32 count = 0;
        const NDIlib_source_t* sources = ndi.findSources(finder, &count);
        for (quint32 i = 0; i < count; ++i) {
            const QString name =
                QString::fromUtf8(sources[i].p_ndi_name ? sources[i].p_ndi_name : "");
            if (name.contains(want)) {
                chosen = sources[i];
                // Copy the name string into a stable buffer (the source array is owned by NDI).
                static QByteArray nameBuf;
                nameBuf = name.toUtf8();
                chosen.p_ndi_name = nameBuf.constData();
                chosen.p_url_address = nullptr;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        fprintf(stderr, "[ndi_recv_probe] no source matching '%s' - SKIP\n",
                want.toUtf8().constData());
        if (ndi.findDestroy) ndi.findDestroy(finder);
        return kSkip;
    }

    NDIlib_recv_create_v3_t recvCfg;
    recvCfg.source_to_connect_to = chosen;
    // NDIlib_recv_color_format_fastest (100): for no-alpha sources NDI delivers UYVY.
    // The marker decoder expects raw luma, so we extract luma from UYVY before decode.
    recvCfg.color_format = 100;
    recvCfg.bandwidth = 100; // highest
    recvCfg.p_ndi_recv_name = "olr-ndi-recv-probe";
    NDIlib_recv_instance_t recv = ndi.recvCreate(&recvCfg);
    if (ndi.findDestroy) ndi.findDestroy(finder);
    if (!recv) {
        fprintf(stderr, "[ndi_recv_probe] recv_create failed\n");
        return 1;
    }

    NdiOutputMarkerConfig mk; // must match the sender's config defaults
    std::vector<qint64> indices;
    std::vector<double> arrivals;
    std::vector<qint64> flashes;
    std::vector<qint64> beeps;

    // Map audio to a frame index by counting received audio samples.
    // audioSampleBase is set once the first video frame is received so that the
    // audio-derived ordinal is anchored to the same capture-relative origin as the
    // video flash ordinals (NDI may buffer several audio frames before the probe
    // connects, so we must ignore those leading audio frames).
    qint64 audioSamplePos = 0;
    qint64 audioSampleBase = -1; // -1 = not yet anchored

    // Timecode round-trip: the sender stamps each frame's programme timecode = ptsMs * 10000
    // (ptsMs = idx*1000*fpsDen/fpsNum), so a received frame's timecode must equal the value
    // recomputed from its decoded marker index. vTcSynth/aTcSynth count frames that arrived with
    // the SDK synthesize sentinel (i.e. our programme timecode did NOT reach the wire).
    qint64 firstVideoTimecode = -1;
    qint64 vTcChecked = 0, vTcMatches = 0, vTcSynth = 0;
    qint64 aTcSeen = 0, aTcSynth = 0;
    const int samplesPerFrame = ndiMarkerSamplesPerFrame(mk);

    // Extract the luma plane from a UYVY buffer (the fastest NDI format for I420 sources).
    // UYVY layout: [U, Y0, V, Y1] per 4 bytes / 2 pixels; Y at odd bytes.
    // Returns a tight luma buffer (stride = width) for ndiMarkerDecode*.
    auto extractLumaFromUyvy = [](const uchar* src, int stride, int width, int height) {
        QByteArray luma(width * height, '\0');
        auto* dst = reinterpret_cast<uchar*>(luma.data());
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                // Y byte is at col*2+1 within a UYVY row.
                dst[row * width + col] = src[row * stride + col * 2 + 1];
            }
        }
        return luma;
    };

    QElapsedTimer run;
    run.start();
    while (run.elapsed() < qint64(seconds * 1000.0)) {
        NDIlib_video_frame_v2_t v;
        NDIlib_audio_frame_v3_t a;
        const int type = ndi.recvCapture(recv, &v, &a, nullptr, 200);
        if (type == FrameTypeVideo) {
            if (v.p_data && v.xres >= mk.width && v.yres >= mk.height) {
                // NDI delivers UYVY; extract luma before marker decode.
                const QByteArray luma =
                    extractLumaFromUyvy(reinterpret_cast<const uchar*>(v.p_data),
                                        v.line_stride_in_bytes, v.xres, v.yres);
                const auto* lumaPtr = reinterpret_cast<const uchar*>(luma.constData());
                const qint64 idx = ndiMarkerDecodeIndex(mk, lumaPtr, v.xres);
                if (idx >= 0) {
                    // Anchor the audio baseline to the first received video frame so that
                    // audio-derived ordinals align with capture-relative video ordinals.
                    if (audioSampleBase < 0) audioSampleBase = audioSamplePos;
                    // Verify the programme timecode round-tripped: it must equal the value the
                    // sender derived from this same decoded index (reorder-immune: each frame's
                    // timecode is checked against its own index).
                    const qint64 tc = v.timecode;
                    if (tc == kTimecodeSynthesize) {
                        ++vTcSynth;
                    } else {
                        if (firstVideoTimecode < 0) firstVideoTimecode = tc;
                        const qint64 expectedTc = (idx * 1000 * mk.fpsDen / mk.fpsNum) * 10000;
                        ++vTcChecked;
                        if (tc == expectedTc) ++vTcMatches;
                    }
                    indices.push_back(idx); // absolute index -> continuity (drops/dupes/reorders)
                    arrivals.push_back(run.elapsed() / 1000.0);
                    if (ndiMarkerDecodeFlash(mk, lumaPtr, v.xres)) {
                        // A-V sync: record the video ordinal of each flash. The analysis
                        // pairs flash[i] with beep[i] and measures jitter relative to the
                        // median offset (absorbing the constant NDI audio buffer delay).
                        flashes.push_back(qint64(indices.size()) - 1);
                    }
                }
            }
            ndi.freeVideo(recv, &v);
        } else if (type == FrameTypeAudio) {
            if (a.p_data && a.no_samples > 0) {
                // Audio shares the video tick's programme timecode (applyNdiFrameTiming), so a
                // received audio frame must not carry the synthesize sentinel.
                ++aTcSeen;
                if (a.timecode == kTimecodeSynthesize) ++aTcSynth;
                const double rms =
                    ndiMarkerAudioRmsFltp(reinterpret_cast<const float*>(a.p_data), a.no_samples);
                // Only count beeps after the audio baseline is anchored to the first video.
                // NOTE: this assumes one audio frame per video frame (true for our sink +
                // loopback). Under NDI audio re-chunking a beep could be double-counted or
                // diluted below threshold, desyncing the flash<->beep pairing; that can only
                // cause a false FAIL, never a vacuous pass. Follow-up: match beeps to flashes
                // by sample position / onset rather than by ordinal.
                if (rms > 0.05 && samplesPerFrame > 0 && audioSampleBase >= 0)
                    beeps.push_back((audioSamplePos - audioSampleBase) / samplesPerFrame);
                audioSamplePos += a.no_samples;
            }
            ndi.freeAudio(recv, &a);
        }
    }
    ndi.recvDestroy(recv);
    if (ndi.destroy) ndi.destroy();

    const NdiContinuity cont = ndiAnalyzeContinuity(indices);
    const int avSync = ndiAvSyncMaxFrames(flashes, beeps);
    const NdiCadence cad = ndiAnalyzeCadence(arrivals, mk.fpsNum, mk.fpsDen);

    printf(
        "NDIRECV source=%s framesReceived=%lld drops=%lld dupes=%lld reorders=%lld "
        "avSyncMaxFrames=%d maxGapFrames=%d meanRateHz=%.3f "
        "vTcFirst=%lld vTcChecked=%lld vTcMatches=%lld vTcSynth=%lld aTcSeen=%lld aTcSynth=%lld\n",
        want.toUtf8().constData(), (long long) cont.framesReceived, (long long) cont.drops,
        (long long) cont.dupes, (long long) cont.reorders, avSync, cad.maxGapFrames, cad.meanRateHz,
        (long long) firstVideoTimecode, (long long) vTcChecked, (long long) vTcMatches,
        (long long) vTcSynth, (long long) aTcSeen, (long long) aTcSynth);
    fflush(stdout);
    return 0;
}
