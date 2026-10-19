// Controlled changing window, real capture textures, and GPU-completed copies.
// Frame marker timestamps measure producer submission to completed source copy, not optical latency.
#include "gpu_capture.h"
#include "gpu_completion.h"
#include "renderer.h"
#include "window_worker.h"
#include <d3d11_1.h>
#include <DirectXPackedVector.h>
#include <map>
#include <winrt/base.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace vision;
static double cpuSeconds(){FILETIME created{},exit{},kernel{},user{};check(GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user)?S_OK:E_FAIL,"Process CPU times");ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return double(k.QuadPart+u.QuadPart)/1e7;}
static double percentile(std::vector<double> values,double p){if(values.empty())return 0;std::sort(values.begin(),values.end());return values[size_t(p*double(values.size()-1))];}
static void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
int main(int argc,char** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        constexpr const char* usage="Usage: vision_gpu_capture_bench [report.csv [idle|load] [waitable] [excluded-overlay]]\n"
            "excluded-overlay temporarily covers the first display with a click-through D3D output excluded from capture.\n";
        if(argc>1&&(std::string(argv[1])=="--help"||std::string(argv[1])=="--?"||std::string(argv[1])=="/?")){std::cout<<usage;return 0;}
        bool load=false,waitable=false,excludedOverlay=false;
        for(int i=2;i<argc;++i){const std::string arg=argv[i];
            if(arg=="load")load=true;else if(arg=="idle")load=false;
            else if(arg=="waitable")waitable=true;else if(arg=="excluded-overlay"||arg=="--excluded-overlay")excludedOverlay=true;
            else throw std::runtime_error(usage);
        }
        // An explicit producer diagnostic; capture implementations stay identical.
        const auto displays=enumerateDisplays();if(displays.empty())throw std::runtime_error("No capture display");const auto& display=displays[0];
        if(display.rect.right-display.rect.left<1320||display.rect.bottom-display.rect.top<760)throw std::runtime_error("Benchmark requires a display at least 1320x760 physical pixels for its 1280x720 producer.");
        std::ofstream file(argc>1?argv[1]:"capture-benchmark.csv");if(!file)throw std::runtime_error("Cannot write benchmark report");
        file<<"backend,path,load,display_hz,producer_fps,captured_fps,producer_to_copy_p50_ms,producer_to_copy_p95_ms,producer_to_copy_max_ms,capture_handling_p50_ms,capture_handling_p95_ms,capture_handling_max_ms,process_cpu_core_percent,frames,unique_producer_frames,stalls_over_100ms,producer_mode,producer_to_capture_p50_ms,producer_to_capture_p95_ms,overlay_mode,overlay_presents,unique_capture_fps,changing_pixels_verified,invalid_markers\n";
        std::cout<<display.name<<" | "<<display.gpu<<" | "<<display.width<<"x"<<display.height<<" | "<<display.refresh<<" Hz | HDR "<<display.hdrEnabled<<" | load "<<load<<" | producer "<<(waitable?"waitable queue=1":"legacy vsync")<<" | overlay "<<(excludedOverlay?"full-display excluded D3D":"none")<<std::endl;
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(display.adapterLuid,IID_PPV_ARGS(&adapter)),"Adapter");
        auto createDevice=[&](ComPtr<ID3D11Device>& device,ComPtr<ID3D11DeviceContext>& context){check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Device");};
        struct Window {HWND h=nullptr;~Window(){if(h)DestroyWindow(h);}} window;
        window.h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Capture benchmark (temporary)",WS_POPUP|WS_VISIBLE,display.rect.left+40,display.rect.top+40,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);if(!window.h)throw std::runtime_error("Benchmark window");
        ComPtr<ID3D11Device> producerDevice;ComPtr<ID3D11DeviceContext> producerContext;createDevice(producerDevice,producerContext);
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=1280;sd.Height=720;sd.Format=display.hdrEnabled?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if(waitable)sd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(producerDevice.Get(),window.h,&sd,nullptr,nullptr,&chain),"Workload swap chain");
        if(display.hdrEnabled){ComPtr<IDXGISwapChain3> hdr;check(chain.As(&hdr),"HDR swap chain");check(hdr->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),"HDR workload color space");}
        struct WaitHandle {HANDLE h=nullptr;~WaitHandle(){if(h)CloseHandle(h);}} frameReady;
        if(waitable){ComPtr<IDXGISwapChain2> paced;check(chain.As(&paced),"Waitable producer swap chain");check(paced->SetMaximumFrameLatency(1),"Producer swap-chain latency");frameReady.h=paced->GetFrameLatencyWaitableObject();if(!frameReady.h)throw std::runtime_error("Missing producer frame-latency event");}
        else {ComPtr<IDXGIDevice1> producerDxgi;check(producerDevice.As(&producerDxgi),"Producer DXGI");check(producerDxgi->SetMaximumFrameLatency(1),"Producer latency");}
        ComPtr<ID3D11Texture2D> back;check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"Workload buffer");ComPtr<ID3D11RenderTargetView> target;check(producerDevice->CreateRenderTargetView(back.Get(),nullptr,&target),"Workload target");
        ComPtr<ID3D11Texture2D> stress;ComPtr<ID3D11RenderTargetView> stressTarget;
        if(load){D3D11_TEXTURE2D_DESC d{};d.Width=3840;d.Height=2160;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET;check(producerDevice->CreateTexture2D(&d,nullptr,&stress),"Stress texture");check(producerDevice->CreateRenderTargetView(stress.Get(),nullptr,&stressTarget),"Stress target");}
        Window overlayWindow;Surface overlay;uint64_t overlayPresents=0;
        if(excludedOverlay){
            WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionCaptureBenchmarkOverlay";
            if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Register benchmark overlay");
            overlayWindow.h=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,
                wc.lpszClassName,L"Capture benchmark excluded output (temporary)",WS_POPUP,display.rect.left,display.rect.top,
                display.rect.right-display.rect.left,display.rect.bottom-display.rect.top,nullptr,nullptr,wc.hInstance,nullptr);
            if(!overlayWindow.h)throw std::runtime_error("Create benchmark overlay");
            check(SetLayeredWindowAttributes(overlayWindow.h,0,255,LWA_ALPHA)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Opaque click-through benchmark overlay");
            check(SetWindowDisplayAffinity(overlayWindow.h,WDA_EXCLUDEFROMCAPTURE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Exclude benchmark output from capture");
            overlay.create(overlayWindow.h,&display.adapterLuid,display.hdrEnabled,1);
            ShowWindow(overlayWindow.h,SW_SHOWNOACTIVATE);
        }
        auto presentOverlay=[&]{if(!excludedOverlay)return;
            const DWORD ready=WaitForSingleObject(overlay.waitable,0);if(ready==WAIT_TIMEOUT)return;
            check(ready==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Benchmark output frame event");
            // Dark, changing color cannot decode as the producer's changing 16-bit marker.
            const float color[]{.02f+float(overlayPresents%120)/6000.f,.025f,.035f,1};
            overlay.context->ClearRenderTargetView(overlay.target.Get(),color);
            const HRESULT hr=overlay.swap->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return;check(hr,"Present benchmark excluded output");
            if(hr==S_OK)++overlayPresents;
        };
        ComPtr<ID3D11DeviceContext1> producerContext1;check(producerContext.As(&producerContext1),"Marker drawing context");
        auto submitted=std::make_unique<std::array<std::atomic<double>,65536>>();for(auto& time:*submitted)time=0;
        std::atomic<uint64_t> generated=0;std::atomic<HRESULT> producerError=S_OK;
        std::jthread producer([&](std::stop_token stop){while(!stop.stop_requested()){
            if(waitable){const DWORD ready=WaitForSingleObject(frameReady.h,50);if(ready==WAIT_TIMEOUT)continue;if(ready!=WAIT_OBJECT_0){producerError=HRESULT_FROM_WIN32(GetLastError());break;}if(stop.stop_requested())break;}
            const auto n=generated.load();const float color[]{float(n%251)/250.f,float((n*3)%251)/250.f,float((n*7)%251)/250.f,1};
            if(load)for(int i=0;i<8;++i){float busy[]{color[0],float(i)/8.f,color[2],1};producerContext->ClearRenderTargetView(stressTarget.Get(),busy);}
            const uint16_t marker=uint16_t(n+1);(*submitted)[marker]=qpc();
            for(unsigned bit=0;bit<16;++bit){const float value=(marker&(1u<<bit))?1.f:0.f;float pixel[]{value,value,value,1};D3D11_RECT rect{LONG(bit*80),0,LONG((bit+1)*80),720};producerContext1->ClearView(target.Get(),pixel,&rect,1);}
            HRESULT hr=chain->Present(1,0);if(FAILED(hr)){producerError=hr;break;}++generated;
        }});
        struct ProducerGuard{std::jthread& worker;~ProducerGuard(){stopWindowWorker(worker);}} producerGuard{producer};
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;createDevice(device,context);
        for(int round=0;round<2;++round)for(int path=0;path<2;++path)for(int order=0;order<2;++order){
            auto method=((order+round)%2)==0?CaptureMethod::DesktopDuplication:CaptureMethod::WindowsGraphicsCapture;
            const char* name=method==CaptureMethod::DesktopDuplication?"DXGI":"WGC";const char* pathName=path==0?"stereo-source-window":"ai-desktop-depth-off";
            SourceConfig config;config.kind=path==0?SourceKind::Window:SourceKind::Screen;config.monitor=display.monitor;config.window=path==0?window.h:nullptr;config.captureMethod=method;config.screen.depth=false;config.screen.pairRate=display.refresh;
            StereoSource source;source.start(config,display.adapterLuid);
            struct Opened {ComPtr<ID3D11Texture2D> texture;ComPtr<IDXGIKeyedMutex> mutex;};std::map<HANDLE,Opened> opened;
            ComPtr<ID3D11Texture2D> staging;DXGI_FORMAT stagingFormat=DXGI_FORMAT_UNKNOWN;
            std::vector<double> ages,handling,upstream;uint64_t lastPair=0,frames=0,stalls=0,invalidMarkers=0;std::bitset<65536> seenMarkers;
            const double start=qpc(),sampleStart=start+.75,finish=sampleStart+3;double cpuStart=0,lastFrame=0;uint64_t producedStart=0,overlayStart=overlayPresents;
            while(qpc()<finish){
                pump();presentOverlay();check(producerError.load(),"Workload producer");const double before=qpc();if(before>=sampleStart&&!cpuStart){cpuStart=cpuSeconds();producedStart=generated.load();overlayStart=overlayPresents;}
                bool received=false;
                auto frame=source.latest();
                if(frame&&frame->pairId!=lastPair){
                    const double arrival=qpc();
                    auto& snapshot=opened[frame->sharedHandle];
                    if(!snapshot.texture){check(device->OpenSharedResource(frame->sharedHandle,IID_PPV_ARGS(&snapshot.texture)),"Open benchmark snapshot");check(snapshot.texture.As(&snapshot.mutex),"Benchmark mutex");}
                    HRESULT locked=snapshot.mutex->AcquireSync(0,0);
                    if(locked==S_OK){
                        D3D11_TEXTURE2D_DESC desc{};snapshot.texture->GetDesc(&desc);
                        if(!staging||stagingFormat!=desc.Format){stagingFormat=desc.Format;desc.Width=path==0?1280:2560;desc.Height=1;desc.MipLevels=desc.ArraySize=1;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&desc,nullptr,&staging),"Marker staging");}
                        unsigned x=path==0?0:40,y=(path==0?0:40)+360;
                        D3D11_BOX box{x,y,0,x+1280,y+1,1};context->CopySubresourceRegion(staging.Get(),0,0,0,0,snapshot.texture.Get(),0,&box);
                        if(path==1){D3D11_BOX right{frame->width/2+x,y,0,frame->width/2+x+1280,y+1,1};context->CopySubresourceRegion(staging.Get(),0,1280,0,0,snapshot.texture.Get(),0,&right);}
                        D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT mappedResult=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped);
                        if(FAILED(mappedResult)){snapshot.mutex->ReleaseSync(0);check(mappedResult,"Read frame marker");}
                        unsigned marker=0;bool identicalEyes=true;
                        for(unsigned bit=0;bit<16;++bit){unsigned pixel=bit*80+40;float value=0;
                            if(stagingFormat==DXGI_FORMAT_R16G16B16A16_FLOAT)value=DirectX::PackedVector::XMConvertHalfToFloat(static_cast<const uint16_t*>(mapped.pData)[pixel*4]);
                            else value=float(static_cast<const uint8_t*>(mapped.pData)[pixel*4])/255.f;
                            if(value>.5f)marker|=1u<<bit;
                            if(path==1){const unsigned bytes=stagingFormat==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;auto* pixels=static_cast<const uint8_t*>(mapped.pData);identicalEyes&=memcmp(pixels+pixel*bytes,pixels+(1280+pixel)*bytes,bytes)==0;}
                        }
                        context->Unmap(staging.Get(),0);check(snapshot.mutex->ReleaseSync(0),"Release benchmark snapshot");
                        if(!identicalEyes)throw std::runtime_error("Flat desktop eyes differ; HDR pixels were not preserved");
                        lastPair=frame->pairId;received=true;
                        const double timestamp=(*submitted)[marker].load();
                        if(before>=sampleStart&&timestamp>0&&timestamp<=arrival){
                            ages.push_back((arrival-timestamp)*1000);if(frame->captureAcquiredAt>0)handling.push_back((frame->publishedAt-frame->captureAcquiredAt)*1000);++frames;if(lastFrame&&arrival-lastFrame>.1)++stalls;lastFrame=arrival;
                            if(frame->captureAcquiredAt>=timestamp)upstream.push_back((frame->captureAcquiredAt-timestamp)*1000);
                            seenMarkers.set(marker);
                        }else if(before>=sampleStart)++invalidMarkers;
                    }else if(locked!=HRESULT(WAIT_TIMEOUT))check(locked,"Lock benchmark snapshot");
                }else if(!source.status().running)throw std::runtime_error(source.status().message);
                if(!received)shortWait();
            }
            const double elapsed=qpc()-sampleStart,producerFps=double(generated.load()-producedStart)/elapsed,captureFps=double(frames)/elapsed,cpu=(cpuSeconds()-cpuStart)/elapsed*100;
            const size_t unique=seenMarkers.count();const uint64_t overlayFrames=overlayPresents-overlayStart;
            const bool changing=unique>=10&&(!excludedOverlay||overlayFrames>=10);
            file<<std::fixed<<std::setprecision(3)<<name<<','<<pathName<<','<<load<<','<<display.refresh<<','<<producerFps<<','<<captureFps<<','<<percentile(ages,.5)<<','<<percentile(ages,.95)<<','<<percentile(ages,1)<<','<<percentile(handling,.5)<<','<<percentile(handling,.95)<<','<<percentile(handling,1)<<','<<cpu<<','<<frames<<','<<unique<<','<<stalls<<','<<(waitable?"waitable-queue-one":"legacy-vsync")<<','<<percentile(upstream,.5)<<','<<percentile(upstream,.95)<<','<<(excludedOverlay?"excluded-full-display":"none")<<','<<overlayFrames<<','<<double(unique)/elapsed<<','<<changing<<','<<invalidMarkers<<'\n';file.flush();
            std::cout<<name<<" "<<pathName<<" round "<<round+1<<": "<<captureFps<<" fps | "<<unique<<" unique producer markers | "<<overlayFrames<<" overlay presents | producer-to-copy median "<<percentile(ages,.5)<<" ms, p95 "<<percentile(ages,.95)<<" ms | before capture "<<percentile(upstream,.5)<<" / "<<percentile(upstream,.95)<<" ms | capture handling "<<percentile(handling,.5)<<" / "<<percentile(handling,.95)<<" ms (median/p95) | CPU "<<cpu<<"% of one core"<<std::endl;
            if(!changing)throw std::runtime_error(std::string(name)+" "+pathName+" did not prove changing source pixels: "+std::to_string(unique)+" unique markers, "+std::to_string(overlayFrames)+" overlay presents; "+source.status().captureBackend);
            source.stop();
        }
        return 0;
    }catch(const winrt::hresult_error& e){std::cerr<<utf8(e.message().c_str());return 1;}
    catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
