#pragma once
#include <windows.h>
#include <thread>

namespace vision {
// DXGI can synchronously send window messages from the render thread. Keep
// servicing those while stopping it, or the UI and renderer can wait on each
// other forever. Leave queued input alone until the transition is complete.
// https://learn.microsoft.com/windows/win32/direct3darticles/dxgi-best-practices
inline void stopWindowWorker(std::jthread& worker){
    if(!worker.joinable())return;
    worker.request_stop();
    HANDLE handle=worker.native_handle();
    while(WaitForSingleObject(handle,0)==WAIT_TIMEOUT){
        MsgWaitForMultipleObjectsEx(1,&handle,50,QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
    }
    worker.join();
}
}
