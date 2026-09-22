#pragma once
#include <d3d11.h>
#include <chrono>
#include <stop_token>
#include <thread>

namespace vision {
// Sleep(1) lasts a whole 15.6 ms scheduler tick whenever Windows ignores the
// process's timer resolution, which Windows 11 does for a covered window: two
// such polls per stereo pair capped a game under the 3D output at 32 pairs/s.
// A high-resolution waitable timer keeps its precision in that state.
// Returns after about half a millisecond, or at once when the optional event is signalled.
inline void shortWait(HANDLE event=nullptr){
    struct Timer {HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);~Timer(){if(handle)CloseHandle(handle);}};
    thread_local Timer timer;LARGE_INTEGER due;due.QuadPart=-5000; // 0.5 ms
    HANDLE handles[2]{timer.handle,event};
    if(timer.handle&&SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE))WaitForMultipleObjects(event?2:1,handles,FALSE,100);
    else if(event)WaitForSingleObject(event,1);else Sleep(1);
}
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
        shortWait();
    }
}
}
