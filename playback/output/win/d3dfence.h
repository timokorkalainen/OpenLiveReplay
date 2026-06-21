#ifndef OLR_D3D_FENCE_H
#define OLR_D3D_FENCE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <QString>

#include <cstdint>
#include <d3d11.h>
#include <memory>

class D3DFence {
public:
    static std::unique_ptr<D3DFence> create(ID3D11Device* device, QString* error);
    ~D3DFence();

    D3DFence(const D3DFence&) = delete;
    D3DFence& operator=(const D3DFence&) = delete;

    uint64_t signal(ID3D11DeviceContext* context);
    bool wait(uint64_t value, int timeoutMs);
    uint64_t completedValue() const;

private:
    D3DFence();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // _WIN32

#endif // OLR_D3D_FENCE_H
