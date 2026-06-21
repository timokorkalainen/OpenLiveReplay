// Phase-0 probe P0.3 / D11: stand up one QRhi on a dedicated render context and
// time a realistic per-frame import -> offscreen composite -> readback loop
// (not a trivial clear). Measurement only; links Qt RHI, never the product.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QThread>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFrames = 240;

struct ProbeResult {
    int exitCode = 1;
    double totalMs = 0.0;
    double importMs = 0.0;
    double compositeMs = 0.0;
    double readbackMs = 0.0;
};

struct Probe {
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiTexture> srcTex;
    std::unique_ptr<QRhiTexture> dstTex;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;

    bool init() {
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
        if (!rhi) return false;

        srcTex.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight), 1,
                                     QRhiTexture::UsedAsTransferSource));
        if (!srcTex->create()) return false;

        dstTex.reset(
            rhi->newTexture(QRhiTexture::RGBA8, QSize(kWidth, kHeight), 1,
                            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!dstTex->create()) return false;

        rt.reset(rhi->newTextureRenderTarget(
            QRhiTextureRenderTargetDescription(QRhiColorAttachment(dstTex.get()))));
        rp.reset(rt->newCompatibleRenderPassDescriptor());
        rt->setRenderPassDescriptor(rp.get());
        return rt->create();
    }

    bool frame(double* importMs, double* compositeMs, double* readbackMs) {
        QImage img(kWidth, kHeight, QImage::Format_RGBA8888);
        img.fill(Qt::gray);

        QRhiCommandBuffer* cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return false;

        QElapsedTimer t;
        t.start();
        QRhiResourceUpdateBatch* upload = rhi->nextResourceUpdateBatch();
        upload->uploadTexture(srcTex.get(),
                              QRhiTextureUploadDescription(QRhiTextureUploadEntry(
                                  0, 0, QRhiTextureSubresourceUploadDescription(img))));
        *importMs = t.nsecsElapsed() / 1.0e6;

        t.restart();
        cb->beginPass(rt.get(), QColor(0, 0, 0, 255), {1.0f, 0}, upload);
        cb->endPass();
        QRhiResourceUpdateBatch* copyAndReadback = rhi->nextResourceUpdateBatch();
        copyAndReadback->copyTexture(dstTex.get(), srcTex.get());
        *compositeMs = t.nsecsElapsed() / 1.0e6;

        t.restart();
        QRhiReadbackResult rb;
        bool done = false;
        rb.completed = [&done]() { done = true; };
        copyAndReadback->readBackTexture(QRhiReadbackDescription(dstTex.get()), &rb);
        cb->resourceUpdate(copyAndReadback);
        rhi->endOffscreenFrame();
        *readbackMs = t.nsecsElapsed() / 1.0e6;
        return done && !rb.data.isEmpty();
    }
};

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

ProbeResult runProbe(double budgetMs) {
    ProbeResult result;
    Probe probe;
    if (!probe.init()) {
        std::fprintf(stderr, "RHI Metal unavailable on this host; probe inconclusive\n");
        result.exitCode = 2;
        return result;
    }

    std::vector<double> total;
    std::vector<double> imp;
    std::vector<double> comp;
    std::vector<double> rb;
    for (int i = 0; i < kFrames; ++i) {
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
        if (!probe.frame(&a, &b, &c)) {
            std::fprintf(stderr, "frame %d failed\n", i);
            result.exitCode = 3;
            return result;
        }
        if (i < 20) continue;
        imp.push_back(a);
        comp.push_back(b);
        rb.push_back(c);
        total.push_back(a + b + c);
    }

    result.totalMs = median(total);
    result.importMs = median(imp);
    result.compositeMs = median(comp);
    result.readbackMs = median(rb);
    result.exitCode = result.totalMs <= budgetMs ? 0 : 1;
    return result;
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const double budgetMs = argc > 1 ? std::atof(argv[1]) : 0.5;

    ProbeResult result;
    QThread* renderThread = QThread::create([&]() { result = runProbe(budgetMs); });
    renderThread->setObjectName(QStringLiteral("rhi_import_probe_render_thread"));
    renderThread->start();
    renderThread->wait();
    delete renderThread;

    if (result.exitCode == 2 || result.exitCode == 3) return result.exitCode;

    std::printf("RHI per-frame overhead: %.4f ms (import=%.4f composite=%.4f readback=%.4f)\n",
                result.totalMs, result.importMs, result.compositeMs, result.readbackMs);
    std::printf("budget=%.4f ms -> %s\n", budgetMs, result.exitCode == 0 ? "WITHIN" : "OVER");
    return result.exitCode;
}
