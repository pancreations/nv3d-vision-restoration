#include "sources.h"
#include "emitter.h"
#include <SpoutDX.h>
#include <chrono>
#include <iostream>
#include <thread>
using namespace vision;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        auto displays=enumerateDisplays();require(!displays.empty(),"No GPU output available for integration test");
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(displays.front().adapterLuid,IID_PPV_ARGS(&adapter)),"Adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Device");
        spoutDX sender;require(sender.OpenDirectX11(device.Get()),"Sender device");std::string name="VisionIntegrationTest_"+std::to_string(GetCurrentProcessId());sender.SetSenderName(name.c_str());
        StereoSource source;SourceConfig config;config.kind=SourceKind::Spout;config.sender=name;config.encoding=Encoding::LinearScRGB;source.start(config,displays.front().adapterLuid);
        auto texture=[&](UINT width){D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=64;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,nullptr,&t),"Sender texture");ComPtr<ID3D11RenderTargetView> rt;check(device->CreateRenderTargetView(t.Get(),nullptr,&rt),"Sender view");float color[]{4,2,.5f,1};context->ClearRenderTargetView(rt.Get(),color);return t;};
        auto first=texture(128);
        auto receive=[&](ID3D11Texture2D* t,UINT width){auto until=qpc()+5;std::shared_ptr<StereoFrame> frame;while(qpc()<until){sender.SendTexture(t);std::this_thread::sleep_for(std::chrono::milliseconds(10));frame=source.latest();if(frame&&frame->width==width)return frame;}throw std::runtime_error("Spout receive timeout: "+source.status().message);};
        auto held=receive(first.Get(),128);auto heldId=held->pairId;require(held->encoding==Encoding::LinearScRGB,"HDR interpretation lost");
        ComPtr<ID3D11Texture2D> shared;check(device->OpenSharedResource(held->sharedHandle,IID_PPV_ARGS(&shared)),"Open source shared texture");ComPtr<IDXGIKeyedMutex> key;check(shared.As(&key),"Shared key");require(key->AcquireSync(0,100)==S_OK,"Consumer synchronization");
        D3D11_TEXTURE2D_DESC readDesc{};shared->GetDesc(&readDesc);require(readDesc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"HDR texture precision lost");readDesc.BindFlags=readDesc.MiscFlags=0;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&readDesc,nullptr,&staging),"Readback");context->CopyResource(staging.Get(),shared.Get());D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Readback map");require(static_cast<uint16_t*>(map.pData)[0]==0x4400,"HDR value 4.0 was clipped or changed");context->Unmap(staging.Get(),0);
        auto resized=texture(256);auto newer=receive(resized.Get(),256);require(held->pairId==heldId&&held->width==128,"Held pair was mutated by producer");require(newer->pairId>heldId,"Pair identifiers did not advance");check(key->ReleaseSync(0),"Consumer release");
        sender.ReleaseSender();std::this_thread::sleep_for(std::chrono::milliseconds(200));auto before=source.latest();require(before && before->width==256,"Source loss removed pair");std::this_thread::sleep_for(std::chrono::milliseconds(80));require(source.latest()==before,"Source loss discarded the last complete pair");
        source.stop();require(!source.latest(),"Stop retained published source");sender.CloseDirectX11();
        Emitter simulated;Settings settings;simulated.configure(settings);simulated.connect({}, {},true);auto until=qpc()+2;while(simulated.status().state!=EmitterState::Simulated && qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));require(simulated.status().state==EmitterState::Simulated,"Simulator startup");simulated.submit(Eye::Left,qpc()+.03);settings.phaseUs=500;simulated.configure(settings);until=qpc()+2;while(simulated.status().commands==0&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));require(simulated.status().commands>0,"Live timing edit preserves queued eye trigger");simulated.suspend();double stopped=qpc();simulated.disconnect();require(qpc()-stopped<1,"Simulator shutdown was not bounded");
        Emitter stale;stale.configure(Settings{});stale.connect({}, {},true);until=qpc()+2;
        while(stale.status().state!=EmitterState::Simulated&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(stale.status().state==EmitterState::Simulated,"Late-trigger simulator startup");
        stale.submit(Eye::Right,qpc()-.002);until=qpc()+2;
        while(stale.status().late==0&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(stale.status().late==1&&stale.status().commands==0,"Stale eye command must be dropped before reaching the emitter");stale.disconnect();
        std::cout<<"PASS: GPU shared ownership, complete pair retention, Spout resize/loss, FP16 HDR value preservation, simulated emitter trigger, stale trigger rejection and shutdown\n";
        CoUninitialize();return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';CoUninitialize();return 1;}
}
