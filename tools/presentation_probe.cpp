#include "renderer.h"
#include "output_window_order.h"
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace vision;
// Runs the production presenter with one steady eye and a disconnected emitter.
// Measures real fullscreen scanout under application load without alternating
// eyes, black insertion, changes to the display mode, or input/focus takeover.
int wmain(int argc,wchar_t** argv){
    if(argc<5){std::cerr<<"usage: vision_present_probe PID queue-depth seconds report.csv [stream] [simulate]\n";return 2;}
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT co=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    HWND output=nullptr,mirror=nullptr;
    try{
        const DWORD wanted=wcstoul(argv[1],nullptr,10);
        const unsigned depth=wcstoul(argv[2],nullptr,10);
        const double seconds=std::clamp(_wtof(argv[3]),1.,90.);
        const std::filesystem::path report=argv[4];
        HWND captured=nullptr;int64_t largest=0;
        for(const auto& item:captureWindows(nullptr)){
            DWORD pid=0;GetWindowThreadProcessId(item.first,&pid);RECT rect{};
            if(pid==wanted&&!IsIconic(item.first)&&GetClientRect(item.first,&rect)){
                const int64_t area=int64_t(rect.right)*rect.bottom;
                if(area>largest){largest=area;captured=item.first;}
            }
        }
        if(!captured)throw std::runtime_error("Capture PID has no drawable window");
        auto displays=enumerateDisplays();const auto monitor=MonitorFromWindow(captured,MONITOR_DEFAULTTONEAREST);
        auto selected=std::find_if(displays.begin(),displays.end(),[&](const Display& d){return d.monitor==monitor;});
        if(selected==displays.end())throw std::runtime_error("Capture display not found");
        const auto d=*selected;
        Settings settings;settings.width=int(d.width);settings.height=int(d.height);settings.refresh=d.refresh;
        settings.hdr=d.hdrEnabled;settings.sequence=Sequence::BlackInsertion;
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionRestorationTimingProbe";
        wc.lpfnWndProc=[](HWND w,UINT m,WPARAM a,LPARAM b)->LRESULT{
            if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
            if(m==WM_NCHITTEST)return HTTRANSPARENT;
            return DefWindowProcW(w,m,a,b);
        };
        if(!RegisterClassW(&wc))throw std::runtime_error("Register probe window");
        constexpr DWORD ex=WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_TOOLWINDOW;
        auto make=[&]{HWND w=CreateWindowExW(ex,wc.lpszClassName,L"Steady presentation diagnostic",WS_POPUP,d.rect.left,d.rect.top,d.width,d.height,nullptr,nullptr,wc.hInstance,nullptr);if(!w||!configureOutputOverlay(w))throw std::runtime_error("Create steady diagnostic overlay");return w;};
        output=make();SetWindowDisplayAffinity(output,WDA_EXCLUDEFROMCAPTURE);
        if(argc>5){mirror=make();SetWindowPos(mirror,output,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}
        StereoSource source;SourceConfig config;config.kind=SourceKind::Window;config.window=captured;
        config.captureHDR=d.hdrEnabled;config.sdrWhiteLevel=d.sdrWhiteNits/80;
        source.start(config,d.adapterLuid);
        const double readyUntil=qpc()+5;
        while(!source.latest()&&qpc()<readyUntil)Sleep(10);
        if(!source.latest())throw std::runtime_error("No captured image: "+source.status().message);
        Emitter disconnected;Presenter presenter(source,disconnected);
        Presenter::Diagnostics options;options.steadyTimingProbe=true;options.queueDepth=depth;options.trace=report;
        const bool simulate=argc>6&&std::wstring(argv[6])==L"simulate";
        if(simulate){
            disconnected.configure(settings);disconnected.connect({}, {},true);
            const double until=qpc()+2;while(disconnected.status().state!=EmitterState::Simulated&&qpc()<until)Sleep(2);
            if(disconnected.status().state!=EmitterState::Simulated)throw std::runtime_error("Simulated emitter did not start");
            options.simulateProbeTriggers=true;
        }
        if(mirror)presenter.streamTo(mirror,false);
        presenter.start(output,d,settings,false,3,options);
        OutputWindowOrder order;
        std::ofstream state(report.wstring()+L".txt");
        state<<"Steady single-eye production-presenter diagnostic. No emitter connection or display-mode changes.\nqueue="<<depth<<" refresh="<<d.refresh<<" HDR="<<d.hdrEnabled<<" stream="<<(mirror!=nullptr)<<"\n";
        double end=qpc()+seconds,nextStatus=0;bool shown=false;
        while(qpc()<end&&IsWindow(output)){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            auto rs=presenter.status();
            if(!rs.running)throw std::runtime_error(rs.message);
            if(!shown&&rs.presents){if(mirror)ShowWindow(mirror,SW_SHOWNOACTIVATE);ShowWindow(output,SW_SHOWNOACTIVATE);shown=true;}
            order.maintain(output,nullptr,GetTickCount64(),GetForegroundWindow(),mirror);
            if(qpc()>=nextStatus){
                nextStatus=qpc()+1;DWORD focused=0;GetWindowThreadProcessId(GetForegroundWindow(),&focused);
                state<<"presents="<<rs.presents<<" misses="<<rs.misses<<" resyncs="<<rs.resyncs<<" composed="<<rs.composed<<" source="<<source.status().frames<<" focus="<<focused<<" repairs="<<order.repairs()<<" message="<<rs.message<<"\n";state.flush();
            }
            MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        const auto result=presenter.status();presenter.stop();source.stop();
        state<<"Final presents="<<result.presents<<" misses="<<result.misses<<" maxIntervalMs="<<result.maxIntervalMs<<" queue="<<result.queueDepth<<" simulated="<<simulate<<" emitterCommands="<<disconnected.status().commands<<" lateCommands="<<disconnected.status().late<<"\n";
        if(disconnected.status().commands&&!simulate)throw std::runtime_error("Diagnostic unexpectedly issued emitter commands");
        if(mirror)DestroyWindow(mirror);DestroyWindow(output);output=mirror=nullptr;
        std::cout<<"presents="<<result.presents<<" misses="<<result.misses<<" report="<<report.string()<<'\n';
    }catch(const std::exception& e){if(mirror)DestroyWindow(mirror);if(output)DestroyWindow(output);std::cerr<<e.what()<<'\n';if(SUCCEEDED(co))CoUninitialize();return 1;}
    if(SUCCEEDED(co))CoUninitialize();return 0;
}
