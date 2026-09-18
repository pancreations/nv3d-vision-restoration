// SPDX-License-Identifier: GPL-3.0-or-later
// Physical output, process-local GPU transfer and lifecycle regression. Uses
// the real runtime with a private host channel; it cannot command the emitter.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "sync_protocol.h"
#include "output_test_api.h"
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
using Microsoft::WRL::ComPtr;
namespace sync=vision::sync;
void require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
void check(HRESULT hr,const char* message){if(FAILED(hr)){char text[256];sprintf_s(text,"%s: 0x%08lx",message,hr);throw std::runtime_error(text);}}
int64_t ticks(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
unsigned clicks=0,keys=0;
LRESULT CALLBACK wndproc(HWND w,UINT m,WPARAM a,LPARAM b){if(m==WM_LBUTTONDOWN)++clicks;if(m==WM_KEYDOWN)++keys;return DefWindowProcW(w,m,a,b);}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
int wmain(int argc,wchar_t** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    std::jthread watchdog([](std::stop_token stop){for(int i=0;i<300&&!stop.stop_requested();++i)Sleep(100);if(!stop.stop_requested()){std::puts("FAIL: output test timed out");TerminateProcess(GetCurrentProcess(),10);}});
    try{
        require(sync::nextDisplayRefresh(103,100,500,500)==504,"Queued slots lost their refresh position");
        require(sync::nextDisplayRefresh(100,100,500,512)==513,"Drained queue resumed using an obsolete refresh");
        require(sync::nextDisplayRefresh(103,100,500,512)==516,"Late queued frames were ignored");
        for(unsigned cycle=2;cycle<=16;cycle+=2)for(unsigned gap=1;gap<=37;++gap){
            const auto target=sync::nextDisplayRefresh(100,100,500,500+gap);
            require(target%cycle==(501+gap)%cycle,"Recovery changed absolute eye phase");
        }
        wchar_t exe[MAX_PATH];GetModuleFileNameW(nullptr,exe,MAX_PATH);auto directory=std::filesystem::path(exe).parent_path();
        auto ini=directory/L"d3dxdm.ini";require(!std::filesystem::exists(ini),"Run the output tests in an empty test directory");
        const std::wstring mode=argc>1?argv[1]:L"katanga_vr";
        const bool katanga=mode==L"katanga_vr",vertical=mode==L"tab"||mode==L"tab_reversed",reversed=mode==L"sbs_reversed"||mode==L"tab_reversed";
        require(katanga||vertical||mode==L"sbs"||mode==L"sbs_reversed","Unknown test packing");
        require(WritePrivateProfileStringW(L"Device",L"direct_mode",mode.c_str(),ini.c_str())!=FALSE,"Cannot create test-only provider configuration");
        std::printf("Existing Geo11 output mode: %ls\n",mode.c_str());
        auto hostName=L"Local\\VisionRestoration.GameSync.Test."+std::to_wstring(GetCurrentProcessId());
        HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(sync::Shared),hostName.c_str());require(mapping!=nullptr,"Host mapping");
        auto* sh=static_cast<sync::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(sync::Shared)));require(sh!=nullptr,"Host view");
        *sh={};sh->magic=sync::magic;sh->version=sync::version;sh->hostPid=GetCurrentProcessId();sh->enabled=1;sh->sequence=1;sh->hostHeartbeatQpc=ticks();
        std::jthread heartbeat([sh](std::stop_token stop){while(!stop.stop_requested()){InterlockedExchange64(&sh->hostHeartbeatQpc,ticks());Sleep(20);}});
        auto before=GetForegroundWindow();WNDCLASSW wc{};wc.lpfnWndProc=wndproc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionOutputTests";RegisterClassW(&wc);
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Stereo output regression (no emitter)",WS_OVERLAPPEDWINDOW,20,20,340,230,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"Test window");
        // Exercise the primary stereo display consistently; the foreground
        // control window can be on another adapter whose composed swapchain
        // returns successful but empty presentation statistics.
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        SetWindowPos(window,nullptr,monitor.rcWork.left+30,monitor.rcWork.top+30,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        // The debugger starts the console hidden. The first ShowWindow may
        // honor that STARTUPINFO hint; the second explicitly shows only this
        // small test window without taking foreground focus.
        ShowWindow(window,SW_SHOWNOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);pump();
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);pump();
        RECT windowRect{};GetWindowRect(window,&windowRect);wchar_t desktop[128]{};DWORD bytes=0;
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&bytes);
        wchar_t inputDesktop[128]{};HDESK inputDesk=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
        if(inputDesk){GetUserObjectInformationW(inputDesk,UOI_NAME,inputDesktop,sizeof(inputDesktop),&bytes);CloseDesktop(inputDesk);}
        std::wprintf(L"Window=%p rect=%ld,%ld-%ld,%ld desktop=%ls input=%ls\n",window,windowRect.left,windowRect.top,windowRect.right,windowRect.bottom,desktop,inputDesktop);
        HMODULE adapter=LoadLibraryW((directory/L"VisionStereo11Test.dll").c_str());require(adapter!=nullptr,"Test runtime");
        auto create=reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(adapter,"D3D11CreateDeviceAndSwapChain"));
        auto read=reinterpret_cast<void(WINAPI*)(VisionOutputTestStats*)>(GetProcAddress(adapter,"VisionOutputReadTestStats"));require(create&&read,"Test exports");
        auto delayProducer=reinterpret_cast<void(WINAPI*)(UINT)>(GetProcAddress(adapter,"VisionOutputDelayTestProducer"));require(delayProducer!=nullptr,"Producer stall test export");
        auto delayDisplay=reinterpret_cast<void(WINAPI*)(UINT)>(GetProcAddress(adapter,"VisionOutputDelayTestDisplay"));require(delayDisplay!=nullptr,"Display stall test export");
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=320;desc.BufferDesc.Height=180;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferCount=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SampleDesc.Count=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
        ComPtr<IDXGIAdapter1> chosen;wchar_t requestedVendor[32]{};
        if(GetEnvironmentVariableW(L"VISION_STEREO_TEST_VENDOR",requestedVendor,32)){
            const UINT vendor=_wcsicmp(requestedVendor,L"AMD")==0?0x1002:_wcsicmp(requestedVendor,L"NVIDIA")==0?0x10de:0;
            require(vendor!=0,"Unknown requested GPU vendor");ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Test adapter factory");
            for(UINT n=0;;++n){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(n,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 description{};candidate->GetDesc1(&description);if(description.VendorId==vendor){chosen=candidate;break;}}
            require(chosen!=nullptr,"Requested vendor is not installed");
        }
        check(create(chosen.Get(),chosen?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_SINGLETHREADED,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&device,nullptr,&context),"Virtual game device and swapchain (single-threaded game request)");
        ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> actual;check(device.As(&gd),"Actual GPU");check(gd->GetAdapter(&actual),"Actual adapter");DXGI_ADAPTER_DESC actualDesc{};actual->GetDesc(&actualDesc);std::printf("Adapter: %ls vendor=%04x device=%04x\n",actualDesc.Description,actualDesc.VendorId,actualDesc.DeviceId);
        // Publish through the same established Geo11 protocol. The runtime
        // isolates the name to this process automatically, without a game rule.
        HANDLE pairMap=katanga?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4,L"Local\\KatangaMappedFile"):nullptr;require(!katanga||pairMap!=nullptr,"Pair mapping");
        auto* handle=pairMap?static_cast<uint32_t*>(MapViewOfFile(pairMap,FILE_MAP_ALL_ACCESS,0,0,4)):nullptr;require(!katanga||handle!=nullptr,"Pair view");
        ComPtr<ID3D11Texture2D> pair;
        auto publish=[&](UINT w,UINT h){
            const UINT packedWidth=katanga?w*2:w;
            std::vector<uint32_t> pixels(size_t(packedWidth)*h);
            for(UINT y=0;y<h;++y)for(UINT x=0;x<packedWidth;++x){const bool first=vertical?y<h/2:x<packedWidth/2;pixels[size_t(y)*packedWidth+x]=(first!=reversed)?0xff0000ff:0xff00ff00;}
            if(katanga){
                D3D11_TEXTURE2D_DESC td{};td.Width=packedWidth;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
                D3D11_SUBRESOURCE_DATA init{pixels.data(),packedWidth*4,0};ComPtr<ID3D11Texture2D> next;check(device->CreateTexture2D(&td,&init,&next),"Producer eye pair");
                ComPtr<IDXGIResource> resource;check(next.As(&resource),"Pair sharing");HANDLE shared=nullptr;check(resource->GetSharedHandle(&shared),"Pair handle");pair=std::move(next);*handle=uint32_t(uintptr_t(shared));
            }else{
                ComPtr<ID3D11Texture2D> back;check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"Geo11 packed backbuffer");
                context->UpdateSubresource(back.Get(),0,nullptr,pixels.data(),packedWidth*4,0);
            }
            context->Flush();
            check(chain->Present(0,0),"Logical Present");
        };
        publish(320,180);
        auto wait=[&](auto predicate,const char* failure){const auto until=GetTickCount64()+5000;VisionOutputTestStats result{};do{pump();read(&result);if(predicate(result))return result;Sleep(5);}while(GetTickCount64()<until);std::printf("Diagnostics: eye samples=%llu/%llu/%llu pixels=%08x/%08x/%08x pairs=%llu presents=%llu active=%u hr=%08x visible=%u iconic=%u status=%s\n",result.samples[0],result.samples[1],result.samples[2],result.pixel[0],result.pixel[1],result.pixel[2],result.pairs,result.presents,sh->active,result.presentResult,result.windowVisible,result.windowIconic,sh->message);throw std::runtime_error(failure);};
        auto first=wait([](const auto& s){return s.samples[0]>2&&s.samples[1]>2&&s.samples[2]>2;},"Did not draw left/black/right/black");
        require(first.pixel[0]==0xff0000ff&&first.pixel[1]==0xff00ff00&&first.pixel[2]==0xff000000,"Physical backbuffer pixels do not match the eyes and black slots");
        require(first.pairs==1,"Unexpected producer pair count");
        auto slow=wait([&](const auto& s){return s.presents>=first.presents+16;},"Display cadence stopped while the game was idle");
        require(slow.pairs==1,"Idle display synthesized another game pair");
        std::puts("PASS: physical GPU pixels are left/red, right/green and black; output continues with one completed pair while the producer is idle");
        require(GetForegroundWindow()==before,"Runtime stole foreground focus");SendMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(45,62));SendMessageW(window,WM_KEYDOWN,'W',0);
        require(clicks==1&&keys==1,"Original game window did not receive input");require(sh->hwnd==reinterpret_cast<uintptr_t>(window),"Runtime presented to a different HWND");
        std::puts("PASS: original HWND and input handler retained; no focus change");
        VisionOutputTestStats pacedBefore{};read(&pacedBefore);
        for(int n=0;n<80;++n){check(chain->Present(0,0),"Continuously producing game Present");pump();}
        auto paced=wait([&](const auto& s){return s.acceptedPairs>=pacedBefore.acceptedPairs+80;},"Display did not consume every producer pair");
        require(paced.outOfOrderPairs==0,"Output skipped or reordered a completed pair");
        require(paced.presents-pacedBefore.presents>=156,"Producer ran without waiting for complete display cycles");
        std::printf("PASS: 80 producer frames paced across %llu physical presents\n",paced.presents-pacedBefore.presents);
        delayProducer(500);
        std::jthread blockedProducer([&]{chain->Present(0,0);});
        auto blocked=wait([](const auto& s){return s.producerStalls==1;},"Producer stall did not start");
        const auto until=GetTickCount64()+200;while(GetTickCount64()<until){pump();Sleep(2);}
        VisionOutputTestStats duringStall{};read(&duringStall);
        require(duringStall.pairs==blocked.pairs&&duringStall.presents>=blocked.presents+4,"Output blocked on the game's producer mutex");
        blockedProducer.join();
        std::puts("PASS: physical output continues while game Present holds its producer lock for 500 ms");
        // A real GPU queue wait distinguishes completed rendering from a CPU
        // frame-rate cap. The old logical Present returned immediately here,
        // allowing arbitrarily many unfinished game frames to accumulate.
        ComPtr<ID3D11Device> gateDevice;ComPtr<ID3D11DeviceContext> gateContext;
        check(D3D11CreateDevice(actual.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&gateDevice,nullptr,&gateContext),"Independent GPU gate device");
        ComPtr<ID3D11Device5> gate5,producer5;ComPtr<ID3D11DeviceContext4> gate4,producer4;
        check(gateDevice.As(&gate5),"Gate fences");check(device.As(&producer5),"Producer fences");check(gateContext.As(&gate4),"Gate context");check(context.As(&producer4),"Producer context");
        ComPtr<ID3D11Fence> gateFence,producerGate;check(gate5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&gateFence)),"GPU gate fence");
        HANDLE gateHandle=nullptr;check(gateFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&gateHandle),"Shared gate");
        auto gateResult=producer5->OpenSharedFence(gateHandle,IID_PPV_ARGS(&producerGate));CloseHandle(gateHandle);check(gateResult,"Import GPU gate");
        check(producer4->Wait(producerGate.Get(),1),"Hold the game's GPU queue");context->Flush();
        std::atomic<bool> gpuReturned{false};HRESULT gpuPresent=E_PENDING;
        VisionOutputTestStats gpuBefore{};read(&gpuBefore);
        std::jthread gpuProducer([&]{gpuPresent=chain->Present(0,0);gpuReturned=true;});
        const auto gpuUntil=GetTickCount64()+200;while(GetTickCount64()<gpuUntil){pump();Sleep(2);}
        VisionOutputTestStats gpuDuring{};read(&gpuDuring);const bool returnedEarly=gpuReturned;
        check(gate4->Signal(gateFence.Get(),1),"Release GPU gate");gateContext->Flush();gpuProducer.join();
        check(gpuPresent,"GPU-backpressured Present");require(!returnedEarly,"Logical Present returned before the GPU completed its frame");
        require(gpuDuring.presents>=gpuBefore.presents+4,"Pending game GPU work stopped independent display output");
        std::puts("PASS: unfinished game GPU work blocks new frames while display continues; Present completes after GPU release");
        wait([](const auto& s){return s.alignedRun>=16;},"Output never aligned to absolute refresh slots");
        for(UINT delay:{17u,53u,131u}){
            VisionOutputTestStats beforeStall{};read(&beforeStall);delayDisplay(delay);
            wait([&](const auto& s){return s.outputStalls>beforeStall.outputStalls;},"Display stall did not start");
            auto recovered=wait([](const auto& s){return s.alignedRun>=24;},"Eye phase did not recover after a dropped display refresh");
            require((sh->hookFlags&sync::HookRefreshSlots)!=0,"Host was not told the fixed refresh phase");
            require(recovered.misaligned-beforeStall.misaligned<=8,"Display stall caused prolonged eye-phase loss");
            std::printf("PASS: fixed eye phase recovered after %u ms display stall; mismatched scanouts=%llu\n",delay,recovered.misaligned-beforeStall.misaligned);
        }
        check(chain->ResizeBuffers(1,400,240,DXGI_FORMAT_R8G8B8A8_UNORM,0),"Logical resize");publish(400,240);
        wait([](const auto& s){return s.width==400&&s.height==240&&s.pixel[0]==0xff0000ff&&s.pixel[1]==0xff00ff00;},"Physical resize / replacement pair failed");
        std::puts("PASS: resize and producer resource replacement");
        VisionOutputTestStats beforeDisable{};read(&beforeDisable);sh->enabled=0;auto mono=wait([&](const auto& s){return s.samples[0]>beforeDisable.samples[0]+4;},"Host disable failed");
        for(int i=0;i<20;++i){pump();Sleep(5);}VisionOutputTestStats after{};read(&after);require(after.samples[1]==mono.samples[1]&&after.samples[2]==mono.samples[2],"Stereo slots continued after host disable");
        std::puts("PASS: host disable returns to mono");
        context->ClearState();chain.Reset();pair.Reset();context.Reset();device.Reset();DestroyWindow(window);
        heartbeat.request_stop();heartbeat.join();if(handle)UnmapViewOfFile(handle);if(pairMap)CloseHandle(pairMap);UnmapViewOfFile(sh);CloseHandle(mapping);
        std::filesystem::remove(ini);std::puts("PASS: clean shutdown; optical sync and actual game scenes are separate checks");return 0;
    }catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}
