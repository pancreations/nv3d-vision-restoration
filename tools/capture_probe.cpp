// Real ReShade + capture add-on smoke test. No emitter or visible game window.
#include "direct_eyes.h"
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#pragma comment(lib,"d3dcompiler.lib")
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
using Microsoft::WRL::ComPtr;
namespace direct=vision::direct;
void check(HRESULT hr,const char* what){if(FAILED(hr)){char text[160];sprintf_s(text,"%s: %08lx",what,hr);throw std::runtime_error(text);}}
int main(int argc,char** argv){try{
    const bool dx12=argc>1&&std::string(argv[1])=="d3d12";
    const bool sequential=argc>2&&std::string(argv[2])=="sequential";
    const bool katanga=argc>2&&std::string(argv[2])=="katanga";
    const bool geo=argc>2&&std::string(argv[2])=="geo";
    const bool rightFirst=argc>3&&std::string(argv[3])=="right-first";
    const UINT eyeWidth=sequential||geo?64u:32u;
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Vision capture offscreen probe",WS_POPUP,0,0,64,32,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)throw std::runtime_error("Probe window");
    ComPtr<ID3D11Device> readerDevice;ComPtr<ID3D11DeviceContext> readerContext;
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12Device> device12;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12Fence> fence;
    if(dx12){check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)),"DX12 device");D3D12_COMMAND_QUEUE_DESC q{};check(device12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");check(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");check(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Commands");check(commands->Close(),"Close commands");D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=1;check(device12->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)),"RTV heap");check(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");}
    else check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Game device");
    struct Katanga {HANDLE map=nullptr;uint32_t* view=nullptr;ComPtr<ID3D11Texture2D> texture;~Katanga(){if(view)UnmapViewOfFile(view);if(map)CloseHandle(map);}} producer;
    if(katanga){
        if(dx12)throw std::runtime_error("Katanga fixture requires D3D11");
        producer.map=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4,L"Local\\KatangaMappedFile");
        if(!producer.map||GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Katanga mapping already in use; existing producer untouched");
        producer.view=static_cast<uint32_t*>(MapViewOfFile(producer.map,FILE_MAP_ALL_ACCESS,0,0,4));if(!producer.view)throw std::runtime_error("Katanga mapping view");
        D3D11_TEXTURE2D_DESC d{};d.Width=64;d.Height=32;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
        check(device->CreateTexture2D(&d,nullptr,&producer.texture),"Katanga texture");ComPtr<IDXGIResource> shared;check(producer.texture.As(&shared),"Katanga shared resource");HANDLE handle;check(shared->GetSharedHandle(&handle),"Katanga shared handle");*producer.view=uint32_t(uintptr_t(handle));
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=32;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> chain;
    if(dx12||geo){ComPtr<IDXGIFactory2> modern;check(factory.As(&modern),"DX12 factory");ComPtr<IDXGISwapChain1> next;check(modern->CreateSwapChainForHwnd(dx12?static_cast<IUnknown*>(queue.Get()):static_cast<IUnknown*>(device.Get()),window,&desc,nullptr,nullptr,&next),"DX12 chain");next.As(&chain);}
    else{DXGI_SWAP_CHAIN_DESC old{};old.BufferDesc.Width=64;old.BufferDesc.Height=32;old.BufferDesc.Format=desc.Format;old.SampleDesc.Count=1;old.BufferCount=2;old.BufferUsage=desc.BufferUsage;old.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;old.OutputWindow=window;old.Windowed=TRUE;check(factory->CreateSwapChain(device.Get(),&old,&chain),"Game chain");}
    wchar_t systemDir[MAX_PATH]{};GetSystemDirectoryW(systemDir,MAX_PATH);
    const auto systemModule=GetModuleHandleW((std::wstring(systemDir)+L"\\d3d11.dll").c_str());
    const auto createReader=systemModule?reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(systemModule,"D3D11CreateDevice")):&D3D11CreateDevice;
    check(createReader(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&readerDevice,nullptr,&readerContext),"Reader device");
    ComPtr<ID3D11VertexShader> geoVS;ComPtr<ID3D11PixelShader> geoPS;
    if(geo){const char shader[]= "float4 vs(uint id:SV_VertexID):SV_Position {return float4(id==1?3:-1,id==2?3:-1,0.5,1);} float4 ps():SV_Target{return float4(1,0,0,1);}";
        ComPtr<ID3DBlob> vs,ps;check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,nullptr),"Geo VS compile");check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,nullptr),"Geo PS compile");
        check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&geoVS),"Geo VS");check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&geoPS),"Geo PS");}
    direct::Reader reader;check(reader.open(readerDevice.Get(),GetCurrentProcessId()),"Add-on provider registration");
    std::atomic<unsigned> received=0;std::string error;
    std::jthread consumer([&](std::stop_token stop){try{while(!stop.stop_requested()){
        if(received==1)Sleep(200);HRESULT hr=reader.acquire();if(hr==S_FALSE){Sleep(1);continue;}check(hr,"Consumer acquire");if(reader.pairId()!=uint64_t(received)+1)throw std::runtime_error("Eye pair skipped or reordered");
        D3D11_TEXTURE2D_DESC d{};reader.texture()->GetDesc(&d);if(d.Width!=eyeWidth||d.Height!=32||d.ArraySize!=2)throw std::runtime_error("Capture dimensions");
        d.MiscFlags=d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;check(readerDevice->CreateTexture2D(&d,nullptr,&staging),"Readback");readerContext->CopyResource(staging.Get(),reader.texture());
        for(UINT eye=0;eye<2;eye++){D3D11_MAPPED_SUBRESOURCE m{};check(readerContext->Map(staging.Get(),eye,D3D11_MAP_READ,0,&m),"Readback map");bool valid=true;for(UINT y=0;y<32;y++)for(UINT x=0;x<eyeWidth;x++)valid&=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData)+y*m.RowPitch)[x]==(eye&&!geo?0xff00ff00u:0xff0000ffu);readerContext->Unmap(staging.Get(),eye);if(!valid)throw std::runtime_error("Capture eye pixels");}
        reader.release();++received;
    }}catch(const std::exception& e){error=e.what();}});
    ComPtr<IDXGISwapChain3> chain3;if(dx12)check(chain.As(&chain3),"Chain index");
    for(UINT frame=1;frame<=(sequential?8u:4u);frame++){
        if(dx12){ComPtr<ID3D12Resource> buffer;check(chain->GetBuffer(chain3->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"DX12 backbuffer");check(allocator->Reset(),"Reset allocator");check(commands->Reset(allocator.Get(),nullptr),"Reset commands");auto rtv=heap->GetCPUDescriptorHandleForHeapStart();device12->CreateRenderTargetView(buffer.Get(),nullptr,rtv);D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};commands->ResourceBarrier(1,&barrier);const float red[]{1,0,0,1},green[]{0,1,0,1};D3D12_RECT left{0,0,32,32},right{32,0,64,32};if(sequential)commands->ClearRenderTargetView(rtv,((frame%2!=0)^rightFirst)?red:green,0,nullptr);else{commands->ClearRenderTargetView(rtv,rightFirst?green:red,1,&left);commands->ClearRenderTargetView(rtv,rightFirst?red:green,1,&right);}std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);check(commands->Close(),"Close frame");ID3D12CommandList* list=commands.Get();queue->ExecuteCommandLists(1,&list);check(queue->Signal(fence.Get(),frame),"Frame signal");while(fence->GetCompletedValue()<frame)Sleep(1);}
        else{ComPtr<ID3D11Texture2D> buffer;check(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Backbuffer");uint32_t pixels[64*32];for(unsigned y=0;y<32;y++)for(unsigned x=0;x<64;x++)pixels[y*64+x]=((geo|| (sequential?frame%2!=0:x<32))^rightFirst)?0xff0000ffu:0xff00ff00u;context->UpdateSubresource(buffer.Get(),0,nullptr,pixels,64*4,0);if(geo){ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view),"Geo RTV");auto* rt=view.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,64,32,0,1};context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(geoVS.Get(),nullptr,0);context->PSSetShader(geoPS.Get(),nullptr,0);context->Draw(3,0);}if(katanga)context->UpdateSubresource(producer.texture.Get(),0,nullptr,pixels,64*4,0);}
        check(chain->Present(0,DXGI_PRESENT_TEST),"Test present");check(chain->Present(0,0),"Game present");Sleep(10);
    }
    const auto until=GetTickCount64()+3000;while(received<4&&GetTickCount64()<until)Sleep(1);consumer.request_stop();consumer.join();
    if(!error.empty())throw std::runtime_error(error);if(received!=4)throw std::runtime_error("Add-on did not transfer every pair");
    chain3.Reset();chain.Reset();DestroyWindow(window);std::printf("PASS: %s real ReShade add-on transferred four complete %s stereo pairs\n",dx12?"D3D12":"D3D11",sequential?"frame-sequential":katanga?"Katanga":"packed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
