#pragma once
#include <d3d11.h>
#include <chrono>
#include <stop_token>
#include <thread>

namespace vision {
// A timeout is never permission to publish a texture. The caller retains its
// last completed pair and abandons this publication on cancellation or failure.
template<class Poll>
HRESULT waitForGpuCompletion(Poll poll,std::stop_token stop,
    std::chrono::steady_clock::duration timeout=std::chrono::seconds(5)) {
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    for(;;){
        if(stop.stop_requested())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        BOOL complete=FALSE;const HRESULT hr=poll(&complete);
        if(FAILED(hr))return hr;
        if(hr==S_OK&&complete)return S_OK;
        if(std::chrono::steady_clock::now()>=deadline)return DXGI_ERROR_WAIT_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}
