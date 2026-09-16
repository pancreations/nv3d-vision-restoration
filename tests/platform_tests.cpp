#include "sources.h"
#include "emitter.h"
#include "renderer.h"
#include "window_worker.h"
#include <fstream>
#include <chrono>
#include <iostream>
#include <thread>
using namespace vision;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        {
            WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WorkerStopMessageTest";
            wc.lpfnWndProc=[](HWND w,UINT m,WPARAM a,LPARAM b)->LRESULT{return m==WM_APP?42:DefWindowProcW(w,m,a,b);};
            require(RegisterClassW(&wc)!=0,"Worker stop test class");
            HWND window=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,32,32,nullptr,nullptr,wc.hInstance,nullptr);
            require(window!=nullptr,"Worker stop test window");
            PostMessageW(window,WM_APP+1,0,0);
            DWORD_PTR response=0;LRESULT sent=0;
            std::jthread worker([&]{sent=SendMessageTimeoutW(window,WM_APP,0,0,SMTO_ABORTIFHUNG,1000,&response);});
            stopWindowWorker(worker);
            MSG pending{};const bool inputPreserved=PeekMessageW(&pending,window,WM_APP+1,WM_APP+1,PM_REMOVE)!=0;
            DestroyWindow(window);UnregisterClassW(wc.lpszClassName,wc.hInstance);
            require(sent&&response==42,"Stopping a renderer blocked its synchronous window message");
            require(inputPreserved,"Stopping a renderer consumed queued UI input");
            std::cout<<"PASS: worker shutdown services sent window messages and preserves queued input\n";
        }
        auto displays=enumerateDisplays();require(!displays.empty(),"No GPU output available for integration test");
        {
            // Stop during first-time renderer startup used to join a 10+ second
            // shader compilation, hanging fullscreen/preview switches in the UI.
            HWND window=CreateWindowExW(0,L"STATIC",L"Presenter startup test",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(window!=nullptr,"Presenter test window creation");
            struct WindowCleanup { HWND handle;~WindowCleanup(){DestroyWindow(handle);} } cleanup{window};
            StereoSource source;Emitter emitter;Presenter presenter(source,emitter);Settings settings;
            const double start=qpc();
            for(int i=0;i<3;++i){
                presenter.start(window,displays.front(),settings,true,2);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                presenter.stop();
                require(!presenter.status().running,"Presenter remained running after stop");
            }
            const double elapsed=qpc()-start;
            std::cout<<"Three renderer starts/stops: "<<elapsed<<" s\n";
            require(elapsed<3,"Output transitions blocked on renderer initialization");
        }
        // Changing HDR must resize the actual output buffer, without replacing
        // the window or D3D device. This catches a checkbox-only HDR update.
        {
            const auto& display=displays.front();
            HWND window=CreateWindowExW(0,L"STATIC",L"HDR format test",WS_POPUP,display.rect.left,display.rect.top,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            require(window!=nullptr,"HDR test window creation");
            struct WindowCleanup { HWND handle;~WindowCleanup(){DestroyWindow(handle);} } cleanup{window};
            Surface surface;surface.create(window,&display.adapterLuid,false);
            auto* originalDevice=surface.device.Get();auto* originalSwap=surface.swap.Get();
            for(bool hdr:{true,false,true,false}){
                surface.setHdr(hdr);DXGI_SWAP_CHAIN_DESC1 desc{};check(surface.swap->GetDesc1(&desc),"Read output format");
                require(surface.hdr==hdr&&desc.Format==(hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM),"HDR toggle must change output precision");
                require(surface.device.Get()==originalDevice&&surface.swap.Get()==originalSwap&&IsWindow(window),"HDR toggle replaced the output");
                surface.bind();float pixel[]{4,2,.5f,1};surface.context->ClearRenderTargetView(surface.target.Get(),pixel);
            }
        }
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(displays.front().adapterLuid,IID_PPV_ARGS(&adapter)),"Adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Device");
        // Exercise the retained image-source publisher directly. The removed Spout
        // transport is no longer a build dependency or a prerequisite for GPU tests.
        struct Fixtures {
            std::filesystem::path paths[2];
            ~Fixtures(){for(const auto& p:paths){std::error_code ec;if(!p.empty())std::filesystem::remove(p,ec);}}
        } fixtures;
        auto fixture=[&](int index,LONG width){
            wchar_t directory[MAX_PATH]{},path[MAX_PATH]{};
            require(GetTempPathW(MAX_PATH,directory)>0&&GetTempFileNameW(directory,L"vrs",0,path)!=0,"Image fixture path");
            fixtures.paths[index]=path;
            BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
            info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-64;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=DWORD(width*64*4);
            file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+info.biSizeImage;
            std::vector<uint32_t> pixels(size_t(width)*64,0xff4080c0);
            std::ofstream out(fixtures.paths[index],std::ios::binary);
            out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
            out.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()*4));
            require(bool(out),"Write stereo image fixture");
        };
        fixture(0,128);fixture(1,256);
        StereoSource source;SourceConfig config;config.kind=SourceKind::Image;
        auto receive=[&](int index,UINT width){
            config.file=fixtures.paths[index];source.start(config,displays.front().adapterLuid);
            auto until=qpc()+5;
            while(qpc()<until){auto frame=source.latest();if(frame&&frame->width==width)return frame;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            throw std::runtime_error("Image source timeout: "+source.status().message);
        };
        auto held=receive(0,128);const auto heldId=held->pairId;
        require(held->encoding==Encoding::SRGB&&held->packing==Packing::SideBySide,"Image stereo/color metadata lost");
        auto newer=receive(1,256);
        require(held->pairId==heldId&&held->width==128&&newer!=held,"Source restart mutated a retained pair");
        source.stop();require(!source.latest(),"Stop retained published source");
        // The consumer's old pair and GPU handle must remain valid after a source
        // resize/restart and stop; the next producer must not recycle its pixels.
        ComPtr<ID3D11Texture2D> shared;check(device->OpenSharedResource(held->sharedHandle,IID_PPV_ARGS(&shared)),"Open retained source texture");
        ComPtr<IDXGIKeyedMutex> key;check(shared.As(&key),"Shared key");require(key->AcquireSync(0,100)==S_OK,"Consumer synchronization");
        D3D11_TEXTURE2D_DESC readDesc{};shared->GetDesc(&readDesc);
        require(readDesc.Width==128&&readDesc.Height==64&&readDesc.Format==DXGI_FORMAT_B8G8R8A8_UNORM,"Retained image dimensions/format changed");
        readDesc.BindFlags=readDesc.MiscFlags=0;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&readDesc,nullptr,&staging),"Readback");
        context->CopyResource(staging.Get(),shared.Get());D3D11_MAPPED_SUBRESOURCE map{};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Readback map");
        const auto* pixel=static_cast<uint8_t*>(map.pData);const bool intact=pixel[0]==0xc0&&pixel[1]==0x80&&pixel[2]==0x40;
        context->Unmap(staging.Get(),0);check(key->ReleaseSync(0),"Consumer release");
        require(intact,"Retained pair pixels changed after source restart");
        Emitter simulated;Settings settings;simulated.configure(settings);simulated.connect({}, {},true);auto until=qpc()+2;while(simulated.status().state!=EmitterState::Simulated && qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));require(simulated.status().state==EmitterState::Simulated,"Simulator startup");simulated.submit(Eye::Left,qpc()+.03);settings.phaseUs=500;simulated.configure(settings);until=qpc()+2;while(simulated.status().commands==0&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));require(simulated.status().commands>0,"Live timing edit preserves queued eye trigger");simulated.suspend();double stopped=qpc();simulated.disconnect();require(qpc()-stopped<1,"Simulator shutdown was not bounded");
        Emitter stale;stale.configure(Settings{});stale.connect({}, {},true);until=qpc()+2;
        while(stale.status().state!=EmitterState::Simulated&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(stale.status().state==EmitterState::Simulated,"Late-trigger simulator startup");
        stale.submit(Eye::Right,qpc()-.002);until=qpc()+2;
        while(stale.status().late==0&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(stale.status().late==1&&stale.status().commands==0,"Stale eye command must be dropped before reaching the emitter");stale.disconnect();
        std::cout<<"PASS: image-source GPU sharing, pair retention across restart/stop, HDR output format changes, simulated emitter trigger, stale trigger rejection and shutdown\n";
        CoUninitialize();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';CoUninitialize();return 1;}
}
