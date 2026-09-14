#include "renderer.h"
#include "gamesync.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
using namespace vision;
static HWND controlWindow=nullptr,outputWindow=nullptr;
static bool closeRequested=false,outputStop=false,pauseRequested=false,resyncRequested=false;
static bool displayRefreshChanged=false;
static int phaseDelta=0,durationDelta=0,offsetDelta=0,bandDelta=0,bandPositionDelta=0;static bool swapRequested=false;
static bool sweepToggleRequested=false,markRequested=false,acceptRequested=false,bfiToggleRequested=false;
static std::filesystem::path executableDirectory(){wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));if(!n||n==std::size(path))throw std::runtime_error("Cannot locate the application directory.");return std::filesystem::path(path).parent_path();}
static std::filesystem::path workspace(){auto directory=executableDirectory(),candidate=directory.parent_path().parent_path().parent_path();return std::filesystem::exists(candidate/L"CMakeLists.txt")?candidate:directory;}
static std::filesystem::path chooseFile(HWND owner,const wchar_t* filter,bool save=false){
    wchar_t path[32768]{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFilter=filter;o.lpstrFile=path;o.nMaxFile=32768;o.Flags=OFN_NOCHANGEDIR|OFN_EXPLORER|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    return (save?GetSaveFileNameW(&o):GetOpenFileNameW(&o))?std::filesystem::path(path):std::filesystem::path{};
}
static LRESULT CALLBACK outputProc(HWND w,UINT m,WPARAM a,LPARAM b){
    if(m==WM_CLOSE){outputStop=true;return 0;}
    if(m==WM_KEYDOWN){int step=(GetKeyState(VK_SHIFT)&0x8000)?10:100;if(a==VK_ESCAPE || a==VK_F1)outputStop=true;else if(a==VK_SPACE)pauseRequested=true;else if(a=='X')swapRequested=true;else if(a==VK_DOWN)phaseDelta-=step;else if(a==VK_UP)phaseDelta+=step;else if(a==VK_LEFT)durationDelta-=step;else if(a==VK_RIGHT)durationDelta+=step;else if(a==VK_PRIOR)bandDelta+=5;else if(a==VK_NEXT)bandDelta-=5;else if(a==VK_HOME)bandPositionDelta-=5;else if(a==VK_END)bandPositionDelta+=5;else if(a=='S')sweepToggleRequested=true;else if(a=='M')markRequested=true;else if(a==VK_RETURN)acceptRequested=true;else if(a=='B')bfiToggleRequested=true;return 0;}
    // A display mode change invalidates the fullscreen presentation target. A USB
    // device change does not: keep presenting while the emitter reconnects.
    if(m==WM_DISPLAYCHANGE){outputStop=true;displayRefreshChanged=true;return 0;}
    return DefWindowProcW(w,m,a,b);
}
static LRESULT CALLBACK controlProc(HWND w,UINT m,WPARAM a,LPARAM b){
    // Plain arrows tune calibration in both the control and fullscreen windows.
    // Handle these before ImGui, which otherwise consumes navigation keys.
    if(m==WM_KEYDOWN && !(ImGui::GetCurrentContext()&&ImGui::GetIO().WantTextInput) && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)){
        int step=(GetKeyState(VK_SHIFT)&0x8000)?10:100;
        if(a==VK_UP)phaseDelta+=step;else if(a==VK_DOWN)phaseDelta-=step;
        else if(a==VK_RIGHT)durationDelta+=step;else if(a==VK_LEFT)durationDelta-=step;
        else return ImGui_ImplWin32_WndProcHandler(w,m,a,b)?1:DefWindowProcW(w,m,a,b);
        return 0;
    }
    if(ImGui_ImplWin32_WndProcHandler(w,m,a,b))return 1;
    if(m==WM_CLOSE){closeRequested=true;return 0;}
    // Global hotkeys: 1 stops the test output; 2-6 tune the profile while a hooked game has focus.
    if(m==WM_HOTKEY){if(a==1)outputStop=true;else if(a==2)phaseDelta+=100;else if(a==3)phaseDelta-=100;else if(a==4)durationDelta-=100;else if(a==5)durationDelta+=100;else if(a==6)swapRequested=true;else if(a==7)resyncRequested=true;return 0;}
    if(m==WM_DISPLAYCHANGE){outputStop=true;displayRefreshChanged=true;return 0;}
    return DefWindowProcW(w,m,a,b);
}
static void label(const char* text){ImGui::TextColored(ImVec4(.463f,.725f,0,1),"%s",text);}
static void paragraph(const char* text){ImGui::PushTextWrapPos();ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool gpuTest=false,probe=false,smoke=false,emitterCheck=false,emitterPulse=false;int pulseEye=-1;double pulseShutter=1500;int liveSeconds=0;bool liveHdr=false,vblank=false,liveSwap=false;double livePhase=0,liveDuration=1500;int liveRefresh=120,liveSequence=0;std::filesystem::path emitterFw;for(int i=1;i<argc;i++){gpuTest|=std::wstring(argv[i])==L"--gpu-test";probe|=std::wstring(argv[i])==L"--probe";smoke|=std::wstring(argv[i])==L"--smoke-test";if(std::wstring(argv[i])==L"--emitter-check"){emitterCheck=true;if(i+1<argc&&std::wstring(argv[i+1]).rfind(L"--",0)!=0)emitterFw=argv[++i];}emitterPulse|=std::wstring(argv[i])==L"--emitter-pulse";if(std::wstring(argv[i])==L"--pulse-eye"&&i+1<argc){std::wstring q=argv[++i];pulseEye=q==L"left"?0:q==L"right"?1:-1;emitterPulse=true;}if(std::wstring(argv[i])==L"--pulse-shutter"&&i+1<argc)pulseShutter=_wtof(argv[++i]);liveHdr|=std::wstring(argv[i])==L"--hdr";liveSwap|=std::wstring(argv[i])==L"--swap";if(std::wstring(argv[i])==L"--phase"&&i+1<argc)livePhase=_wtof(argv[++i]);if(std::wstring(argv[i])==L"--duration"&&i+1<argc)liveDuration=_wtof(argv[++i]);if(std::wstring(argv[i])==L"--refresh"&&i+1<argc)liveRefresh=_wtoi(argv[++i]);if(std::wstring(argv[i])==L"--sequence"&&i+1<argc){std::wstring q=argv[++i];liveSequence=q==L"lbrb"?1:q==L"llrr"?2:0;}vblank|=std::wstring(argv[i])==L"--vblank";if(std::wstring(argv[i])==L"--live"){liveSeconds=10;if(i+1<argc&&std::wstring(argv[i+1]).rfind(L"--",0)!=0)liveSeconds=std::max(1,_wtoi(argv[++i]));}}LocalFree(argv);
    try{
        // Offscreen render checks open no window and must run while the app is open.
        if(gpuTest){runGpuSelfTest(workspace()/L"reports");if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(vblank){
            // Vblank probe for every output (docs/TIMING-CALIBRATION.md): the graphics kernel's scan-line
            // counter against the presentation timestamps the presenter predicts from. Runs beside an open instance.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/vblank.txt");
            for(auto& d:enumerateDisplays()){auto r=measureVblank(d,3.0);f<<d.name<<" ("<<utf8(d.gdiName)<<") "<<d.width<<'x'<<d.height<<" @ "<<std::fixed<<std::setprecision(3)<<d.refresh<<" Hz, "<<d.activeLines<<" active of "<<d.totalLines<<" lines\n  "<<r.message<<"\n";
                if(r.ok)f<<std::setprecision(4)<<"  measuredHz="<<r.measuredHz<<std::setprecision(1)<<" vblankLengthUs="<<r.vblankLengthUs<<" syncAfterVblankStartUs="<<r.syncAfterVblankStartUs<<" scanStartAfterSyncUs="<<r.scanStartAfterSyncUs<<" dxgiJitterRmsUs="<<r.dxgiJitterRmsUs<<" dxgiJitterMaxUs="<<r.dxgiJitterMaxUs<<" scanlineJitterRmsUs="<<r.scanlineJitterRmsUs<<" wakeLatencyP50Us="<<r.wakeLatencyP50Us<<" wakeLatencyMaxUs="<<r.wakeLatencyMaxUs<<" samples="<<r.dxgiSamples<<'/'<<r.scanlineSamples<<"\n";}
            if(SUCCEEDED(co))CoUninitialize();return 0;}
        HANDLE singleInstance=CreateMutexW(nullptr,FALSE,L"Local\\VisionRestoration.SingleInstance.9F170E91");if(!singleInstance)throw std::runtime_error("Cannot create the application instance guard.");
        if(GetLastError()==ERROR_ALREADY_EXISTS){if(HWND existing=FindWindowW(L"VisionRestorationControl",nullptr)){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}CloseHandle(singleInstance);if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(probe){auto displays=enumerateDisplays();Emitter usb;auto devices=usb.discover();std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/hardware.txt");for(auto& d:displays){f<<d.name<<"\n"<<d.id<<"\nGPU: "<<d.gpu<<"\nMode: "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<"\nHDR supported/enabled: "<<d.hdrSupported<<'/'<<d.hdrEnabled<<"\n";}for(auto& d:devices)f<<d.description<<"\n";if(devices.empty())f<<"No supported NVIDIA or RP2040 USB emitter enumerated.\n";if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(emitterCheck){
            // Headless USB check: connect to the single supported NVIDIA emitter, optionally
            // uploading RAM firmware if it is in boot state. No timing/eye commands are sent.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/emitter-check.txt");
            Emitter usb;auto found=usb.discover();std::vector<UsbDeviceInfo> nv;for(auto& d:found)if(d.supported&&!d.rp2040)nv.push_back(d);
            for(auto& d:found)f<<"Found: "<<d.description<<"\n";
            if(nv.size()!=1){f<<"Expected exactly one supported NVIDIA emitter, found "<<nv.size()<<".\n";if(SUCCEEDED(co))CoUninitialize();return 2;}
            usb.connect(nv[0],emitterFw,false);
            EmitterStatus st;for(int i=0;i<200;i++){st=usb.status();if(st.state!=EmitterState::Initializing)break;Sleep(50);}
            const char* name=st.state==EmitterState::Ready?"Ready":st.state==EmitterState::Error?"Error":st.state==EmitterState::Disconnected?"Disconnected":st.state==EmitterState::Initializing?"Initializing (timeout)":"Other";
            f<<"Identity: "<<st.identity<<"\nState: "<<name<<"\nMessage: "<<st.message<<"\n";
            bool ok=st.state==EmitterState::Ready||(st.state==EmitterState::Disconnected&&st.message.rfind("Firmware loaded",0)==0);
            if(ok&&emitterPulse&&st.state==EmitterState::Ready){
                // Drive the emitter alone: alternate L/R at 120 Hz for 10 s with no display pipeline.
                // Watch the emitter LED and the glasses. This is a protocol test, not a stereo test.
                Settings s;s.refresh=120;s.leftUs=s.rightUs=std::clamp(pulseShutter,250.0,periodUs(120)-100);s.phaseUs=0;usb.configure(s);
                // --pulse-eye left|right drives one lens only (10 s) so each shutter can be judged by eye.
                double period=1.0/120,t=qpc()+0.01;Eye eye=pulseEye==1?Eye::Right:Eye::Left;f<<"Pulse eye: "<<(pulseEye<0?"alternating":pulseEye==1?"right only":"left only")<<" shutter "<<s.leftUs<<" us\n";
                for(int n=0;n<1200;n++){usb.submit(eye,t);if(pulseEye<0)eye=eye==Eye::Left?Eye::Right:Eye::Left;t+=period;while(qpc()<t-0.0005)Sleep(0);}
                Sleep(100);st=usb.status();f<<"Pulse: commands="<<st.commands<<" errors="<<st.errors<<" late="<<st.late<<" lastTransferUs="<<st.lastTransferUs<<" maxTransferUs="<<st.maxTransferUs<<"\nAfter pulse: "<<st.message<<"\n";ok=st.commands>1000&&st.errors==0;}
            usb.disconnect();if(SUCCEEDED(co))CoUninitialize();return ok?0:1;}
        WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_CLASSDC;wc.lpfnWndProc=controlProc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpszClassName=L"VisionRestorationControl";RegisterClassExW(&wc);
        controlWindow=CreateWindowExW(0,wc.lpszClassName,L"Vision Restoration | Stereo setup",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1180,850,nullptr,nullptr,instance,nullptr);
        if(!controlWindow)throw std::runtime_error("Cannot create control window.");
        float windowScale=float(GetDpiForWindow(controlWindow))/96.f;MONITORINFO work{sizeof(work)};GetMonitorInfoW(MonitorFromWindow(controlWindow,MONITOR_DEFAULTTONEAREST),&work);
        int initialWidth=std::min(int(1180*windowScale),int(work.rcWork.right-work.rcWork.left)-40),initialHeight=std::min(int(850*windowScale),int(work.rcWork.bottom-work.rcWork.top)-40);
        SetWindowPos(controlWindow,nullptr,work.rcWork.left+(work.rcWork.right-work.rcWork.left-initialWidth)/2,work.rcWork.top+(work.rcWork.bottom-work.rcWork.top-initialHeight)/2,initialWidth,initialHeight,SWP_NOZORDER|SWP_NOACTIVATE);
        wc.lpfnWndProc=outputProc;wc.lpszClassName=L"VisionRestorationOutput";RegisterClassExW(&wc);
        if(liveSeconds>0){
            // Headless fullscreen stereo output with the real emitter: the same presenter path the
            // GUI uses with Preview unchecked. Writes reports/live-check.txt. Escape stops early.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/live-check.txt");
            auto displays=enumerateDisplays();if(displays.empty())throw std::runtime_error("No physical output found.");
            int di=0;for(size_t i=0;i<displays.size();i++)if(displays[i].name.find("G80SD")!=std::string::npos){di=int(i);break;}
            auto d=displays[di];f<<"Display: "<<d.name<<" "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<" HDR enabled "<<d.hdrEnabled<<"\n";
            DisplayModeGuard modeGuard;if(std::abs(d.refresh-liveRefresh)>0.5){bool applied=false;for(auto& m:d.modes)if(m.dmPelsWidth==3840&&m.dmPelsHeight==2160&&int(m.dmDisplayFrequency)==liveRefresh){modeGuard.apply(d,m);applied=true;break;}if(!applied)throw std::runtime_error("Requested 3840x2160 mode not offered by this display.");displays=enumerateDisplays();d=displays[di];f<<"Switched to: "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<"\n";}
            StereoSource source;Emitter emitter;Presenter presenter(source,emitter);
            auto devices=emitter.discover();std::vector<UsbDeviceInfo> nv;for(auto& x:devices)if(x.supported&&!x.rp2040)nv.push_back(x);
            if(nv.size()!=1)throw std::runtime_error("Expected exactly one NVIDIA emitter in runtime state (run tools/Prepare-Emitter.ps1).");
            Settings settings;settings.displayId=d.id;settings.connection=d.connection;settings.width=int(d.width);settings.height=int(d.height);settings.refresh=d.refresh;settings.hdr=liveHdr&&d.hdrEnabled;settings.leftUs=settings.rightUs=liveDuration;settings.phaseUs=livePhase;settings.swapEyes=liveSwap;settings.sequence=Sequence(liveSequence);
            emitter.configure(settings);emitter.connect(nv[0],{},false);
            EmitterStatus es;for(int i=0;i<200;i++){es=emitter.status();if(es.state!=EmitterState::Initializing)break;Sleep(50);}
            if(es.state!=EmitterState::Ready)throw std::runtime_error("Emitter not ready: "+es.message);
            outputWindow=CreateWindowExW(WS_EX_TOPMOST,L"VisionRestorationOutput",L"Stereo test | Escape: stop | X: swap | Up/Down: phase | Left/Right: duration",WS_POPUP,d.rect.left,d.rect.top,d.rect.right-d.rect.left,d.rect.bottom-d.rect.top,nullptr,nullptr,instance,nullptr);
            ShowWindow(outputWindow,SW_SHOW);SetForegroundWindow(outputWindow);presenter.start(outputWindow,d,settings,false,1);
            double end=qpc()+liveSeconds;outputStop=false;
            while(qpc()<end&&!outputStop){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                bool changed=false;if(swapRequested){settings.swapEyes=!settings.swapEyes;swapRequested=false;changed=true;}
                if(phaseDelta){settings.phaseUs=wrapPhase(settings.phaseUs+phaseDelta,phaseCycleUs(settings.refresh,settings.sequence));phaseDelta=0;changed=true;}
                if(offsetDelta){settings.rightOffsetUs=std::clamp(settings.rightOffsetUs+float(offsetDelta),-1500.f,1500.f);offsetDelta=0;changed=true;}
                if(durationDelta){settings.leftUs=settings.rightUs=std::clamp(settings.leftUs+durationDelta,minimumShutterUs,nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence)));durationDelta=0;changed=true;}
                if(changed){emitter.configure(settings);presenter.configure(settings,1);}
                Sleep(5);}
            auto rs=presenter.status();es=emitter.status();presenter.stop();emitter.disconnect();DestroyWindow(outputWindow);outputWindow=nullptr;
            {unsigned bins[5]{};for(double ms:rs.intervals){bins[ms<6?0:ms<11?1:ms<19?2:ms<27?3:4]++;}f<<"Intervals(ms) <6:"<<bins[0]<<" 6-11:"<<bins[1]<<" 11-19:"<<bins[2]<<" 19-27:"<<bins[3]<<" >27:"<<bins[4]<<" (samples "<<rs.intervals.size()<<")\nHDR surface: "<<settings.hdr<<"\n";}
            f<<"Presenter: running="<<rs.running<<" preview="<<rs.preview<<" locked="<<rs.locked<<" timingPassed="<<rs.timingPassed<<" presents="<<rs.presents<<" misses="<<rs.misses<<" resyncs="<<rs.resyncs<<" lastResyncSec="<<rs.lastResyncSec<<" composed="<<rs.composed<<" modeChanges="<<rs.modeChanges<<" leadMs[<0,0-2,2-4,4-6,6-8,>8]="<<rs.leadBins[0]<<"/"<<rs.leadBins[1]<<"/"<<rs.leadBins[2]<<"/"<<rs.leadBins[3]<<"/"<<rs.leadBins[4]<<"/"<<rs.leadBins[5]<<" elapsed="<<rs.elapsed<<" measuredHz="<<rs.measuredHz<<" maxIntervalMs="<<rs.maxIntervalMs<<"\nPresenter message: "<<rs.message<<"\n";
            f<<"Emitter: state="<<int(es.state)<<" commands="<<es.commands<<" errors="<<es.errors<<" late="<<es.late<<" lastTransferUs="<<es.lastTransferUs<<" maxTransferUs="<<es.maxTransferUs<<"\nEmitter message: "<<es.message<<"\nFinal settings: refresh="<<settings.refresh<<" sequence="<<int(settings.sequence)<<" swap="<<settings.swapEyes<<" phaseUs="<<settings.phaseUs<<" durationUs="<<settings.leftUs<<"\n";
            if(SUCCEEDED(co))CoUninitialize();return (rs.presents>0&&es.commands>0&&es.errors==0)?0:1;}
        Surface ui;ui.create(controlWindow,nullptr,false);IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        float dpi=float(GetDpiForWindow(controlWindow))/96.f;io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",18*dpi);ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();style.WindowRounding=0;style.ChildRounding=12;style.FrameRounding=6;style.GrabRounding=5;style.FramePadding={12,8};style.ItemSpacing={12,10};style.WindowPadding={24,22};
        style.Colors[ImGuiCol_WindowBg]={.012f,.015f,.012f,1};style.Colors[ImGuiCol_ChildBg]={.026f,.032f,.024f,1};style.Colors[ImGuiCol_FrameBg]={.07f,.085f,.065f,1};style.Colors[ImGuiCol_FrameBgHovered]={.11f,.16f,.075f,1};style.Colors[ImGuiCol_FrameBgActive]={.15f,.22f,.07f,1};style.Colors[ImGuiCol_Button]={.16f,.24f,.055f,1};style.Colors[ImGuiCol_ButtonHovered]={.31f,.47f,.025f,1};style.Colors[ImGuiCol_ButtonActive]={.463f,.725f,0,1};style.Colors[ImGuiCol_CheckMark]={.55f,.88f,.08f,1};style.Colors[ImGuiCol_SliderGrab]={.463f,.725f,0,1};style.Colors[ImGuiCol_SliderGrabActive]={.62f,.94f,.08f,1};style.Colors[ImGuiCol_Header]={.19f,.30f,.055f,1};style.Colors[ImGuiCol_HeaderHovered]={.30f,.46f,.035f,1};style.Colors[ImGuiCol_Separator]={.20f,.28f,.16f,1};style.Colors[ImGuiCol_Text]={.90f,.93f,.88f,1};style.ScaleAllSizes(dpi);
        ImGui_ImplWin32_Init(controlWindow);ImGui_ImplDX11_Init(ui.device.Get(),ui.context.Get());
        auto displays=enumerateDisplays();std::vector<UsbDeviceInfo> devices;StereoSource source;Emitter emitter;Presenter presenter(source,emitter);DisplayModeGuard modeGuard;GameSync gameSync(emitter);if(!smoke)gameSync.start();
        Settings settings;int displayIndex=0,modeIndex=0,deviceIndex=0,step=2,sourceKind=0,windowIndex=0;SourceConfig sourceConfig;std::vector<std::pair<HWND,std::string>> windows;
        bool outputEmbedded=false;RECT previewRect{0,0,640,360},placedPreviewRect{};
        // Blender frame channel: the input and the viewport overlay start and stop with Blender's 3D button.
        // blenderHold keeps a stopped or replaced Blender output from restarting until 3D is switched off and on.
        FrameChannelHost frameChannel;FrameChannelLink channelLink;double nextChannelPoll=0;bool outputPassThrough=false,blenderSource=false,blenderOutput=false,blenderHold=false;RECT overlayRect{};
        // Phase sweep: the phase advances slowly through the whole cycle while the viewer watches
        // through the glasses and marks where the image is cleanest or brightest.
        struct Sweep{bool active=false;double origin=0,start=0,lastStep=0,secondsPerCycle=30;std::vector<double> marks;} sweep;
        std::filesystem::path firmware=executableDirectory()/L"emitter.fw";if(!std::filesystem::exists(firmware))firmware=workspace()/L"STUFF"/L"emitter.fw";if(!std::filesystem::exists(firmware))firmware.clear();char name[192]="My stereo setup",notes[2048]{},sender[256]{};bool paused=false,autoReconnectEmitter=true;double nextEmitterScan=0;std::string notice="Select an eye pattern, then start stereo output. Check either lens independently.";std::string error;
        auto syncDisplay=[&](bool followDisplayRate=false){if(displays.empty())return;displayIndex=std::clamp(displayIndex,0,int(displays.size()-1));auto& d=displays[displayIndex];settings.displayId=d.id;settings.connection=d.connection;settings.width=int(d.width);settings.height=int(d.height);if(followDisplayRate)settings.refresh=d.refresh;settings.hdr=settings.hdr&&d.hdrEnabled;modeIndex=0;for(size_t i=0;i<d.modes.size();i++)if(d.modes[i].dmPelsWidth==d.width && d.modes[i].dmPelsHeight==d.height && std::abs(double(d.modes[i].dmDisplayFrequency)-d.refresh)<.5){modeIndex=int(i);break;}settings.validated=false;};syncDisplay(true);devices=emitter.discover();
        {int real=-1,count=0;for(size_t i=0;i<devices.size();i++)if(devices[i].supported&&!devices[i].rp2040){real=int(i);count++;}
            if(!smoke&&count==1&&(devices[real].pid==0x0007||!firmware.empty())){deviceIndex=real;settings.hdr=displays.empty()?false:displays[displayIndex].hdrEnabled;emitter.configure(settings);emitter.connect(devices[real],firmware,false);notice="NVIDIA emitter found. Driver, firmware and runtime connection are being restored automatically.";}
            else for(auto& x:devices)if(x.pid==0x7003)notice="NVIDIA emitter is in boot state (0955:7003). Connect emitter to upload firmware and restart it automatically.";}
        auto pattern=[&]{if(sourceKind!=0)return 3;return step<3?step:step==3?4:step+1;};
        auto stopOutput=[&]{bool wasFullscreen=outputWindow&&!outputEmbedded&&!presenter.status().preview;sweep.active=false;presenter.showPhase(false);presenter.stop();gameSync.enable(true);paused=false;if(outputWindow){DestroyWindow(outputWindow);outputWindow=nullptr;}outputEmbedded=false;bool keepFocus=outputPassThrough;outputPassThrough=false;if(!smoke&&wasFullscreen&&!keepFocus){ShowWindow(controlWindow,SW_RESTORE);SetForegroundWindow(controlWindow);}};
        auto clampTiming=[&]{double p=periodUs(settings.refresh);auto emitterState=emitter.status();settings.phaseUs=emitterState.scheduled?wrapSignedPhase(settings.phaseUs,p):wrapPhase(settings.phaseUs,phaseCycleUs(settings.refresh,settings.sequence));double maximum=emitterState.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));settings.leftUs=std::clamp(settings.leftUs,minimumShutterUs,maximum);settings.rightUs=std::clamp(settings.rightUs,minimumShutterUs,maximum);
            settings.imageGain=std::clamp(settings.imageGain,1.f,8.f);
            settings.bandHeight=std::clamp(settings.bandHeight,.1f,1.f);settings.bandCenter=std::clamp(settings.bandCenter,settings.bandHeight/2,1-settings.bandHeight/2);
            settings.panelResponseUs=std::clamp(settings.panelResponseUs,0.,8000.);settings.panelScanUs=std::clamp(settings.panelScanUs,0.,50000.);
            settings.strobeStartUs=std::clamp(settings.strobeStartUs,0.,50000.);settings.strobeLengthUs=std::clamp(settings.strobeLengthUs,50.,50000.);settings.scanStartUs=std::clamp(settings.scanStartUs,-5000.,5000.);
        };
        auto update=[&](bool persist=true){clampTiming();settings.validated=false;validate(settings);{Settings presented=settings;if(blenderOutput)presented.convergence=0;presenter.configure(presented,pattern());}gameSync.configure(settings,displays.empty()?nullptr:displays[displayIndex].monitor);
            // Calibration is live state, not UI focus state. Persist every change so
            // Alt-Tab, minimization, a USB reconnect or an app restart cannot lose it.
            // A running sweep changes the phase ten times a second and saves when it stops.
            if(!smoke&&persist)try{settings.name=name;settings.monitorNotes=notes;saveProfile(workspace()/L"profiles"/L"autosave.ini",settings);}catch(...){}
        };
        auto panelTiming=[&]{PanelTiming t;t.scanUs=settings.panelScanUs>0?settings.panelScanUs:displays.empty()?0:displays[displayIndex].scanUs;t.responseUs=settings.panelResponseUs;t.illumination=settings.illumination;t.strobeStartUs=settings.strobeStartUs;t.strobeLengthUs=settings.strobeLengthUs;return t;};
        auto usbLatency=[&]{auto e=emitter.status();return e.scheduled?0.0:e.meanTransferUs;};
        auto stopSweep=[&](const char* why){if(!sweep.active)return;sweep.active=false;presenter.showPhase(false);update();notice=why;};
        auto startSweep=[&]{if(!presenter.status().running||presenter.status().preview)throw std::runtime_error("Start the 3D preview or fullscreen output before sweeping the phase.");sweep.active=true;sweep.origin=settings.phaseUs;sweep.start=sweep.lastStep=qpc();presenter.showPhase(true);notice="Phase sweep running: watch through the glasses; M marks the current phase, Enter keeps it, S stops.";};
        // Profiles: user-named .ini files under profiles/. Timing values (phase, shutter, swap, depth, sequence)
        // apply live; display identity is compared and reported but never blocks a load.
        std::vector<std::pair<std::string,std::filesystem::path>> profileList;int profileIndex=0;bool profilesScanned=false;
        auto scanProfiles=[&]{profileList.clear();std::error_code ec;auto dir=workspace()/L"profiles";std::filesystem::create_directories(dir,ec);
            for(auto& e:std::filesystem::directory_iterator(dir,ec))if(e.is_regular_file()&&e.path().extension()==L".ini"){std::string label=utf8(e.path().stem().wstring());try{auto l=loadProfile(e.path());if(!l.name.empty()&&l.name!=label)label=l.name+"  ["+label+"]";}catch(...){label+="  [unreadable]";}profileList.push_back({label,e.path()});}
            std::sort(profileList.begin(),profileList.end(),[](auto& a,auto& b){return a.first<b.first;});profileIndex=profileList.empty()?0:std::clamp(profileIndex,0,int(profileList.size())-1);profilesScanned=true;};
        auto profileMatches=[&](const Settings& l){return !displays.empty()&&l.displayId==displays[displayIndex].id&&l.width==settings.width&&l.height==settings.height&&std::abs(l.refresh-settings.refresh)<.02&&l.hdr==settings.hdr;};
        auto applyProfile=[&](const Settings& l){bool same=profileMatches(l);settings.name=l.name;settings.refresh=l.refresh;settings.swapEyes=l.swapEyes;settings.phaseUs=l.phaseUs;settings.leftUs=l.leftUs;settings.rightUs=l.rightUs;settings.depth=l.depth;settings.convergence=l.convergence;settings.rightOffsetUs=l.rightOffsetUs;settings.peakNits=l.peakNits;settings.monitorNotes=l.monitorNotes;settings.sequence=l.sequence;settings.assessment=l.assessment;
            settings.bandHeight=l.bandHeight;settings.bandCenter=l.bandCenter;settings.panelResponseUs=l.panelResponseUs;settings.panelScanUs=l.panelScanUs;settings.illumination=l.illumination;settings.strobeStartUs=l.strobeStartUs;settings.strobeLengthUs=l.strobeLengthUs;settings.scanStartUs=l.scanStartUs;
            strncpy_s(name,settings.name.c_str(),_TRUNCATE);strncpy_s(notes,settings.monitorNotes.c_str(),_TRUNCATE);update();notice=same?"Profile applied: "+l.name:"Profile applied: "+l.name+" (saved for a different display or mode; verify the timing).";};
        auto profileFileFor=[&](std::string n){std::string clean;for(char c:n)clean+=(isalnum((unsigned char)c)||c==' '||c=='-'||c=='_'||c=='.')?c:'_';while(!clean.empty()&&clean.back()==' ')clean.pop_back();if(clean.empty())clean="profile";return workspace()/L"profiles"/(wide(clean)+L".ini");};
        auto resetTiming=[&]{Settings d;settings.swapEyes=d.swapEyes;settings.phaseUs=d.phaseUs;settings.leftUs=d.leftUs;settings.rightUs=d.rightUs;settings.rightOffsetUs=d.rightOffsetUs;settings.sequence=d.sequence;update();notice="Timing reset: phase 0, shutters 1500 us, eyes normal.";};
        // Keep the last IR preset if the monitor rate was changed outside the app.
        try{scanProfiles();std::filesystem::path best;std::filesystem::file_time_type bestTime{};Settings bestSettings;
            for(auto& [label,path]:profileList){try{auto l=loadProfile(path);if(l.displayId!=settings.displayId||l.width!=settings.width||l.height!=settings.height||l.hdr!=settings.hdr)continue;auto t=std::filesystem::last_write_time(path);if(best.empty()||t>bestTime){best=path;bestTime=t;bestSettings=l;}}catch(...){}}
            if(!best.empty()){applyProfile(bestSettings);notice="Loaded last saved profile for this display: "+bestSettings.name;}}catch(const std::exception& e){error=e.what();}
        if(!smoke){int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);std::wstring preset;for(int i=1;i+1<count;i++)if(std::wstring(args[i])==L"--ir-preset")preset=args[++i];LocalFree(args);if(!preset.empty()){applyTimingPreset(settings,unsigned(std::stoul(preset)));update();notice=preset.empty()?notice:"Applied requested "+utf8(preset)+" Hz IR preset.";}}
        auto startOutput=[&](bool sideBySide=false,bool embedded=false,const RECT* overlay=nullptr){
            if(displays.empty())throw std::runtime_error("No output found.");
            validate(settings);auto& d=displays[displayIndex];
            if(!sideBySide){
                if(settings.refresh<100)throw std::runtime_error("Stereo output requires at least 100 Hz.");
                if(!refreshRatesMatch(settings.refresh,d.refresh))throw std::runtime_error("Emitter timing and output refresh differ. Use Match output rate first.");
                auto e=emitter.status();
                if(e.scheduled&&(settings.sequence!=Sequence::Alternating||!refreshRatesMatch(settings.refresh,120)))throw std::runtime_error("RP2040 currently supports 120 Hz Left / Right output only.");
                if(e.state!=EmitterState::Ready&&e.state!=EmitterState::Running)throw std::runtime_error("Connect an emitter to start stereo output.");
            }
            stopOutput();
            // The preview and its controls must share the selected presentation output.
            if(embedded&&!smoke&&MonitorFromWindow(controlWindow,MONITOR_DEFAULTTONEAREST)!=d.monitor){
                MONITORINFO target{sizeof(target)};RECT current{};
                if(!GetMonitorInfoW(d.monitor,&target)||!GetWindowRect(controlWindow,&current))throw std::runtime_error("Cannot position the live preview.");
                int w=std::min(current.right-current.left,target.rcWork.right-target.rcWork.left),h=std::min(current.bottom-current.top,target.rcWork.bottom-target.rcWork.top);
                SetWindowPos(controlWindow,nullptr,target.rcWork.left+(target.rcWork.right-target.rcWork.left-w)/2,target.rcWork.top+(target.rcWork.bottom-target.rcWork.top-h)/2,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
            }
            outputEmbedded=embedded;
            int width=sideBySide?std::min(960L,d.rect.right-d.rect.left):d.rect.right-d.rect.left;
            int height=sideBySide?std::min(600L,d.rect.bottom-d.rect.top):d.rect.bottom-d.rect.top;
            int x=d.rect.left+(d.rect.right-d.rect.left-width)/2,y=d.rect.top+(d.rect.bottom-d.rect.top-height)/2;
            if(embedded){x=previewRect.left;y=previewRect.top;width=previewRect.right-x;height=previewRect.bottom-y;placedPreviewRect=previewRect;}
            if(overlay){x=overlay->left;y=overlay->top;width=overlay->right-overlay->left;height=overlay->bottom-overlay->top;}
            // The Blender overlay lies exactly over Blender's viewport, which keeps focus and receives mouse and keys.
            // Alpha 254 keeps it DWM-composed on every refresh (experiments/overlay-present); at 255 it moves onto a hardware
            // overlay plane and falls back to composition whenever the desktop changes, and each switch forces a resync.
            DWORD exStyle=sideBySide||embedded?0:WS_EX_TOPMOST|(overlay?WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW:0);
            outputWindow=CreateWindowExW(exStyle,L"VisionRestorationOutput",sideBySide?L"Inspect eye images | 2D side by side | Esc: close":L"Stereo output | Esc: return | Arrows: timing | X: swap | Space: pause",embedded?WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS:sideBySide?WS_OVERLAPPEDWINDOW:WS_POPUP,x,y,width,height,embedded?controlWindow:nullptr,nullptr,instance,nullptr);
            if(!outputWindow)throw std::runtime_error("Could not create the output window.");
            outputPassThrough=overlay!=nullptr;if(overlay)SetLayeredWindowAttributes(outputWindow,0,254,LWA_ALPHA);
            // A single owner controls the emitter while either app output is open.
            gameSync.enable(false);
            // Blender's eye images already converge on its screen plane; an extra shift would misalign them with the mouse.
            Settings presented=settings;if(overlay)presented.convergence=0;
            try{presenter.start(outputWindow,d,presented,sideBySide,pattern());}catch(...){stopOutput();throw;}
            if(overlay)ShowWindow(outputWindow,SW_SHOWNOACTIVATE);else{ShowWindow(outputWindow,SW_SHOW);if(!embedded)SetForegroundWindow(outputWindow);}
            notice=overlay?"Blender's viewport is on the glasses. Mouse and keys go to Blender; Ctrl+Alt+F8 stops the output.":embedded?"Live 3D preview: adjust phase, shutter and convergence while watching through the glasses.":sideBySide?"2D inspection of the separate eye images.":"Fullscreen stereo. Escape returns to the controls.";
        };
        auto runAction=[&](auto&& action){try{action();error.clear();}catch(const std::exception& e){error=e.what();}};
        std::string renderCheck="Render checks have not run in this session.";
        std::string vblankReport="No vblank measurement in this session. It times the display's blanking interval against the presentation timestamps and stores the scan start offset.";std::future<VblankMeasurement> vblankJob;
        // Deterministic UI coverage without changing a real display or profile.
        if(smoke&&wcsstr(GetCommandLineW(),L"--timing-mismatch")){settings.refresh=120;if(!displays.empty())displays[displayIndex].refresh=144;}
        RegisterHotKey(controlWindow,1,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F8);
        RegisterHotKey(controlWindow,2,MOD_CONTROL|MOD_ALT,VK_UP);RegisterHotKey(controlWindow,3,MOD_CONTROL|MOD_ALT,VK_DOWN);RegisterHotKey(controlWindow,4,MOD_CONTROL|MOD_ALT,VK_LEFT);RegisterHotKey(controlWindow,5,MOD_CONTROL|MOD_ALT,VK_RIGHT);RegisterHotKey(controlWindow,6,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'X');
        // Re-locking must be reachable while the game holds focus, so it is a global hotkey as well as a button.
        RegisterHotKey(controlWindow,7,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'R');
        gameSync.configure(settings,displays.empty()?nullptr:displays[displayIndex].monitor);
        if(!smoke){ShowWindow(controlWindow,SW_SHOWDEFAULT);UpdateWindow(controlWindow);}
        double smokeStart=qpc();bool smokeSnapshot=false,smokePreviewStarted=false,smokePreviewVerified=false;
        bool previewOnLaunch=!smoke&&wcsstr(GetCommandLineW(),L"--start-preview");
        while(!closeRequested){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(smoke && qpc()-smokeStart>3)break;
            if(outputStop){outputStop=false;stopOutput();}
            if(displayRefreshChanged){displayRefreshChanged=false;try{auto selectedId=settings.displayId;displays=enumerateDisplays();for(size_t i=0;i<displays.size();i++)if(displays[i].id==selectedId){displayIndex=int(i);break;}syncDisplay();update();}catch(const std::exception& e){error=e.what();}}
            if(pauseRequested){pauseRequested=false;paused=!paused;presenter.pause(paused);}
            if(resyncRequested){resyncRequested=false;presenter.resync();notice="Stereo synchronization re-locked.";}
            if(offsetDelta){settings.rightOffsetUs=std::clamp(settings.rightOffsetUs+float(offsetDelta),-1500.f,1500.f);offsetDelta=0;update();}
            if(bandDelta||bandPositionDelta){settings.bandHeight=std::clamp(settings.bandHeight+bandDelta/100.f,.1f,1.f);settings.bandCenter=std::clamp(settings.bandCenter+bandPositionDelta/100.f,settings.bandHeight/2,1-settings.bandHeight/2);bandDelta=bandPositionDelta=0;update();}
            if(phaseDelta||durationDelta||swapRequested){settings.phaseUs+=phaseDelta;settings.leftUs+=durationDelta;settings.rightUs+=durationDelta;if(swapRequested)settings.swapEyes=!settings.swapEyes;phaseDelta=durationDelta=0;swapRequested=false;update();}
            if(sweepToggleRequested){sweepToggleRequested=false;if(sweep.active)stopSweep("Phase sweep stopped; the phase stays where it is.");else runAction(startSweep);}
            if(markRequested){markRequested=false;if(sweep.active){sweep.marks.push_back(settings.phaseUs);std::ostringstream t;t<<"Marked phase "<<std::fixed<<std::setprecision(0)<<settings.phaseUs<<" us (mark "<<sweep.marks.size()<<").";notice=t.str();}}
            if(acceptRequested){acceptRequested=false;stopSweep("Phase sweep stopped at the current phase.");}
            if(bfiToggleRequested){bfiToggleRequested=false;if(!emitter.status().scheduled){settings.sequence=settings.sequence==Sequence::BlackInsertion?Sequence::Alternating:Sequence::BlackInsertion;runAction(update);notice=settings.sequence==Sequence::BlackInsertion?"Software black frame insertion on: Left / Black / Right / Black.":"Software black frame insertion off: Left / Right.";}}
            if(sweep.active){double now=qpc();if(now-sweep.lastStep>=0.1){sweep.lastStep=now;double cycle=phaseCycleUs(settings.refresh,settings.sequence);settings.phaseUs=wrapPhase(sweep.origin+(now-sweep.start)/sweep.secondsPerCycle*cycle,cycle);runAction([&]{update(false);});}}
            if(vblankJob.valid()&&vblankJob.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto measured=vblankJob.get();vblankReport=measured.message;if(measured.ok&&measured.scanlineSamples>=8){settings.scanStartUs=std::clamp(measured.scanStartAfterSyncUs,-5000.,5000.);runAction(update);notice="Vblank measured; the scan start offset is stored with the profile and used by Suggest phase.";}}
            // Preserve the running presentation and calibration across unplug/replug.
            // The NVIDIA firmware is volatile, so a replug appears as boot PID 7003;
            // reconnecting here reloads it and returns to runtime PID 0007 in place.
            if(!smoke&&autoReconnectEmitter&&qpc()>=nextEmitterScan){nextEmitterScan=qpc()+0.5;auto current=emitter.status();if(current.state!=EmitterState::Initializing&&current.state!=EmitterState::Simulated){
                auto scanned=emitter.discover();int runtime=-1,boot=-1;for(size_t i=0;i<scanned.size();++i)if(scanned[i].supported&&!scanned[i].rp2040){if(scanned[i].pid==0x0007)runtime=int(i);else if(scanned[i].pid==0x7003)boot=int(i);}
                int reconnect=-1;if(current.state==EmitterState::Error||current.state==EmitterState::Disconnected)reconnect=runtime>=0?runtime:boot;else if(boot>=0&&runtime<0)reconnect=boot;
                devices=std::move(scanned);if(reconnect>=0){deviceIndex=reconnect;emitter.configure(settings);emitter.connect(devices[deviceIndex],firmware,false);notice="Emitter replug detected. Restoring WinUSB, firmware and shutter output automatically; the stereo session is still running.";}
            }}
            if(!smoke&&qpc()>=nextChannelPoll){nextChannelPoll=qpc()+0.05;
                auto rsNow=presenter.status();auto esNow=emitter.status();const Display* display=displays.empty()?nullptr:&displays[displayIndex];
                auto viewOnDisplay=[&](const RECT& v){RECT clipped{};return display&&v.right-v.left>=32&&v.bottom-v.top>=32&&IntersectRect(&clipped,&v,&display->rect)&&EqualRect(&clipped,&v);};
                FrameChannelReport report;report.outputRunning=blenderOutput&&rsNow.running;report.emitterReady=esNow.state==EmitterState::Ready||esNow.state==EmitterState::Running;
                if(!channelLink.active)report.message="Vision Restoration is ready";
                else if(blenderHold)report.message=error.empty()?"3D output stopped in the app; switch 3D off and on in Blender to restart":"3D output did not start: "+error;
                else if(!report.emitterReady)report.message="Connect the emitter to see the viewport in 3D";
                else if(!viewOnDisplay(channelLink.view))report.message="Move Blender's viewport fully onto "+(display?display->name:std::string("the 3D display"));
                else if(outputWindow&&!blenderOutput)report.message="The app's own output is open; close it to show Blender";
                else if(blenderOutput)report.message=rsNow.locked?"3D viewport on the glasses":"Locking onto the display";
                else report.message="Starting the 3D output";
                channelLink=frameChannel.poll(report);
                bool wanted=channelLink.present&&channelLink.active;RECT view=channelLink.view;bool onDisplay=viewOnDisplay(view);
                if(blenderOutput&&!outputWindow){blenderOutput=false;blenderHold=true;}
                if(!wanted){
                    if(blenderOutput){stopOutput();blenderOutput=false;}
                    if(blenderSource){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;blenderSource=false;runAction(update);notice=channelLink.present?"Blender's 3D viewport is switched off.":"Blender disconnected; back to the built-in patterns.";}
                    blenderHold=false;
                }else{
                    // Another input chosen in the app wins until Blender's 3D is switched off and on again.
                    if(blenderSource&&sourceKind!=4){blenderSource=false;blenderHold=true;if(blenderOutput){stopOutput();blenderOutput=false;}}
                    if(!blenderSource&&!blenderHold&&display)runAction([&]{source.stop();sourceConfig.kind=SourceKind::SharedFrames;sourceConfig.packing=Packing::SideBySide;sourceKind=4;source.start(sourceConfig,display->adapterLuid);blenderSource=true;notice="Blender connected: its 3D viewport is the input.";});
                    if(blenderSource&&!blenderHold&&!outputWindow&&onDisplay&&report.emitterReady){overlayRect=view;runAction([&]{startOutput(false,false,&overlayRect);});if(outputWindow){blenderOutput=true;runAction(update);}else blenderHold=true;}
                    if(blenderOutput){
                        bool show=channelLink.visible&&onDisplay;
                        if(show&&!EqualRect(&view,&overlayRect)){overlayRect=view;SetWindowPos(outputWindow,HWND_TOPMOST,view.left,view.top,view.right-view.left,view.bottom-view.top,SWP_NOACTIVATE);}
                        if(show!=(IsWindowVisible(outputWindow)!=FALSE))ShowWindow(outputWindow,show?SW_SHOWNOACTIVATE:SW_HIDE);
                    }
                }
            }
            RECT client{};GetClientRect(controlWindow,&client);if(client.right>0&&client.bottom>0&&(unsigned(client.right)!=ui.width||unsigned(client.bottom)!=ui.height))ui.resize(client.right,client.bottom);
            if(IsIconic(controlWindow)){Sleep(20);continue;}
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);ImGui::Begin("Vision setup",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
            label("VISION RESTORATION");ImGui::SetWindowFontScale(1.6f);ImGui::TextUnformatted("Stereo output");ImGui::SetWindowFontScale(1);ImGui::TextDisabled("Separate eye images. Live shutter timing.");ImGui::Spacing();
            auto rs=presenter.status();auto es=emitter.status();auto ss=source.status();
            {static double lastLog=0;static bool fresh=true;double now=qpc();if(!smoke&&now-lastLog>2){lastLog=now;try{std::filesystem::create_directories(workspace()/L"reports");std::ofstream log(workspace()/L"reports/session.log",fresh?std::ios::trunc:std::ios::app);fresh=false;
                auto sch=es.scheduled?NvidiaSchedule{0,0,settings.phaseUs}:nvidiaSchedule(settings.refresh,settings.phaseUs);
                log<<std::fixed<<std::setprecision(1)<<"t="<<now<<" refresh="<<settings.refresh<<" hdr="<<settings.hdr<<" phase="<<settings.phaseUs<<" boundary="<<sch.boundaryUs<<" x="<<sch.delayUs<<" shutter="<<settings.leftUs<<"/"<<settings.rightUs<<" swap="<<settings.swapEyes<<" seq="<<int(settings.sequence)
                   <<" | out="<<rs.running<<" preview="<<rs.preview<<" locked="<<rs.locked<<" hz="<<std::setprecision(3)<<rs.measuredHz<<std::setprecision(1)<<" presents="<<rs.presents<<" misses="<<rs.misses<<" resyncs="<<rs.resyncs<<" slipsPerSec="<<rs.slipsLastSecond<<" composed="<<rs.composed<<" modeChanges="<<rs.modeChanges<<" lead="<<rs.leadBins[0]<<"/"<<rs.leadBins[1]<<"/"<<rs.leadBins[2]<<"/"<<rs.leadBins[3]<<"/"<<rs.leadBins[4]<<"/"<<rs.leadBins[5]<<" maxIntervalMs="<<rs.maxIntervalMs<<" vblankJitterUs="<<rs.vblankJitterRmsUs<<"/"<<rs.vblankJitterMaxUs<<" presentJitterUs="<<rs.presentJitterUs<<" gpuPriority=\""<<rs.gpuPriority<<"\" illum="<<int(settings.illumination)<<" strobe="<<settings.strobeStartUs<<"+"<<settings.strobeLengthUs<<" scanStart="<<settings.scanStartUs<<" band="<<settings.bandHeight<<"@"<<settings.bandCenter<<" msg=\""<<rs.message<<"\""
                   <<" | emitter="<<int(es.state)<<" cmds="<<es.commands<<" late="<<es.late<<" errors="<<es.errors<<" lastUs="<<es.lastTransferUs<<" maxUs="<<es.maxTransferUs<<" sendErrUs="<<es.sendErrorLastUs<<"/"<<es.sendErrorRmsUs<<"/"<<es.sendErrorMaxUs<<" writes="<<es.timingWrites<<" msg=\""<<es.message<<"\"";
                // Capture source: frame count, drops, packed frame size and age, so a capture that stalls,
                // resizes or crops when the game gains focus shows up next to the presenter's timing.
                {auto frame=source.latest();log<<" | capture: kind="<<sourceKind<<" frames="<<ss.frames<<" dropped="<<ss.dropped<<" size="<<(frame?frame->width:0)<<"x"<<(frame?frame->height:0)<<" ageMs="<<(ss.lastFrame>0?(now-ss.lastFrame)*1000:-1.)<<" msg=\""<<ss.message<<"\"";}
                auto gs=gameSync.status();
                log<<" | hook: hosting="<<gs.hosting<<" hooked="<<gs.hooked<<" driving="<<gs.driving<<" game=\""<<gs.game<<"\" pid="<<gs.pid<<" frames="<<gs.frames<<" triggers="<<gs.triggers<<" misses="<<gs.misses<<" guesses="<<gs.guesses<<" hz="<<std::setprecision(3)<<gs.measuredHz<<std::setprecision(1)<<" msg=\""<<gs.message<<"\"\n";}catch(...){}}}
            if(es.identity!="unassigned" && (settings.emitterId!=es.identity || settings.emitterFirmware!=es.firmwareVersion)){settings.emitterId=es.identity;settings.emitterFirmware=es.firmwareVersion;settings.validated=false;}

            bool changed=false;
            bool emitterOnline=es.state==EmitterState::Ready||es.state==EmitterState::Running;
            bool rateMatches=!displays.empty()&&refreshRatesMatch(settings.refresh,displays[displayIndex].refresh);
            const char* outputState=!rs.running?"STOPPED":rs.preview?"2D INSPECTION":paused?"PAUSED":rs.locked?"STEREO OUTPUT":"ACQUIRING TIMING";
            ImGui::TextColored(rs.running?ImVec4(.55f,.88f,.35f,1):ImVec4(.65f,.70f,.72f,1),"%s",outputState);
            ImGui::SameLine();ImGui::TextDisabled("| %s",emitterOnline?"Emitter connected":es.state==EmitterState::Initializing?"Emitter connecting":"Emitter disconnected");
            ImGui::BeginDisabled(!emitterOnline||displays.empty()||!rateMatches);
            if(ImGui::Button("Start 3D preview",{170*dpi,36*dpi}))runAction([&]{startOutput(false,true);});
            ImGui::SameLine();if(ImGui::Button("Fullscreen 3D",{150*dpi,36*dpi}))runAction([&]{startOutput();});
            ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(displays.empty());
            if(ImGui::Button("Inspect eye images",{170*dpi,36*dpi}))runAction([&]{startOutput(true);});
            ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!rs.running);
            if(ImGui::Button(paused?"Resume":"Pause",{85*dpi,36*dpi})){paused=!paused;presenter.pause(paused);}
            ImGui::SameLine();if(ImGui::Button("Stop",{80*dpi,36*dpi}))stopOutput();ImGui::EndDisabled();
            ImGui::BeginDisabled(!rs.running||rs.preview);
            if(ImGui::Button("Re-lock sync (Ctrl+Alt+R)",{250*dpi,36*dpi}))resyncRequested=true;
            ImGui::EndDisabled();ImGui::SameLine();
            ImGui::TextDisabled(rs.occluded?"Output window is behind the game: normal, sync is held.":"Sync is never reset automatically.");
            ImGui::TextDisabled("Fullscreen: Esc returns, Space pauses, X swaps, S sweeps the phase, M marks, Enter keeps, B toggles black frames. Arrows tune timing; Shift selects fine steps. PageUp/PageDown: area height, Home/End: area position.");
            ImGui::Separator();
            float workspaceHeight=std::max(180*dpi,ImGui::GetContentRegionAvail().y-64*dpi);
            ImGui::BeginTable("live_workspace",2,ImGuiTableFlags_SizingStretchProp);
            ImGui::TableSetupColumn("controls",ImGuiTableColumnFlags_WidthStretch,.48f);ImGui::TableSetupColumn("preview",ImGuiTableColumnFlags_WidthStretch,.52f);
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("workspace",{0,workspaceHeight},false);
            if(ImGui::BeginTabBar("workspace_tabs")){
                if(ImGui::BeginTabItem("Output & timing")){
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(280*dpi);
                    if(ImGui::BeginCombo("Output",displays.empty()?"No output":displays[displayIndex].name.c_str())){
                        for(size_t i=0;i<displays.size();++i)if(ImGui::Selectable(displays[i].name.c_str(),int(i)==displayIndex))runAction([&]{stopOutput();source.stop();displayIndex=int(i);syncDisplay(true);update();});
                        ImGui::EndCombo();
                    }
                    if(!displays.empty()){auto& d=displays[displayIndex];ImGui::TextDisabled("%u x %u | %.3f Hz | %s",d.width,d.height,d.refresh,settings.hdr?"HDR output":"SDR output");if(d.scanUs>0){ImGui::SameLine();ImGui::TextDisabled("| signal scan %.1f ms",d.scanUs/1000);}}
                    if(!rateMatches)ImGui::TextColored({1,.45f,.3f,1},"TIMING MISMATCH: match the emitter rate before starting stereo.");
                    ImGui::Text("Emitter rate: %.3f Hz",settings.refresh);ImGui::SameLine();ImGui::BeginDisabled(displays.empty());
                    if(ImGui::Button("Match output rate"))runAction([&]{
                        double hz=displays[displayIndex].refresh;if(es.scheduled&&!refreshRatesMatch(hz,120))throw std::runtime_error("This RP2040 backend supports 120 Hz only.");
                        if(hz<100)throw std::runtime_error("Stereo output requires at least 100 Hz.");
                        double scale=settings.refresh/hz;settings.phaseUs*=scale;settings.leftUs*=scale;settings.rightUs*=scale;settings.refresh=hz;update();notice="Emitter timing now follows the selected output rate.";
                    });ImGui::EndDisabled();ImGui::Spacing();ImGui::Separator();
                    {label("SHUTTER TIMING");
                        double period=periodUs(settings.refresh),phaseMin=es.scheduled?-period*.5:0,phaseMax=es.scheduled?period*.5:phaseCycleUs(settings.refresh,settings.sequence)-1;
                        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderScalar("##phase",ImGuiDataType_Double,&settings.phaseUs,&phaseMin,&phaseMax,"Phase %.0f us");
                        ImGui::SetNextItemWidth(220*dpi);changed|=ImGui::InputDouble("Phase us",&settings.phaseUs,100,10,"%.0f");
                        if(!es.scheduled)ImGui::TextDisabled("Full eye cycle: 0 to %.0f us",phaseMax);
                        // The shutter may span the whole emitter period, which is two display refreshes in a
                        // four-slot sequence. Capping it at one refresh discarded half of a black-frame sequence's light.
                        double shutter=settings.leftUs,shutterMin=minimumShutterUs,shutterMax=es.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));
                        ImGui::SetNextItemWidth(-1);bool shutterChanged=ImGui::SliderScalar("##shutter",ImGuiDataType_Double,&shutter,&shutterMin,&shutterMax,"Shutter %.0f us");
                        ImGui::SetNextItemWidth(220*dpi);shutterChanged|=ImGui::InputDouble("Shutter us",&shutter,100,10,"%.0f");
                        if(shutterChanged){settings.leftUs=settings.rightUs=shutter;changed=true;}
                        if(settings.leftUs!=settings.rightUs)ImGui::Text("Per-eye durations: L %.0f / R %.0f us",settings.leftUs,settings.rightUs);
                        if(settings.sequence!=Sequence::Alternating)ImGui::TextDisabled("This sequence gives the emitter a %.1f ms period, so the shutter can stay open far longer than one refresh; a wider shutter collects more light without new leakage.",periodUs(sequenceEmitterHz(settings.refresh,settings.sequence))/1000);
                        {float gain=settings.imageGain;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##gain",&gain,1,8,"Image brightness %.2fx")){settings.imageGain=gain;changed=true;}
                         paragraph(settings.hdr?"Brightness multiplies the image the app presents, using the display's HDR headroom. It does not reach a game through the hook; raise the game's or the display's own brightness there.":"Brightness multiplies the image the app presents. In SDR it clips to white, so most of the recovery has to come from a wider shutter or the display's brightness control. It does not reach a game through the hook.");}
                        changed|=ImGui::Checkbox("Swap eye order",&settings.swapEyes);
                        paragraph("Phase moves the shutter timing relative to the image. Shutter sets its duration. Changes apply live.");
                        if(ImGui::Button("Reset timing"))runAction(resetTiming);
                        if(es.state==EmitterState::Simulated)ImGui::TextDisabled("Simulated emitter: timing controls change nothing physical.");
                        else if(!es.scheduled&&emitterOnline){auto sch=nvidiaSchedule(settings.refresh/(settings.sequence==Sequence::Alternating?1:2),settings.phaseUs);ImGui::TextDisabled("Emitter path: eye command at the predicted vblank, boundary %.0f us, X delay %.0f us; %llu timing writes, %llu eye commands.",sch.boundaryUs,sch.delayUs,es.timingWrites,es.commands);}
                        {bool bfi=settings.sequence==Sequence::BlackInsertion;ImGui::BeginDisabled(es.scheduled);if(ImGui::Checkbox("Software black frame insertion (Left / Black / Right / Black)",&bfi)){settings.sequence=bfi?Sequence::BlackInsertion:Sequence::Alternating;changed=true;}ImGui::EndDisabled();
                         if(bfi)paragraph("Every second refresh is black, so no row shows the other eye while the panel scans and the whole screen can be clean. Each eye then gets refresh/4 flashes per second: 60 at 240 Hz (as 3D Vision at 120 Hz), only 30 at 120 Hz. Brightness halves. The game hook needs a build with sequence support; otherwise the game keeps Left / Right.");}
                        ImGui::Spacing();label("PHASE SWEEP");
                        ImGui::SetNextItemWidth(160*dpi);{const char* speeds[]{"15 s per cycle","30 s per cycle","60 s per cycle"};int speed=sweep.secondsPerCycle<=15?0:sweep.secondsPerCycle<=30?1:2;if(ImGui::Combo("##sweepspeed",&speed,speeds,3))sweep.secondsPerCycle=speed==0?15:speed==1?30:60;}
                        ImGui::SameLine();ImGui::BeginDisabled(!rs.running||rs.preview);if(ImGui::Button(sweep.active?"Stop sweep (S)":"Start sweep (S)")){if(sweep.active)stopSweep("Phase sweep stopped; the phase stays where it is.");else runAction(startSweep);}ImGui::EndDisabled();
                        ImGui::SameLine();ImGui::BeginDisabled(!sweep.active);if(ImGui::Button("Mark (M)"))markRequested=true;ImGui::EndDisabled();
                        ImGui::SameLine();ImGui::BeginDisabled(sweep.marks.empty());if(ImGui::Button("Clear marks"))sweep.marks.clear();ImGui::EndDisabled();
                        paragraph(sweep.active?"The phase advances through the whole cycle and its value is drawn in the output. Mark where the image is cleanest (sample-and-hold panel) or where it brightens and fades (strobed panel, shortest shutter). Enter keeps the current phase.":"Sweeps the phase slowly through the whole cycle while you watch through the glasses; start the 3D output first. Marks record phases to return to.");
                        for(size_t i=0;i<sweep.marks.size();i++){ImGui::Text("Mark %zu: %.0f us",i+1,sweep.marks[i]);ImGui::SameLine();ImGui::PushID(int(i));if(ImGui::Button("Use")){settings.phaseUs=sweep.marks[i];changed=true;}ImGui::PopID();}
                        if(sweep.marks.size()>=2){double cycle=phaseCycleUs(settings.refresh,settings.sequence);double a=sweep.marks[sweep.marks.size()-2],b=sweep.marks.back();double span=wrapPhase(b-a,cycle);
                            if(ImGui::Button("Use the middle of the last two marks")){settings.phaseUs=wrapPhase(a+span/2,cycle);changed=true;}
                            ImGui::SameLine();if(ImGui::Button("Set strobe from the last two marks")){
                                // The image appears when the shutter [phase, phase+shutter] first overlaps the pulse and
                                // fades when the shutter has passed it: pulse start = first mark + shutter, end = last mark.
                                double shutterNow=std::max(settings.leftUs,settings.rightUs),refreshPeriod=periodUs(settings.refresh);settings.illumination=Illumination::Strobed;
                                settings.strobeStartUs=std::clamp(wrapPhase(modelPhaseFromEmitter(a,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency())+shutterNow,refreshPeriod),0.,50000.);settings.strobeLengthUs=std::clamp(span-shutterNow,50.,50000.);changed=true;
                                notice="Strobe pulse set from the marks (start = first mark + shutter, end = last mark). Press Suggest phase to center the shutter on the pulse, then verify by eye.";}
                            ImGui::TextDisabled("Marks %.0f us apart.",span);}
                    }
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Sources & games")){
                    label("STEREO SOURCE");
                    if(ImGui::Combo("Input",&sourceKind,"Built-in patterns\0Stereo image\0Capture window\0Spout texture\0Blender viewport\0")){source.stop();sourceConfig.kind=SourceKind(sourceKind);changed=true;}
                    if(sourceKind==4||channelLink.present)paragraph(channelLink.present?("Blender connected (pid "+std::to_string(channelLink.pid)+"): 3D viewport "+(channelLink.active?"on":"off")+".").c_str():"Press the 3D button in a Blender viewport header; this input and the output start on their own.");
                    int packing=int(sourceConfig.packing);if(ImGui::Combo("Packing",&packing,"Side by side\0Top / bottom\0"))sourceConfig.packing=Packing(packing);
                    if(sourceKind==1&&ImGui::Button("Choose image..."))runAction([&]{auto file=chooseFile(controlWindow,L"Stereo images\0*.png;*.jpg;*.jpeg;*.bmp;*.jps\0\0");if(!file.empty())sourceConfig.file=file;});
                    if(sourceKind==2){
                        if(ImGui::Button("Refresh windows"))runAction([&]{windows=captureWindows(controlWindow);std::erase_if(windows,[&](auto& w){return w.first==outputWindow;});windowIndex=0;});
                        if(ImGui::BeginCombo("Source window",windows.empty()?"None":windows[windowIndex].second.c_str())){for(size_t i=0;i<windows.size();++i)if(ImGui::Selectable(windows[i].second.c_str(),int(i)==windowIndex))windowIndex=int(i);ImGui::EndCombo();}
                        if(!windows.empty())sourceConfig.window=windows[windowIndex].first;
                        ImGui::Checkbox("Capture HDR / scRGB",&sourceConfig.captureHDR);
                    }
                    if(sourceKind==3){ImGui::InputText("Spout sender",sender,sizeof(sender));sourceConfig.sender=sender;int encoding=int(sourceConfig.encoding);if(ImGui::Combo("Color space",&encoding,"SDR sRGB\0Linear scRGB\0HDR10 PQ\0"))sourceConfig.encoding=Encoding(encoding);}
                    ImGui::BeginDisabled(displays.empty()||sourceKind==0);if(ImGui::Button("Start source"))runAction([&]{source.start(sourceConfig,displays[displayIndex].adapterLuid);changed=true;});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Use built-in patterns")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;changed=true;}paragraph(ss.message.c_str());
                    ImGui::Spacing();ImGui::Separator();label("GAME HOOK");auto gs=gameSync.status();paragraph(gs.message.c_str());
                    paragraph("Use a game that renders side-by-side stereo through Direct3D 11. Leave this app open for emitter control.");
                    auto addon=executableDirectory()/L"VisionGameHook.addon64";
                    if(ImGui::Button("Install game hook..."))runAction([&]{auto exe=chooseFile(controlWindow,L"Game executable\0*.exe\0\0");if(!exe.empty())notice=installGameHook(exe,addon);});
                    ImGui::SameLine();if(ImGui::Button("Remove game hook..."))runAction([&]{auto exe=chooseFile(controlWindow,L"Game executable\0*.exe\0\0");if(!exe.empty())notice=removeGameHook(exe,addon);});
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Profiles")){
                    if(!profilesScanned)runAction(scanProfiles);
                    ImGui::InputText("Name",name,sizeof(name));
                    if(ImGui::BeginCombo("Saved profiles",profileList.empty()?"No saved profiles":profileList[profileIndex].first.c_str())){for(size_t i=0;i<profileList.size();++i)if(ImGui::Selectable(profileList[i].first.c_str(),int(i)==profileIndex))profileIndex=int(i);ImGui::EndCombo();}
                    ImGui::BeginDisabled(profileList.empty());if(ImGui::Button("Load profile"))runAction([&]{applyProfile(loadProfile(profileList[profileIndex].second));});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Save profile"))runAction([&]{settings.name=name;settings.monitorNotes=notes;saveProfile(profileFileFor(name),settings);scanProfiles();notice=std::string("Saved profile: ")+name;});
                    ImGui::SameLine();if(ImGui::Button("Refresh list"))runAction(scanProfiles);
                    paragraph("Timing and scene changes autosave. Named profiles let you keep separate setups.");
                    ImGui::InputTextMultiline("Notes",notes,sizeof(notes),{0,120*dpi});
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Diagnostics")){
                    label("PRESENTATION");paragraph(rs.message.c_str());
                    ImGui::Text("Refresh %.3f Hz | Presents %llu | Slips %llu (%u in the last second) | Resyncs %llu | GPU scheduling %s",rs.measuredHz,rs.presents,rs.misses,rs.slipsLastSecond,rs.resyncs,rs.gpuPriority.c_str());
                    ImGui::Text("Missed timing %llu | Late USB %llu | USB errors %llu",rs.misses,es.late,es.errors);
                    ImGui::Text("USB transfer: last %.1f us / maximum %.1f us",es.lastTransferUs,es.maxTransferUs);
                    ImGui::Text("Vblank jitter: rms %.1f us / max %.1f us over %u refreshes | present interval jitter %.1f us",rs.vblankJitterRmsUs,rs.vblankJitterMaxUs,rs.clockSamples,rs.presentJitterUs);
                    ImGui::Text("Eye command timing error: last %+.0f us / rms %.0f us / max %.0f us | mean USB transfer %.0f us | timing writes %llu",es.sendErrorLastUs,es.sendErrorRmsUs,es.sendErrorMaxUs,es.meanTransferUs,es.timingWrites);
                    paragraph("Vblank jitter is the scatter of the display's presentation timestamps around the fitted refresh clock; the eye command error is how far from the predicted vblank the USB write started. Both are host timings, not optical measurements.");
                    label("VBLANK MEASUREMENT");paragraph(vblankReport.c_str());
                    {bool measuring=vblankJob.valid();ImGui::BeginDisabled(rs.running||measuring||displays.empty());
                     if(ImGui::Button(measuring?"Measuring...":"Measure vblank (3 s)")){auto d=displays[displayIndex];vblankJob=std::async(std::launch::async,[d]{return measureVblank(d,3.0);});vblankReport="Measuring the selected output for 3 seconds...";}
                     ImGui::EndDisabled();if(rs.running)ImGui::TextDisabled("Stop output before measuring.");}
                    label("EMITTER");paragraph(es.message.c_str());
                    if(es.scheduled)ImGui::Text("Device open / close: %llu / %llu",es.deviceOpens,es.deviceCloses);
                    ImGui::Spacing();label("IMAGE SEPARATION");paragraph(renderCheck.c_str());
                    ImGui::BeginDisabled(rs.running);if(ImGui::Button("Run render checks"))runAction([&]{renderCheck="Render checks failed; see the error below.";runGpuSelfTest(workspace()/L"reports");renderCheck="PASS: separate eye images and black frames in SDR and HDR.";});ImGui::EndDisabled();
                    if(rs.running)ImGui::TextDisabled("Stop output before running render checks.");
                    paragraph("Render checks inspect the app's pixels. Presentation timing and USB success do not measure leakage through a lens.");
                    if(ImGui::Button("Export timing report"))runAction([&]{auto path=workspace()/L"reports"/L"stereo-diagnostics.txt";presenter.exportReport(path,settings,ss.message);notice="Saved reports/stereo-diagnostics.txt";});
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Advanced")){
                    label("EMITTER CONNECTION");
                    if(ImGui::Button("Refresh USB"))runAction([&]{devices=emitter.discover();deviceIndex=0;});
                    deviceIndex=devices.empty()?0:std::clamp(deviceIndex,0,int(devices.size())-1);
                    if(ImGui::BeginCombo("Emitter",devices.empty()?"None":devices[deviceIndex].description.c_str())){for(size_t i=0;i<devices.size();++i)if(ImGui::Selectable(devices[i].description.c_str(),int(i)==deviceIndex))deviceIndex=int(i);ImGui::EndCombo();}
                    bool canConnect=!devices.empty()&&devices[deviceIndex].supported;ImGui::BeginDisabled(!canConnect);
                    if(ImGui::Button("Reconnect emitter"))runAction([&]{stopOutput();emitter.configure(settings);emitter.connect(devices[deviceIndex],firmware,false);autoReconnectEmitter=!devices[deviceIndex].rp2040;});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Disconnect emitter")){autoReconnectEmitter=false;stopOutput();emitter.disconnect();}
                    if(!devices.empty()&&devices[deviceIndex].descriptorFailed&&ImGui::Button("Recover failed USB port"))runAction([&]{emitter.recoverPort(devices[deviceIndex]);nextEmitterScan=0;});
                    ImGui::Spacing();label("PER-EYE TIMING");
                    // Per-eye X/Y writes exist only on the NVIDIA emitter and only inside a two-slot cycle.
                    bool perEyeApplies=!es.scheduled&&es.state!=EmitterState::Simulated&&settings.sequence==Sequence::Alternating;
                    ImGui::BeginDisabled(!perEyeApplies);
                    changed|=ImGui::InputDouble("Left shutter us",&settings.leftUs,100,10,"%.0f");changed|=ImGui::InputDouble("Right shutter us",&settings.rightUs,100,10,"%.0f");
                    changed|=ImGui::SliderFloat("Right eye offset us",&settings.rightOffsetUs,-2000,2000,"%.0f");
                    ImGui::EndDisabled();
                    if(!perEyeApplies)ImGui::TextDisabled("%s",es.scheduled?"Per-eye timing is not available on the RP2040 backend.":settings.sequence!=Sequence::Alternating?"Per-eye timing applies to the Left / Right sequence only; the emitter cannot rewrite X/Y inside a four-slot cycle.":"Per-eye timing needs a connected NVIDIA emitter.");
                    ImGui::BeginDisabled(es.scheduled);int seq=int(settings.sequence);if(ImGui::Combo("Sequence",&seq,"Left / Right\0Left / Black / Right / Black\0Left / Left / Right / Right\0")){settings.sequence=Sequence(seq);changed=true;}ImGui::EndDisabled();
                    if(es.scheduled)ImGui::TextDisabled("The RP2040 backend drives Left / Right only.");
                    ImGui::Spacing();label("PANEL");
                    changed|=ImGui::InputDouble("Panel response us",&settings.panelResponseUs,100,500,"%.0f");
                    changed|=ImGui::InputDouble("Panel scan us (0 = signal timing)",&settings.panelScanUs,100,1000,"%.0f");
                    {int illumination=int(settings.illumination);if(ImGui::Combo("Illumination",&illumination,"Sample and hold (rows stay lit)\0Strobed backlight (LightBoost, black frame insertion, Motion Clearness)\0")){settings.illumination=Illumination(illumination);changed=true;}
                     if(settings.illumination==Illumination::Strobed){changed|=ImGui::InputDouble("Strobe start after scan start us",&settings.strobeStartUs,100,500,"%.0f");changed|=ImGui::InputDouble("Strobe length us",&settings.strobeLengthUs,100,500,"%.0f");
                        paragraph("The panel lights only during this pulse each refresh. Find it with a phase sweep and the shortest shutter (Output & timing > Phase sweep > Set strobe from the last two marks) or from a high-speed clip; Suggest phase then centers the shutter on the pulse.");}}
                    changed|=ImGui::InputDouble("Scan start after presentation timestamp us",&settings.scanStartUs,10,100,"%.0f");ImGui::SameLine();ImGui::TextDisabled("(Diagnostics > Measure vblank)");
                    if(!displays.empty()&&displays[displayIndex].totalLines)ImGui::TextDisabled("Signal: %u active of %u lines, scan %.2f ms of %.2f ms.",displays[displayIndex].activeLines,displays[displayIndex].totalLines,displays[displayIndex].scanUs/1000,periodUs(displays[displayIndex].refresh)/1000);
                    paragraph("Response: time a rewritten row needs to settle (OLED about 200 us, VA TV 3000-5000 us). Scan: measured panel scan if it differs from the signal timing.");
                    label("MANUAL EMITTER RATE");
                    for(unsigned hz:{100u,120u,144u,165u,240u}){ImGui::SameLine();ImGui::BeginDisabled(es.scheduled&&hz!=120);if(ImGui::Button((std::to_string(hz)+" Hz").c_str()))runAction([&]{applyTimingPreset(settings,hz);changed=true;});ImGui::EndDisabled();}
                    ImGui::Spacing();label("OUTPUT FORMAT");
                    if(!displays.empty()){
                        auto& d=displays[displayIndex];ImGui::BeginDisabled(!d.hdrEnabled);if(ImGui::Checkbox("HDR output",&settings.hdr)){stopOutput();changed=true;}ImGui::EndDisabled();
                        std::string mode=d.modes.empty()?"No modes":std::to_string(d.modes[modeIndex].dmPelsWidth)+" x "+std::to_string(d.modes[modeIndex].dmPelsHeight)+" @ "+std::to_string(d.modes[modeIndex].dmDisplayFrequency)+" Hz";
                        if(ImGui::BeginCombo("Mode",mode.c_str())){for(size_t i=0;i<d.modes.size();++i){auto& m=d.modes[i];std::string title=std::to_string(m.dmPelsWidth)+" x "+std::to_string(m.dmPelsHeight)+" @ "+std::to_string(m.dmDisplayFrequency)+" Hz";if(ImGui::Selectable(title.c_str(),int(i)==modeIndex))modeIndex=int(i);}ImGui::EndCombo();}
                        ImGui::BeginDisabled(d.modes.empty());if(ImGui::Button("Apply selected mode"))runAction([&]{stopOutput();source.stop();modeGuard.apply(d,d.modes[modeIndex]);displays=enumerateDisplays();syncDisplay(true);update();});ImGui::EndDisabled();
                    }
                    if(ImGui::Button("Brightness pattern")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=3;changed=true;}
                    ImGui::BeginDisabled(!settings.hdr);changed|=ImGui::SliderFloat("Highlight nits",&settings.peakNits,80,1000,"%.0f");ImGui::EndDisabled();if(!settings.hdr)ImGui::TextDisabled("Highlight nits applies to the brightness ramp in HDR output only.");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();ImGui::TableSetColumnIndex(1);
            label("LIVE 3D PREVIEW");
            float previewWidth=std::max(1.f,ImGui::GetContentRegionAvail().x);
            float previewHeight=std::min(previewWidth*9.f/16.f,workspaceHeight*.48f);
            ImGui::InvisibleButton("preview_surface",{previewHeight*16.f/9.f,previewHeight});
            auto previewMin=ImGui::GetItemRectMin(),previewMax=ImGui::GetItemRectMax(),origin=ImGui::GetMainViewport()->Pos;
            // ImGui coordinates are relative to the main viewport, not desktop screen coordinates.
            previewRect={LONG(previewMin.x-origin.x),LONG(previewMin.y-origin.y),LONG(previewMax.x-origin.x),LONG(previewMax.y-origin.y)};
            ImGui::GetWindowDrawList()->AddRectFilled(previewMin,previewMax,IM_COL32(0,0,0,255));
            if(outputEmbedded&&outputWindow){
                if(!EqualRect(&placedPreviewRect,&previewRect)){SetWindowPos(outputWindow,nullptr,previewRect.left,previewRect.top,previewRect.right-previewRect.left,previewRect.bottom-previewRect.top,SWP_NOZORDER|SWP_NOACTIVATE);placedPreviewRect=previewRect;}
            }else ImGui::GetWindowDrawList()->AddText({previewMin.x+16*dpi,previewMin.y+16*dpi},IM_COL32(190,205,185,255),"Press Start 3D preview to view through your glasses.");
            ImGui::BeginChild("preview_controls",{0,std::max(1.f,workspaceHeight-(previewMax.y-previewMin.y)-42*dpi)},false);
            label("IMAGE ALIGNMENT");
            ImGui::SetNextItemWidth(-1);float convergence=settings.convergence*100;
            if(ImGui::SliderFloat("##convergence",&convergence,-5,5,"Convergence %+.2f %%")){settings.convergence=convergence/100;changed=true;}
            ImGui::SetNextItemWidth(210*dpi);if(ImGui::InputFloat("Convergence %",&convergence,.01f,.1f,"%+.2f")){settings.convergence=std::clamp(convergence,-5.f,5.f)/100;changed=true;}
            ImGui::BeginDisabled(sourceKind!=0||step!=2);ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderFloat("##depth",&settings.depth,0,.12f,"Scene depth %.3f");ImGui::EndDisabled();
            paragraph("Convergence shifts the eye images horizontally. Scene depth adjusts the built-in 3D scene.");
            if(ImGui::Button("Reset alignment")){settings.depth=Settings{}.depth;settings.convergence=0;changed=true;}
            ImGui::Spacing();label("STEREO AREA");
            {float bandPercent=settings.bandHeight*100;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##band",&bandPercent,10,100,"Area height %.0f %% of the screen")){settings.bandHeight=bandPercent/100;settings.bandCenter=std::clamp(settings.bandCenter,settings.bandHeight/2,1-settings.bandHeight/2);changed=true;}
             float bandPos=settings.bandCenter*100;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##bandpos",&bandPos,0,100,"Area center %.0f %% from the top")){settings.bandCenter=std::clamp(bandPos/100,settings.bandHeight/2,1-settings.bandHeight/2);changed=true;}
             double scanUs=settings.panelScanUs>0?settings.panelScanUs:displays.empty()?0:displays[displayIndex].scanUs;double shutter=std::max(settings.leftUs,settings.rightUs);
             if(scanUs>0){auto panel=panelTiming();auto w=settledWindow(settings.refresh,settings.sequence,scanUs,settings.bandHeight,settings.bandCenter,settings.panelResponseUs);double window=w.closeUs-w.openUs;auto full=settledWindow(settings.refresh,settings.sequence,scanUs,1,.5,settings.panelResponseUs);
                if(settings.illumination==Illumination::Strobed)ImGui::TextWrapped("Strobed panel: pulse %.1f ms long, %.1f ms after the scan start; scan %.1f ms of the %.2f ms refresh. Rows settled before the pulse are clean; the shutter should cover the pulse. Shutter %.1f ms.",settings.strobeLengthUs/1000,settings.strobeStartUs/1000,scanUs/1000,periodUs(settings.refresh)/1000,shutter/1000);
                else ImGui::TextWrapped("Panel scan %.1f ms of the %.2f ms refresh (%s). Whole screen shows one eye for %.1f ms; this area for %.1f ms. Shutter %.1f ms.",scanUs/1000,periodUs(settings.refresh)/1000,settings.panelScanUs>0?"manual":"from the signal timing",std::max(0.,full.closeUs-full.openUs)/1000,std::max(0.,window)/1000,shutter/1000);
                // The model follows every row through the cycle; it predicts where this phase leaks and by how much.
                auto predicted=estimateLeakage(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,modelPhaseFromEmitter(settings.phaseUs,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency()),shutter);
                ImVec4 tone=predicted.mean<.01?ImVec4(.55f,.88f,.35f,1):predicted.mean<.05?ImVec4(1,.8f,.3f,1):ImVec4(1,.45f,.3f,1);
                ImGui::TextColored(tone,"Predicted other-eye leakage at this phase: top %.0f %%, center %.0f %%, bottom %.0f %% (mean %.1f %%).",std::min(predicted.top,9.99)*100,std::min(predicted.center,9.99)*100,std::min(predicted.bottom,9.99)*100,std::min(predicted.mean,9.99)*100);
                // Light actually reaching the eye, as a fraction of one fully lit refresh per row. The lit
                // fraction of the shutter falls as the shutter widens even while the collected light rises,
                // so reporting that fraction would argue for the dimmer setting.
                ImGui::Text("Light reaching this eye: %.0f %% of a fully lit frame.",predicted.brightness*shutter/periodUs(settings.refresh)*100);
                if(window<shutter&&settings.illumination==Illumination::SampleAndHold&&settings.sequence==Sequence::Alternating)ImGui::TextColored({1,.45f,.3f,1},"The shutter is longer than the settled window: rows near the area edges show both eyes. Fit the area, shorten the shutter, or use black frame insertion.");
                if(ImGui::Button("Fit area to shutter")){settings.bandHeight=float(bandHeightForLeakage(settings.refresh,settings.sequence,panel,settings.bandCenter,shutter,.01));settings.bandCenter=std::clamp(settings.bandCenter,settings.bandHeight/2,1-settings.bandHeight/2);changed=true;}
                ImGui::SameLine();if(ImGui::Button("Maximize brightness")){
                    double ceiling=es.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));
                    double widest=brightestShutterUs(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,.01,ceiling);
                    settings.leftUs=settings.rightUs=widest;settings.rightOffsetUs=0;
                    double model=bestModelPhaseUs(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,widest);
                    settings.phaseUs=emitterPhaseFromModel(model,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency());changed=true;
                    auto after=estimateLeakage(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,model,widest);
                    std::ostringstream t;t<<std::fixed<<std::setprecision(0)<<"Shutter widened to "<<widest<<" us and the phase recentred: "<<after.brightness*widest/periodUs(settings.refresh)*100<<" % of a fully lit frame at "<<std::setprecision(1)<<std::min(after.mean,9.99)*100<<" % predicted leakage. Verify through the glasses.";notice=t.str();}
                ImGui::SameLine();if(ImGui::Button("Suggest phase")){double model=bestModelPhaseUs(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,shutter);settings.phaseUs=emitterPhaseFromModel(model,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency());changed=true;
                    std::ostringstream t;t<<std::fixed<<std::setprecision(0)<<"Phase set to the least-leakage window of the model (scan starts "<<settings.scanStartUs<<" us after the timestamp, USB latency "<<usbLatency()<<" us). Lens latency is not modelled: sweep from here.";notice=t.str();}
             }else paragraph("Panel scan time unknown; enter it under Advanced > Panel.");
             paragraph("A panel rewrites its rows top to bottom over most of each refresh, so the whole screen is never one eye for long. A shorter area covers less of that scan and leaves a longer window for the shutter. PageUp/PageDown and Home/End adjust it in fullscreen.");}
            ImGui::Separator();label("PREVIEW IMAGE");
            auto selectPattern=[&](int selected){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=selected;changed=true;};
            if(ImGui::Button("Stereo scene"))selectPattern(2);ImGui::SameLine();if(ImGui::Button("Both eye targets"))selectPattern(1);
            if(ImGui::Button("Left image only"))selectPattern(4);ImGui::SameLine();if(ImGui::Button("Right image only"))selectPattern(5);
            const char* patternNames[]{"Eye identification","Both eye targets","Stereo scene","Brightness ramp","Left image only","Right image only"};
            ImGui::TextWrapped("Selected: %s",sourceKind==0?patternNames[step]:"External stereo source");
            if(sourceKind==0&&step!=2)paragraph("Eye-isolation targets stay fixed. Select Stereo scene to adjust alignment.");
            if(changed)runAction(update);
            ImGui::EndChild();ImGui::EndTable();
            if(!error.empty()){ImGui::TextWrapped("%s",error.c_str());}else paragraph(notice.c_str());
            if(previewOnLaunch&&emitterOnline&&rateMatches){previewOnLaunch=false;runAction([&]{startOutput(false,true);});}
            // Exercise the actual child-window presenter without USB writes or profile edits.
            // The production button passes sideBySide=false to enable frame-sequential 3D.
            if(smoke&&!smokePreviewStarted&&qpc()-smokeStart>1){startOutput(true,true);smokePreviewStarted=true;settings.convergence=.01f;settings.phaseUs+=100;update();}
            if(smoke&&smokePreviewStarted&&!smokePreviewVerified&&qpc()-smokeStart>2.5){
                RECT actual{};GetWindowRect(outputWindow,&actual);MapWindowPoints(nullptr,controlWindow,reinterpret_cast<POINT*>(&actual),2);
                auto test=presenter.status();
                // A hidden parent is occluded, so Present need not advance in this smoke test.
                if(GetParent(outputWindow)!=controlWindow||!EqualRect(&actual,&previewRect)||!test.running||emitter.status().commands!=0)throw std::runtime_error("Embedded live preview regression: window placement, presenter startup or USB isolation failed: "+test.message);
                smokePreviewVerified=true;
            }
            ImGui::End();ImGui::Render();ui.bind();float clear[]{.035f,.047f,.07f,1};ui.context->ClearRenderTargetView(ui.target.Get(),clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());if(smoke && !smokeSnapshot && qpc()-smokeStart>.75){saveSurfacePng(ui,workspace()/L"reports/ui-preview.png");smokeSnapshot=true;}HRESULT shown=ui.swap->Present(1,0);if(shown==DXGI_STATUS_OCCLUDED)Sleep(20);else check(shown,"Control window present");
        }
        stopOutput();gameSync.stop();source.stop();emitter.disconnect();modeGuard.restore();for(int id=1;id<=7;id++)UnregisterHotKey(controlWindow,id);ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();DestroyWindow(controlWindow);
        if(smoke){std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/ui-smoke.txt");if(!smokePreviewVerified)throw std::runtime_error("Embedded preview check did not complete.");f<<"PASS: native UI, embedded child-window placement and presenter startup after live alignment/timing edits; clean shutdown. Hidden parent: visible presentation not measured. No emitter writes, profile edits or display mode changes.\n";}
    }catch(const std::exception& e){std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/last-error.txt");f<<e.what();if(!gpuTest&&!probe&&!smoke)MessageBoxA(nullptr,e.what(),"Vision Restoration",MB_OK|MB_ICONERROR);if(SUCCEEDED(co))CoUninitialize();return 1;}
    if(SUCCEEDED(co))CoUninitialize();return 0;
}
