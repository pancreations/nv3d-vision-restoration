// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace vision::hook {
// Preserve the complete D3D11 state, including constant-buffer ranges, class
// instances, predication and UAV bindings. Restoring just the buffer pointer
// with PSSetConstantBuffers loses PSSetConstantBuffers1's suballocation offset.
inline HRESULT createContextState(ID3D11Device* device, ID3DDeviceContextState** state) {
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&device1));
    if(FAILED(hr)) return hr;
    const auto level=device->GetFeatureLevel();
    const UINT flags=(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    return device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,
        __uuidof(ID3D11Device),nullptr,state);
}
class ContextScope {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> previous_;
public:
    ContextScope(ID3D11DeviceContext1* context, ID3DDeviceContextState* state):context_(context) {
        context_->SwapDeviceContextState(state,&previous_);
        context_->ClearState();
    }
    ~ContextScope() {
        // Leave no back-buffer references in the private state: ResizeBuffers
        // must work immediately after this frame, even while the addon lives.
        context_->ClearState();
        context_->SwapDeviceContextState(previous_.Get(),nullptr);
    }
    ContextScope(const ContextScope&)=delete;
    ContextScope& operator=(const ContextScope&)=delete;
};
}
