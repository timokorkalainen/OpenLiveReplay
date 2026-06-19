#include "recorder_engine/benchmark/realcodecrunners.h"

#include "recorder_engine/codec/avcc.h"
#include "recorder_engine/codec/nativevideoencoder.h"
#include "recorder_engine/ingest/nativevideodecoder.h"
#include "recorder_engine/ingest/h26xaccessunit.h"

#include <QElapsedTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

// ---------------------------------------------------------------------------
// Shared aggregation helpers
// ---------------------------------------------------------------------------
namespace {

// RAII deleters for FFmpeg handles so the benchmark threads release every
// resource on all exit paths (the C3 early returns and the I1 catch(...))
// without manual frees — mirrors the RAII the H.264 runner already gets from
// unique_ptr<NativeVideoEncoder> + a stack NativeVideoDecoder.
struct AvCodecContextDeleter {
    void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
};
struct AvPacketDeleter {
    void operator()(AVPacket* pkt) const { av_packet_free(&pkt); }
};
struct AvFrameDeleter {
    void operator()(AVFrame* frm) const { av_frame_free(&frm); }
};
using AvCodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct ThreadResult {
    int pairs = 0;
    int overrunCount = 0;       // I4: count frames over budget, not a sticky flag
    int frameCount = 0;         // I4: total frames counted for % calculation
    bool startupFailed = false; // C3: codec session construction failed
    double totalEncodeMs = 0.0;
    double totalDecodeMs = 0.0;
};

RampStepResult aggregate(int concurrency, int64_t framesRequired,
                         const std::vector<ThreadResult>& results) {
    RampStepResult r;
    r.concurrency = concurrency;
    r.framesRequired = framesRequired;
    bool anyStartupFailed = false;
    bool anyOverBudget = false;
    for (const auto& t : results) {
        r.framesProcessed += t.pairs;
        if (t.startupFailed) anyStartupFailed = true;
        // I4: thread is over-budget only if >10% of its frames exceeded the limit
        if (t.frameCount > 0 && t.overrunCount * 10 > t.frameCount) anyOverBudget = true;
        r.avgEncodeMs += t.totalEncodeMs;
        r.avgDecodeMs += t.totalDecodeMs;
    }
    // C3: startup failure forces budgetMet=false and flags startupFailed on result
    r.startupFailed = anyStartupFailed;
    r.budgetMet = !anyStartupFailed && !anyOverBudget;
    const int total = r.framesProcessed;
    if (total > 0) {
        r.avgEncodeMs /= total;
        r.avgDecodeMs /= total;
    }
    return r;
}

// Convert avcC length-prefixed NALUs to Annex B (\x00\x00\x00\x01 + payload).
QByteArray avccToAnnexB(const QByteArray& data) {
    QByteArray out;
    out.reserve(data.size() + 16);
    static const char kStart[4] = {'\x00', '\x00', '\x00', '\x01'};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.constData());
    const uint8_t* end = p + data.size();
    while (p + 4 <= end) {
        const uint32_t nalLen = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        p += 4;
        if (nalLen == 0 || p + nalLen > end) break;
        out.append(kStart, 4);
        out.append(reinterpret_cast<const char*>(p), int(nalLen));
        p += nalLen;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// MPEG-2 runner
// ---------------------------------------------------------------------------
RampStepResult Mpeg2CodecRunner::runStep(int concurrency, const BenchmarkConfig& cfg,
                                         const std::atomic<bool>& cancel) {
    const int64_t framesRequired =
        static_cast<int64_t>(concurrency) * cfg.fps * cfg.durationMsPerStep / 1000;
    const double budgetMs = 1000.0 / cfg.fps;
    const int windowMs = cfg.durationMsPerStep;

    std::vector<ThreadResult> results(concurrency);

    // C1: capture cancel by reference so threads can observe it
    auto threadFn = [&](int idx) {
        // I1: catch all exceptions so no std::terminate on thread exit
        try {
            ThreadResult& res = results[idx];

            // --- Set up FFmpeg MPEG-2 encoder (mirrors StreamWorker::setupEncoder) ---
            // RAII owners release encCtx/decCtx/pkt/decFrm on every exit path
            // (the C3 early returns AND the I1 catch(...) below) — no manual frees.
            const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
            if (!encoder) {
                res.startupFailed = true;
                return;
            } // C3
            AvCodecContextPtr encCtxOwner(avcodec_alloc_context3(encoder));
            if (!encCtxOwner) {
                res.startupFailed = true;
                return;
            } // C3
            AVCodecContext* encCtx = encCtxOwner.get();
            encCtx->width = cfg.width;
            encCtx->height = cfg.height;
            encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
            encCtx->time_base = {1, cfg.fps};
            encCtx->framerate = {cfg.fps, 1};
            encCtx->gop_size = 1; // intra-only
            encCtx->bit_rate = cfg.bitrate;
            if (avcodec_open2(encCtx, encoder, nullptr) < 0) {
                res.startupFailed = true;
                return; // C3
            }

            // --- Set up FFmpeg MPEG-2 decoder ---
            const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            if (!decoder) {
                res.startupFailed = true;
                return;
            } // C3
            AvCodecContextPtr decCtxOwner(avcodec_alloc_context3(decoder));
            if (!decCtxOwner) {
                res.startupFailed = true;
                return;
            } // C3
            AVCodecContext* decCtx = decCtxOwner.get();
            if (avcodec_open2(decCtx, decoder, nullptr) < 0) {
                res.startupFailed = true;
                return; // C3
            }

            AvPacketPtr pktOwner(av_packet_alloc());
            AvFramePtr decFrmOwner(av_frame_alloc());
            if (!pktOwner || !decFrmOwner) {
                res.startupFailed = true;
                return; // C3
            }
            AVPacket* pkt = pktOwner.get();
            AVFrame* decFrm = decFrmOwner.get();

            // Warm-up: encode+decode 2 frames before measurement to prime FFmpeg's
            // internal pipeline (avoids counting codec-init latency against the budget).
            {
                int64_t wPts = 1;
                for (int w = 0; w < 2; ++w) {
                    AVFrame* wf = makeSyntheticFrame(cfg.width, cfg.height, w);
                    if (!wf) break;
                    wf->pts = wPts++;
                    if (avcodec_send_frame(encCtx, wf) == 0) {
                        av_frame_free(&wf);
                        if (avcodec_receive_packet(encCtx, pkt) == 0) {
                            pkt->pts = AV_NOPTS_VALUE;
                            pkt->dts = AV_NOPTS_VALUE;
                            if (avcodec_send_packet(decCtx, pkt) == 0) {
                                while (avcodec_receive_frame(decCtx, decFrm) == 0)
                                    av_frame_unref(decFrm);
                            }
                            av_packet_unref(pkt);
                        }
                    } else {
                        av_frame_free(&wf);
                    }
                }
            }

            QElapsedTimer wall;
            wall.start();
            int seq = idx * 1000; // distinct sequence space per thread
            int64_t pts = 100;    // start well above 0 and warm-up pts to avoid conflicts

            // C1: also check cancel inside the measurement loop
            while (wall.elapsed() < windowMs && !cancel.load(std::memory_order_acquire)) {
                AVFrame* f = makeSyntheticFrame(cfg.width, cfg.height, seq++);
                if (!f) break;
                f->pts = pts++; // monotonically increasing, starting from 100

                QElapsedTimer encTimer;
                encTimer.start();

                bool encOk = false;
                if (avcodec_send_frame(encCtx, f) == 0) {
                    if (avcodec_receive_packet(encCtx, pkt) == 0) {
                        encOk = true;
                    }
                }
                av_frame_free(&f);
                const double encMs = encTimer.nsecsElapsed() / 1e6;

                if (!encOk) {
                    av_packet_unref(pkt);
                    continue;
                }

                QElapsedTimer decTimer;
                decTimer.start();

                bool decOk = false;
                pkt->pts = AV_NOPTS_VALUE;
                pkt->dts = AV_NOPTS_VALUE;
                if (avcodec_send_packet(decCtx, pkt) == 0) {
                    // Drain all output frames — MPEG-2 may buffer one frame.
                    while (avcodec_receive_frame(decCtx, decFrm) == 0) {
                        decOk = true;
                        av_frame_unref(decFrm);
                    }
                }
                av_packet_unref(pkt);
                const double decMs = decTimer.nsecsElapsed() / 1e6;

                if (!decOk) continue;

                const double totalMs = encMs + decMs;
                // I4: count overruns instead of setting a sticky flag
                ++res.frameCount;
                if (totalMs > budgetMs) ++res.overrunCount;
                res.totalEncodeMs += encMs;
                res.totalDecodeMs += decMs;
                ++res.pairs;
            }

            // RAII owners (encCtxOwner/decCtxOwner/pktOwner/decFrmOwner) free
            // every handle here and on all early-return / exception paths.
        } catch (...) {
            // I1: prevent exception from escaping the thread entry function;
            // treat as a startup failure so the step is definitively non-sustained.
            results[idx].startupFailed = true;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    try {
        for (int i = 0; i < concurrency; ++i)
            threads.emplace_back(threadFn, i);
        for (auto& t : threads)
            t.join();
    } catch (...) {
        // I1 Fix 1: if std::thread spawn or join throws (resource exhaustion),
        // join any successfully-started threads to avoid leaks, then mark step as failed.
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        RampStepResult r;
        r.concurrency = concurrency;
        r.framesRequired = framesRequired;
        r.startupFailed = true;
        r.budgetMet = false;
        return r;
    }

    return aggregate(concurrency, framesRequired, results);
}

// ---------------------------------------------------------------------------
// H.264 runner
// ---------------------------------------------------------------------------
bool H264CodecRunner::available() const {
    return queryNativeVideoEncodeCapabilities().h264 && queryNativeVideoDecodeCapabilities().h264;
}

RampStepResult H264CodecRunner::runStep(int concurrency, const BenchmarkConfig& cfg,
                                        const std::atomic<bool>& cancel) {
    const int64_t framesRequired =
        static_cast<int64_t>(concurrency) * cfg.fps * cfg.durationMsPerStep / 1000;
    const double budgetMs = 1000.0 / cfg.fps;
    const int windowMs = cfg.durationMsPerStep;

    std::vector<ThreadResult> results(concurrency);

    // C1: capture cancel by reference so threads can observe it
    auto threadFn = [&](int idx) {
        // I1: catch all exceptions so no std::terminate on thread exit
        try {
            ThreadResult& res = results[idx];

            // --- Create encoder ---
            NativeVideoEncoder::Config encCfg;
            encCfg.width = cfg.width;
            encCfg.height = cfg.height;
            encCfg.fpsNum = cfg.fps;
            encCfg.fpsDen = 1;
            encCfg.bitrate = cfg.bitrate;
            QString err;
            auto enc = NativeVideoEncoder::create(encCfg, &err);
            if (!enc) {
                res.startupFailed = true;
                return;
            } // C3: HW pool exhausted

            // --- Prime encoder to obtain avcC ---
            AVFrame* prime = makeSyntheticFrame(cfg.width, cfg.height, 0);
            if (!prime) {
                res.startupFailed = true;
                return;
            } // C3
            enc->encode(prime, 0, [](const QByteArray&, int64_t, bool) {}, &err);
            av_frame_free(&prime);
            const QByteArray avcc = enc->avccExtradata();
            if (avcc.isEmpty()) {
                res.startupFailed = true;
                return;
            } // C3

            // --- Parse SPS/PPS from avcC ---
            QList<QByteArray> sps, pps;
            if (!parseAvcc(avcc, &sps, &pps)) {
                res.startupFailed = true;
                return;
            } // C3

            H26xParameterSets paramSets;
            paramSets.h264Sps = sps;
            paramSets.h264Pps = pps;

            // --- Create decoder ---
            NativeVideoDecoder decoder(cfg.width, cfg.height);

            // Warm-up: run a few encode+decode cycles before the measurement window
            // so that VideoToolbox / GPU pipeline startup latency is excluded.
            for (int w = 0; w < 3; ++w) {
                AVFrame* wf = makeSyntheticFrame(cfg.width, cfg.height, w);
                if (!wf) break;
                QByteArray wData;
                enc->encode(
                    wf, w + 1, [&](const QByteArray& d, int64_t, bool) { wData = d; }, &err);
                av_frame_free(&wf);
                if (wData.isEmpty()) continue;
                const QByteArray wAnnex = avccToAnnexB(wData);
                if (wAnnex.isEmpty()) continue;
                CompressedAccessUnit wu;
                wu.codec = NativeVideoCodec::H264;
                wu.parameterSets = paramSets;
                wu.pts90k = w + 1;
                wu.dts90k = w + 1;
                wu.annexB = wAnnex;
                QString wErr;
                decoder.decode(wu, [](AVFrame* df) { av_frame_free(&df); }, &wErr);
            }

            QElapsedTimer wall;
            wall.start();
            int seq = idx * 1000 + 100; // offset to avoid priming/warm-up seq values

            // C1: also check cancel inside the measurement loop
            while (wall.elapsed() < windowMs && !cancel.load(std::memory_order_acquire)) {
                AVFrame* f = makeSyntheticFrame(cfg.width, cfg.height, seq++);
                if (!f) break;

                QElapsedTimer encTimer;
                encTimer.start();

                QByteArray encodedData;
                bool encOk = enc->encode(
                    f, res.pairs,
                    [&](const QByteArray& data, int64_t, bool) {
                        encodedData = data; // avcC length-prefixed
                    },
                    &err);
                av_frame_free(&f);
                const double encMs = encTimer.nsecsElapsed() / 1e6;

                if (!encOk || encodedData.isEmpty()) continue;

                // Convert avcC length-prefixed to Annex B for the decoder
                const QByteArray annexB = avccToAnnexB(encodedData);
                if (annexB.isEmpty()) continue;

                CompressedAccessUnit unit;
                unit.codec = NativeVideoCodec::H264;
                unit.parameterSets = paramSets;
                unit.pts90k = res.pairs;
                unit.dts90k = res.pairs;
                unit.annexB = annexB;

                QElapsedTimer decTimer;
                decTimer.start();

                bool decOk = false;
                QString decErr;
                decoder.decode(
                    unit,
                    [&](AVFrame* df) {
                        decOk = true;
                        av_frame_free(&df);
                    },
                    &decErr);
                const double decMs = decTimer.nsecsElapsed() / 1e6;

                if (!decOk) continue;

                const double totalMs = encMs + decMs;
                // I4: count overruns instead of setting a sticky flag
                ++res.frameCount;
                if (totalMs > budgetMs) ++res.overrunCount;
                res.totalEncodeMs += encMs;
                res.totalDecodeMs += decMs;
                ++res.pairs;
            }
        } catch (...) {
            // I1: prevent exception from escaping the thread entry function;
            // treat as a startup failure so the step is definitively non-sustained.
            results[idx].startupFailed = true;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    try {
        for (int i = 0; i < concurrency; ++i)
            threads.emplace_back(threadFn, i);
        for (auto& t : threads)
            t.join();
    } catch (...) {
        // I1 Fix 1: if std::thread spawn or join throws (resource exhaustion),
        // join any successfully-started threads to avoid leaks, then mark step as failed.
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        RampStepResult r;
        r.concurrency = concurrency;
        r.framesRequired = framesRequired;
        r.startupFailed = true;
        r.budgetMet = false;
        return r;
    }

    return aggregate(concurrency, framesRequired, results);
}
