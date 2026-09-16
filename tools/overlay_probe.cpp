// Does a flip-model swap chain present on a layered, click-through window that is excluded from
// screen capture? Each variant runs for a second in a small window and reports the frame-latency
// waits, the client size, the Present results, the statistics and the composition mode.
#include "renderer.h"
#include <iostream>
using namespace vision;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
static LRESULT CALLBACK probeProc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(){try{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=probeProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionOverlayProbe";RegisterClassExW(&wc);
    for(int variant=0;variant<8;variant++){
        const bool layered=variant&1,exclude=variant&2,hdr=variant&4;
        const DWORD ex=WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|(layered?WS_EX_LAYERED|WS_EX_TRANSPARENT:0);
        HWND w=CreateWindowExW(ex,wc.lpszClassName,L"Overlay probe",WS_POPUP,100,100,320,180,nullptr,nullptr,wc.hInstance,nullptr);
        if(!w)throw std::runtime_error("Cannot create the probe window.");
        if(layered)SetLayeredWindowAttributes(w,0,254,LWA_ALPHA);
        const BOOL affinity=exclude?SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE):TRUE;const DWORD affinityError=affinity?0:GetLastError();
        ShowWindow(w,SW_SHOWNOACTIVATE);
        Surface s;try{s.create(w,nullptr,hdr);}catch(const std::exception& e){std::cout<<"layered="<<layered<<" exclude="<<exclude<<" hdr="<<hdr<<" surface failed: "<<e.what()<<"\n";DestroyWindow(w);continue;}
        RECT r{};GetClientRect(w,&r);
        int timeouts=0,presents=0,statsOk=0,occluded=0;HRESULT lastPresent=S_OK,lastStats=S_OK;UINT lastRefresh=0;
        const double start=qpc();
        while(qpc()-start<1){
            MSG msg;while(PeekMessageW(&msg,w,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            const DWORD wait=WaitForSingleObject(s.waitable,50);if(wait==WAIT_TIMEOUT){timeouts++;continue;}
            s.bind();const float green[]{0,1,0,1};s.context->ClearRenderTargetView(s.target.Get(),green);lastPresent=s.swap->Present(1,0);presents++;if(lastPresent==DXGI_STATUS_OCCLUDED)occluded++;
            DXGI_FRAME_STATISTICS st{};lastStats=s.swap->GetFrameStatistics(&st);if(SUCCEEDED(lastStats)){statsOk++;lastRefresh=st.PresentRefreshCount;}
        }
        ComPtr<IDXGISwapChainMedia> media;s.swap.As(&media);DXGI_FRAME_STATISTICS_MEDIA fm{};if(media)media->GetFrameStatisticsMedia(&fm);
        std::cout<<"layered="<<layered<<" exclude="<<exclude<<" hdr="<<hdr<<" affinityOk="<<affinity<<" (error "<<affinityError<<") client="<<r.right<<"x"<<r.bottom<<" waitTimeouts="<<timeouts<<" presents="<<presents<<" occluded="<<occluded<<std::hex<<" lastPresent=0x"<<uint32_t(lastPresent)<<" lastStats=0x"<<uint32_t(lastStats)<<std::dec<<" statsOk="<<statsOk<<" lastRefresh="<<lastRefresh<<" compositionMode="<<int(fm.CompositionMode)<<"\n";
        DestroyWindow(w);
    }
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
