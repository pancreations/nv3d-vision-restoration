#pragma once
#include <windows.h>
#include <dxgi.h>
namespace vision {
// Present may only report success after the display grants a producer slot.
// In particular, a wait timeout is not a successfully presented frame.
template<class Unavailable>
HRESULT waitForProducerPermit(HANDLE permit,bool noWait,Unavailable unavailable,ULONGLONG timeoutMs=5000){
    const auto begin=GetTickCount64();
    for(;;){
        if(unavailable())return DXGI_ERROR_DEVICE_REMOVED;
        const auto waited=WaitForSingleObject(permit,noWait?0:20);
        if(waited==WAIT_OBJECT_0)return S_OK;
        if(waited!=WAIT_TIMEOUT)return HRESULT_FROM_WIN32(GetLastError());
        if(noWait)return DXGI_ERROR_WAS_STILL_DRAWING;
        if(GetTickCount64()-begin>=timeoutMs)return DXGI_ERROR_WAIT_TIMEOUT;
    }
}
}
