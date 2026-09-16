#include "renderer.h"
#include "rp2040_timing.h"
#include "lcd_ui.h"
#include "lcd_rp2040.h"
#include "control_menu.h"
#include "key_bindings.h"
#include "gamesync.h"
#include "stereo_compatibility.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3dkmthk.h>
#include <dbghelp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
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
static bool fullscreenMode=false,fullscreenRequested=false,controlsToggleRequested=false,controlsShowRequested=false;
static KeyBindings* shortcuts=nullptr;
static constexpr UINT trayMessage=WM_APP+100;
static int lcdTargetRequested=-1;
static int phaseDelta=0,durationDelta=0,offsetDelta=0,bandDelta=0,bandPositionDelta=0;static bool swapRequested=false;
static bool sweepToggleRequested=false,markRequested=false,acceptRequested=false,bfiToggleRequested=false,cancelToggleRequested=false;
static int screenDepthDelta=0,screenPlaneDelta=0;static bool screenToggleRequested=false;
static int brightnessDelta=0,convergenceDelta=0;static bool hdrToggleRequested=false;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
static std::filesystem::path executableDirectory(){wchar_t path[32768]{};DWORD n=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));if(!n||n==std::size(path))throw std::runtime_error("Cannot locate the application directory.");return std::filesystem::path(path).parent_path();}
static std::filesystem::path workspace(){auto directory=executableDirectory(),candidate=directory.parent_path().parent_path().parent_path();return std::filesystem::exists(candidate/L"CMakeLists.txt")?candidate:directory;}
static wchar_t crashDumpPath[32768]{};
static LONG WINAPI recordNativeCrash(EXCEPTION_POINTERS* exception){
    HANDLE file=CreateFileW(crashDumpPath,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception,FALSE};MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,MiniDumpNormal,&info,nullptr,nullptr);CloseHandle(file);}
    return EXCEPTION_EXECUTE_HANDLER;
}
// Depth models for the whole-screen conversion: every .onnx beside the app or in models/
// (tools/Get-Dependencies.ps1, tools/Get-DepthModel.ps1). The default is Depth Anything V2 Small.
static std::vector<std::filesystem::path> listDepthModels(){
    std::vector<std::filesystem::path> found;
    for(const auto& dir:{executableDirectory(),workspace()/L"models"}){std::error_code ec;if(!std::filesystem::is_directory(dir,ec))continue;
        for(auto& e:std::filesystem::directory_iterator(dir,ec))if(e.is_regular_file()&&e.path().extension()==L".onnx")found.push_back(e.path());}
    std::sort(found.begin(),found.end());return found;
}
static std::filesystem::path findDepthModel(){
    auto models=listDepthModels();
    for(auto& m:models)if(m.filename().wstring().find(L"depth-anything-v2-small")!=std::wstring::npos)return m;
    return models.empty()?std::filesystem::path{}:models.front();
}
static std::filesystem::path chooseFile(HWND owner,const wchar_t* filter,bool save=false){
    wchar_t path[32768]{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=owner;o.lpstrFilter=filter;o.lpstrFile=path;o.nMaxFile=32768;o.Flags=OFN_NOCHANGEDIR|OFN_EXPLORER|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    return (save?GetSaveFileNameW(&o):GetOpenFileNameW(&o))?std::filesystem::path(path):std::filesystem::path{};
}
static void dispatchShortcut(KeyAction action){
    switch(action){
    case KeyAction::Menu:controlsToggleRequested=true;break;
    case KeyAction::Fullscreen:fullscreenRequested=true;break;
    case KeyAction::Stop:outputStop=true;break;
    case KeyAction::Pause:pauseRequested=true;break;
    case KeyAction::PhaseUp:phaseDelta+=100;break;case KeyAction::PhaseDown:phaseDelta-=100;break;
    case KeyAction::ShutterUp:durationDelta+=100;break;case KeyAction::ShutterDown:durationDelta-=100;break;
    case KeyAction::Swap:swapRequested=true;break;case KeyAction::Relock:resyncRequested=true;break;
    case KeyAction::DepthUp:screenDepthDelta++;break;case KeyAction::DepthDown:screenDepthDelta--;break;
    case KeyAction::PlaneNear:screenPlaneDelta++;break;case KeyAction::PlaneFar:screenPlaneDelta--;break;
    case KeyAction::DepthToggle:screenToggleRequested=true;break;
    case KeyAction::BandUp:bandDelta+=5;break;case KeyAction::BandDown:bandDelta-=5;break;
    case KeyAction::BandMoveUp:bandPositionDelta-=5;break;case KeyAction::BandMoveDown:bandPositionDelta+=5;break;
    case KeyAction::Sweep:sweepToggleRequested=true;break;case KeyAction::Mark:markRequested=true;break;case KeyAction::Accept:acceptRequested=true;break;
    case KeyAction::Bfi:bfiToggleRequested=true;break;case KeyAction::Crosstalk:cancelToggleRequested=true;break;
    case KeyAction::Target0:lcdTargetRequested=0;break;case KeyAction::Target1:lcdTargetRequested=1;break;case KeyAction::Target2:lcdTargetRequested=2;break;case KeyAction::Target3:lcdTargetRequested=3;break;
    case KeyAction::BrightnessUp:brightnessDelta++;break;case KeyAction::BrightnessDown:brightnessDelta--;break;
    case KeyAction::Hdr:hdrToggleRequested=true;break;
    case KeyAction::ConvergenceUp:convergenceDelta++;break;case KeyAction::ConvergenceDown:convergenceDelta--;break;
    }
}
static LRESULT CALLBACK outputProc(HWND w,UINT m,WPARAM a,LPARAM b){
    if(m==WM_CLOSE){outputStop=true;return 0;}
    if(fullscreenMode&&(m==WM_KEYDOWN||m==WM_SYSKEYDOWN)){
        if(shortcuts)shortcuts->keyDown(UINT(a),b);return 0;
    }
    if(m==WM_KEYDOWN&&a==VK_F11){fullscreenRequested=true;return 0;}
    if(m==WM_KEYDOWN){int step=(GetKeyState(VK_SHIFT)&0x8000)?10:100;if(a==VK_ESCAPE || a==VK_F1)outputStop=true;else if(a==VK_SPACE)pauseRequested=true;else if(a=='X')swapRequested=true;else if(a==VK_DOWN)phaseDelta-=step;else if(a==VK_UP)phaseDelta+=step;else if(a==VK_LEFT)durationDelta-=step;else if(a==VK_RIGHT)durationDelta+=step;else if(a==VK_PRIOR)bandDelta+=5;else if(a==VK_NEXT)bandDelta-=5;else if(a==VK_HOME)bandPositionDelta-=5;else if(a==VK_END)bandPositionDelta+=5;else if(a>='0'&&a<='3')lcdTargetRequested=int(a-'0');else if(a=='S')sweepToggleRequested=true;else if(a=='M')markRequested=true;else if(a==VK_RETURN)acceptRequested=true;else if(a=='B')bfiToggleRequested=true;else if(a=='C')cancelToggleRequested=true;return 0;}
    // A display mode change invalidates the fullscreen presentation target. A USB
    // device change does not: keep presenting while the emitter reconnects.
    if(m==WM_DISPLAYCHANGE){outputStop=true;displayRefreshChanged=true;return 0;}
    return DefWindowProcW(w,m,a,b);
}
static LRESULT CALLBACK controlProc(HWND w,UINT m,WPARAM a,LPARAM b){
    if(m==trayMessage+1){controlsShowRequested=true;return 0;}
    if(m==trayMessage){
        if(b==WM_LBUTTONUP||b==WM_LBUTTONDBLCLK)controlsShowRequested=true;
        if(b==WM_RBUTTONUP){
            HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Show controls");AppendMenuW(menu,MF_STRING|(shortcuts&&shortcuts->enabled()?MF_CHECKED:0),2,L"Enable fullscreen shortcuts");AppendMenuW(menu,MF_STRING,3,L"Exit");
            POINT cursor{};GetCursorPos(&cursor);SetForegroundWindow(w);const UINT action=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,cursor.x,cursor.y,0,w,nullptr);DestroyMenu(menu);
            if(action==1)controlsShowRequested=true;else if(action==2&&shortcuts)shortcuts->enable(!shortcuts->enabled());else if(action==3)closeRequested=true;
            PostMessageW(w,WM_NULL,0,0);
        }return 0;
    }
    if(m==WM_HOTKEY&&shortcuts){
        if(!(GetForegroundWindow()==w&&ImGui::GetCurrentContext()&&ImGui::GetIO().WantTextInput))shortcuts->hotkey(UINT(a));return 0;
    }
    if((m==WM_KEYDOWN||m==WM_SYSKEYDOWN)&&shortcuts){
        const bool editing=ImGui::GetCurrentContext()&&(ImGui::GetIO().WantTextInput||ImGui::IsAnyItemActive()||ImGui::IsAnyItemFocused());
        if((shortcuts->capturing()>=0||(!editing&&fullscreenMode))&&shortcuts->keyDown(UINT(a),b))return 0;
    }
    if(m==WM_KEYDOWN&&!(ImGui::GetCurrentContext()&&ImGui::GetIO().WantTextInput)){
        if(!fullscreenMode&&a==VK_F11){fullscreenRequested=true;return 0;}
    }
    // Plain arrows tune calibration in both the control and fullscreen windows.
    // Handle these before ImGui, which otherwise consumes navigation keys.
    if(!fullscreenMode&&m==WM_KEYDOWN && !(ImGui::GetCurrentContext()&&(ImGui::GetIO().WantTextInput||ImGui::IsAnyItemActive()||ImGui::IsAnyItemFocused())) && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)){
        int step=(GetKeyState(VK_SHIFT)&0x8000)?10:100;
        if(a==VK_UP)phaseDelta+=step;else if(a==VK_DOWN)phaseDelta-=step;
        else if(a==VK_RIGHT)durationDelta+=step;else if(a==VK_LEFT)durationDelta-=step;
        else return ImGui_ImplWin32_WndProcHandler(w,m,a,b)?1:DefWindowProcW(w,m,a,b);
        return 0;
    }
    if(ImGui_ImplWin32_WndProcHandler(w,m,a,b))return 1;
    if(m==WM_CLOSE){closeRequested=true;return 0;}
    if(m==WM_DISPLAYCHANGE){outputStop=true;displayRefreshChanged=true;return 0;}
    return DefWindowProcW(w,m,a,b);
}
static void label(const char* text){ImGui::TextColored(ImVec4(.463f,.725f,0,1),"%s",text);}
static void paragraph(const char* text){ImGui::PushTextWrapPos();ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool gpuTest=false,probe=false,smoke=false,emitterCheck=false,emitterPulse=false;int pulseEye=-1;double pulseShutter=1500,lcdSettle=4000;int liveSeconds=0;bool liveHdr=false,vblank=false,liveSwap=false;double livePhase=0,liveDuration=1500;int liveRefresh=120,liveSequence=0;std::filesystem::path emitterFw;int screenTestSeconds=0,testDisplay=-1;for(int i=1;i<argc;i++){if(std::wstring(argv[i])==L"--screen-test"){screenTestSeconds=8;if(i+1<argc&&std::wstring(argv[i+1]).rfind(L"--",0)!=0)screenTestSeconds=std::max(1,_wtoi(argv[++i]));}if(std::wstring(argv[i])==L"--display"&&i+1<argc)testDisplay=_wtoi(argv[++i]);gpuTest|=std::wstring(argv[i])==L"--gpu-test";probe|=std::wstring(argv[i])==L"--probe";smoke|=std::wstring(argv[i])==L"--smoke-test";if(std::wstring(argv[i])==L"--emitter-check"){emitterCheck=true;if(i+1<argc&&std::wstring(argv[i+1]).rfind(L"--",0)!=0)emitterFw=argv[++i];}emitterPulse|=std::wstring(argv[i])==L"--emitter-pulse";if(std::wstring(argv[i])==L"--pulse-eye"&&i+1<argc){std::wstring q=argv[++i];pulseEye=q==L"left"?0:q==L"right"?1:-1;emitterPulse=true;}if(std::wstring(argv[i])==L"--pulse-shutter"&&i+1<argc)pulseShutter=_wtof(argv[++i]);liveHdr|=std::wstring(argv[i])==L"--hdr";liveSwap|=std::wstring(argv[i])==L"--swap";if(std::wstring(argv[i])==L"--phase"&&i+1<argc)livePhase=_wtof(argv[++i]);if(std::wstring(argv[i])==L"--duration"&&i+1<argc)liveDuration=_wtof(argv[++i]);if(std::wstring(argv[i])==L"--settle"&&i+1<argc)lcdSettle=_wtof(argv[++i]);if(std::wstring(argv[i])==L"--refresh"&&i+1<argc)liveRefresh=_wtoi(argv[++i]);if(std::wstring(argv[i])==L"--sequence"&&i+1<argc){std::wstring q=argv[++i];liveSequence=q==L"lbrb"?1:q==L"llrr"?2:0;}vblank|=std::wstring(argv[i])==L"--vblank";if(std::wstring(argv[i])==L"--live"){liveSeconds=10;if(i+1<argc&&std::wstring(argv[i+1]).rfind(L"--",0)!=0)liveSeconds=std::max(1,_wtoi(argv[++i]));}}LocalFree(argv);
    try{
        std::filesystem::create_directories(workspace()/L"reports");
        wcscpy_s(crashDumpPath,(workspace()/L"reports/native-crash.dmp").c_str());SetUnhandledExceptionFilter(recordNativeCrash);
        // Offscreen render checks open no window and must run while the app is open.
        if(gpuTest){runGpuSelfTest(workspace()/L"reports");if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(vblank){
            // Vblank probe for every output (docs/TIMING-CALIBRATION.md): the graphics kernel's scan-line
            // counter against the presentation timestamps the presenter predicts from. Runs beside an open instance.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/vblank.txt");
            for(auto& d:enumerateDisplays()){auto r=measureVblank(d,3.0);f<<d.name<<" ("<<utf8(d.gdiName)<<") "<<d.width<<'x'<<d.height<<" @ "<<std::fixed<<std::setprecision(3)<<d.refresh<<" Hz, "<<d.activeLines<<" active of "<<d.totalLines<<" lines\n  "<<r.message<<"\n";
                if(r.ok)f<<std::setprecision(4)<<"  measuredHz="<<r.measuredHz<<std::setprecision(1)<<" vblankLengthUs="<<r.vblankLengthUs<<" syncAfterVblankStartUs="<<r.syncAfterVblankStartUs<<" scanStartAfterSyncUs="<<r.scanStartAfterSyncUs<<" dxgiJitterRmsUs="<<r.dxgiJitterRmsUs<<" dxgiJitterMaxUs="<<r.dxgiJitterMaxUs<<" scanlineJitterRmsUs="<<r.scanlineJitterRmsUs<<" wakeLatencyP50Us="<<r.wakeLatencyP50Us<<" wakeLatencyMaxUs="<<r.wakeLatencyMaxUs<<" samples="<<r.dxgiSamples<<'/'<<r.scanlineSamples<<"\n";}
            if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(screenTestSeconds>0){
            // Headless whole-screen conversion: captures a display, runs the depth helper and writes the
            // side-by-side result and the depth view as PNGs with a timing report. No overlay, no emitter.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/screen-test.txt");
            auto displays=enumerateDisplays();if(displays.empty())throw std::runtime_error("No display to capture.");
            auto& d=displays[testDisplay>=0&&testDisplay<int(displays.size())?testDisplay:0];
            // The same GPU scheduling class the presenter obtains, so the timings reflect a real session.
            if(D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH)!=0)D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL);
            StereoSource source;SourceConfig c;c.kind=SourceKind::Screen;c.monitor=d.monitor;c.depthHelper=executableDirectory()/L"VisionDepth.exe";c.depthModel=findDepthModel();c.depthLog=workspace()/L"reports"/L"depth-helper.log";c.screen.pairRate=60;
            f<<"Display: "<<d.name<<" "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<" Hz\nHelper: "<<utf8(c.depthHelper.wstring())<<"\nModel: "<<utf8(c.depthModel.wstring())<<"\n";
            source.start(c,d.adapterLuid);
            const double until=qpc()+screenTestSeconds;while(qpc()<until)Sleep(50);
            auto st=source.status();auto frame=source.latest();
            f<<std::fixed<<std::setprecision(1)<<"Frames: "<<st.frames<<" (dropped "<<st.dropped<<")\nDepth maps: "<<st.depthFrames<<", network "<<st.netWidth<<'x'<<st.netHeight<<", inference "<<st.depthMs<<" ms, age "<<st.depthAgeMs<<" ms\nConversion: "<<st.convertMs<<" ms per pair\nStatus: "<<st.message<<"\nHelper: "<<st.depthMessage<<"\n";
            if(frame){D3D11_TEXTURE2D_DESC format{};frame->texture->GetDesc(&format);f<<"Capture format: "<<int(format.Format)<<"; encoding "<<int(frame->encoding)<<"; SDR white "<<frame->sdrWhiteLevel*80<<" nits; packing "<<int(frame->packing)<<"; alignment applied "<<frame->alignmentApplied<<"\n";
                if(format.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||frame->encoding!=Encoding::LinearScRGB||frame->packing!=Packing::SideBySide||!frame->alignmentApplied)throw std::runtime_error("AI capture lost HDR precision or stereo metadata");
                saveSharedFramePng(*frame,d.adapterLuid,workspace()/L"reports/screen-sbs.png");f<<"Saved reports/screen-sbs.png ("<<frame->width<<'x'<<frame->height<<")\n";}
            c.screen.showDepth=true;source.configureScreen(c.screen);Sleep(1500);
            if(auto shown=source.latest()){saveSharedFramePng(*shown,d.adapterLuid,workspace()/L"reports/screen-depth.png");f<<"Saved reports/screen-depth.png\n";}
            source.stop();const bool ok=st.frames>0&&st.depthFrames>0;f<<(ok?"PASS":"FAIL")<<"\n";
            if(SUCCEEDED(co))CoUninitialize();return ok?0:1;}
        const std::wstring instanceName=smoke?L"Local\\VisionRestoration.Smoke."+std::to_wstring(GetCurrentProcessId()):L"Local\\VisionRestoration.SingleInstance.9F170E91";
        HANDLE singleInstance=CreateMutexW(nullptr,FALSE,instanceName.c_str());if(!singleInstance)throw std::runtime_error("Cannot create the application instance guard.");
        if(GetLastError()==ERROR_ALREADY_EXISTS){if(HWND existing=FindWindowW(L"VisionRestorationControl",nullptr)){PostMessageW(existing,trayMessage+1,0,0);SetForegroundWindow(existing);}CloseHandle(singleInstance);if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(probe){auto displays=enumerateDisplays();Emitter usb;auto devices=usb.discover();std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/hardware.txt");for(auto& d:displays){f<<d.name<<"\n"<<d.id<<"\nGPU: "<<d.gpu<<"\nMode: "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<"\nHDR supported/enabled: "<<d.hdrSupported<<'/'<<d.hdrEnabled<<"\n";}for(auto& d:devices)f<<d.description<<"\n";if(devices.empty())f<<"No supported NVIDIA or RP2040 USB emitter enumerated.\n";if(SUCCEEDED(co))CoUninitialize();return 0;}
        if(emitterCheck){
            // Headless USB check: connect to the single supported emitter. NVIDIA boot
            // devices may need RAM firmware; RP2040 devices already contain their firmware.
            // Eye commands require the separate, explicit --emitter-pulse option.
            std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/emitter-check.txt");
            Emitter usb;auto found=usb.discover();std::vector<UsbDeviceInfo> nv;for(auto& d:found)if(d.supported)nv.push_back(d);
            for(auto& d:found)f<<"Found: "<<d.description<<"\n";
            if(nv.size()!=1){f<<"Expected exactly one supported NVIDIA or RP2040 emitter, found "<<nv.size()<<".\n";if(SUCCEEDED(co))CoUninitialize();return 2;}
            usb.connect(nv[0],emitterFw,false);
            EmitterStatus st;for(int i=0;i<200;i++){st=usb.status();if(st.state!=EmitterState::Initializing)break;Sleep(50);}
            const char* name=st.state==EmitterState::Ready?"Ready":st.state==EmitterState::Error?"Error":st.state==EmitterState::Disconnected?"Disconnected":st.state==EmitterState::Initializing?"Initializing (timeout)":"Other";
            f<<"Identity: "<<st.identity<<"\nFirmware: "<<st.firmwareVersion<<"\nState: "<<name<<"\nMessage: "<<st.message<<"\nClock ready: "<<st.clockReady<<"\nClock uncertainty us: "<<st.clockUncertaintyUs<<"\n";
            bool ok=st.state==EmitterState::Ready||(st.state==EmitterState::Disconnected&&st.message.rfind("Firmware loaded",0)==0);
            if(ok&&emitterPulse&&st.state==EmitterState::Ready){
                if(st.scheduled&&pulseEye>=0)throw std::runtime_error("The RP2040 pulse check requires alternating eyes; omit --pulse-eye.");
                // Drive the emitter alone: alternate L/R at 120 Hz for 10 s with no display pipeline.
                // Watch the emitter LED and the glasses. This is a protocol test, not a stereo test.
                Settings s;s.refresh=120;s.leftUs=s.rightUs=std::clamp(pulseShutter,250.0,periodUs(120)-(st.scheduled?500:100));s.phaseUs=0;usb.configure(s);
                // --pulse-eye left|right drives one lens only (10 s) so each shutter can be judged by eye.
                double period=1.0/120,t=qpc()+0.01;Eye eye=pulseEye==1?Eye::Right:Eye::Left;f<<"Pulse eye: "<<(pulseEye<0?"alternating":pulseEye==1?"right only":"left only")<<" shutter "<<s.leftUs<<" us\n";
                for(int n=0;n<1200;n++){usb.submit(eye,t);if(pulseEye<0)eye=eye==Eye::Left?Eye::Right:Eye::Left;t+=period;while(qpc()<t-(st.scheduled?0.01:0.0005))Sleep(0);}
                // Let the last scheduled RP2040 close finish, then stop before
                // its 100 ms host-loss watchdog expires during this test's idle.
                Sleep(st.scheduled?20:100);st=usb.status();f<<"Pulse: commands="<<st.commands<<" errors="<<st.errors<<" late="<<st.late<<" lastTransferUs="<<st.lastTransferUs<<" maxTransferUs="<<st.maxTransferUs<<"\nAfter pulse: "<<st.message<<"\n";ok=st.commands>1000&&st.errors==0;}
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
            int di=0;
            if(testDisplay>=0){if(testDisplay>=int(displays.size()))throw std::runtime_error("Invalid test display index");di=testDisplay;}
            auto d=displays[di];f<<"Display: "<<d.name<<" "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<" HDR enabled "<<d.hdrEnabled<<"\n";
            DisplayModeGuard modeGuard;if(std::abs(d.refresh-liveRefresh)>0.5){bool applied=false;for(auto& m:d.modes)if(m.dmPelsWidth==d.width&&m.dmPelsHeight==d.height&&int(m.dmDisplayFrequency)==liveRefresh){modeGuard.apply(d,m);applied=true;break;}if(!applied)throw std::runtime_error("Requested refresh at the current resolution is not offered by this display.");displays=enumerateDisplays();d=displays[di];f<<"Switched to: "<<d.width<<'x'<<d.height<<" @ "<<d.refresh<<"\n";}
            StereoSource source;Emitter emitter;Presenter presenter(source,emitter);
            auto devices=emitter.discover();std::vector<UsbDeviceInfo> nv;for(auto& x:devices)if(x.supported)nv.push_back(x);
            if(nv.size()!=1)throw std::runtime_error("Expected exactly one supported emitter for the live test.");
            Settings settings;settings.displayId=d.id;settings.connection=d.connection;settings.width=int(d.width);settings.height=int(d.height);settings.refresh=d.refresh;settings.hdr=liveHdr&&d.hdrEnabled;settings.leftUs=settings.rightUs=liveDuration;settings.phaseUs=livePhase;settings.swapEyes=liveSwap;settings.sequence=Sequence(liveSequence);if(wcsstr(GetCommandLineW(),L"--lcd-calibrate")){settings.lcd.enabled=true;settings.sequence=Sequence::Alternating;settings.lcd.settleUs=lcdSettle;settings.lcd.durationUs=liveDuration;settings.lcd.phaseUs=livePhase;settings.signalScanUs=d.scanUs;}
            emitter.configure(settings);emitter.connect(nv[0],{},false);
            EmitterStatus es;for(int i=0;i<200;i++){es=emitter.status();if(es.state!=EmitterState::Initializing)break;Sleep(50);}
            if(es.state!=EmitterState::Ready)throw std::runtime_error("Emitter not ready: "+es.message);
            if(es.scheduled&&settings.lcd.enabled&&!es.aperture)throw std::runtime_error("LCD timing needs RP2040 firmware 0.4.0");
            if(es.scheduled&&!settings.lcd.enabled&&!rp2040TimingSupported(settings.refresh,settings.sequence,es.extendedCadence,es.fastCadence))throw std::runtime_error("Live test timing is unsupported by the connected RP2040 firmware");
            outputWindow=CreateWindowExW(WS_EX_TOPMOST,L"VisionRestorationOutput",L"Stereo test | Escape: stop | X: swap | Up/Down: phase | Left/Right: duration",WS_POPUP,d.rect.left,d.rect.top,d.rect.right-d.rect.left,d.rect.bottom-d.rect.top,nullptr,nullptr,instance,nullptr);
            ShowWindow(outputWindow,SW_SHOW);SetForegroundWindow(outputWindow);presenter.start(outputWindow,d,settings,false,settings.lcd.enabled?7:1);
            double end=qpc()+liveSeconds;outputStop=false;LcdTiming lastLiveLcd=settings.lcd;
            while(qpc()<end&&!outputStop){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                bool changed=false;if(swapRequested){settings.swapEyes=!settings.swapEyes;swapRequested=false;changed=true;}
                if(phaseDelta){if(settings.lcd.enabled)settings.lcd.settleUs=std::clamp(settings.lcd.settleUs+phaseDelta,0.,8000.);else settings.phaseUs=wrapPhase(settings.phaseUs+phaseDelta,phaseCycleUs(settings.refresh,settings.sequence));phaseDelta=0;changed=true;}
                if(offsetDelta){settings.rightOffsetUs=std::clamp(settings.rightOffsetUs+float(offsetDelta),-1500.f,1500.f);offsetDelta=0;changed=true;}
                if(durationDelta){if(settings.lcd.enabled)settings.lcd.durationUs=std::clamp(settings.lcd.durationUs+durationDelta,250.,8000.);else settings.leftUs=settings.rightUs=std::clamp(settings.leftUs+durationDelta,minimumShutterUs,nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence)));durationDelta=0;changed=true;}
                if(changed){if(settings.lcd.enabled)constrainLcdAdjustment(settings.lcd,lastLiveLcd,settings.refresh,settings.signalScanUs);lastLiveLcd=settings.lcd;emitter.configure(settings);presenter.configure(settings,settings.lcd.enabled?7:1);}
                Sleep(5);}
            auto rs=presenter.status();es=emitter.status();presenter.stop();emitter.disconnect();DestroyWindow(outputWindow);outputWindow=nullptr;
            {unsigned bins[5]{};for(double ms:rs.intervals){bins[ms<6?0:ms<11?1:ms<19?2:ms<27?3:4]++;}f<<"Intervals(ms) <6:"<<bins[0]<<" 6-11:"<<bins[1]<<" 11-19:"<<bins[2]<<" 19-27:"<<bins[3]<<" >27:"<<bins[4]<<" (samples "<<rs.intervals.size()<<")\nHDR surface: "<<settings.hdr<<"\n";}
            f<<"Presenter: running="<<rs.running<<" preview="<<rs.preview<<" locked="<<rs.locked<<" timingPassed="<<rs.timingPassed<<" presents="<<rs.presents<<" misses="<<rs.misses<<" resyncs="<<rs.resyncs<<" lastResyncSec="<<rs.lastResyncSec<<" composed="<<rs.composed<<" modeChanges="<<rs.modeChanges<<" leadMs[<0,0-2,2-4,4-6,6-8,>8]="<<rs.leadBins[0]<<"/"<<rs.leadBins[1]<<"/"<<rs.leadBins[2]<<"/"<<rs.leadBins[3]<<"/"<<rs.leadBins[4]<<"/"<<rs.leadBins[5]<<" elapsed="<<rs.elapsed<<" measuredHz="<<rs.measuredHz<<" maxIntervalMs="<<rs.maxIntervalMs<<"\nPresenter message: "<<rs.message<<"\n";
            f<<"Emitter: state="<<int(es.state)<<" commands="<<es.commands<<" errors="<<es.errors<<" late="<<es.late<<" lastTransferUs="<<es.lastTransferUs<<" maxTransferUs="<<es.maxTransferUs<<"\nEmitter message: "<<es.message<<"\nFinal settings: refresh="<<settings.refresh<<" sequence="<<int(settings.sequence)<<" swap="<<settings.swapEyes<<" phaseUs="<<settings.phaseUs<<" durationUs="<<settings.leftUs<<"\n";
            if(settings.lcd.enabled){const auto& t=settings.lcd;f<<"LCD settleUs="<<t.settleUs<<" durationUs="<<t.durationUs<<" phaseUs="<<t.phaseUs<<" guardUs="<<t.guardUs<<" devicePeriodUs="<<es.aperturePeriodUs<<" deviceDurationUs="<<es.apertureDurationUs<<" opens="<<es.deviceOpens<<" closes="<<es.deviceCloses<<"\n";}
            if(SUCCEEDED(co))CoUninitialize();return (rs.presents>0&&es.commands>0&&es.errors==0)?0:1;}
        Surface ui;ui.create(controlWindow,nullptr,false);IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
        float dpi=float(GetDpiForWindow(controlWindow))/96.f;io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",16*dpi);ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();style.WindowRounding=0;style.ChildRounding=5;style.FrameRounding=3;style.GrabRounding=2;style.FramePadding={8,4};style.ItemSpacing={8,6};style.WindowPadding={16,14};
        style.Colors[ImGuiCol_WindowBg]={.012f,.015f,.012f,1};style.Colors[ImGuiCol_ChildBg]={.026f,.032f,.024f,1};style.Colors[ImGuiCol_FrameBg]={.07f,.085f,.065f,1};style.Colors[ImGuiCol_FrameBgHovered]={.11f,.16f,.075f,1};style.Colors[ImGuiCol_FrameBgActive]={.15f,.22f,.07f,1};style.Colors[ImGuiCol_Button]={.16f,.24f,.055f,1};style.Colors[ImGuiCol_ButtonHovered]={.31f,.47f,.025f,1};style.Colors[ImGuiCol_ButtonActive]={.463f,.725f,0,1};style.Colors[ImGuiCol_CheckMark]={.55f,.88f,.08f,1};style.Colors[ImGuiCol_SliderGrab]={.463f,.725f,0,1};style.Colors[ImGuiCol_SliderGrabActive]={.62f,.94f,.08f,1};style.Colors[ImGuiCol_Header]={.19f,.30f,.055f,1};style.Colors[ImGuiCol_HeaderHovered]={.30f,.46f,.035f,1};style.Colors[ImGuiCol_Separator]={.20f,.28f,.16f,1};style.Colors[ImGuiCol_Text]={.90f,.93f,.88f,1};style.ScaleAllSizes(dpi);
        ImGui_ImplWin32_Init(controlWindow);ImGui_ImplDX11_Init(ui.device.Get(),ui.context.Get());
        auto displays=enumerateDisplays();std::vector<UsbDeviceInfo> devices;StereoSource source;Emitter emitter;Presenter presenter(source,emitter);DisplayModeGuard modeGuard;GameSync gameSync(emitter);if(!smoke)gameSync.start();
        const bool liveUiTest=wcsstr(GetCommandLineW(),L"--ui-live-test")!=nullptr;
        Settings settings;LcdCalibrationUi lcdUi;int requestedTool=-1;int displayIndex=std::max(0,testDisplay),modeIndex=0,deviceIndex=0,step=2,sourceKind=0,windowIndex=0;SourceConfig sourceConfig;std::vector<std::pair<HWND,std::string>> windows;
        int screenDisplayIndex=0;sourceConfig.depthHelper=executableDirectory()/L"VisionDepth.exe";sourceConfig.depthModel=findDepthModel();sourceConfig.depthLog=workspace()/L"reports"/L"depth-helper.log";
        ControlMenu controlMenu(controlWindow,smoke);
        KeyBindings keyBindings(controlWindow,smoke?std::filesystem::path{}:workspace()/L"profiles/shortcuts.ini",dispatchShortcut);shortcuts=&keyBindings;
        NOTIFYICONDATAW tray{sizeof(tray)};tray.hWnd=controlWindow;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;tray.uCallbackMessage=trayMessage;tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"Vision Restoration - show controls / enable shortcuts");if(!smoke)Shell_NotifyIconW(NIM_ADD,&tray);
        bool outputEmbedded=false;RECT previewRect{0,0,640,360},placedPreviewRect{};
        bool outputPassThrough=false,screenOutput=false;
        // Phase sweep: the phase advances slowly through the whole cycle while the viewer watches
        // through the glasses and marks where the image is cleanest or brightest.
        struct Sweep{bool active=false;double origin=0,start=0,lastStep=0,secondsPerCycle=30;std::vector<double> marks;} sweep;
        // Sequence comparison from the model (Advanced > Sequence), valid for the rate it was run at.
        std::vector<SequenceOption> compared;double comparedHz=0;
        std::array<PhaseWindow,6> opticalWindows{};
        std::array<unsigned,6> opticalMarks{};
        std::string opticalKey;
        std::filesystem::path firmware=executableDirectory()/L"emitter.fw";if(!std::filesystem::exists(firmware))firmware=workspace()/L"STUFF"/L"emitter.fw";if(!std::filesystem::exists(firmware))firmware.clear();char name[192]="My stereo setup",notes[2048]{};bool paused=false,autoReconnectEmitter=true;double nextEmitterScan=0;std::string notice="Select an eye pattern, then start stereo output. Check either lens independently.";std::string error;
        auto syncDisplay=[&](bool followDisplayRate=false){if(displays.empty())return;displayIndex=std::clamp(displayIndex,0,int(displays.size()-1));auto& d=displays[displayIndex];settings.displayId=d.id;settings.connection=d.connection;settings.width=int(d.width);settings.height=int(d.height);settings.signalScanUs=d.scanUs;if(followDisplayRate)settings.refresh=d.refresh;settings.hdr=settings.hdr&&d.hdrEnabled;modeIndex=0;for(size_t i=0;i<d.modes.size();i++)if(d.modes[i].dmPelsWidth==d.width && d.modes[i].dmPelsHeight==d.height && std::abs(double(d.modes[i].dmDisplayFrequency)-d.refresh)<.5){modeIndex=int(i);break;}settings.validated=false;};syncDisplay(true);devices=emitter.discover();
        {int real=-1,count=0;for(size_t i=0;i<devices.size();i++)if(devices[i].supported&&!devices[i].rp2040){real=int(i);count++;}
            if(!smoke&&count==1&&(devices[real].pid==0x0007||!firmware.empty())){deviceIndex=real;settings.hdr=displays.empty()?false:displays[displayIndex].hdrEnabled;emitter.configure(settings);emitter.connect(devices[real],firmware,false);notice="NVIDIA emitter found. Driver, firmware and runtime connection are being restored automatically.";}
            else for(auto& x:devices)if(x.pid==0x7003)notice="NVIDIA emitter is in boot state (0955:7003). Connect emitter to upload firmware and restart it automatically.";}
        if(!smoke&&emitter.status().state==EmitterState::Disconnected){
            int board=-1,count=0;for(size_t i=0;i<devices.size();++i)if(devices[i].supported&&devices[i].rp2040){board=int(i);++count;}
            if(count==1){deviceIndex=board;settings.refresh=120;settings.hdr=displays.empty()?false:displays[displayIndex].hdrEnabled;
                autoReconnectEmitter=false;emitter.configure(settings);emitter.connect(devices[board],{},false);
                notice="RP2040 emitter found. Checking USB and clock; select a 120 Hz display for the first Left / Right test.";}
        }
        auto pattern=[&]{if(sourceKind!=0)return 3;if(settings.lcd.enabled&&step==6)return settings.lcd.target?8+settings.lcd.target:7;return step<3?step:step==3?4:step+1;};
        auto restoreControls=[&]{fullscreenMode=false;keyBindings.activate(false);controlMenu.leaveFullscreen();};
        auto stopOutput=[&]{restoreControls();lcdUi.sweeping=false;opticalMarks.fill(0);opticalKey.clear();bool wasFullscreen=outputWindow&&!outputEmbedded&&!presenter.status().preview;sweep.active=false;presenter.showPhase(false);presenter.stop();gameSync.enable(true);paused=false;if(outputWindow){DestroyWindow(outputWindow);outputWindow=nullptr;}outputEmbedded=false;bool keepFocus=outputPassThrough;outputPassThrough=false;screenOutput=false;if(!smoke&&wasFullscreen&&!keepFocus){ShowWindow(controlWindow,SW_RESTORE);SetForegroundWindow(controlWindow);}};
        auto clampTiming=[&]{if(!displays.empty())settings.signalScanUs=displays[displayIndex].scanUs;double p=periodUs(settings.refresh);auto emitterState=emitter.status();settings.phaseUs=emitterState.scheduled&&emitterState.firmwareVersion.starts_with("VRP1/0.2.0/")?wrapSignedPhase(settings.phaseUs,p):wrapPhase(settings.phaseUs,phaseCycleUs(settings.refresh,settings.sequence));double maximum=emitterState.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));settings.leftUs=std::clamp(settings.leftUs,minimumShutterUs,maximum);settings.rightUs=std::clamp(settings.rightUs,minimumShutterUs,maximum);
            settings.imageGain=std::clamp(settings.imageGain,1.f,8.f);
            settings.bandHeight=std::clamp(settings.bandHeight,.1f,1.f);settings.bandCenter=std::clamp(settings.bandCenter,settings.bandHeight/2,1-settings.bandHeight/2);
            settings.panelResponseUs=std::clamp(settings.panelResponseUs,0.,8000.);settings.panelScanUs=std::clamp(settings.panelScanUs,0.,50000.);
            settings.strobeStartUs=std::clamp(settings.strobeStartUs,0.,50000.);settings.strobeLengthUs=std::clamp(settings.strobeLengthUs,50.,50000.);settings.scanStartUs=std::clamp(settings.scanStartUs,-5000.,5000.);
            settings.panelRiseUs=std::clamp(settings.panelRiseUs,0.,8000.);settings.cancelStrength=std::clamp(settings.cancelStrength,0.f,2.f);settings.blackFloor=std::clamp(settings.blackFloor,0.f,.3f);
            settings.guardLevel=std::clamp(settings.guardLevel,0.f,1.f);if(settings.guardLevel>0)settings.cancelCrosstalk=false;
            auto& sc=settings.screen;sc.separation=std::clamp(sc.separation,0.f,.1f);sc.convergence=std::clamp(sc.convergence,.01f,1.f);sc.popOut=std::clamp(sc.popOut,0.f,2.f);sc.smoothing=std::clamp(sc.smoothing,0.f,.95f);sc.quality=std::clamp(sc.quality,14u,1036u);sc.steps=std::clamp(sc.steps,4u,64u);sc.depthRate=std::clamp(sc.depthRate,5.,60.);
        };
        auto panelTiming=[&]{PanelTiming t;t.scanUs=settings.panelScanUs>0?settings.panelScanUs:displays.empty()?0:displays[displayIndex].scanUs;t.responseUs=settings.panelResponseUs;t.illumination=settings.illumination;t.strobeStartUs=settings.strobeStartUs;t.strobeLengthUs=settings.strobeLengthUs;t.riseUs=settings.panelRiseUs;return t;};
        auto usbLatency=[&]{auto e=emitter.status();return e.scheduled?0.0:e.meanTransferUs;};
        // Crosstalk cancellation profile: the model's other-eye leak per row at the current phase and
        // shutter, scaled by the strength. Unknown scan leaves the profile disabled.
        auto refreshLeakProfile=[&]{settings.leakProfile.fill(0.f);if(!settings.cancelCrosstalk)return;auto panel=panelTiming();
            if(panel.scanUs>0){try{auto profile=leakageProfile(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,modelPhaseFromEmitter(settings.phaseUs,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency()),std::max(settings.leftUs,settings.rightUs),8);
                for(size_t i=0;i<8;i++)settings.leakProfile[i]=float(std::clamp(profile[i]*settings.cancelStrength,0.,.95));}catch(...){settings.leakProfile.fill(0.f);}}
            }; // No invented fallback leak when the scan is unknown.
        LcdTiming lastAppliedLcd=settings.lcd;Sequence lastAppliedSequence=settings.sequence;double lastAppliedRefresh=settings.refresh;
        auto updateCaptureExclusion=[&]{
            // A child preview belongs to the controls' top-level window. Exclude
            // both top-level windows in every screen-source view, including F11.
            for(HWND window:{controlWindow,outputEmbedded?nullptr:outputWindow})if(window){
                const DWORD desired=(sourceKind==3||(window==controlWindow&&fullscreenMode))?WDA_EXCLUDEFROMCAPTURE:WDA_NONE;
                DWORD current=0;
                if((!GetWindowDisplayAffinity(window,&current)||current!=desired)&&!SetWindowDisplayAffinity(window,desired)&&sourceKind==3)
                    throw std::runtime_error("Windows could not exclude the stereo output from desktop capture.");
            }
        };
        auto outputSettings=[&]{auto active=settings;if(settings.lcd.enabled){active.lcd=lastAppliedLcd;active.lcd.enabled=true;active.lcd.target=settings.lcd.target;active.sequence=lastAppliedSequence;active.refresh=lastAppliedRefresh;}return active;};
        auto update=[&](bool persist=true){updateCaptureExclusion();clampTiming();if(settings.lcd.enabled){
            const auto live=presenter.status();const double hz=live.locked?std::max(settings.refresh,live.measuredHz):settings.refresh;
            bool valid=lcdExposure(settings.lcd,apertureWindowHz(hz,settings.sequence),settings.signalScanUs).valid;
            if(valid&&emitter.status().scheduled)try{lcdDeviceConfig(settings.lcd,periodUs(hz),settings.signalScanUs,1+sequenceBlack(settings.sequence),sequenceHold(settings.sequence)+sequenceBlack(settings.sequence));}catch(const rp2040::InvalidTiming& e){valid=false;lcdUi.message=e.what();}
            if(valid){lastAppliedLcd=settings.lcd;lastAppliedSequence=settings.sequence;lastAppliedRefresh=settings.refresh;}
            else notice="Pending timing edits. Glasses continue with the last valid aperture.";
        }settings.validated=false;refreshLeakProfile();auto active=outputSettings();validate(active);{auto presented=active;if(screenOutput)presented.convergence=0;presenter.configure(presented,pattern());}
            // The screen conversion draws at most one pair per pair the sequence shows; its image settings apply live.
            {ScreenSettings sc=settings.screen;sc.pairRate=settings.refresh/cycleLength(settings.sequence);sourceConfig.screen=sc;source.configureScreen(sc);}
            gameSync.configure(outputSettings(),displays.empty()?nullptr:displays[displayIndex].monitor);
            // Calibration is live state, not UI focus state. Persist every change so
            // Alt-Tab, minimization, a USB reconnect or an app restart cannot lose it.
            // A running sweep changes the phase ten times a second and saves when it stops.
            if(!smoke&&!liveUiTest&&persist)try{settings.name=name;settings.monitorNotes=notes;auto es=emitter.status();if(es.scheduled){settings.emitterId=es.identity;settings.emitterFirmware=es.firmwareVersion;}saveProfile(workspace()/L"profiles"/(settings.lcd.enabled?(es.scheduled?L"autosave-timing-rp2040.ini":L"autosave-timing-nvidia.ini"):(es.scheduled?L"autosave-rp2040.ini":L"autosave.ini")),outputSettings());}catch(...){}
        };
        auto stopSweep=[&](const char* why){if(!sweep.active)return;sweep.active=false;presenter.showPhase(false);update();notice=why;};
        auto startSweep=[&]{if(!presenter.status().running||presenter.status().preview)throw std::runtime_error("Start the 3D preview or fullscreen output before sweeping the phase.");sweep.active=true;sweep.origin=settings.phaseUs;sweep.start=sweep.lastStep=qpc();presenter.showPhase(true);notice="Phase sweep running: watch through the glasses; M marks the current phase, Enter keeps it, S stops.";};
        // Profiles: user-named .ini files under profiles/. Timing values (phase, shutter, swap, depth, sequence)
        // apply live; display identity is compared and reported but never blocks a load.
        std::vector<std::pair<std::string,std::filesystem::path>> profileList;int profileIndex=0;bool profilesScanned=false;
        auto scanProfiles=[&]{profileList.clear();std::error_code ec;auto dir=workspace()/L"profiles";std::filesystem::create_directories(dir,ec);
            for(auto& e:std::filesystem::directory_iterator(dir,ec))if(e.is_regular_file()&&e.path().extension()==L".ini"){std::string label=utf8(e.path().stem().wstring());try{auto l=loadProfile(e.path());if(!l.name.empty()&&l.name!=label)label=l.name+"  ["+label+"]";}catch(...){label+="  [unreadable]";}profileList.push_back({label,e.path()});}
            std::sort(profileList.begin(),profileList.end(),[](auto& a,auto& b){return a.first<b.first;});profileIndex=profileList.empty()?0:std::clamp(profileIndex,0,int(profileList.size())-1);profilesScanned=true;};
        auto profileMatches=[&](const Settings& l){return !displays.empty()&&l.displayId==displays[displayIndex].id&&l.width==settings.width&&l.height==settings.height&&std::abs(l.refresh-settings.refresh)<.02&&l.hdr==settings.hdr;};
        auto importLegacyTiming=[&]{
            if(settings.lcd.enabled)return;
            const auto phase=rp2040Phase(settings.phaseUs,settings.refresh,settings.sequence);
            settings.lcd=LcdTiming{};settings.lcd.enabled=true;settings.lcd.settleUs=0;
            settings.lcd.phaseUs=phase.delayUs;settings.lcd.durationUs=std::min(settings.leftUs,settings.rightUs);
            settings.lcd.rightAdjustUs=settings.rightOffsetUs;settings.swapEyes^=phase.flipEye;
        };
        auto applyProfile=[&](const Settings& l){bool same=profileMatches(l);settings.lcd=l.lcd;settings.name=l.name;settings.refresh=l.refresh;settings.swapEyes=l.swapEyes;settings.phaseUs=l.phaseUs;settings.leftUs=l.leftUs;settings.rightUs=l.rightUs;settings.depth=l.depth;settings.convergence=l.convergence;settings.rightOffsetUs=l.rightOffsetUs;settings.peakNits=l.peakNits;settings.monitorNotes=l.monitorNotes;settings.sequence=l.sequence;settings.assessment=l.assessment;
            settings.imageGain=l.imageGain;settings.blackFloor=l.blackFloor;
            settings.hdr=l.hdr&&!displays.empty()&&displays[displayIndex].hdrEnabled;
            settings.bandHeight=l.bandHeight;settings.bandCenter=l.bandCenter;settings.panelResponseUs=l.panelResponseUs;settings.panelScanUs=l.panelScanUs;settings.illumination=l.illumination;settings.strobeStartUs=l.strobeStartUs;settings.strobeLengthUs=l.strobeLengthUs;settings.scanStartUs=l.scanStartUs;
            settings.screen.separation=l.screen.separation;settings.screen.convergence=l.screen.convergence;settings.screen.popOut=l.screen.popOut;settings.screen.smoothing=l.screen.smoothing;settings.screen.quality=l.screen.quality;settings.screen.steps=l.screen.steps;settings.screen.model=l.screen.model;
            strncpy_s(name,settings.name.c_str(),_TRUNCATE);strncpy_s(notes,settings.monitorNotes.c_str(),_TRUNCATE);update();notice=same?"Profile applied: "+l.name:"Profile applied: "+l.name+" (saved for a different display or mode; verify the timing).";};
        auto profileFileFor=[&](std::string n){std::string clean;for(char c:n)clean+=(isalnum((unsigned char)c)||c==' '||c=='-'||c=='_'||c=='.')?c:'_';while(!clean.empty()&&clean.back()==' ')clean.pop_back();if(clean.empty())clean="profile";return workspace()/L"profiles"/(wide(clean)+L".ini");};
        auto resetTiming=[&]{Settings d;settings.swapEyes=d.swapEyes;settings.phaseUs=d.phaseUs;settings.leftUs=d.leftUs;settings.rightUs=d.rightUs;settings.rightOffsetUs=d.rightOffsetUs;settings.sequence=d.sequence;update();notice="Timing reset: phase 0, shutters 1500 us, eyes normal.";};
        // Keep the last IR preset if the monitor rate was changed outside the app.
        try{scanProfiles();std::filesystem::path best;std::filesystem::file_time_type bestTime{};Settings bestSettings;
            for(auto& [label,path]:profileList){try{auto l=loadProfile(path);const bool rp=emitter.status().scheduled;if(l.emitterId.starts_with("rp2040:")!=rp||(wcsstr(GetCommandLineW(),L"--lcd-calibrate")&&!l.lcd.enabled))continue;if(rp&&!rp2040TimingSupported(l.refresh,l.sequence,true,emitter.status().fastCadence))continue;if(l.displayId!=settings.displayId||l.width!=settings.width||l.height!=settings.height||l.hdr!=settings.hdr)continue;auto t=std::filesystem::last_write_time(path);if(best.empty()||t>bestTime){best=path;bestTime=t;bestSettings=l;}}catch(...){}}
            if(!best.empty()){applyProfile(bestSettings);notice="Loaded last saved profile for this display: "+bestSettings.name;}}catch(const std::exception& e){error=e.what();}
        if(!smoke){int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);std::wstring preset;for(int i=1;i+1<count;i++)if(std::wstring(args[i])==L"--ir-preset")preset=args[++i];LocalFree(args);if(!preset.empty()){applyTimingPreset(settings,unsigned(std::stoul(preset)));update();notice=preset.empty()?notice:"Applied requested "+utf8(preset)+" Hz IR preset.";}}
        auto startOutput=[&](bool sideBySide=false,bool embedded=false,const RECT* overlay=nullptr,bool screen=false,bool fullscreen=false){
            if(displays.empty())throw std::runtime_error("No output found.");
            auto active=outputSettings();validate(active);auto& d=displays[displayIndex];
            // The whole-screen conversion lies over the output display and passes input through.
            if(screen)overlay=&d.rect;
            if(!sideBySide){
                if(settings.refresh<100&&!emitter.status().extendedCadence)throw std::runtime_error("Stereo output requires at least 100 Hz with this emitter firmware.");
                if(!refreshRatesMatch(settings.refresh,d.refresh))throw std::runtime_error("Emitter timing and output refresh differ. Use Match output rate first.");
                auto e=emitter.status();
                if(e.scheduled&&settings.lcd.enabled&&!e.aperture)throw std::runtime_error("Flash RP2040 firmware 0.4.0 for LCD aperture timing.");
                if(e.scheduled&&!settings.lcd.enabled&&!rp2040TimingSupported(settings.refresh,settings.sequence,e.extendedCadence,e.fastCadence))throw std::runtime_error(e.extendedCadence?"RP2040 needs 30-120 eye openings/s. Select a compatible repeated/black sequence under Panel experiments.":"Update RP2040 firmware for LCD preload; this build supports only 120 Hz Left / Right.");
                if(e.state!=EmitterState::Ready&&e.state!=EmitterState::Running)throw std::runtime_error("Connect an emitter to start stereo output.");
            }
            stopOutput();
            if(sourceKind==3&&!smoke&&!source.status().running){
                sourceConfig.kind=SourceKind::Screen;sourceConfig.monitor=displays[std::clamp(screenDisplayIndex,0,int(displays.size())-1)].monitor;
                sourceConfig.screen=settings.screen;sourceConfig.screen.pairRate=settings.refresh/cycleLength(settings.sequence);
                source.start(sourceConfig,d.adapterLuid);
            }
            // The preview and its controls must share the selected presentation output.
            if(embedded&&!smoke&&MonitorFromWindow(controlWindow,MONITOR_DEFAULTTONEAREST)!=d.monitor){
                MONITORINFO target{sizeof(target)};RECT current{};
                if(!GetMonitorInfoW(d.monitor,&target)||!GetWindowRect(controlWindow,&current))throw std::runtime_error("Cannot position the live preview.");
                int w=std::min(current.right-current.left,target.rcWork.right-target.rcWork.left),h=std::min(current.bottom-current.top,target.rcWork.bottom-target.rcWork.top);
                SetWindowPos(controlWindow,nullptr,target.rcWork.left+(target.rcWork.right-target.rcWork.left-w)/2,target.rcWork.top+(target.rcWork.bottom-target.rcWork.top-h)/2,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
            }
            outputEmbedded=embedded;
            int width=sideBySide&&!fullscreen?std::min(960L,d.rect.right-d.rect.left):d.rect.right-d.rect.left;
            int height=sideBySide&&!fullscreen?std::min(600L,d.rect.bottom-d.rect.top):d.rect.bottom-d.rect.top;
            int x=d.rect.left+(d.rect.right-d.rect.left-width)/2,y=d.rect.top+(d.rect.bottom-d.rect.top-height)/2;
            if(embedded){x=previewRect.left;y=previewRect.top;width=previewRect.right-x;height=previewRect.bottom-y;placedPreviewRect=previewRect;}
            if(overlay){x=overlay->left;y=overlay->top;width=overlay->right-overlay->left;height=overlay->bottom-overlay->top;}
            // Screen output leaves the underlying application focused and passes mouse input through.
            // Keep the image opaque so Windows can use a hardware overlay / independent flip.
            // Alpha 254 forced DWM composition at 240 Hz and caused repeated timing mismatches
            // under GPU load (reports/overlay-opacity-ab.log). Composition changes no longer
            // force a resync. WS_EX_TRANSPARENT still passes mouse input to the underlying app.
            DWORD exStyle=(sideBySide&&!fullscreen)||embedded?0:WS_EX_TOPMOST|(overlay?WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW:0);
            outputWindow=CreateWindowExW(exStyle,L"VisionRestorationOutput",sideBySide?L"Inspect eye images | 2D side by side":L"Stereo output",embedded?WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS:sideBySide&&!fullscreen?WS_OVERLAPPEDWINDOW:WS_POPUP,x,y,width,height,embedded?controlWindow:nullptr,nullptr,instance,nullptr);
            if(!outputWindow)throw std::runtime_error("Could not create the output window.");
            outputPassThrough=overlay!=nullptr;if(overlay)SetLayeredWindowAttributes(outputWindow,0,255,LWA_ALPHA);
            try{updateCaptureExclusion();}catch(...){stopOutput();throw;}
            screenOutput=screen;
            // A single owner controls the emitter while either app output is open.
            gameSync.enable(false);
            // Screen conversion already establishes its screen plane; an extra shift would misalign the mouse.
            Settings presented=active;if(overlay)presented.convergence=0;
            try{presenter.start(outputWindow,d,presented,sideBySide,pattern());}catch(...){stopOutput();throw;}
            if(overlay)ShowWindow(outputWindow,SW_SHOWNOACTIVATE);else{ShowWindow(outputWindow,SW_SHOW);if(!embedded)SetForegroundWindow(outputWindow);}
            fullscreenMode=!embedded&&(fullscreen||screen||(!sideBySide&&!overlay));
            if(fullscreenMode){controlMenu.enterFullscreen(d.rect);keyBindings.activate(!smoke);}
            notice=screen?"The whole display is on the glasses in 3D; mouse and keys go through to the desktop. Ctrl+Alt+F8 stops. Ctrl+Alt+PageUp/PageDown: depth strength, Ctrl+Alt+Home/End: screen plane, Ctrl+Alt+Insert: 2D/3D.":embedded?"Live 3D preview: adjust phase, shutter and convergence while watching through the glasses.":sideBySide?"2D inspection of the separate eye images.":"Fullscreen stereo. Escape returns to the controls.";
        };
        auto startFullscreenOutput=[&]{const auto current=presenter.status();startOutput(smoke||(current.running&&current.preview),false,nullptr,false,true);};
        auto toggleFullscreenControls=[&]{controlMenu.toggle();};
        auto runAction=[&](auto&& action){try{action();error.clear();}catch(const std::exception& e){error=e.what();}};
        // Starting points change values, never the window or the calibration UI.
        auto applyStartingPreset=[&](int preset){
            settings.lcd=LcdTiming{};settings.lcd.enabled=preset==0;settings.sequence=preset==3?Sequence::BlackInsertion:Sequence::Alternating;
            if(!displays.empty())settings.refresh=displays[displayIndex].refresh;
            if(preset==0){settings.lcd.settleUs=std::min(4000.,periodUs(settings.refresh)*.55);settings.lcd.durationUs=750;settings.name="LCD / VA starting point";}
            else {settings.phaseUs=841.*120/settings.refresh;settings.leftUs=settings.rightUs=1265.*120/settings.refresh;settings.name=preset==3?"OLED BFI starting point":"OLED starting point";}
            settings.bandHeight=1;settings.bandCenter=.5f;settings.blackFloor=settings.guardLevel=0;settings.cancelCrosstalk=false;
            strncpy_s(name,settings.name.c_str(),_TRUNCATE);lcdUi.sweeping=false;update();
        };
        // Saved manual profiles retain their phase/shutter backend. LCD timing
        // is opt-in; converting every profile here made the old sliders inert.
        if(wcsstr(GetCommandLineW(),L"--lcd-calibrate"))importLegacyTiming();
        if(!lcdExposure(lastAppliedLcd,apertureWindowHz(settings.refresh,settings.sequence),settings.signalScanUs).valid){
            lastAppliedLcd=LcdTiming{};lastAppliedLcd.enabled=true;lastAppliedLcd.settleUs=std::min(4000.,periodUs(settings.refresh)*.55);
            fitLcdDuration(lastAppliedLcd,apertureWindowHz(settings.refresh,settings.sequence),settings.signalScanUs);lastAppliedSequence=settings.sequence;lastAppliedRefresh=settings.refresh;
        }
        // Preserve the old launch argument; it selects a default only when there is no saved aperture.
        if(wcsstr(GetCommandLineW(),L"--lcd-calibrate")&&settings.lcd.settleUs==0&&settings.phaseUs==0)runAction([&]{applyStartingPreset(0);});
        runAction([&]{update(false);});
        {int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
            for(int i=1;i+1<count;++i)if(std::wstring(args[i])==L"--profile"){
                auto file=std::filesystem::path(args[++i]);auto loaded=loadProfile(file);applyProfile(loaded);
                if(smoke&&(settings.lcd.enabled!=loaded.lcd.enabled||settings.leftUs!=loaded.leftUs||settings.rightUs!=loaded.rightUs||settings.imageGain!=loaded.imageGain||settings.blackFloor!=loaded.blackFloor||std::abs(wrapPhase(settings.phaseUs-loaded.phaseUs,phaseCycleUs(settings.refresh,settings.sequence)))>.01))
                    throw std::runtime_error("Profile changed timing mode, phase, shutter or brightness on load");
                break;
            }LocalFree(args);}
        if(smoke&&wcsstr(GetCommandLineW(),L"--source-smoke")){sourceKind=3;sourceConfig.kind=SourceKind::Screen;requestedTool=1;}
        std::string renderCheck="Render checks have not run in this session.";
        std::string vblankReport="No vblank measurement in this session. It times the display's blanking interval against the presentation timestamps and stores the scan start offset.";std::future<VblankMeasurement> vblankJob;
        // Deterministic UI coverage without changing a real display or profile.
        if(smoke&&wcsstr(GetCommandLineW(),L"--timing-mismatch")){settings.refresh=120;if(!displays.empty())displays[displayIndex].refresh=144;}
        gameSync.configure(outputSettings(),displays.empty()?nullptr:displays[displayIndex].monitor);
        if(!smoke){ShowWindow(controlWindow,SW_SHOWDEFAULT);UpdateWindow(controlWindow);}
        double smokeStart=qpc();bool smokeSnapshot=false,smokePreviewStarted=false,smokePreviewVerified=false;
        bool previewOnLaunch=!smoke&&wcsstr(GetCommandLineW(),L"--start-preview");
        bool screenOnLaunch=!smoke&&wcsstr(GetCommandLineW(),L"--start-screen");
        bool fullscreenOnLaunch=wcsstr(GetCommandLineW(),L"--start-fullscreen")||liveUiTest;
        double liveTestNext=0,liveTestStarted=qpc(),liveTestSteadyStart=0;unsigned liveTestPresets=0;uint64_t liveTestSteadyMisses=0;unsigned liveTestEdits=0;uint64_t liveTestStartCommands=0;HWND liveTestWindow=nullptr;
        bool fullscreenSmoke=smoke&&wcsstr(GetCommandLineW(),L"--fullscreen-smoke");
        while(!closeRequested){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}if(smoke && qpc()-smokeStart>3)break;
            if(outputStop){outputStop=false;stopOutput();}
            if(fullscreenRequested){fullscreenRequested=false;runAction([&]{if(fullscreenMode)startOutput(presenter.status().preview,true);else startFullscreenOutput();});}
            if(controlsToggleRequested){controlsToggleRequested=false;toggleFullscreenControls();}
            if(controlsShowRequested){controlsShowRequested=false;if(fullscreenMode)controlMenu.show();else{ShowWindow(controlWindow,SW_RESTORE);SetForegroundWindow(controlWindow);}}
            if(displayRefreshChanged){displayRefreshChanged=false;try{auto selectedId=settings.displayId;displays=enumerateDisplays();for(size_t i=0;i<displays.size();i++)if(displays[i].id==selectedId){displayIndex=int(i);break;}syncDisplay();update();}catch(const std::exception& e){error=e.what();}}
            if(pauseRequested){pauseRequested=false;paused=!paused;presenter.pause(paused);}
            if(resyncRequested){resyncRequested=false;presenter.resync();notice="Stereo synchronization re-locked.";}
            if(brightnessDelta||convergenceDelta||hdrToggleRequested){
                settings.imageGain=std::clamp(settings.imageGain+brightnessDelta*.1f,1.f,8.f);
                settings.convergence=std::clamp(settings.convergence+convergenceDelta*.001f,-.08f,.08f);
                if(hdrToggleRequested&&!displays.empty()&&displays[displayIndex].hdrEnabled)settings.hdr=!settings.hdr;
                brightnessDelta=convergenceDelta=0;hdrToggleRequested=false;runAction(update);
            }
            if(offsetDelta){settings.rightOffsetUs=std::clamp(settings.rightOffsetUs+float(offsetDelta),-1500.f,1500.f);offsetDelta=0;update();}
            if(screenDepthDelta||screenPlaneDelta||screenToggleRequested){
                if(screenDepthDelta)settings.screen.separation=std::clamp(settings.screen.separation+screenDepthDelta*.0025f,0.f,.1f);
                if(screenPlaneDelta)settings.screen.convergence=std::clamp(settings.screen.convergence+screenPlaneDelta*.05f,.01f,1.f);
                if(screenToggleRequested)settings.screen.depth=!settings.screen.depth;
                screenDepthDelta=screenPlaneDelta=0;screenToggleRequested=false;runAction(update);
                std::ostringstream t;t<<std::fixed<<std::setprecision(2)<<"Screen conversion: "<<(settings.screen.depth?"3D":"2D")<<", depth strength "<<settings.screen.separation*100<<" % of the width, screen plane "<<settings.screen.convergence<<".";notice=t.str();}
            if(bandDelta||bandPositionDelta){settings.bandHeight=std::clamp(settings.bandHeight+bandDelta/100.f,.1f,1.f);settings.bandCenter=std::clamp(settings.bandCenter+bandPositionDelta/100.f,settings.bandHeight/2,1-settings.bandHeight/2);bandDelta=bandPositionDelta=0;update();}
            if(phaseDelta||durationDelta||swapRequested){if(settings.lcd.enabled){settings.lcd.settleUs=std::clamp(settings.lcd.settleUs+phaseDelta,0.,8000.);settings.lcd.durationUs=std::clamp(settings.lcd.durationUs+durationDelta,250.,8000.);lcdUi.sweeping=false;}else{settings.phaseUs+=phaseDelta;settings.leftUs+=durationDelta;settings.rightUs+=durationDelta;}if(swapRequested)settings.swapEyes=!settings.swapEyes;phaseDelta=durationDelta=0;swapRequested=false;runAction(update);}
            if(lcdTargetRequested>=0){if(settings.lcd.enabled){settings.lcd.target=lcdTargetRequested;runAction([&]{update(false);});}lcdTargetRequested=-1;}
            if(sweepToggleRequested){sweepToggleRequested=false;if(settings.lcd.enabled){if(lcdUi.sweeping)lcdUi.sweeping=false;else if(presenter.status().running&&!presenter.status().preview&&sourceKind==0&&step==6)runAction([&]{if(startLcdSweep(lcdUi,settings,apertureWindowHz(presenter.status().locked?presenter.status().measuredHz:settings.refresh,settings.sequence),qpc()))update(false);});}else if(sweep.active)stopSweep("Phase sweep stopped; the phase stays where it is.");else runAction(startSweep);}
            if(markRequested){markRequested=false;if(sweep.active){sweep.marks.push_back(settings.phaseUs);std::ostringstream t;t<<"Marked phase "<<std::fixed<<std::setprecision(0)<<settings.phaseUs<<" us (mark "<<sweep.marks.size()<<").";notice=t.str();}}
            if(acceptRequested){acceptRequested=false;lcdUi.sweeping=false;stopSweep("Phase sweep stopped at the current phase.");}
            if(cancelToggleRequested){cancelToggleRequested=false;settings.cancelCrosstalk=!settings.cancelCrosstalk;runAction(update);notice=settings.cancelCrosstalk?"Legacy ghost subtraction on (unverified).":"Legacy ghost subtraction off.";}
            if(bfiToggleRequested){bfiToggleRequested=false;if((!emitter.status().scheduled||emitter.status().extendedCadence)){settings.sequence=settings.sequence==Sequence::BlackInsertion?Sequence::Alternating:Sequence::BlackInsertion;runAction(update);notice=settings.sequence==Sequence::BlackInsertion?"Software black frame insertion on: Left / Black / Right / Black.":"Software black frame insertion off: Left / Right.";}}
            if(settings.lcd.enabled){auto live=presenter.status();double hz=apertureWindowHz(live.locked?live.measuredHz:settings.refresh,settings.sequence);if(tickLcdCalibration(lcdUi,settings,hz,qpc(),live.running&&live.locked&&!live.preview&&!paused&&!outputEmbedded&&sameLcdAperture(settings.lcd,lastAppliedLcd)&&sourceKind==0&&step==6))runAction([&]{update(false);});}
            if(sweep.active){double now=qpc();if(now-sweep.lastStep>=0.1){sweep.lastStep=now;double cycle=phaseCycleUs(settings.refresh,settings.sequence);settings.phaseUs=wrapPhase(sweep.origin+(now-sweep.start)/sweep.secondsPerCycle*cycle,cycle);runAction([&]{update(false);});}}
            if(vblankJob.valid()&&vblankJob.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto measured=vblankJob.get();vblankReport=measured.message;if(measured.ok&&measured.scanlineSamples>=8){settings.scanStartUs=std::clamp(measured.scanStartAfterSyncUs,-5000.,5000.);runAction(update);notice="Vblank measured; the scan start offset is stored with the profile and used by Suggest phase.";}}
            // Preserve the running presentation and calibration across unplug/replug.
            // The NVIDIA firmware is volatile, so a replug appears as boot PID 7003;
            // reconnecting here reloads it and returns to runtime PID 0007 in place.
            if(!smoke&&autoReconnectEmitter&&qpc()>=nextEmitterScan){nextEmitterScan=qpc()+0.5;auto current=emitter.status();if(current.state!=EmitterState::Initializing&&current.state!=EmitterState::Simulated){
                auto scanned=emitter.discover();int runtime=-1,boot=-1;for(size_t i=0;i<scanned.size();++i)if(scanned[i].supported&&!scanned[i].rp2040){if(scanned[i].pid==0x0007)runtime=int(i);else if(scanned[i].pid==0x7003)boot=int(i);}
                int reconnect=-1;if(current.state==EmitterState::Error||current.state==EmitterState::Disconnected)reconnect=runtime>=0?runtime:boot;else if(boot>=0&&runtime<0)reconnect=boot;
                devices=std::move(scanned);if(reconnect>=0){deviceIndex=reconnect;emitter.configure(outputSettings());emitter.connect(devices[deviceIndex],firmware,false);notice="Emitter replug detected. Restoring WinUSB, firmware and shutter output automatically; the stereo session is still running.";}
            }}
            RECT client{};GetClientRect(controlWindow,&client);if(client.right>0&&client.bottom>0&&(unsigned(client.right)!=ui.width||unsigned(client.bottom)!=ui.height))ui.resize(client.right,client.bottom);
            if(IsIconic(controlWindow)){Sleep(20);continue;}
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize(io.DisplaySize);ImGui::Begin("Vision setup",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);

            auto rs=presenter.status();auto es=emitter.status();auto ss=source.status();
            {static double lastLog=0;static bool fresh=true;double now=qpc();if(!smoke&&now-lastLog>2){lastLog=now;try{std::filesystem::create_directories(workspace()/L"reports");std::ofstream log(workspace()/L"reports/session.log",fresh?std::ios::trunc:std::ios::app);fresh=false;
                auto sch=es.scheduled?NvidiaSchedule{0,0,settings.phaseUs}:nvidiaSchedule(settings.refresh,settings.phaseUs);
                log<<std::fixed<<std::setprecision(1)<<"t="<<now<<" refresh="<<settings.refresh<<" hdr="<<settings.hdr<<" phase="<<settings.phaseUs<<" boundary="<<sch.boundaryUs<<" x="<<sch.delayUs<<" shutter="<<settings.leftUs<<"/"<<settings.rightUs<<" swap="<<settings.swapEyes<<" seq="<<int(settings.sequence)
                   <<" | out="<<rs.running<<" preview="<<rs.preview<<" locked="<<rs.locked<<" hz="<<std::setprecision(3)<<rs.measuredHz<<std::setprecision(1)<<" presents="<<rs.presents<<" misses="<<rs.misses<<" resyncs="<<rs.resyncs<<" slipsPerSec="<<rs.slipsLastSecond<<" composed="<<rs.composed<<" modeChanges="<<rs.modeChanges<<" lead="<<rs.leadBins[0]<<"/"<<rs.leadBins[1]<<"/"<<rs.leadBins[2]<<"/"<<rs.leadBins[3]<<"/"<<rs.leadBins[4]<<"/"<<rs.leadBins[5]<<" maxIntervalMs="<<rs.maxIntervalMs<<" vblankJitterUs="<<rs.vblankJitterRmsUs<<"/"<<rs.vblankJitterMaxUs<<" presentJitterUs="<<rs.presentJitterUs<<" gpuPriority=\""<<rs.gpuPriority<<"\" illum="<<int(settings.illumination)<<" strobe="<<settings.strobeStartUs<<"+"<<settings.strobeLengthUs<<" scanStart="<<settings.scanStartUs<<" band="<<settings.bandHeight<<"@"<<settings.bandCenter<<" guard="<<settings.guardLevel<<" floor="<<settings.blackFloor<<" cancel="<<settings.cancelCrosstalk<<"x"<<settings.cancelStrength<<" leak="<<settings.leakProfile[0]<<"/"<<settings.leakProfile[3]<<"/"<<settings.leakProfile[7]<<" msg=\""<<rs.message<<"\""
                   <<" | emitter="<<int(es.state)<<" cmds="<<es.commands<<" late="<<es.late<<" errors="<<es.errors<<" lastUs="<<es.lastTransferUs<<" maxUs="<<es.maxTransferUs<<" sendErrUs="<<es.sendErrorLastUs<<"/"<<es.sendErrorRmsUs<<"/"<<es.sendErrorMaxUs<<" writes="<<es.timingWrites<<" msg=\""<<es.message<<"\"";
                if(settings.lcd.enabled){const auto& t=settings.lcd;log<<" | lcd settle="<<t.settleUs<<" duration="<<t.durationUs<<" phase="<<t.phaseUs<<" leftAdjust="<<t.leftAdjustUs<<" rightAdjust="<<t.rightAdjustUs<<" guard="<<t.guardUs<<" scanComp="<<t.compensateScanout<<" scan="<<t.scanoutUs<<" row="<<t.referencePosition<<" devicePeriod="<<es.aperturePeriodUs<<" deviceDuration="<<es.apertureDurationUs;}
                // Capture source: frame count, drops, packed frame size and age, so a capture that stalls,
                // resizes or crops when the game gains focus shows up next to the presenter's timing.
                {auto frame=source.latest();log<<" | capture: kind="<<sourceKind<<" frames="<<ss.frames<<" dropped="<<ss.dropped<<" size="<<(frame?frame->width:0)<<"x"<<(frame?frame->height:0)<<" encoding="<<(frame?int(frame->encoding):-1)<<" sdrWhite="<<(frame?frame->sdrWhiteLevel:0)<<" aligned="<<(frame&&frame->alignmentApplied)<<" ageMs="<<(ss.lastFrame>0?(now-ss.lastFrame)*1000:-1.)<<" msg=\""<<ss.message<<"\"";
                 if(sourceKind==3)log<<" depth: maps="<<ss.depthFrames<<" net="<<ss.netWidth<<"x"<<ss.netHeight<<" inferMs="<<ss.depthMs<<" ageMs="<<ss.depthAgeMs<<" convertMs="<<ss.convertMs<<" helper=\""<<ss.depthMessage<<"\"";}
                auto gs=gameSync.status();
                log<<" | hook: hosting="<<gs.hosting<<" hooked="<<gs.hooked<<" driving="<<gs.driving<<" game=\""<<gs.game<<"\" pid="<<gs.pid<<" frames="<<gs.frames<<" triggers="<<gs.triggers<<" misses="<<gs.misses<<" guesses="<<gs.guesses<<" hz="<<std::setprecision(3)<<gs.measuredHz<<std::setprecision(1)<<" msg=\""<<gs.message<<"\"\n";}catch(...){}}}
            if(es.identity!="unassigned" && (settings.emitterId!=es.identity || settings.emitterFirmware!=es.firmwareVersion)){settings.emitterId=es.identity;settings.emitterFirmware=es.firmwareVersion;settings.validated=false;runAction(update);}

            bool changed=false;
            auto selectPattern=[&](int selected){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=selected;lcdUi.sweeping=false;changed=true;};
            auto patternButton=[&](const char* text,int selected){
                const bool active=sourceKind==0&&step==selected;
                if(active)ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if(ImGui::Button(text))selectPattern(selected);
                if(active)ImGui::PopStyleColor();
            };
            auto testSelector=[&]{
                patternButton("Left / Right",0);ImGui::SameLine();patternButton("3D shapes",2);ImGui::SameLine();patternButton("Both eyes",1);
                patternButton("Left only",4);ImGui::SameLine();patternButton("Right only",5);ImGui::SameLine();patternButton("LCD rows",6);
                int selected=sourceKind==0?step:-1;ImGui::SetNextItemWidth(-1);
                if(ImGui::Combo("##all_tests",&selected,"Left / Right rectangle + circle\0Both eye targets\0Moving 3D shapes\0Brightness ramp\0Left image only\0Right image only\0LCD rows / dark transitions\0Refresh-code camera test\0"))selectPattern(selected);
            };
            auto startingPresets=[&]{
                int preset=-1;ImGui::SetNextItemWidth(-1);
                if(ImGui::BeginCombo("##starting_preset","Starting preset (optional)")){
                    const double hz=displays.empty()?settings.refresh:displays[displayIndex].refresh;
                    const char* names[]{"LCD / VA settling","OLED 120 Hz","OLED 240 Hz","OLED 240 Hz + BFI"};
                    for(int i=0;i<4;++i){ImGui::BeginDisabled(i>0&&!refreshRatesMatch(hz,i==1?120:240));if(ImGui::Selectable(names[i]))preset=i;ImGui::EndDisabled();}
                    ImGui::TextDisabled("Presets use the selected display's current refresh.");ImGui::EndCombo();
                }
                if(preset>=0)runAction([&]{applyStartingPreset(preset);});
                bool bfi=settings.sequence==Sequence::BlackInsertion;
                if(ImGui::Checkbox("Black frame insertion",&bfi)){settings.sequence=bfi?Sequence::BlackInsertion:Sequence::Alternating;changed=true;}
                ImGui::SameLine();ImGui::TextDisabled("%s",sequencePattern(settings.sequence).c_str());
            };
            bool emitterOnline=es.state==EmitterState::Ready||es.state==EmitterState::Running;
            bool rateMatches=!displays.empty()&&refreshRatesMatch(settings.refresh,displays[displayIndex].refresh);
            auto primaryTiming=[&]{
                const double period=periodUs(settings.refresh);
                const bool signedPhase=es.scheduled&&!es.extendedCadence;
                const double low=settings.lcd.enabled?-period:signedPhase?-period*.5:0;
                const double high=settings.lcd.enabled?period:signedPhase?period*.5:phaseCycleUs(settings.refresh,settings.sequence)-1;
                const double maximum=settings.lcd.enabled?8000:es.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));
                changed|=drawTimingControls(settings,low,high,maximum,lcdUi.fine);
                if(settings.lcd.enabled&&!sameLcdAperture(settings.lcd,lastAppliedLcd)){
                    auto e=lcdExposure(settings.lcd,apertureWindowHz(settings.refresh,settings.sequence),settings.signalScanUs);
                    if(!e.valid)ImGui::TextWrapped("%s",e.message.c_str());
                }
            };
            auto advancedTiming=[&]{
                if(ImGui::CollapsingHeader("Advanced LCD calibration")){
                    bool enabled=settings.lcd.enabled;
                    if(ImGui::Checkbox("Use guarded LCD timing",&enabled)){
                        if(enabled)importLegacyTiming();else settings.lcd.enabled=false;
                        lcdUi.sweeping=false;changed=true;
                    }
                    if(settings.lcd.enabled)changed|=drawLcdCalibration(lcdUi,settings,outputSettings(),rs.locked?rs.measuredHz:settings.refresh,rs.running&&!rs.preview&&!paused&&!outputEmbedded&&sourceKind==0&&step==6,dpi,workspace()/L"reports",qpc());
                }
            };
            if(fullscreenMode){
                if(controlMenu.visible()&&!smoke)SetWindowPos(controlWindow,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
                ImGui::Button("FULLSCREEN CONTROLS  |  drag to move",{-1,0});
                static POINT dragOrigin{};static RECT dragWindow{};
                if(ImGui::IsItemActivated()){GetCursorPos(&dragOrigin);GetWindowRect(controlWindow,&dragWindow);}
                if(ImGui::IsItemActive()&&ImGui::IsMouseDragging(0)){POINT cursor{};GetCursorPos(&cursor);SetWindowPos(controlWindow,nullptr,dragWindow.left+cursor.x-dragOrigin.x,dragWindow.top+cursor.y-dragOrigin.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
                if(ImGui::Button("Hide menu"))controlsToggleRequested=true;ImGui::SameLine();
                if(ImGui::Button("Windowed output"))fullscreenRequested=true;ImGui::SameLine();
                bool enabled=keyBindings.enabled();if(ImGui::Checkbox("Keyboard shortcuts",&enabled))keyBindings.enable(enabled);ImGui::SameLine();
                ImGui::TextDisabled("%s",enabled?KeyBindings::chord(keyBindings.bindings().front()).c_str():"Disabled - use tray icon to reopen");
            }
            ImGui::TextUnformatted("Vision Restoration");ImGui::SameLine();
            ImGui::SameLine();if(ImGui::Button("Capture window")){if(sourceKind!=2){source.stop();sourceKind=2;sourceConfig.kind=SourceKind::Window;changed=true;}requestedTool=1;}
            ImGui::SameLine();if(ImGui::Button("AI desktop")){if(sourceKind!=3){source.stop();sourceKind=3;sourceConfig.kind=SourceKind::Screen;changed=true;}requestedTool=1;}
            ImGui::SameLine();if(ImGui::Button("All inputs"))requestedTool=1;
            ImGui::SameLine();if(ImGui::Button("Profiles"))requestedTool=2;
            ImGui::SameLine();if(ImGui::Button("Shortcuts"))requestedTool=7;
            auto lcdWindow=lcdExposure(lastAppliedLcd,apertureWindowHz(rs.locked?rs.measuredHz:settings.refresh,lastAppliedSequence),settings.signalScanUs);
            const bool timingAllowed=settings.lcd.enabled?(lcdWindow.valid&&(!es.scheduled||es.aperture)):(!es.scheduled||rp2040TimingSupported(settings.refresh,settings.sequence,es.extendedCadence,es.fastCadence));
            const char* outputState=!rs.running?"STOPPED":rs.preview?"2D INSPECTION":paused?"PAUSED":rs.locked?"STEREO OUTPUT":"ACQUIRING TIMING";
            ImGui::TextColored(rs.running?ImVec4(.55f,.88f,.35f,1):ImVec4(.65f,.70f,.72f,1),"%s",outputState);
            ImGui::SameLine();ImGui::TextDisabled("| %s",emitterOnline?"Emitter connected":es.state==EmitterState::Initializing?"Emitter connecting":"Emitter disconnected");
            ImGui::BeginDisabled(!emitterOnline||displays.empty()||!rateMatches||!timingAllowed);
            {if(ImGui::Button("Start 3D preview",{170*dpi,32*dpi}))runAction([&]{startOutput(false,true);});ImGui::SameLine();}
            if(ImGui::Button("Fullscreen (F11)",{175*dpi,32*dpi}))runAction(startFullscreenOutput);
            ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(displays.empty());
            if(ImGui::Button("Inspect eye images",{170*dpi,32*dpi}))runAction([&]{startOutput(true);});
            ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(!rs.running);
            if(ImGui::Button(paused?"Resume":"Pause",{85*dpi,32*dpi})){paused=!paused;presenter.pause(paused);}
            ImGui::SameLine();if(ImGui::Button("Stop",{80*dpi,32*dpi}))stopOutput();ImGui::EndDisabled();
            ImGui::SameLine();ImGui::BeginDisabled(!rs.running||rs.preview);if(ImGui::Button("Re-lock sync"))resyncRequested=true;ImGui::EndDisabled();
            ImGui::TextDisabled(fullscreenMode?"Configure or disable fullscreen keys in Shortcuts.":settings.lcd.enabled?"Arrows: settle / duration   Shift: fine   S: sweep   Enter: keep   X: swap   Esc: stop":"Arrows: phase / duration   Shift: fine   X: swap   Esc: stop");
            ImGui::Separator();
            float workspaceHeight=std::max(120*dpi,ImGui::GetContentRegionAvail().y-44*dpi);
            ImGui::BeginTable("live_workspace",2,ImGuiTableFlags_SizingStretchProp);
            ImGui::TableSetupColumn("controls",ImGuiTableColumnFlags_WidthStretch,.48f);ImGui::TableSetupColumn("preview",ImGuiTableColumnFlags_WidthStretch,.52f);
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("workspace",{0,workspaceHeight},false);
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##selected_output",displays.empty()?"No output":displays[displayIndex].name.c_str())){
                for(size_t i=0;i<displays.size();++i)if(ImGui::Selectable(displays[i].name.c_str(),int(i)==displayIndex))runAction([&]{stopOutput();source.stop();displayIndex=int(i);syncDisplay(true);update();});
                ImGui::EndCombo();
            }
            if(!displays.empty())ImGui::TextDisabled("%u x %u  |  %.3f Hz  |  %s",displays[displayIndex].width,displays[displayIndex].height,displays[displayIndex].refresh,settings.hdr?"HDR":"SDR");
            ImGui::TextWrapped("Profile: %s",settings.name.c_str());
            if(!rateMatches&&ImGui::Button("Match display refresh"))runAction([&]{stopOutput();settings.refresh=displays[displayIndex].refresh;update();});
            primaryTiming();ImGui::Separator();
            changed|=drawImageControls(settings,!displays.empty()&&displays[displayIndex].hdrEnabled,sourceKind==3);
            if(ImGui::Button("Brightness pattern"))selectPattern(3);
            ImGui::Separator();
            startingPresets();
            advancedTiming();
            ImGui::EndChild();ImGui::TableSetColumnIndex(1);
            label(fullscreenMode?"ACTIVE OUTPUT":"LIVE 3D PREVIEW");
            float previewWidth=std::max(1.f,ImGui::GetContentRegionAvail().x);
            float previewHeight=fullscreenMode?32*dpi:std::min(previewWidth*9.f/16.f,workspaceHeight*(settings.lcd.enabled?.35f:.48f));
            ImGui::InvisibleButton("preview_surface",{previewHeight*16.f/9.f,previewHeight});
            auto previewMin=ImGui::GetItemRectMin(),previewMax=ImGui::GetItemRectMax(),origin=ImGui::GetMainViewport()->Pos;
            // ImGui coordinates are relative to the main viewport, not desktop screen coordinates.
            previewRect={LONG(previewMin.x-origin.x),LONG(previewMin.y-origin.y),LONG(previewMax.x-origin.x),LONG(previewMax.y-origin.y)};
            ImGui::GetWindowDrawList()->AddRectFilled(previewMin,previewMax,IM_COL32(0,0,0,255));
            if(outputEmbedded&&outputWindow){
                if(!EqualRect(&placedPreviewRect,&previewRect)){SetWindowPos(outputWindow,nullptr,previewRect.left,previewRect.top,previewRect.right-previewRect.left,previewRect.bottom-previewRect.top,SWP_NOZORDER|SWP_NOACTIVATE);placedPreviewRect=previewRect;}
            }else ImGui::GetWindowDrawList()->AddText({previewMin.x+16*dpi,previewMin.y+16*dpi},IM_COL32(190,205,185,255),fullscreenMode?"Output continues behind this menu.":"Press Start 3D preview to view through your glasses.");
            ImGui::BeginChild("preview_controls",{0,std::max(1.f,workspaceHeight-(previewMax.y-previewMin.y)-42*dpi)},false);
            if(ImGui::BeginTabBar("preview_tools",ImGuiTabBarFlags_FittingPolicyScroll)){
                if(ImGui::BeginTabItem("Shortcuts",nullptr,requestedTool==7?ImGuiTabItemFlags_SetSelected:0)){
                    bool enabled=keyBindings.enabled();if(ImGui::Checkbox("Enable fullscreen shortcuts",&enabled))keyBindings.enable(enabled);
                    paragraph("Shortcuts are active only during fullscreen output. Turn them off for gaming. The tray icon or Launch.cmd can always reopen this menu.");
                    paragraph("Enable Outside app for a shortcut to work while a game or the desktop has focus. Click a key to replace it.");
                    if(keyBindings.capturing()>=0){paragraph(keyBindings.message.c_str());if(ImGui::Button("Cancel binding"))keyBindings.cancelCapture();}
                    if(ImGui::Button("Reset shortcut defaults"))keyBindings.defaults();
                    if(ImGui::BeginTable("shortcut_bindings",4,ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
                        ImGui::TableSetupColumn("On",ImGuiTableColumnFlags_WidthFixed,30*dpi);ImGui::TableSetupColumn("Function");ImGui::TableSetupColumn("Shortcut");ImGui::TableSetupColumn("Outside app",ImGuiTableColumnFlags_WidthFixed,90*dpi);ImGui::TableHeadersRow();
                        for(size_t i=0;i<keyBindings.bindings().size();++i){const auto binding=keyBindings.bindings()[i];ImGui::PushID(int(i));ImGui::TableNextRow();
                            ImGui::TableNextColumn();bool on=binding.enabled;if(ImGui::Checkbox("##enabled",&on))keyBindings.edit(i,on,binding.global);
                            ImGui::TableNextColumn();ImGui::TextWrapped("%s",binding.label);
                            ImGui::TableNextColumn();if(ImGui::Button(keyBindings.capturing()==int(i)?"Press shortcut...":KeyBindings::chord(binding).c_str()))keyBindings.beginCapture(i);
                            if(binding.key){ImGui::SameLine();if(ImGui::SmallButton("Clear"))keyBindings.clear(i);}
                            if(keyBindings.active()&&keyBindings.enabled()&&keyBindings.capturing()<0&&binding.enabled&&binding.global&&binding.key&&!binding.registered)ImGui::TextWrapped("Unavailable: used by Windows or another app.");
                            ImGui::TableNextColumn();bool global=binding.global;if(ImGui::Checkbox("##global",&global))keyBindings.edit(i,binding.enabled,global);
                            ImGui::PopID();
                        }ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Tests",nullptr,requestedTool==0?ImGuiTabItemFlags_SetSelected:0)){
            // The familiar eye/shape checks are available in every timing mode.
            // Selecting LCD timing does not require using the measurement chart.
            testSelector();
            if(settings.lcd.enabled){
                if(sourceKind==0&&step==6){
                    ImGui::BeginDisabled(!sameLcdAperture(settings.lcd,lastAppliedLcd));
                    changed|=drawLcdObservations(lcdUi,settings,apertureWindowHz(rs.locked?rs.measuredHz:settings.refresh,settings.sequence),workspace()/L"reports");ImGui::EndDisabled();
                }else if(sourceKind==0&&step==0){
                    paragraph("Left lens: LEFT and a rectangle. Right lens: RIGHT and a circle.");
                }
                ImGui::Separator();
                if(ImGui::Button("Save profile"))runAction([&]{update();saveProfile(workspace()/L"profiles"/profileKey(settings),outputSettings());profilesScanned=false;notice="Saved a separate LCD profile.";});
                ImGui::Text("%.3f Hz | %.3f ms/frame | %.2f stereo pairs/s",rs.locked?rs.measuredHz:settings.refresh,1000/(rs.locked?rs.measuredHz:settings.refresh),(rs.locked?rs.measuredHz:settings.refresh)/2);
                if(es.scheduled&&!es.aperture)ImGui::TextWrapped("RP2040 firmware 0.4.0 is needed to start LCD timing.");
                if(ImGui::CollapsingHeader("Connection details")){
                    ImGui::TextWrapped("%s",es.message.c_str());ImGui::Text("Missed refreshes: %llu | USB errors: %llu",rs.misses,es.errors);
                    if(ImGui::Button("Reconnect emitter"))runAction([&]{stopOutput();devices=emitter.discover();for(size_t i=0;i<devices.size();++i)if(devices[i].supported){deviceIndex=int(i);break;}if(devices.empty())throw std::runtime_error("No emitter found");emitter.configure(outputSettings());emitter.connect(devices[deviceIndex],firmware,false);});
                    ImGui::SameLine();if(ImGui::Button("Save timing report"))runAction([&]{presenter.exportReport(workspace()/L"reports/stereo-diagnostics.txt",settings,source.status().message);notice="Saved reports/stereo-diagnostics.txt";});
                }
            }else{
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
             if(scanUs>0&&settings.guardLevel==0){auto panel=panelTiming();auto w=settledWindow(settings.refresh,settings.sequence,scanUs,settings.bandHeight,settings.bandCenter,settings.panelResponseUs);double window=w.closeUs-w.openUs;auto full=settledWindow(settings.refresh,settings.sequence,scanUs,1,.5,settings.panelResponseUs);
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
                    auto found=brightestShutter(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,.01,ceiling);double widest=found.shutterUs;
                    settings.leftUs=settings.rightUs=widest;settings.rightOffsetUs=0;
                    double model=found.modelPhaseUs;
                    settings.phaseUs=emitterPhaseFromModel(model,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency());changed=true;
                    auto after=estimateLeakage(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,model,widest);
                    std::ostringstream t;t<<std::fixed<<std::setprecision(0)<<"Shutter widened to "<<widest<<" us and the phase recentred: "<<after.brightness*widest/periodUs(settings.refresh)*100<<" % of a fully lit frame at "<<std::setprecision(1)<<std::min(after.mean,9.99)*100<<" % predicted leakage. Verify through the glasses.";notice=t.str();}
                ImGui::SameLine();if(ImGui::Button("Suggest phase")){double model=bestModelPhaseUs(settings.refresh,settings.sequence,panel,settings.bandHeight,settings.bandCenter,shutter);settings.phaseUs=emitterPhaseFromModel(model,settings.refresh,settings.sequence,settings.scanStartUs,usbLatency());changed=true;
                    std::ostringstream t;t<<std::fixed<<std::setprecision(0)<<"Phase set to the least-leakage window of the model (scan starts "<<settings.scanStartUs<<" us after the timestamp, USB latency "<<usbLatency()<<" us). Lens latency is not modelled: sweep from here.";notice=t.str();}
             }else paragraph(settings.guardLevel>0?"Neutral reset is not covered by the old leakage model. Use the measured ranges under Panel experiments.":"Panel scan time unknown; enter it under Advanced > Panel.");
             paragraph("A panel rewrites its rows top to bottom over most of each refresh, so the whole screen is never one eye for long. A shorter area covers less of that scan and leaves a longer window for the shutter. PageUp/PageDown and Home/End adjust it in fullscreen.");}
            const char* patternNames[]{"Eye identification","Both eye targets","Stereo scene","Brightness ramp","Left image only","Right image only","Nine-row optical targets","Refresh-code camera test"};
            ImGui::TextWrapped("Selected: %s",sourceKind==0?patternNames[step]:"External stereo source");
            if(sourceKind==0&&step!=2)paragraph("Eye-isolation targets stay fixed. Select Stereo scene to adjust alignment.");
            }
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Input",nullptr,requestedTool==1?ImGuiTabItemFlags_SetSelected:0)){
                    label("STEREO SOURCE");
                    if(ImGui::Combo("Input",&sourceKind,"Built-in patterns\0Stereo image\0Capture window\0Whole screen (AI depth)\0")){source.stop();sourceConfig.kind=SourceKind(sourceKind);changed=true;}
                    if(sourceKind!=3){int packing=int(sourceConfig.packing);if(ImGui::Combo("Packing",&packing,"Side by side\0Top / bottom\0"))sourceConfig.packing=Packing(packing);}
                    if(sourceKind==1&&ImGui::Button("Choose image..."))runAction([&]{auto file=chooseFile(controlWindow,L"Stereo images\0*.png;*.jpg;*.jpeg;*.bmp;*.jps\0\0");if(!file.empty())sourceConfig.file=file;});
                    if(sourceKind==2){
                        if(ImGui::Button("Refresh windows"))runAction([&]{windows=captureWindows(controlWindow);std::erase_if(windows,[&](auto& w){return w.first==outputWindow;});windowIndex=0;});
                        if(ImGui::BeginCombo("Source window",windows.empty()?"None":windows[windowIndex].second.c_str())){for(size_t i=0;i<windows.size();++i)if(ImGui::Selectable(windows[i].second.c_str(),int(i)==windowIndex))windowIndex=int(i);ImGui::EndCombo();}
                        if(!windows.empty())sourceConfig.window=windows[windowIndex].first;
                        ImGui::TextDisabled("Capture color: automatic HDR / SDR");
                    }
                    if(sourceKind==3){
                        label("WHOLE SCREEN IN 3D (AI DEPTH)");
                        paragraph("Captures a display, estimates every pixel's depth with a neural network (Depth Anything V2 Small through ONNX Runtime and DirectML, in its own low-priority process) and shows the result over the output display. Mouse and keyboard go through to the desktop, and Windows draws the cursor above it at screen depth. Not yet verified through the glasses.");
                        if(!displays.empty()){screenDisplayIndex=std::clamp(screenDisplayIndex,0,int(displays.size())-1);ImGui::SetNextItemWidth(280*dpi);
                            if(ImGui::BeginCombo("Convert display",displays[screenDisplayIndex].name.c_str())){for(size_t i=0;i<displays.size();++i)if(ImGui::Selectable(displays[i].name.c_str(),int(i)==screenDisplayIndex))screenDisplayIndex=int(i);ImGui::EndCombo();}
                            sourceConfig.monitor=displays[screenDisplayIndex].monitor;}
                        {float strength=settings.screen.separation*100;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##screensep",&strength,0,10,"Depth strength %.2f %% of the width (Ctrl+Alt+PageUp/PageDown)")){settings.screen.separation=strength/100;changed=true;}}
                        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderFloat("##screenconv",&settings.screen.convergence,.01f,1,"Screen plane %.2f: 1 keeps everything behind the screen (Ctrl+Alt+Home/End)");
                        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderFloat("##screenpop",&settings.screen.popOut,0,2,"Pop-out limit %.2f x depth strength");
                        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderFloat("##screensmooth",&settings.screen.smoothing,0,.95f,"Depth smoothing %.2f");
                        {int quality=settings.screen.quality<=518?0:settings.screen.quality<=700?1:2;ImGui::SetNextItemWidth(280*dpi);if(ImGui::Combo("Network input",&quality,"Fast (518 px)\0Balanced (700 px)\0Fine (924 px)\0")){settings.screen.quality=quality==0?518u:quality==1?700u:924u;changed=true;}
                         int steps=int(settings.screen.steps);ImGui::SetNextItemWidth(280*dpi);if(ImGui::SliderInt("Search steps per pixel",&steps,4,64)){settings.screen.steps=unsigned(steps);changed=true;}
                         float rate=float(settings.screen.depthRate);ImGui::SetNextItemWidth(280*dpi);if(ImGui::SliderFloat("Depth updates per second",&rate,5,60,"%.0f")){settings.screen.depthRate=rate;changed=true;}
                         ImGui::SameLine();ImGui::TextDisabled("(never more than half the GPU: %.0f ms per map now)",ss.depthMs);}
                        changed|=ImGui::Checkbox("3D depth (Ctrl+Alt+Insert)",&settings.screen.depth);ImGui::SameLine();changed|=ImGui::Checkbox("Show depth map",&settings.screen.showDepth);
                        {static std::vector<std::filesystem::path> models;static double modelsScanned=-10;if(qpc()-modelsScanned>5){modelsScanned=qpc();models=listDepthModels();if(sourceConfig.depthModel.empty())sourceConfig.depthModel=findDepthModel();}
                         std::error_code ec;const bool helperFound=std::filesystem::exists(sourceConfig.depthHelper,ec);
                         const std::string current=settings.screen.model.empty()?(sourceConfig.depthModel.empty()?std::string("none found"):utf8(sourceConfig.depthModel.filename().wstring())+" (default)"):utf8(std::filesystem::path(wide(settings.screen.model)).filename().wstring());
                         ImGui::SetNextItemWidth(360*dpi);
                         if(ImGui::BeginCombo("Model",current.c_str())){
                             if(ImGui::Selectable("Default: Depth Anything V2 Small",settings.screen.model.empty())){settings.screen.model.clear();changed=true;}
                             for(auto& m:models){const std::string path=utf8(m.wstring());if(ImGui::Selectable(utf8(m.filename().wstring()).c_str(),settings.screen.model==path)){settings.screen.model=path;changed=true;}}
                             ImGui::EndCombo();}
                         ImGui::SameLine();if(ImGui::Button("Choose model..."))runAction([&]{auto file=chooseFile(controlWindow,L"ONNX depth model\0*.onnx\0\0");if(!file.empty()){settings.screen.model=utf8(file.wstring());changed=true;}});
                         paragraph("Small is the fast one for games and the desktop. tools\\Get-DepthModel.ps1 -Size base or -Size large fetches the stronger Depth Anything V2 models for films: finer depth, slower, more of the GPU. A model change restarts the depth helper; the picture stays up meanwhile.");
                         if(!helperFound)ImGui::TextColored({1,.45f,.3f,1},"VisionDepth.exe is missing: run tools/Get-Dependencies.ps1, then build.ps1.");}
                        ImGui::BeginDisabled(!emitterOnline||displays.empty()||!rateMatches||!timingAllowed);
                        if(ImGui::Button("Start screen 3D",{200*dpi,32*dpi}))runAction([&]{source.stop();sourceConfig.kind=SourceKind::Screen;sourceConfig.monitor=displays[screenDisplayIndex].monitor;update();source.start(sourceConfig,displays[displayIndex].adapterLuid);startOutput(false,false,nullptr,true);update();});
                        ImGui::EndDisabled();
                        paragraph("Start screen 3D captures the chosen display and lays the click-through stereo output over the output display. Start source alone feeds the conversion to the 3D preview instead. Strength, screen plane, smoothing, steps and the depth map view apply live; the network input restarts the helper.");
                        if(!ss.depthMessage.empty())ImGui::TextWrapped("Depth network: %s",ss.depthMessage.c_str());
                    }
                    ImGui::BeginDisabled(displays.empty()||sourceKind==0);if(ImGui::Button("Start source"))runAction([&]{update();source.start(sourceConfig,displays[displayIndex].adapterLuid);changed=true;});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Use built-in patterns")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;changed=true;}paragraph(ss.message.c_str());
                    ImGui::Spacing();ImGui::Separator();label("EXISTING GAME FIX");
                    paragraph("Inspect the installed community fix without changing its files. Original-window Geo11 output through this app is not currently available.");
                    if(ImGui::Button("Inspect installed fix..."))runAction([&]{auto exe=chooseFile(controlWindow,L"Game executable\0*.exe\0\0");if(!exe.empty())notice=compatibility::inspect(exe).description;});
                    ImGui::SameLine();if(ImGui::Button("Remove old prototype adapter..."))runAction([&]{auto exe=chooseFile(controlWindow,L"Game executable\0*.exe\0\0");if(!exe.empty())notice=compatibility::disconnect(exe);});
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Profiles",nullptr,requestedTool==2?ImGuiTabItemFlags_SetSelected:0)){
                    if(!profilesScanned)runAction(scanProfiles);
                    ImGui::InputText("Name",name,sizeof(name));
                    if(ImGui::BeginCombo("Saved profiles",profileList.empty()?"No saved profiles":profileList[profileIndex].first.c_str())){for(size_t i=0;i<profileList.size();++i)if(ImGui::Selectable(profileList[i].first.c_str(),int(i)==profileIndex))profileIndex=int(i);ImGui::EndCombo();}
                    ImGui::BeginDisabled(profileList.empty());if(ImGui::Button("Load profile"))runAction([&]{applyProfile(loadProfile(profileList[profileIndex].second));});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Save profile"))runAction([&]{settings.name=name;settings.monitorNotes=notes;saveProfile(profileFileFor(name),outputSettings());scanProfiles();notice=std::string("Saved profile: ")+name;});
                    ImGui::SameLine();if(ImGui::Button("Refresh list"))runAction(scanProfiles);
                    paragraph("Timing and scene changes autosave. Named profiles let you keep separate setups.");
                    ImGui::InputTextMultiline("Notes",notes,sizeof(notes),{0,120*dpi});
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Panel",nullptr,requestedTool==3||(smoke&&wcsstr(GetCommandLineW(),L"--panel-smoke"))?ImGuiTabItemFlags_SetSelected:0)){
                    label("LCD / QLED: PANEL DRIVE");
                    paragraph("For LCD/QLED, begin with Preload: give each eye an extra refresh to settle, then sweep phase while checking top, middle and bottom through each lens.");
                    ImGui::BeginDisabled(es.scheduled&&!es.extendedCadence);
                    const char* driveNames[]{"Direct L R","Black reset L B R B","Neutral reset L G R G","Preload L L R R"};
                    for(int i=0;i<4;i++){if(ImGui::Button(driveNames[i]))runAction([&]{applyPanelDrive(settings,PanelDrive(i));compared.clear();changed=true;notice="Panel experiment selected. Shutter starts at 1500 us (or emitter limit); phase is retained. Measure both lenses before judging separation.";});}
                    ImGui::EndDisabled();
                    ImGui::Text("Display %.3f Hz | %.2f new frames/eye/s | emitter %.3f Hz",settings.refresh,settings.refresh/cycleLength(settings.sequence),sequenceEmitterHz(settings.refresh,settings.sequence));
                    paragraph("Preload sends the same eye twice, holding the captured stereo pair constant. The emitter trigger is on the second refresh, leaving the first for settling. At 240 Hz this gives 60 new frames/eye/s; at 120 Hz it gives 30. Longer runs are under Advanced > Sequence.");
                    ImGui::BeginDisabled((es.scheduled&&!es.extendedCadence)||sequenceBlack(settings.sequence)==0);
                    changed|=ImGui::SliderFloat("Reset gray (sRGB code)",&settings.guardLevel,0,1,"%.3f");
                    ImGui::EndDisabled();
                    paragraph("Neutral reset replaces the black refresh with uniform gray. This tests whether a different LCD transition clears the previous eye sooner. It may add gray haze or worsen ghosts. It is unproven, and the old leakage model does not describe it. Eye-image blacks stay unchanged. Zero restores black reset.");
                    if(settings.guardLevel>0)ImGui::TextDisabled("Reset %.3f applies only between eyes. Pause and acquisition stay black. App output only; game hook does not implement neutral reset.",settings.guardLevel);
                    if(ImGui::Button("Nine-row optical targets")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=6;changed=true;}
                    ImGui::SameLine();if(ImGui::Button("Refresh-code camera test")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=7;changed=true;}
                    paragraph("Targets: in the left lens, columns 1/3/5 should be brighter than 2/4/6; the right lens is the reverse. Check all nine rows, including screen edges. Three pairs test white/black, gray/black and light/dark gray. A dark row with no visible target is not a pass.");
                    paragraph("Camera test: eight binary bits (most significant on the left) encode the submitted refresh index modulo 256, repeated in nine rows. Red/green/blue identify L/R/reset slots. The moving marker advances through 16 positions. Film without glasses to inspect panel cadence. A 240 fps camera alone cannot reliably prove every 240 Hz refresh; aliasing and exposure matter.");
                    ImGui::Separator();label("MEASURE A COMMON PHASE RANGE");
                    auto key=opticalCalibrationKey(settings);
                    if(!opticalKey.empty()&&opticalKey!=key){opticalMarks.fill(0);opticalKey.clear();}
                    paragraph("Keep shutter width fixed. Stop the sweep and let the image settle before marking. For each lens and screen third, mark the start and end of one contiguous acceptable phase range while increasing phase. End below start means it crosses zero. Both desired-image visibility and ghosting matter. The intersection uses your observations, not guessed panel timings.");
                    const char* rows[]{"Left: top third","Left: middle third","Left: bottom third","Right: top third","Right: middle third","Right: bottom third"};
                    bool canMark=(!es.scheduled||es.extendedCadence)&&rs.running&&!rs.preview&&!outputEmbedded&&!rs.occluded&&settings.leftUs==settings.rightUs&&settings.rightOffsetUs==0&&!paused&&!sweep.active&&sourceKind==0&&step==6&&settings.bandHeight==1&&settings.blackFloor==0&&!settings.cancelCrosstalk;
                    if(!canMark)ImGui::TextDisabled("Marking needs fullscreen stereo (not embedded preview), equal shutters, zero eye offset, nine-row targets, full area, no floor/subtraction, and a stopped sweep.");
                    for(size_t i=0;i<opticalWindows.size();i++){
                        ImGui::PushID(int(i));ImGui::TextUnformatted(rows[i]);ImGui::SameLine();ImGui::BeginDisabled(!canMark);
                        auto mark=[&](bool end){
                            double hz=sequenceEmitterHz(settings.refresh,settings.sequence);
                            if(!es.scheduled&&(std::abs(nvidiaEffectiveShutterUs(hz,settings.phaseUs,settings.leftUs)-settings.leftUs)>1||std::abs(nvidiaEffectiveShutterUs(hz,settings.phaseUs,settings.rightUs)-settings.rightUs)>1))
                                throw std::runtime_error("Emitter clamps the shutter at this phase. Use a shorter fixed shutter and remeasure all regions.");
                            opticalKey=key;if(end){opticalWindows[i].endUs=settings.phaseUs;opticalMarks[i]|=2;}else{opticalWindows[i].beginUs=settings.phaseUs;opticalMarks[i]|=1;}
                        };
                        if(ImGui::Button("Mark start"))runAction([&]{mark(false);});ImGui::SameLine();if(ImGui::Button("Mark end"))runAction([&]{mark(true);});
                        ImGui::EndDisabled();ImGui::SameLine();ImGui::Text("%s %.0f -> %s %.0f us",opticalMarks[i]&1?"":"unset",opticalWindows[i].beginUs,opticalMarks[i]&2?"":"unset",opticalWindows[i].endUs);ImGui::PopID();
                    }
                    bool complete=std::all_of(opticalMarks.begin(),opticalMarks.end(),[](unsigned m){return m==3;});
                    ImGui::BeginDisabled(!complete);
                    if(ImGui::Button("Use common measured phase"))runAction([&]{
                        double cycle=phaseCycleUs(settings.refresh,settings.sequence);auto common=intersectPhaseWindows(cycle,opticalWindows);
                        if(common.empty())throw std::runtime_error("No common measured phase for both lenses and all screen thirds. Change the panel drive or display mode, then remeasure; moving phase alone cannot satisfy these observations.");
                        auto widest=*std::max_element(common.begin(),common.end(),[&](auto a,auto b){return wrapPhase(a.endUs-a.beginUs,cycle)<wrapPhase(b.endUs-b.beginUs,cycle);});
                        double selected=wrapPhase(widest.beginUs+wrapPhase(widest.endUs-widest.beginUs,cycle)/2,cycle);
                        double hz=sequenceEmitterHz(settings.refresh,settings.sequence);
                        if(std::abs(nvidiaEffectiveShutterUs(hz,selected,settings.leftUs)-settings.leftUs)>1||std::abs(nvidiaEffectiveShutterUs(hz,selected,settings.rightUs)-settings.rightUs)>1)
                            throw std::runtime_error("The common range requires a clamped shutter. Shorten the fixed shutter and remeasure.");
                        settings.phaseUs=selected;changed=true;notice="Common measured phase selected; shutter width unchanged. Check all nine rows again, then the captured game. This is a visual calibration, not an optical instrument measurement.";
                    });
                    ImGui::SameLine();if(ImGui::Button("Export optical observations"))runAction([&]{
                        auto path=workspace()/L"reports/panel-optical-observations.txt";std::filesystem::create_directories(path.parent_path());std::ofstream f(path);
                        f<<std::setprecision(17)<<"User-observed phase ranges; not instrument measurements or automatic certification.\nSetup: "<<key<<"\nCurrent phase: "<<settings.phaseUs<<"\nSource: nine-row optical targets\nNotes: "<<notes<<"\n";
                        for(size_t i=0;i<6;i++)f<<rows[i]<<": "<<opticalWindows[i].beginUs<<" -> "<<opticalWindows[i].endUs<<" us\n";
                        auto common=intersectPhaseWindows(phaseCycleUs(settings.refresh,settings.sequence),opticalWindows);
                        f<<"Common ranges: "<<common.size()<<"\n";for(auto w:common)f<<w.beginUs<<" -> "<<w.endUs<<" us\n";
                        f<<"Host slips: "<<rs.misses<<"; USB late/errors: "<<es.late<<"/"<<es.errors<<"\n";f.close();if(!f)throw std::runtime_error("Could not save optical observations.");notice="Saved reports/panel-optical-observations.txt";
                    });ImGui::EndDisabled();
                    if(ImGui::Button("Clear optical marks")){opticalMarks.fill(0);opticalKey.clear();}
                    paragraph("Marks clear when the app's display/timing/drive settings change. After changing monitor overdrive, local dimming, motion processing, cable/input, or glasses position, clear them manually. Each marked range must stay acceptable throughout, not only at its endpoints.");
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Diagnostics",nullptr,requestedTool==4?ImGuiTabItemFlags_SetSelected:0)){
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
                if(ImGui::BeginTabItem("Settings",nullptr,requestedTool==5?ImGuiTabItemFlags_SetSelected:0)){
                    label("EMITTER CONNECTION");
                    if(ImGui::Button("Refresh USB"))runAction([&]{devices=emitter.discover();deviceIndex=0;});
                    deviceIndex=devices.empty()?0:std::clamp(deviceIndex,0,int(devices.size())-1);
                    if(ImGui::BeginCombo("Emitter",devices.empty()?"None":devices[deviceIndex].description.c_str())){for(size_t i=0;i<devices.size();++i)if(ImGui::Selectable(devices[i].description.c_str(),int(i)==deviceIndex))deviceIndex=int(i);ImGui::EndCombo();}
                    bool canConnect=!devices.empty()&&devices[deviceIndex].supported;ImGui::BeginDisabled(!canConnect);
                    if(ImGui::Button("Reconnect emitter"))runAction([&]{stopOutput();emitter.configure(outputSettings());emitter.connect(devices[deviceIndex],firmware,false);autoReconnectEmitter=!devices[deviceIndex].rp2040;});ImGui::EndDisabled();ImGui::SameLine();
                    if(ImGui::Button("Disconnect emitter")){autoReconnectEmitter=false;stopOutput();emitter.disconnect();}
                    if(!devices.empty()&&devices[deviceIndex].descriptorFailed&&ImGui::Button("Recover failed USB port"))runAction([&]{emitter.recoverPort(devices[deviceIndex]);nextEmitterScan=0;});
                    ImGui::Spacing();label("PER-EYE TIMING");
                    // Both backends support independent durations in LR mode;
                    // the separate right-eye phase offset is NVIDIA-only.
                    bool perEyeApplies=!settings.lcd.enabled&&es.state!=EmitterState::Simulated&&(es.scheduled||settings.sequence==Sequence::Alternating);
                    ImGui::BeginDisabled(!perEyeApplies);
                    changed|=ImGui::InputDouble("Left shutter us",&settings.leftUs,100,10,"%.0f");changed|=ImGui::InputDouble("Right shutter us",&settings.rightUs,100,10,"%.0f");
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(!perEyeApplies||es.scheduled);
                    changed|=ImGui::SliderFloat("Right eye offset us",&settings.rightOffsetUs,-2000,2000,"%.0f");
                    ImGui::EndDisabled();
                    if(es.scheduled)ImGui::TextDisabled("Right-eye offset is not implemented for RP2040.");
                    if(!perEyeApplies)ImGui::TextDisabled("%s",settings.sequence!=Sequence::Alternating?"Per-eye timing applies to the Left / Right sequence only.":"Per-eye timing needs a connected emitter.");
                    ImGui::Spacing();label("SEQUENCE");
                    ImGui::BeginDisabled(es.scheduled&&!es.extendedCadence);
                    {int hold=int(sequenceHold(settings.sequence)),black=int(sequenceBlack(settings.sequence));const int maxRun=es.scheduled?std::clamp(int(std::floor(settings.refresh*rp2040::Scheduler::maxPeriodUs/1e6)),1,4):4;
                     ImGui::SetNextItemWidth(160*dpi);if(ImGui::InputInt("Refreshes per eye",&hold)){settings.sequence=makeSequence(unsigned(std::clamp(hold,1,maxRun)),unsigned(std::min(black,maxRun-std::clamp(hold,1,maxRun))));changed=true;}
                     ImGui::SetNextItemWidth(160*dpi);if(ImGui::InputInt("Black refreshes after each eye",&black)){settings.sequence=makeSequence(unsigned(hold),unsigned(std::clamp(black,0,std::max(0,maxRun-hold))));changed=true;}
                     struct Quick{unsigned hold,black;const char* name;};for(const Quick& q:{Quick{1,0,"L R"},Quick{1,1,"L B R B"},Quick{2,0,"L L R R"},Quick{2,1,"L L B R R B"},Quick{3,0,"L L L R R R"}}){ImGui::BeginDisabled(es.scheduled&&!rp2040TimingSupported(settings.refresh,makeSequence(q.hold,q.black),es.extendedCadence,es.fastCadence));if(ImGui::Button(q.name)){settings.sequence=makeSequence(q.hold,q.black);changed=true;}ImGui::EndDisabled();ImGui::SameLine();}
                     ImGui::NewLine();
                     ImGui::Text("Sequence %s: %.0f new frames per eye per second at %.0f Hz; emitter period %.1f ms.",sequencePattern(settings.sequence).c_str(),settings.refresh/cycleLength(settings.sequence),settings.refresh,periodUs(sequenceEmitterHz(settings.refresh,settings.sequence))/1000);
                     paragraph("A black refresh gives the panel time to go dark before the other eye is written. Compare sequences runs the model for this panel at this rate; Use applies a row with its shutter and phase.");
                     ImGui::BeginDisabled(settings.guardLevel>0);if(ImGui::Button("Compare sequences for this panel")){auto panel=panelTiming();if(panel.scanUs<=0)notice="Panel scan time unknown; enter it under Panel.";else{const bool scheduled=es.scheduled;compared=compareSequences(settings.refresh,panel,settings.bandHeight,settings.bandCenter,.01,[scheduled](double hz){return scheduled?(hz<29.97||hz>120.48?0.:calibrationMaxShutterUs(hz)):nvidiaMaxShutterUs(hz);});comparedHz=settings.refresh;}}
                     if(!compared.empty()&&comparedHz==settings.refresh&&ImGui::BeginTable("sequences",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)){
                        ImGui::TableSetupColumn("Sequence");ImGui::TableSetupColumn("Frames per eye");ImGui::TableSetupColumn("Shutter");ImGui::TableSetupColumn("Light");ImGui::TableSetupColumn("Leak");ImGui::TableSetupColumn("Apply");ImGui::TableHeadersRow();
                        for(size_t i=0;i<compared.size();i++){const auto& o=compared[i];const bool clean=o.estimate.mean<=.01;ImGui::TableNextRow();
                            ImGui::TableNextColumn();ImGui::TextUnformatted(sequencePattern(o.sequence).c_str());ImGui::TableNextColumn();ImGui::Text("%.0f",o.framesPerEye);
                            ImGui::TableNextColumn();ImGui::Text(clean?"%.0f us":"none clean (%.0f us)",o.shutterUs);ImGui::TableNextColumn();ImGui::Text("%.0f %%",o.estimate.brightness*o.shutterUs/periodUs(settings.refresh)*100);
                            ImGui::TableNextColumn();ImGui::TextColored(clean?ImVec4(.55f,.88f,.35f,1):ImVec4(1,.45f,.3f,1),"%.1f %%",std::min(o.estimate.mean,9.99)*100);
                            ImGui::TableNextColumn();ImGui::PushID(int(i));if(ImGui::Button("Use")){settings.sequence=o.sequence;settings.leftUs=settings.rightUs=o.shutterUs;settings.rightOffsetUs=0;settings.phaseUs=emitterPhaseFromModel(o.modelPhaseUs,settings.refresh,o.sequence,settings.scanStartUs,usbLatency());changed=true;notice="Sequence "+sequencePattern(o.sequence)+" applied with the model's shutter and phase; sweep the phase by eye from here (Up/Down).";}ImGui::PopID();}
                        ImGui::EndTable();}
                    ImGui::EndDisabled();}
                    ImGui::EndDisabled();
                    if(es.scheduled)ImGui::TextDisabled(es.extendedCadence?"RP2040: 30-120 eye openings/s, up to four refreshes per eye including reset frames. Results require visual calibration.":"Flash firmware 0.3.0 to enable LCD preload and reset sequences.");
                    ImGui::Spacing();label("PANEL");
                    {bool edited=ImGui::InputDouble("Panel response us",&settings.panelResponseUs,100,500,"%.0f");edited|=ImGui::InputDouble("Panel rise us (0 = same as response)",&settings.panelRiseUs,100,500,"%.0f");if(edited){settings.panelKind=PanelKind::Custom;changed=true;}}
                    ImGui::TextDisabled("Display type: %s (Live tab).",panelKindName(settings.panelKind));
                    changed|=ImGui::InputDouble("Panel scan us (0 = signal timing)",&settings.panelScanUs,100,1000,"%.0f");
                    {int illumination=int(settings.illumination);if(ImGui::Combo("Illumination",&illumination,"Sample and hold (rows stay lit)\0Strobed backlight (LightBoost, black frame insertion, Motion Clearness)\0")){settings.illumination=Illumination(illumination);changed=true;}
                     if(settings.illumination==Illumination::Strobed){changed|=ImGui::InputDouble("Strobe start after scan start us",&settings.strobeStartUs,100,500,"%.0f");changed|=ImGui::InputDouble("Strobe length us",&settings.strobeLengthUs,100,500,"%.0f");
                        paragraph("The panel lights only during this pulse each refresh. Find it with a phase sweep and the shortest shutter (Output & timing > Phase sweep > Set strobe from the last two marks) or from a high-speed clip; Suggest phase then centers the shutter on the pulse.");}}
                    changed|=ImGui::InputDouble("Scan start after presentation timestamp us",&settings.scanStartUs,10,100,"%.0f");ImGui::SameLine();ImGui::TextDisabled("(Diagnostics > Measure vblank)");
                    if(!displays.empty()&&displays[displayIndex].totalLines)ImGui::TextDisabled("Signal: %u active of %u lines, scan %.2f ms of %.2f ms.",displays[displayIndex].activeLines,displays[displayIndex].totalLines,displays[displayIndex].scanUs/1000,periodUs(displays[displayIndex].refresh)/1000);
                    paragraph("These are assumptions for the old model unless independently measured. Actual response varies with both gray levels, overdrive and frame history. Signal scan time describes the input, not necessarily the panel. Illumination only configures this model; it cannot enable hardware strobing.");
                    label("MANUAL EMITTER RATE");
                    for(unsigned hz:{60u,100u,120u,144u,165u,240u}){ImGui::SameLine();ImGui::BeginDisabled(es.scheduled&&!rp2040TimingSupported(hz,settings.sequence,es.extendedCadence,es.fastCadence));if(ImGui::Button((std::to_string(hz)+" Hz").c_str()))runAction([&]{applyTimingPreset(settings,hz);changed=true;});ImGui::EndDisabled();}
                    ImGui::Spacing();label("OUTPUT FORMAT");
                    if(!displays.empty()){
                        auto& d=displays[displayIndex];ImGui::BeginDisabled(!d.hdrEnabled);changed|=ImGui::Checkbox("HDR output",&settings.hdr);ImGui::EndDisabled();
                        std::string mode=d.modes.empty()?"No modes":std::to_string(d.modes[modeIndex].dmPelsWidth)+" x "+std::to_string(d.modes[modeIndex].dmPelsHeight)+" @ "+std::to_string(d.modes[modeIndex].dmDisplayFrequency)+" Hz";
                        if(ImGui::BeginCombo("Mode",mode.c_str())){for(size_t i=0;i<d.modes.size();++i){auto& m=d.modes[i];std::string title=std::to_string(m.dmPelsWidth)+" x "+std::to_string(m.dmPelsHeight)+" @ "+std::to_string(m.dmDisplayFrequency)+" Hz";if(ImGui::Selectable(title.c_str(),int(i)==modeIndex))modeIndex=int(i);}ImGui::EndCombo();}
                        ImGui::BeginDisabled(d.modes.empty());if(ImGui::Button("Apply selected mode"))runAction([&]{stopOutput();source.stop();modeGuard.apply(d,d.modes[modeIndex]);displays=enumerateDisplays();syncDisplay(true);update();});ImGui::EndDisabled();
                    }
                    if(ImGui::Button("Brightness pattern")){source.stop();sourceKind=0;sourceConfig.kind=SourceKind::Patterns;step=3;changed=true;}
                    ImGui::BeginDisabled(!settings.hdr);changed|=ImGui::SliderFloat("Highlight nits",&settings.peakNits,80,1000,"%.0f");ImGui::EndDisabled();if(!settings.hdr)ImGui::TextDisabled("Highlight nits applies to the brightness ramp in HDR output only.");
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Display",nullptr,requestedTool==6?ImGuiTabItemFlags_SetSelected:0)){
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
                        double hz=displays[displayIndex].refresh;if(es.scheduled&&!rp2040TimingSupported(hz,settings.sequence,es.extendedCadence,es.fastCadence))throw std::runtime_error("Select a compatible RP2040 sequence first (30-120 eye openings/s with updated firmware).");
                        if(hz<100&&!es.extendedCadence)throw std::runtime_error("Stereo output requires at least 100 Hz with this emitter firmware.");
                        double scale=settings.refresh/hz;settings.phaseUs*=scale;settings.leftUs*=scale;settings.rightUs*=scale;settings.refresh=hz;update();notice="Emitter timing now follows the selected output rate.";
                    });ImGui::EndDisabled();ImGui::Spacing();ImGui::Separator();
                        {label("DISPLAY TYPE AND FRAME RATE");
                        {int kind=int(settings.panelKind);ImGui::SetNextItemWidth(320*dpi);if(ImGui::Combo("Display type",&kind,"Custom (numbers under Advanced > Panel)\0OLED\0LCD (VA)\0LCD (IPS / TN)\0QLED / mini-LED (local dimming)\0")){applyPanelKind(settings,PanelKind(kind));changed=true;}
                         ImGui::Text("New frames per eye per second at %.0f Hz:",settings.refresh);ImGui::SameLine();
                         ImGui::BeginDisabled((es.scheduled&&!es.extendedCadence)||settings.guardLevel>0);
                         for(unsigned run=1;run<=2;run++){ImGui::PushID(int(run));const bool current=sequenceHold(settings.sequence)+sequenceBlack(settings.sequence)==run;if(current)ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.2f,.5f,.35f,1));
                            char labelText[32];snprintf(labelText,sizeof labelText,"%.0f",settings.refresh/(2*run));
                            ImGui::BeginDisabled(es.scheduled&&!rp2040TimingSupported(settings.refresh,makeSequence(run,0),es.extendedCadence,es.fastCadence));if(ImGui::Button(labelText)){auto panel=panelTiming();if(panel.scanUs<=0)notice="Panel scan time unknown; enter it under Advanced > Panel.";else{const bool scheduled=es.scheduled;auto ceiling=[scheduled](double hz){return scheduled?(hz<29.97||hz>120.48?0.:calibrationMaxShutterUs(hz)):nvidiaMaxShutterUs(hz);};
                                auto o=bestSequenceForRun(settings.refresh,panel,settings.bandHeight,settings.bandCenter,run,.01,ceiling);
                                settings.sequence=o.sequence;settings.leftUs=settings.rightUs=o.shutterUs;settings.rightOffsetUs=0;settings.phaseUs=emitterPhaseFromModel(o.modelPhaseUs,settings.refresh,o.sequence,settings.scanStartUs,usbLatency());
                                settings.cancelCrosstalk=false;settings.guardLevel=0;changed=true;
                                std::ostringstream t;t<<std::fixed<<std::setprecision(0)<<"Sequence "<<sequencePattern(o.sequence)<<": "<<o.framesPerEye<<" new frames per eye per second, shutter "<<o.shutterUs<<" us, predicted leak "<<std::setprecision(1)<<std::min(o.estimate.mean,9.99)*100<<" %"<<(o.estimate.mean>.01?" (no clean window at this rate on this panel; the least leaking pattern was chosen)":"")<<std::setprecision(0)<<", light "<<o.estimate.brightness*o.shutterUs/periodUs(settings.refresh)*100<<" % of a lit frame. Sweep the phase by eye from here (Up/Down).";notice=t.str();}}
                            ImGui::EndDisabled();if(current)ImGui::PopStyleColor();ImGui::PopID();ImGui::SameLine();}
                         ImGui::EndDisabled();ImGui::NewLine();
                         ImGui::Text("Now: %s, %.0f new frames per eye per second, %s.",sequencePattern(settings.sequence).c_str(),settings.refresh/cycleLength(settings.sequence),panelKindName(settings.panelKind));
                         paragraph("Display type is a label, not a response-time measurement. These frame-rate buttons use the approximate model under Advanced. For LCD/QLED tests without guessed response times, use Panel experiments.");}
                    }
                    if(!settings.lcd.enabled){label("SHUTTER TIMING");
                        double period=periodUs(settings.refresh),phaseMin=es.scheduled&&!es.extendedCadence?-period*.5:0,phaseMax=es.scheduled&&!es.extendedCadence?period*.5:phaseCycleUs(settings.refresh,settings.sequence)-1;
                        ImGui::SetNextItemWidth(-1);changed|=ImGui::SliderScalar("##phase",ImGuiDataType_Double,&settings.phaseUs,&phaseMin,&phaseMax,"Phase %.0f us");
                        ImGui::SetNextItemWidth(220*dpi);changed|=ImGui::InputDouble("Phase us",&settings.phaseUs,100,10,"%.0f");
                        if(!es.scheduled||es.extendedCadence)ImGui::TextDisabled("Full eye cycle: 0 to %.0f us",phaseMax);
                        // The shutter may span the whole emitter period, which is two display refreshes in a
                        // four-slot sequence. Capping it at one refresh discarded half of a black-frame sequence's light.
                        double shutter=settings.leftUs,shutterMin=minimumShutterUs,shutterMax=es.scheduled?calibrationMaxShutterUs(settings.refresh):nvidiaMaxShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence));
                        ImGui::SetNextItemWidth(-1);bool shutterChanged=ImGui::SliderScalar("##shutter",ImGuiDataType_Double,&shutter,&shutterMin,&shutterMax,"Shutter %.0f us");
                        ImGui::SetNextItemWidth(220*dpi);shutterChanged|=ImGui::InputDouble("Shutter us",&shutter,100,10,"%.0f");
                        if(shutterChanged){settings.leftUs=settings.rightUs=shutter;changed=true;}
                        if(settings.leftUs!=settings.rightUs)ImGui::Text("Per-eye durations: L %.0f / R %.0f us",settings.leftUs,settings.rightUs);
                        if(!es.scheduled){double effective=nvidiaEffectiveShutterUs(sequenceEmitterHz(settings.refresh,settings.sequence),settings.phaseUs,std::max(settings.leftUs,settings.rightUs));
                            if(effective<std::max(settings.leftUs,settings.rightUs)-1)ImGui::TextColored({1,.8f,.3f,1},"At this phase the emitter shortens the window to %.0f us (its delay register has to move away from the period boundary). Shift the phase by about 1 ms either way for the full shutter.",effective);}
                        if(settings.sequence!=Sequence::Alternating)ImGui::TextDisabled("This sequence gives the emitter a %.1f ms period, so the shutter can stay open far longer than one refresh; a wider shutter can collect more light but must be checked again for leakage.",periodUs(sequenceEmitterHz(settings.refresh,settings.sequence))/1000);
                        {float gain=settings.imageGain;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##gain",&gain,1,8,"Image brightness %.2fx")){settings.imageGain=gain;changed=true;}
                         paragraph(settings.hdr?"Brightness multiplies the image the app presents, using the display's HDR headroom. It does not reach a game through the hook; raise the game's or the display's own brightness there.":"Brightness multiplies the image the app presents. In SDR it clips to white, so most of the recovery has to come from a wider shutter or the display's brightness control. It does not reach a game through the hook.");}
                         {float floorPercent=settings.blackFloor*100;ImGui::SetNextItemWidth(-1);if(ImGui::SliderFloat("##blackfloor",&floorPercent,0,30,"LCD black floor %.0f %% of drive")){settings.blackFloor=floorPercent/100;changed=true;}
                          paragraph("Legacy black-level lift changes the eye images. It does not control panel voltage and has not been shown to make the LCD settle faster. Leave at zero when measuring separation. Neutral reset under Panel experiments instead changes only the refreshes between eyes.");}
                        changed|=ImGui::Checkbox("Swap eye order",&settings.swapEyes);
                        paragraph("Phase moves the shutter timing relative to the image. Shutter sets its duration. Changes apply live.");
                        if(ImGui::Button("Reset timing"))runAction(resetTiming);
                        if(es.state==EmitterState::Simulated)ImGui::TextDisabled("Simulated emitter: timing controls change nothing physical.");
                        else if(!es.scheduled&&emitterOnline){auto sch=nvidiaSchedule(sequenceEmitterHz(settings.refresh,settings.sequence),settings.phaseUs);ImGui::TextDisabled("Emitter path: eye command at the predicted vblank, boundary %.0f us, X delay %.0f us; %llu timing writes, %llu eye commands.",sch.boundaryUs,sch.delayUs,es.timingWrites,es.commands);}
                        {bool bfi=settings.sequence==Sequence::BlackInsertion;ImGui::BeginDisabled(es.scheduled&&!es.extendedCadence);if(ImGui::Checkbox("Software black frame insertion (Left / Black / Right / Black)",&bfi)){settings.sequence=bfi?Sequence::BlackInsertion:Sequence::Alternating;settings.guardLevel=0;changed=true;}ImGui::EndDisabled();
                         if(bfi)paragraph("Every second refresh is black. Each eye gets refresh/4 new images per second: 30 at 120 Hz, 60 at 240 Hz. LCD pixels can retain the previous eye during reset frames; compare Preload under Panel experiments. Brightness and separation depend on panel response and shutter timing. Software black frames do not switch off the backlight.");
                         else if(settings.sequence!=Sequence::Alternating)ImGui::TextDisabled("Sequence %s (Advanced > Sequence): %.0f new frames per eye per second.",sequencePattern(settings.sequence).c_str(),settings.refresh/cycleLength(settings.sequence));}
                        {bool cancel=settings.cancelCrosstalk;ImGui::BeginDisabled(settings.guardLevel>0);if(ImGui::Checkbox("Legacy ghost subtraction (experimental, C)",&cancel)){settings.cancelCrosstalk=cancel;changed=true;}ImGui::EndDisabled();
                         if(cancel){ImGui::SetNextItemWidth(-1);float strength=settings.cancelStrength;if(ImGui::SliderFloat("##cancelstrength",&strength,0,2,"Cancellation strength %.2fx")){settings.cancelStrength=strength;changed=true;}
                            const auto& k=settings.leakProfile;ImGui::TextDisabled("Leak subtracted per row, top to bottom: %.0f / %.0f / %.0f / %.0f / %.0f / %.0f / %.0f / %.0f %%.",k[0]*100,k[1]*100,k[2]*100,k[3]*100,k[4]*100,k[5]*100,k[6]*100,k[7]*100);
                            paragraph("Legacy subtraction uses an approximate, unmeasured leak model. It cannot remove all bright-on-black ghosts, and clipping or panel overdrive can invalidate its prediction. It does not enable a strobe backlight. Leave off for panel experiments.");}}
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
                ImGui::EndTabBar();requestedTool=-1;
            }
            ImGui::EndChild();
            if(changed)runAction(update);
            ImGui::EndTable();
            if(!error.empty()){ImGui::TextWrapped("%s",error.c_str());}else paragraph(notice.c_str());

            if(previewOnLaunch&&emitterOnline&&rateMatches){previewOnLaunch=false;runAction([&]{startOutput(false,true);});}
            if(screenOnLaunch&&emitterOnline&&rateMatches){screenOnLaunch=false;runAction([&]{sourceKind=3;sourceConfig.kind=SourceKind::Screen;sourceConfig.monitor=displays[screenDisplayIndex].monitor;requestedTool=1;update();source.start(sourceConfig,displays[displayIndex].adapterLuid);startOutput(false,false,nullptr,true);update();});}
            if(fullscreenOnLaunch&&emitterOnline&&rateMatches){fullscreenOnLaunch=false;runAction(startFullscreenOutput);}
            if(liveUiTest){
                if(es.errors||es.state==EmitterState::Error)throw std::runtime_error("Live UI emitter failure: "+es.message);
                if(qpc()-liveTestStarted>60)throw std::runtime_error("Live UI test did not finish: "+rs.message+" / "+es.message);
                if(fullscreenMode&&rs.locked&&es.commands>50&&qpc()>=liveTestNext){
                    if(!liveTestWindow){liveTestWindow=outputWindow;liveTestStartCommands=es.commands;}
                    if(outputWindow!=liveTestWindow||!rs.running||rs.preview)throw std::runtime_error("Live edit replaced or disabled 3D output");
                    if(liveTestEdits<64){
                        constexpr double LcdTiming::* fields[]{&LcdTiming::settleUs,&LcdTiming::durationUs,&LcdTiming::phaseUs,&LcdTiming::guardUs,&LcdTiming::leftAdjustUs,&LcdTiming::rightAdjustUs,&LcdTiming::scanoutUs,&LcdTiming::referencePosition};
                        const double extremes[]{8000,8000,periodUs(settings.refresh),2000,1000,-1000,50000,1};
                        const auto before=settings.lcd;auto field=fields[liveTestEdits%8];
                        settings.lcd.*field=liveTestEdits%16<8?extremes[liveTestEdits%8]:(before.*field)*.9;
                        const auto requested=settings.lcd;update(false);if(settings.lcd!=requested)throw std::runtime_error("Slider failed to retain requested position");auto unrelated=settings.lcd;unrelated.*field=before.*field;
                        if(unrelated!=before)throw std::runtime_error("Live edit changed another slider");
                        selectPattern(int(liveTestEdits/8)%8);update(false);
                        if(liveTestEdits==20||liveTestEdits==24)toggleFullscreenControls();
                        ++liveTestEdits;liveTestNext=qpc()+.15;
                    }else if(liveTestPresets<12){
                        // Change all starting presets and LR/BFI on the same live
                        // output, including completing previously invalid drafts.
                        applyStartingPreset(int(liveTestPresets%4));
                        if(outputWindow!=liveTestWindow||!presenter.status().running)throw std::runtime_error("Preset closed the live preview");
                        ++liveTestPresets;liveTestNext=qpc()+.4;
                    }else if(!liveTestSteadyStart){liveTestSteadyStart=qpc();liveTestSteadyMisses=es.late;}
                    else if(qpc()-liveTestSteadyStart>5&&es.commands>liveTestStartCommands+100&&es.state==EmitterState::Running){
                        if(es.late-liveTestSteadyMisses>5)throw std::runtime_error("Live timing did not settle after edits");
                        std::ofstream report(workspace()/L"reports/ui-live-test.txt");
                        report<<"PASS: 64 freely editable timing requests, 12 preset/LR/BFI changes, all 8 test patterns, fullscreen controls hidden/restored; same 3D output window throughout.\n"
                            <<"Firmware: "<<es.firmwareVersion<<"\nCommands: "<<es.commands<<"; timing writes: "<<es.timingWrites<<"; errors: "<<es.errors<<"; recovered misses: "<<es.late<<"\n"
                            <<"Misses in five seconds after edits: "<<(es.late-liveTestSteadyMisses)<<"\n"
                            <<"Device opens/closes: "<<es.deviceOpens<<'/'<<es.deviceCloses<<"; measured Hz: "<<rs.measuredHz<<"; optical separation not measured.\n";
                        closeRequested=true;
                    }
                }
            }
            // Exercise the actual child-window presenter without USB writes or profile edits.
            // The production button passes sideBySide=false to enable frame-sequential 3D.
            if(smoke&&!smokePreviewStarted&&qpc()-smokeStart>1){if(fullscreenSmoke)startFullscreenOutput();else startOutput(true,true);smokePreviewStarted=true;settings.convergence=.01f;settings.phaseUs+=100;update();}
            if(smoke&&settings.lcd.enabled&&smokePreviewStarted&&!smokePreviewVerified&&qpc()-smokeStart>1.8){
                HWND original=outputWindow;
                for(int i=0;i<10;++i){
                    settings.lcd.settleUs=i%2?8000:0;settings.lcd.phaseUs=i%2?periodUs(settings.refresh):-periodUs(settings.refresh);settings.lcd.durationUs=8000;
                    update(false);
                    if(outputWindow!=original||!presenter.status().running||!lcdExposure(lastAppliedLcd,settings.refresh,settings.signalScanUs).valid)throw std::runtime_error("LCD slider edit interrupted the live preview.");
                }
            }
            if(smoke&&smokePreviewStarted&&!smokePreviewVerified&&qpc()-smokeStart>2.5){
                RECT actual{};GetWindowRect(outputWindow,&actual);MapWindowPoints(nullptr,controlWindow,reinterpret_cast<POINT*>(&actual),2);
                auto test=presenter.status();
                // A hidden parent is occluded, so Present need not advance in this smoke test.
                if((!fullscreenSmoke&&(GetParent(outputWindow)!=controlWindow||!EqualRect(&actual,&previewRect)))||(fullscreenSmoke&&!fullscreenMode)||!test.running||emitter.status().commands!=0)throw std::runtime_error("Embedded live preview regression: window placement, presenter startup or USB isolation failed: "+test.message);
                if(sourceKind==3){
                    for(HWND window:{controlWindow,outputEmbedded?controlWindow:outputWindow}){
                        DWORD affinity=0;if(!GetWindowDisplayAffinity(window,&affinity)||affinity!=WDA_EXCLUDEFROMCAPTURE)
                            throw std::runtime_error("Screen source output would capture itself after switching views.");
                    }
                }
                if(fullscreenSmoke){
                    const HWND original=outputWindow;const auto originalKind=sourceKind;const auto packing=sourceConfig.packing;const auto originalPhase=settings.phaseUs;const auto originalSequence=settings.sequence;
                    RECT before{};GetWindowRect(original,&before);const auto outputStyle=GetWindowLongPtrW(original,GWL_EXSTYLE);
                    for(int i=0;i<6;++i)controlMenu.toggle();
                    RECT after{};GetWindowRect(outputWindow,&after);
                    if(outputWindow!=original||GetParent(original)||!EqualRect(&before,&after)||GetWindowLongPtrW(original,GWL_EXSTYLE)!=outputStyle||sourceKind!=originalKind||sourceConfig.packing!=packing||settings.phaseUs!=originalPhase||settings.sequence!=originalSequence||!presenter.status().running||!presenter.status().preview)
                        throw std::runtime_error("Fullscreen menu changed the active source, output or timing.");
                    controlMenu.show();
                }
                smokePreviewVerified=true;
            }
            ImGui::End();ImGui::Render();ui.bind();float clear[]{.035f,.047f,.07f,1};ui.context->ClearRenderTargetView(ui.target.Get(),clear);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if(fullscreenSmoke&&smokePreviewVerified)saveSurfacePng(ui,workspace()/L"reports/ui-fullscreen-controls.png");
            if(smoke && !smokeSnapshot && qpc()-smokeStart>.75){saveSurfacePng(ui,workspace()/L"reports/ui-preview.png");smokeSnapshot=true;}HRESULT shown=ui.swap->Present(1,0);if(shown==DXGI_STATUS_OCCLUDED)Sleep(20);else check(shown,"Control window present");
        }
        stopOutput();gameSync.stop();source.stop();emitter.disconnect();modeGuard.restore();keyBindings.activate(false);shortcuts=nullptr;if(!smoke)Shell_NotifyIconW(NIM_DELETE,&tray);ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();DestroyWindow(controlWindow);
        if(smoke){std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/ui-smoke.txt");if(!smokePreviewVerified)throw std::runtime_error("Preview check did not complete.");f<<"PASS: native UI, "<<(fullscreenSmoke?"fullscreen controls":"embedded child-window placement")<<" and presenter startup after live alignment/timing edits; clean shutdown. Hidden controls: optical behavior not measured. No emitter writes, profile edits or display mode changes.\n";}
    }catch(const std::exception& e){std::filesystem::create_directories(workspace()/L"reports");std::ofstream f(workspace()/L"reports/last-error.txt");f<<e.what();if(!gpuTest&&!probe&&!smoke)MessageBoxA(nullptr,e.what(),"Vision Restoration",MB_OK|MB_ICONERROR);if(SUCCEEDED(co))CoUninitialize();return 1;}
    if(SUCCEEDED(co))CoUninitialize();return 0;
}
