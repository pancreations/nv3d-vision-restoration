// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises an unmodified Geo11 binary in an isolated directory. No screen
// capture, ReShade, emitter, game files or display-mode changes are involved.
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
using CreateDevice = decltype(&D3D11CreateDevice);
using CreateDeviceAndSwapChain = decltype(&D3D11CreateDeviceAndSwapChain);
void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { char message[256]; sprintf_s(message,"%s: 0x%08lx",what,hr); throw std::runtime_error(message); }
}
ComPtr<ID3DBlob> compile(const char* source, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, error;
    auto hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,0,0,&code,&error);
    if (FAILED(hr) && error) std::printf("%s\n",static_cast<char*>(error->GetBufferPointer()));
    check(hr,"D3DCompile"); return code;
}
// Ordinary perspective geometry, deliberately using a vertex buffer and matrix
// constant so the same shader analysis used for games sees a conventional draw.
constexpr char shader[]=R"(
cbuffer Camera : register(b0) { row_major float4x4 projection; };
struct V { float4 position: SV_Position; float3 color: COLOR; };
V vs(float3 position:POSITION,float3 color:COLOR) {
    V o; o.position=mul(float4(position,1),projection); o.color=color; return o;
}
float4 ps(V i):SV_Target { return float4(i.color,1); }
)";
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    std::jthread watchdog([](std::stop_token stop){
        for(int i=0;i<450&&!stop.stop_requested();++i) Sleep(100);
        if(!stop.stop_requested()){std::puts("FAIL: probe timed out");TerminateProcess(GetCurrentProcess(),10);}
    });
    try {
        if(argc<2||argc>4) throw std::runtime_error("Usage: vision_geo11_probe <isolated Geo11 directory> [--adapter] [--factory]");
        auto directory=std::filesystem::absolute(argv[1]);
        bool adapter=false,separateFactory=false;
        for(int i=2;i<argc;++i){if(std::wstring(argv[i])==L"--adapter")adapter=true;else if(std::wstring(argv[i])==L"--factory")separateFactory=true;else throw std::runtime_error("Unknown probe option");}
        const std::wstring mappingName=adapter?L"Local\\KatangaMappedFile.Vision."+std::to_wstring(GetCurrentProcessId()):L"Local\\KatangaMappedFile";
        std::filesystem::current_path(directory);
        // Katanga's upstream protocol is one session-wide 32-bit DXGI handle.
        // Refuse an existing publisher rather than accidentally test another game.
        HANDLE existing=OpenFileMappingW(FILE_MAP_READ,FALSE,mappingName.c_str());
        if(existing){CloseHandle(existing);throw std::runtime_error("Another Katanga publisher is active");}
        HMODULE geo=LoadLibraryExW((directory/L"d3d11.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
        if(!geo) throw std::runtime_error("Geo11 load failed; Win32 error="+std::to_string(GetLastError()));
        // Import the wrapper in the ordinary game startup order. Loading a
        // stereo backend after an unrelated D3D11 device exists is not equivalent.
        auto createGame=&D3D11CreateDeviceAndSwapChain;
        wchar_t loaded[MAX_PATH]; GetModuleFileNameW(geo,loaded,MAX_PATH); std::wprintf(L"Backend: %ls\n",loaded);
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"VisionGeo11Probe";
        RegisterClassW(&wc);
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Vision stereo backend probe",WS_OVERLAPPEDWINDOW,30,30,640,360,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window) throw std::runtime_error("CreateWindow failed");
        // A hidden window is sufficient: only the backend eye resource is tested.
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=640;desc.BufferDesc.Height=360;
        desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;
        desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
        if(separateFactory){
            ComPtr<IDXGIFactory2> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory created before the game device");
            check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Separate Geo11 device");
            DXGI_SWAP_CHAIN_DESC1 d{};d.Width=640;d.Height=360;d.Format=desc.BufferDesc.Format;d.SampleDesc.Count=1;d.BufferCount=2;d.BufferUsage=desc.BufferUsage;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> created;check(factory->CreateSwapChainForHwnd(device.Get(),window,&d,nullptr,nullptr,&created),"Separate game factory swapchain");check(created.As(&chain),"Base swapchain interface");
        }else check(createGame(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&device,nullptr,&context),"Geo11 device/swapchain");
        ComPtr<ID3D11Texture2D> back;check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"game backbuffer");
        ComPtr<ID3D11RenderTargetView> target;check(device->CreateRenderTargetView(back.Get(),nullptr,&target),"game RTV");
        D3D11_TEXTURE2D_DESC depthDesc{};depthDesc.Width=640;depthDesc.Height=360;
        depthDesc.MipLevels=1;depthDesc.ArraySize=1;depthDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDesc.SampleDesc.Count=1;depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;check(device->CreateTexture2D(&depthDesc,nullptr,&depth),"game depth");
        ComPtr<ID3D11DepthStencilView> depthView;check(device->CreateDepthStencilView(depth.Get(),nullptr,&depthView),"game DSV");
        auto vsCode=compile(shader,"vs","vs_5_0"),psCode=compile(shader,"ps","ps_5_0");
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"VS");
        check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"PS");
        D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
        ComPtr<ID3D11InputLayout> input;check(device->CreateInputLayout(layout,2,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&input),"input layout");
        const float vertices[]={-2,-1,5,1,0,0, 0,1,5,0,1,0, 2,-1,5,0,0,1};
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(vertices);bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{vertices};ComPtr<ID3D11Buffer> vb;check(device->CreateBuffer(&bd,&init,&vb),"VB");
        const float matrix[]={1,0,0,0, 0,1.77778f,0,0, 0,0,1.001f,1, 0,0,-.1001f,0};
        bd.ByteWidth=sizeof(matrix);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;init.pSysMem=matrix;
        ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,&init,&cb),"CB");
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        ComPtr<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rs,&raster),"raster state");
        for(int frame=0;frame<90;++frame){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            const float clear[]={.03f,.03f,.03f,1};context->ClearRenderTargetView(target.Get(),clear);
            context->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
            auto* rt=target.Get();context->OMSetRenderTargets(1,&rt,depthView.Get());
            D3D11_VIEWPORT vp{0,0,640,360,0,1};context->RSSetViewports(1,&vp);context->RSSetState(raster.Get());
            context->IASetInputLayout(input.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            auto* v=vb.Get();UINT stride=6*sizeof(float),offset=0;context->IASetVertexBuffers(0,1,&v,&stride,&offset);
            auto* c=cb.Get();context->VSSetConstantBuffers(0,1,&c);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
            context->Draw(3,0);check(chain->Present(0,0),"game Present");context->Flush();Sleep(10);
        }
        HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,mappingName.c_str());
        if(!mapping)throw std::runtime_error("Geo11 did not publish its Katanga mapping");
        auto* view=static_cast<const volatile uint32_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,4));
        if(!view){CloseHandle(mapping);throw std::runtime_error("MapViewOfFile failed");}
        const uint32_t handle=*view;UnmapViewOfFile(const_cast<uint32_t*>(view));CloseHandle(mapping);
        if(!handle)throw std::runtime_error("Geo11 published a null eye-resource handle");
        // Find the already loaded system module without passing a LoadLibrary
        // call through the backend's intentional DLL redirection hooks.
        HMODULE modules[1024]{},native=nullptr;DWORD needed=0;
        if(!K32EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&needed)||needed>sizeof(modules))
            throw std::runtime_error("Module enumeration failed");
        wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);
        auto systemDll=(std::filesystem::path(system)/L"d3d11.dll").wstring();
        for(DWORD i=0;i<needed/sizeof(HMODULE);++i){wchar_t path[MAX_PATH];
            if(GetModuleFileNameW(modules[i],path,MAX_PATH)&&_wcsicmp(path,systemDll.c_str())==0)native=modules[i];
        }
        if(!native||native==geo)throw std::runtime_error("Separate native D3D11 module not found");
        auto create=reinterpret_cast<CreateDevice>(GetProcAddress(native,"D3D11CreateDevice"));
        ComPtr<ID3D11Device> reader;ComPtr<ID3D11DeviceContext> readContext;
        check(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&reader,nullptr,&readContext),"reader device");
        ComPtr<ID3D11Texture2D> pair;check(reader->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(handle)),IID_PPV_ARGS(&pair)),"open Geo11 eyes");
        D3D11_TEXTURE2D_DESC pd{};pair->GetDesc(&pd);
        std::printf("Geo11 eye texture: %ux%u format=%u array=%u samples=%u misc=%u\n",pd.Width,pd.Height,pd.Format,pd.ArraySize,pd.SampleDesc.Count,pd.MiscFlags);
        if(pd.Width!=1280||pd.Height!=360||pd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||pd.ArraySize!=1||pd.SampleDesc.Count!=1)
            throw std::runtime_error("Unexpected eye layout; refusing to assume a full-resolution pair");
        auto sd=pd;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;sd.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;check(reader->CreateTexture2D(&sd,nullptr,&staging),"staging");
        readContext->CopyResource(staging.Get(),pair.Get());D3D11_MAPPED_SUBRESOURCE pixels{};
        check(readContext->Map(staging.Get(),0,D3D11_MAP_READ,0,&pixels),"read eyes");
        size_t differences=0,coloredLeft=0,coloredRight=0;
        std::ofstream ppm(directory/L"geo11-eyes.ppm",std::ios::binary);ppm<<"P6\n1280 360\n255\n";
        for(UINT y=0;y<360;++y){auto* row=static_cast<uint8_t*>(pixels.pData)+y*pixels.RowPitch;
            for(UINT x=0;x<1280;++x)ppm.write(reinterpret_cast<char*>(row+x*4),3);
            for(UINT x=0;x<640;++x){auto* l=row+x*4;auto* r=row+(x+640)*4;
                bool different=false;for(int c=0;c<3;++c)different|=std::abs(int(l[c])-int(r[c]))>3;
                differences+=different;coloredLeft+=std::max({l[0],l[1],l[2]})>40;coloredRight+=std::max({r[0],r[1],r[2]})>40;
            }
        }
        readContext->Unmap(staging.Get(),0);
        std::printf("Eye evidence: differing=%zu left-content=%zu right-content=%zu\n",differences,coloredLeft,coloredRight);
        if(differences<100||coloredLeft<100||coloredRight<100) throw std::runtime_error("No distinct geometry stereo proven");
        context->ClearState();context->Flush();
        staging.Reset();pair.Reset();readContext.Reset();reader.Reset();
        raster.Reset();cb.Reset();vb.Reset();input.Reset();ps.Reset();vs.Reset();depthView.Reset();depth.Reset();target.Reset();back.Reset();
        std::puts("Releasing game graphics objects...");chain.Reset();context.Reset();device.Reset();
        DestroyWindow(window);
        std::puts("PASS: unmodified Geo11 generated two distinct full-resolution eyes from an ordinary DX11 draw.");
        std::puts("Scope: backend resource access only; game compatibility and shutter timing remain untested.");
        return 0;
    }catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}
