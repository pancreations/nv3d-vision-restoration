// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-screen conversion (the SpaceWalker / Reality Hub idea): a display is captured, a depth
// network estimates every pixel's depth, and both eyes are resampled from the picture with that
// depth. The network runs in VisionDepth.exe, a separate process at a low GPU scheduling class,
// so its work never queues in front of the stereo presenter (the black flashing of 2026-09-14
// came from exactly that kind of contention). This thread captures, exchanges pictures and depth
// with the helper through shared memory, and renders the side-by-side pair the presenter shows.
#include "sources.h"
#include "screen_stereo.h"
#include "screen_depth.h"
#include "screen_depth_shader.h"
#include "depth_channel.h"
#include <d3dcompiler.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

namespace vision {
namespace {
ComPtr<ID3DBlob> compileScreenShader(const char* entry,const char* profile){
    ComPtr<ID3DBlob> code,error;HRESULT h=D3DCompile(screenDepthShader,strlen(screenDepthShader),"screen.hlsl",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
    if(FAILED(h))throw std::runtime_error(error?static_cast<char*>(error->GetBufferPointer()):"Screen shader failed");return code;
}
uint32_t load32(uint32_t& v){return std::atomic_ref<uint32_t>(v).load(std::memory_order_acquire);}
void store32(uint32_t& v,uint32_t x){std::atomic_ref<uint32_t>(v).store(x,std::memory_order_release);}
// The depth helper: its process, its channel, and the exchange of pictures and depth maps.
class DepthService {
    HANDLE mapping_=nullptr,pictureEvent_=nullptr,depthEvent_=nullptr;uint8_t* view_=nullptr;depth::Header* header_=nullptr;PROCESS_INFORMATION process_{};uint32_t lastDepth_=0;std::string error_;
public:
    ~DepthService(){stop();}
    HANDLE depthEvent()const{return depthEvent_;}
    const std::string& error()const{return error_;}
    bool started()const{return header_!=nullptr;}
    bool start(const std::filesystem::path& helper,const std::filesystem::path& model,const LUID& adapter,unsigned sourceWidth,unsigned sourceHeight,unsigned maxNetSide,const std::filesystem::path& log){
        stop();error_.clear();std::error_code ec;
        if(helper.empty()||!std::filesystem::exists(helper,ec)){error_="VisionDepth.exe is not beside the app: run tools/Get-Dependencies.ps1 and build.ps1 to build the depth helper.";return false;}
        if(model.empty()||!std::filesystem::exists(model,ec)){error_="No depth model found: tools/Get-Dependencies.ps1 downloads Depth Anything V2 Small into models/, or choose a .onnx file.";return false;}
        const std::wstring suffix=std::to_wstring(GetCurrentProcessId());
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,DWORD(depth::channelBytes>>32),DWORD(depth::channelBytes&0xffffffffu),(std::wstring(depth::mappingPrefix)+suffix).c_str());
        if(!mapping_){error_="Cannot create the depth channel.";return false;}
        view_=static_cast<uint8_t*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,0));if(!view_){error_="Cannot map the depth channel.";stop();return false;}
        header_=new(view_) depth::Header{};header_->magic=depth::magic;header_->version=depth::version;header_->hostPid=GetCurrentProcessId();header_->sourceWidth=sourceWidth;header_->sourceHeight=sourceHeight;header_->maxNetSide=maxNetSide;
        pictureEvent_=CreateEventW(nullptr,FALSE,FALSE,(std::wstring(depth::pictureEventPrefix)+suffix).c_str());depthEvent_=CreateEventW(nullptr,FALSE,FALSE,(std::wstring(depth::depthEventPrefix)+suffix).c_str());
        if(!pictureEvent_||!depthEvent_){error_="Cannot create the depth channel events.";stop();return false;}
        std::wstring command=L"\""+helper.wstring()+L"\" --host "+suffix+L" --model \""+model.wstring()+L"\" --adapter "+std::to_wstring(adapter.HighPart)+L":"+std::to_wstring(adapter.LowPart);
        if(!log.empty())command+=L" --log \""+log.wstring()+L"\"";
        STARTUPINFOW si{sizeof(si)};std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
        if(!CreateProcessW(helper.c_str(),buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&process_)){error_="Cannot start VisionDepth.exe (error "+std::to_string(GetLastError())+").";process_={};stop();return false;}
        CloseHandle(process_.hThread);process_.hThread=nullptr;return true;
    }
    void stop(){
        if(process_.hProcess){if(WaitForSingleObject(process_.hProcess,0)==WAIT_TIMEOUT)TerminateProcess(process_.hProcess,0);CloseHandle(process_.hProcess);}process_={};
        if(view_)UnmapViewOfFile(view_);view_=nullptr;header_=nullptr;
        if(mapping_)CloseHandle(mapping_);mapping_=nullptr;if(pictureEvent_)CloseHandle(pictureEvent_);pictureEvent_=nullptr;if(depthEvent_)CloseHandle(depthEvent_);depthEvent_=nullptr;lastDepth_=0;
    }
    bool running()const{return process_.hProcess&&WaitForSingleObject(process_.hProcess,0)==WAIT_TIMEOUT;}
    bool ready(){return header_&&load32(header_->state)==depth::Ready&&header_->netWidth&&header_->netHeight;}
    bool failed(){return header_&&load32(header_->state)==depth::Failed;}
    std::string message()const{return header_?std::string(header_->message,strnlen(header_->message,sizeof(header_->message))):std::string();}
    NetSize netSize()const{return {header_->netWidth,header_->netHeight};}
    double loadMs()const{return header_?header_->loadMs:0;}
    // The helper has finished reading the last picture, so the buffer may be rewritten.
    bool pictureFree(){return header_&&load32(header_->pictureTaken)==load32(header_->pictureSeq);}
    void submit(const uint8_t* bgra,size_t pitch,unsigned width,unsigned height,double timestamp){
        uint8_t* dst=view_+depth::headerBytes;for(unsigned y=0;y<height;y++)memcpy(dst+size_t(y)*width*4,bgra+size_t(y)*pitch,size_t(width)*4);
        header_->pictureTimestamp=timestamp;store32(header_->pictureSeq,load32(header_->pictureSeq)+1);SetEvent(pictureEvent_);
    }
    // Copies a new depth map out, if the helper produced one since the last call.
    bool take(std::vector<float>& depthMap,double& timestamp,double& inferenceMs){
        if(!header_)return false;
        for(int attempt=0;attempt<4;attempt++){
            const uint32_t seq=load32(header_->depthSeq);if(seq==lastDepth_||load32(header_->depthBusy))return false;
            const size_t count=size_t(header_->netWidth)*header_->netHeight;depthMap.resize(count);
            memcpy(depthMap.data(),view_+depth::headerBytes+depth::pictureBytes,count*sizeof(float));timestamp=header_->depthTimestamp;inferenceMs=header_->inferenceMs;
            if(load32(header_->depthSeq)==seq&&!load32(header_->depthBusy)){lastDepth_=seq;return true;}
        }
        return false;
    }
};
}
void ScreenStereoRenderer::init(ID3D11Device* device){
    auto v=compileScreenShader("vs","vs_5_0"),p=compileScreenShader("ps","ps_5_0"),d=compileScreenShader("psDown","ps_5_0");
    check(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs_),"Screen vertex shader");
    check(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps_),"Screen stereo shader");
    check(device->CreatePixelShader(d->GetBufferPointer(),d->GetBufferSize(),nullptr,&down_),"Screen downscale shader");
    D3D11_BUFFER_DESC b{};b.ByteWidth=48;b.Usage=D3D11_USAGE_DYNAMIC;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(device->CreateBuffer(&b,nullptr,&params_),"Screen shader constants");
    D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&s,&sampler_),"Screen sampler");
}
void ScreenStereoRenderer::constants(ID3D11DeviceContext* context,const float* values){D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(params_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"Map screen constants");memcpy(m.pData,values,48);context->Unmap(params_.Get(),0);}
void ScreenStereoRenderer::render(ID3D11DeviceContext* context,ID3D11ShaderResourceView* picture,ID3D11ShaderResourceView* nearness,ID3D11RenderTargetView* target,unsigned eyeWidth,unsigned height,const ScreenSettings& s){
    context->OMSetRenderTargets(1,&target,nullptr);
    ID3D11ShaderResourceView* views[2]{picture,nearness};auto* cb=params_.Get();auto* sm=sampler_.Get();
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs_.Get(),nullptr,0);context->PSSetShader(ps_.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,2,views);context->PSSetSamplers(0,1,&sm);
    for(int eye=0;eye<2;eye++){
        const float p[12]{eye==0?-1.f:1.f,s.depth?s.separation:0.f,s.convergence,s.popOut, nearness?1.f:0.f,s.showDepth&&nearness?1.f:0.f,float(s.steps),0, 1.f/float(eyeWidth),1.f/float(height),0,0};
        constants(context,p);D3D11_VIEWPORT vp{float(eye*eyeWidth),0,float(eyeWidth),float(height),0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);
    }
    ID3D11ShaderResourceView* none[2]{};context->PSSetShaderResources(0,2,none);
}
void ScreenStereoRenderer::downscale(ID3D11DeviceContext* context,ID3D11ShaderResourceView* picture,ID3D11RenderTargetView* target,unsigned width,unsigned height,float mipLevel,bool hdr,float sdrWhiteLevel){
    context->OMSetRenderTargets(1,&target,nullptr);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    const float p[12]{0,0,0,0, 0,0,0,mipLevel, 1.f/float(width),1.f/float(height),hdr?1.f:0.f,std::max(sdrWhiteLevel,.01f)};constants(context,p);
    auto* cb=params_.Get();auto* sm=sampler_.Get();context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs_.Get(),nullptr,0);context->PSSetShader(down_.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,1,&picture);context->PSSetSamplers(0,1,&sm);context->Draw(3,0);
    ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(0,1,&none);
}
void StereoSource::runScreen(std::stop_token stop,const SourceConfig& config,LUID adapterId,ID3D11Device* device,ID3D11DeviceContext* context,const Publish& publish){
    namespace cap=winrt::Windows::Graphics::Capture;namespace dx=winrt::Windows::Graphics::DirectX;namespace meta=winrt::Windows::Foundation::Metadata;
    if(!cap::GraphicsCaptureSession::IsSupported())throw std::runtime_error("Windows Graphics Capture is unavailable.");
    if(!config.monitor)throw std::runtime_error("No display selected for the screen conversion.");
    auto interop=winrt::get_activation_factory<cap::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();cap::GraphicsCaptureItem item{nullptr};
    check(interop->CreateForMonitor(config.monitor,winrt::guid_of<cap::GraphicsCaptureItem>(),winrt::put_abi(item)),"Capture the selected display");
    ComPtr<IDXGIDevice> dxDevice;check(device->QueryInterface(IID_PPV_ARGS(&dxDevice)),"Capture DXGI device");winrt::com_ptr<IInspectable> inspectable;check(CreateDirect3D11DeviceFromDXGIDevice(dxDevice.Get(),inspectable.put()),"Capture WinRT device");
    auto rtDevice=inspectable.as<dx::Direct3D11::IDirect3DDevice>();auto size=item.Size();
    const auto format=config.captureHDR?dx::DirectXPixelFormat::R16G16B16A16Float:dx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    // Windows 11 outlines a captured display in yellow unless borderless capture is granted (automatic for an unpackaged app).
    try{if(meta::ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess"))cap::GraphicsCaptureAccess::RequestAccessAsync(cap::GraphicsCaptureAccessKind::Borderless).get();}catch(...){}
    auto frames=cap::Direct3D11CaptureFramePool::CreateFreeThreaded(rtDevice,format,2,size);auto session=frames.CreateCaptureSession(item);
    // The cursor stays out of the picture: Windows draws it above the overlay, at screen depth, with no delay.
    session.IsCursorCaptureEnabled(false);
    try{if(meta::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",L"IsBorderRequired"))session.IsBorderRequired(false);}catch(...){}
    session.StartCapture();
    ScreenStereoRenderer renderer;renderer.init(device);
    DepthService service;DepthNormalizer normalizer;
    struct Textures{ComPtr<ID3D11Texture2D> picture,netInput,netStaging,nearMap,sbs;ComPtr<ID3D11ShaderResourceView> pictureView,nearView;ComPtr<ID3D11RenderTargetView> netTarget,sbsTarget;unsigned width=0,height=0,netW=0,netH=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;} t;
    auto ensurePicture=[&](unsigned w,unsigned h,DXGI_FORMAT f){
        if(t.picture&&t.width==w&&t.height==h&&t.format==f)return;
        t.picture.Reset();t.pictureView.Reset();t.sbs.Reset();t.sbsTarget.Reset();
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=0;d.ArraySize=1;d.SampleDesc.Count=1;d.Format=f;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
        check(device->CreateTexture2D(&d,nullptr,&t.picture),"Screen picture");check(device->CreateShaderResourceView(t.picture.Get(),nullptr,&t.pictureView),"Screen picture view");
        D3D11_TEXTURE2D_DESC s{};s.Width=w*2;s.Height=h;s.MipLevels=1;s.ArraySize=1;s.SampleDesc.Count=1;s.Format=f;s.Usage=D3D11_USAGE_DEFAULT;s.BindFlags=D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&s,nullptr,&t.sbs),"Screen stereo pair");check(device->CreateRenderTargetView(t.sbs.Get(),nullptr,&t.sbsTarget),"Screen stereo target");
        t.width=w;t.height=h;t.format=f;
    };
    auto ensureNet=[&](unsigned w,unsigned h){
        if(t.netW==w&&t.netH==h)return;
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=1;d.ArraySize=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&d,nullptr,&t.netInput),"Network picture");check(device->CreateRenderTargetView(t.netInput.Get(),nullptr,&t.netTarget),"Network picture target");
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&d,nullptr,&t.netStaging),"Network picture readback");
        D3D11_TEXTURE2D_DESC n{};n.Width=w;n.Height=h;n.MipLevels=1;n.ArraySize=1;n.SampleDesc.Count=1;n.Format=DXGI_FORMAT_R32_FLOAT;n.Usage=D3D11_USAGE_DYNAMIC;n.BindFlags=D3D11_BIND_SHADER_RESOURCE;n.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        check(device->CreateTexture2D(&n,nullptr,&t.nearMap),"Nearness map");check(device->CreateShaderResourceView(t.nearMap.Get(),nullptr,&t.nearView),"Nearness view");
        t.netW=w;t.netH=h;
    };
    std::vector<float> rawDepth,nearness;bool haveDepth=false,dirty=false,netDirty=false,stagingPending=false,serviceStarted=false;unsigned serviceQuality=0;std::string serviceModel;
    uint64_t settingsRevision=0,depthFrames=0;double lastRender=0,lastTimestamp=0,stagingTimestamp=0,depthTimestamp=0,inferenceMs=0,convertMs=0,lastStatus=0,lastSubmit=0;
    while(!stop.stop_requested()){
        ScreenSettings s;uint64_t revision;{std::lock_guard l(mutex_);s=screen_;revision=screenRevision_;}
        if(revision!=settingsRevision){settingsRevision=revision;dirty=true;normalizer.smoothing=s.smoothing;}
        bool newPicture=false;
        if(auto frame=frames.TryGetNextFrame()){
            // Drain to the most recent picture; a frame arrives for every change of the desktop.
            while(auto newer=frames.TryGetNextFrame()){frame.Close();frame=newer;}
            auto content=frame.ContentSize();
            if(content.Width!=size.Width||content.Height!=size.Height){frame.Close();if(content.Width>0&&content.Height>0){size=content;frames.Recreate(rtDevice,format,2,size);}continue;}
            auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();ComPtr<ID3D11Texture2D> texture;check(access->GetInterface(IID_PPV_ARGS(&texture)),"Capture texture");
            D3D11_TEXTURE2D_DESC cd{};texture->GetDesc(&cd);ensurePicture(cd.Width,cd.Height,cd.Format);
            context->CopySubresourceRegion(t.picture.Get(),0,0,0,0,texture.Get(),0,nullptr);
            lastTimestamp=double(frame.SystemRelativeTime().count())/1e7;frame.Close();newPicture=true;dirty=true;netDirty=true;
        }
        if(!t.picture){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        // The helper starts once the picture size is known and restarts when the quality or the model changes.
        if(s.depth&&(!serviceStarted||serviceQuality!=s.quality||serviceModel!=s.model)){
            service.stop();serviceStarted=true;serviceQuality=s.quality;serviceModel=s.model;normalizer.reset();haveDepth=false;stagingPending=false;netDirty=true;
            service.start(config.depthHelper,s.model.empty()?config.depthModel:std::filesystem::path(wide(s.model)),adapterId,t.width,t.height,std::clamp(s.quality,depth::patch,depth::maxSide),config.depthLog);
        }
        if(serviceStarted&&service.ready()){
            const NetSize n=service.netSize();ensureNet(n.width,n.height);
            // Picture for the network, at most depthRate times a second and never more often than
            // twice the network's own run time, so the network takes at most half of the GPU (a
            // 38 ms Large model then runs about 13 times a second, Small keeps its 30). Shrunk
            // through the mip chain and read back once the GPU is done with it.
            const double interval=std::max(s.depthRate>0?1/s.depthRate:0.,inferenceMs*2/1000);
            if(netDirty&&!stagingPending&&service.pictureFree()&&qpc()-lastSubmit>=interval){
                context->GenerateMips(t.pictureView.Get());
                renderer.downscale(context,t.pictureView.Get(),t.netTarget.Get(),n.width,n.height,std::max(std::log2(float(t.width)/float(n.width)),0.f),config.captureHDR,config.sdrWhiteLevel);
                context->CopyResource(t.netStaging.Get(),t.netInput.Get());context->Flush();stagingPending=true;stagingTimestamp=lastTimestamp;netDirty=false;lastSubmit=qpc();
            }
            if(stagingPending){
                D3D11_MAPPED_SUBRESOURCE m{};HRESULT h=context->Map(t.netStaging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
                if(SUCCEEDED(h)){service.submit(static_cast<const uint8_t*>(m.pData),m.RowPitch,n.width,n.height,stagingTimestamp);context->Unmap(t.netStaging.Get(),0);stagingPending=false;}
                else if(h!=DXGI_ERROR_WAS_STILL_DRAWING)check(h,"Read back the network picture");
            }
            // A new depth map: normalise, smooth, upload.
            if(service.take(rawDepth,depthTimestamp,inferenceMs)&&rawDepth.size()==size_t(n.width)*n.height&&normalizer.process(rawDepth.data(),n.width,n.height,nearness)){
                D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(t.nearMap.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"Upload the nearness map");
                for(unsigned y=0;y<n.height;y++)memcpy(static_cast<uint8_t*>(m.pData)+size_t(y)*m.RowPitch,nearness.data()+size_t(y)*n.width,size_t(n.width)*sizeof(float));
                context->Unmap(t.nearMap.Get(),0);haveDepth=true;depthFrames++;dirty=true;
            }
        }
        // Both eyes are drawn when the picture, the depth or the settings changed, at most pairRate times a second.
        const double now=qpc();
        if(dirty&&now-lastRender>=(s.pairRate>0?1/s.pairRate:0)){
            renderer.render(context,t.pictureView.Get(),haveDepth&&s.depth?t.nearView.Get():nullptr,t.sbsTarget.Get(),t.width,t.height,s);
            publish(t.sbs.Get(),t.width*2,t.height,lastTimestamp?lastTimestamp:now,config.captureHDR?Encoding::LinearScRGB:Encoding::SRGB,0,0);
            convertMs=(qpc()-now)*1000;lastRender=now;dirty=false;
        }
        if(now-lastStatus>.25){lastStatus=now;
            std::ostringstream m;m<<std::fixed<<std::setprecision(1);std::string depthMessage=service.started()?service.message():service.error();
            if(!s.depth)m<<"Screen "<<t.width<<"x"<<t.height<<" mirrored in 2D (depth off)";
            else if(!service.started())m<<"Screen "<<t.width<<"x"<<t.height<<" in 2D: "<<service.error();
            else if(service.failed())m<<"Screen "<<t.width<<"x"<<t.height<<" in 2D: the depth helper failed: "<<depthMessage;
            else if(!service.running()&&!service.ready())m<<"Screen "<<t.width<<"x"<<t.height<<" in 2D: the depth helper exited";
            else if(!service.ready())m<<"Screen "<<t.width<<"x"<<t.height<<" in 2D while the depth network loads (a few seconds)";
            else if(!haveDepth)m<<"Screen "<<t.width<<"x"<<t.height<<": waiting for the first depth map";
            else m<<"Screen "<<t.width<<"x"<<t.height<<" in 3D: depth "<<t.netW<<"x"<<t.netH<<" every "<<inferenceMs<<" ms, "<<(now-depthTimestamp)*1000<<" ms old; conversion "<<convertMs<<" ms";
            std::lock_guard l(mutex_);status_.message=m.str();status_.depthMessage=depthMessage;status_.netWidth=t.netW;status_.netHeight=t.netH;status_.depthMs=inferenceMs;status_.depthAgeMs=haveDepth?(now-depthTimestamp)*1000:0;status_.convertMs=convertMs;status_.depthFrames=depthFrames;
        }
        if(!newPicture){if(service.depthEvent())WaitForSingleObject(service.depthEvent(),1);else std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    }
    session.Close();frames.Close();service.stop();
}
}
