#ifndef OLR_WIN_GPU_FAULT_WORKER_ORACLE_H
#define OLR_WIN_GPU_FAULT_WORKER_ORACLE_H

#include <QJsonObject>
#include <QString>

#include <memory>
#include <functional>
#include <cstdint>

class GpuFence;

struct WinGpuFaultRemovalObservation {
    int64_t hresult = 0;
    uint64_t generation = 0;
    bool productionPollObserved = false;
};

class WinGpuFaultWorkerOracle final {
public:
    WinGpuFaultWorkerOracle();
    ~WinGpuFaultWorkerOracle();

    bool prepare(const QJsonObject& capability, QString* error);
    bool recover(QJsonObject* evidence, QString* error);
    void* d3dDevice() const;
    WinGpuFaultRemovalObservation pollWorkerDeviceLoss();
    bool invokeOnRenderThread(const std::function<void(void*)>& operation) const;
    bool submitWorkerFaultOperation(const std::shared_ptr<GpuFence>& fence,
                                    const std::function<bool(void*, void*)>& dispatch,
                                    qsizetype* pendingRetains, QString* error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // OLR_WIN_GPU_FAULT_WORKER_ORACLE_H
