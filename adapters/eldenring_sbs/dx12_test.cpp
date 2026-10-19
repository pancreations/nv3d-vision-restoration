// SPDX-License-Identifier: GPL-3.0-or-later
// A real DX12 swapchain + real ReShade test build. The scene is rendered twice
// from different origins. Near/far disparity must survive the production packer.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "stereo.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using vision::elden::Camera;
namespace {
void require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
void check(HRESULT h,const char* text){if(FAILED(h)){char s[200];sprintf_s(s,"%s: 0x%08lx",text,h);throw std::runtime_error(s);}}
const char scene[]=R"HLSL(
cbuffer Camera:register(b0){float cameraX;float width;float unused0;float unused1;};
struct V{float4 p:SV_POSITION;};
V vs(uint id:SV_VertexID){V v;float2 uv=float2((id<<1)&2,id&2);v.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 ps(V v):SV_TARGET {
    float direction=v.p.x/width*2-1;
    if(abs(cameraX+direction*1)<.2)return float4(1,0,0,1);
    if(abs(cameraX+direction*4-2.0)<.3)return float4(0,0,1,1);
    return float4(.1,.1,.1,1);
}
)HLSL";
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* image,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
}
struct Game {
    UINT width=640,height=480;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>,2> buffers;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
    explicit Game(HWND window){
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"factory");
        check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        DXGI_SWAP_CHAIN_DESC1 s{};s.Width=width;s.Height=height;s.Format=DXGI_FORMAT_R8G8B8A8_UNORM;s.SampleDesc.Count=1;s.BufferCount=2;s.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;s.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(queue.Get(),window,&s,nullptr,nullptr,&chain),"swapchain");check(chain.As(&swap),"swapchain3");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"RTV heap");
        acquire();check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");check(list->Close(),"close");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter.Constants.Num32BitValues=4;parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=1;rd.pParameters=&parameter;ComPtr<ID3DBlob> signature,error,vs,ps;
        check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error),"root serialization");
        check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
        check(D3DCompile(scene,sizeof(scene)-1,"scene",nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error),"scene VS");
        check(D3DCompile(scene,sizeof(scene)-1,"scene",nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error),"scene PS");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        auto& blend=pd.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask=UINT_MAX;pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=1;pd.RTVFormats[0]=s.Format;pd.SampleDesc.Count=1;
        check(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
    }
    ~Game(){wait();CloseHandle(event);}
    void wait(){check(queue->Signal(fence.Get(),++serial),"signal");if(fence->GetCompletedValue()<serial){check(fence->SetEventOnCompletion(serial,event),"event");require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU fence timeout");}}
    void acquire(){auto handle=heap->GetCPUDescriptorHandleForHeapStart();const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);for(UINT i=0;i<2;++i){check(swap->GetBuffer(i,IID_PPV_ARGS(&buffers[i])),"buffer");device->CreateRenderTargetView(buffers[i].Get(),nullptr,handle);handle.ptr+=stride;}}
    void begin(){wait();check(allocator->Reset(),"allocator reset");check(list->Reset(allocator.Get(),pipeline.Get()),"list reset");}
    void execute(){check(list->Close(),"list close");ID3D12CommandList* command=list.Get();queue->ExecuteCommandLists(1,&command);}
    UINT frame(float cameraX){
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        begin();const auto index=swap->GetCurrentBackBufferIndex();barrier(list.Get(),buffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target=heap->GetCPUDescriptorHandleForHeapStart();target.ptr+=size_t(index)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);list->OMSetRenderTargets(1,&target,FALSE,nullptr);
        D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT rect{0,0,LONG(width),LONG(height)};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&rect);list->SetGraphicsRootSignature(root.Get());
        const float values[]{cameraX,float(width),0,0};list->SetGraphicsRoot32BitConstants(0,4,values,0);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        barrier(list.Get(),buffers[index].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);execute();check(swap->Present(1,0),"Present");wait();return index;
    }
    std::vector<unsigned char> read(UINT index){
        const auto desc=buffers[index]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 size=0;device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&size);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=size;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
        begin();barrier(list.Get(),buffers[index].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=buffers[index].Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=footprint;list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
        barrier(list.Get(),buffers[index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);execute();wait();
        void* data=nullptr;check(readback->Map(0,nullptr,&data),"map");std::vector<unsigned char> result(size_t(width)*height*4);
        for(UINT y=0;y<height;++y)memcpy(result.data()+size_t(y)*width*4,static_cast<unsigned char*>(data)+size_t(y)*footprint.Footprint.RowPitch,size_t(width)*4);
        D3D12_RANGE none{};readback->Unmap(0,&none);return result;
    }
    void resize(){wait();for(auto& buffer:buffers)buffer.Reset();width=800;height=500;check(swap->ResizeBuffers(2,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,0),"resize");acquire();}
};
double center(const std::vector<unsigned char>& image,UINT width,UINT height,unsigned eye,unsigned channel){double sum=0,count=0;for(UINT x=0;x<width/2;++x){auto* pixel=&image[(size_t(height-20)*width+x+eye*(width/2))*4];if(pixel[channel]>240&&pixel[1]<10){sum+=x+.5;count+=1;}}require(count>5,"geometry stripe missing");return sum/count;}
void verify(Game& game,const std::vector<unsigned char>& image,bool reverse){
    const double nearDisparity=center(image,game.width,game.height,0,0)-center(image,game.width,game.height,1,0);
    const double farDisparity=center(image,game.width,game.height,0,2)-center(image,game.width,game.height,1,2);
    printf("DX12 %ux%u: near disparity %.2f px, far disparity %.2f px\n",game.width,game.height,nearDisparity,farDisparity);
    const double sign=reverse?-1:1;
    require(std::abs(nearDisparity-sign*double(game.width)*.05)<2,"near two-camera disparity incorrect");
    require(std::abs(farDisparity-sign*double(game.width)*.0125)<2,"far two-camera disparity incorrect");
}
}
int main(int argc,char**){try{
    const bool reverse=argc>1;
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionEldenSBSRegression";RegisterClassW(&wc);
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Elden Ring SBS renderer test",WS_OVERLAPPEDWINDOW,30,30,660,520,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"window");ShowWindow(window,SW_SHOWNOACTIVATE);
    {Game game(window);using CameraFn=bool(*)(Camera*);auto module=GetModuleHandleW(L"VisionEldenSBSTest.addon64");require(module!=nullptr,"ReShade test add-on did not load");auto update=reinterpret_cast<CameraFn>(GetProcAddress(module,"VisionEldenTestCamera"));require(update!=nullptr,"test camera interface");
        UINT index=0;for(auto end=GetTickCount64()+6500;GetTickCount64()<end;){Camera camera;require(update(&camera),"camera update");index=game.frame(camera.basis[3][0]);}
        // Always end with a right view so the last completed pair is available.
        for(int i=0;i<4;++i){Camera camera;require(update(&camera),"camera update");index=game.frame(camera.basis[3][0]);}
        verify(game,game.read(index),reverse);
        // A repeated frame with no new camera stamp must become mono SBS.
        index=game.frame(0);auto mono=game.read(index);require(std::abs(center(mono,game.width,game.height,0,0)-center(mono,game.width,game.height,1,0))<1,"duplicate camera must not invent a stereo pair");
        game.resize();for(int i=0;i<6;++i){Camera camera;require(update(&camera),"camera after resize");index=game.frame(camera.basis[3][0]);}verify(game,game.read(index),reverse);
    }DestroyWindow(window);puts("PASS: real DX12 add-on renders distinct camera views, preserves near/far disparity, rejects stale camera stamps, and recovers after resize.");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
