#include "sources.h"
#include "frame_channel.h"
#include <SpoutDX.h>
#include <wincodec.h>
#include <d3dkmthk.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <chrono>

namespace vision {
namespace {
LONGLONG qpcTicks(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
double qpcTicksPerSecond(){static double f=[]{LARGE_INTEGER v;QueryPerformanceFrequency(&v);return double(v.QuadPart);}();return f;}
bool senderAlive(const frames::Header* h){return h->magic==frames::magic&&h->version==frames::version&&h->senderPid&&double(qpcTicks()-h->senderHeartbeatQpc)/qpcTicksPerSecond()<1.5;}
std::string senderName(const frames::Header* h){return std::string(h->senderName,strnlen(h->senderName,sizeof(h->senderName)));}
// Opens the sender's header and pixel mappings for the receiving thread; closes everything on exit.
struct FrameChannelReader {
    HANDLE headerMapping=nullptr,dataMapping=nullptr,event=nullptr;frames::Header* header=nullptr;const uint8_t* data=nullptr;uint32_t pid=0,generation=0;uint64_t slotBytes=0;
    void closeData(){if(data)UnmapViewOfFile(data);data=nullptr;if(dataMapping)CloseHandle(dataMapping);dataMapping=nullptr;pid=generation=0;slotBytes=0;}
    ~FrameChannelReader(){closeData();if(header)UnmapViewOfFile(header);if(headerMapping)CloseHandle(headerMapping);if(event)CloseHandle(event);}
    bool openHeader(){
        if(header)return true;
        headerMapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,frames::headerName);if(!headerMapping)return false;
        header=static_cast<frames::Header*>(MapViewOfFile(headerMapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(frames::Header)));
        if(!header){CloseHandle(headerMapping);headerMapping=nullptr;return false;}
        if(!event)event=OpenEventW(SYNCHRONIZE,FALSE,frames::frameEventName);
        return true;
    }
    bool openData(uint32_t senderPid,uint32_t senderGeneration,uint64_t bytes){
        if(data&&pid==senderPid&&generation==senderGeneration&&slotBytes==bytes)return true;
        closeData();wchar_t name[192];swprintf_s(name,L"%s.Data.%u.%u",frames::headerName,senderPid,senderGeneration);
        dataMapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name);if(!dataMapping)return false;
        data=static_cast<const uint8_t*>(MapViewOfFile(dataMapping,FILE_MAP_READ,0,0,SIZE_T(bytes*frames::slotCount)));
        if(!data){closeData();return false;}
        pid=senderPid;generation=senderGeneration;slotBytes=bytes;return true;
    }
};
}
void FrameChannelHost::close(){if(view_)UnmapViewOfFile(view_);view_=nullptr;if(mapping_)CloseHandle(mapping_);mapping_=nullptr;staleSince_=0;}
FrameChannelLink FrameChannelHost::poll(const FrameChannelReport& r){
    FrameChannelLink link;double now=qpc();
    if(!view_){
        if(now<nextOpen_)return link;nextOpen_=now+.25;
        mapping_=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,frames::headerName);if(!mapping_)return link;
        view_=MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(frames::Header));if(!view_){close();return link;}
    }
    auto* h=static_cast<frames::Header*>(view_);
    h->hostPid=GetCurrentProcessId();h->hostHeartbeatQpc=qpcTicks();
    h->outputRunning=r.outputRunning;h->emitterReady=r.emitterReady;h->hostState=r.outputRunning?frames::HostPresenting:r.emitterReady?frames::HostIdle:frames::HostWaitingForEmitter;
    strncpy_s(h->hostMessage,r.message.c_str(),_TRUNCATE);
    link.present=senderAlive(h);
    if(!link.present){
        // Let go of a dead sender's mapping so the next Blender session starts from a fresh one.
        if(!staleSince_)staleSince_=now;else if(now-staleSince_>3)close();
        return link;
    }
    staleSince_=0;link.active=h->active!=0;link.visible=h->visible!=0;link.view={h->viewLeft,h->viewTop,h->viewLeft+h->viewWidth,h->viewTop+h->viewHeight};link.pid=h->senderPid;link.name=senderName(h);
    return link;
}
std::vector<std::pair<HWND,std::string>> captureWindows(HWND exclude){
    std::vector<std::pair<HWND,std::string>> result;
    EnumWindows([](HWND h,LPARAM p)->BOOL{auto& r=*reinterpret_cast<std::vector<std::pair<HWND,std::string>>*>(p);if(!IsWindowVisible(h) || GetWindow(h,GW_OWNER))return TRUE;wchar_t title[512]{};GetWindowTextW(h,title,512);if(title[0])r.emplace_back(h,utf8(title));return TRUE;},reinterpret_cast<LPARAM>(&result));
    std::erase_if(result,[&](auto& w){return w.first==exclude;});return result;
}
void StereoSource::start(const SourceConfig& c,LUID adapter){stop();if(c.kind==SourceKind::Patterns)return;{std::lock_guard l(mutex_);status_={"Starting source...",0,0,0,true};}thread_=std::jthread([this,c,adapter](std::stop_token s){run(s,c,adapter);});}
void StereoSource::stop(){if(thread_.joinable()){thread_.request_stop();thread_.join();}std::lock_guard l(mutex_);latest_.reset();status_={};}
std::shared_ptr<StereoFrame> StereoSource::latest()const{std::lock_guard l(mutex_);return latest_;}
SourceStatus StereoSource::status()const{std::lock_guard l(mutex_);return status_;}
void StereoSource::run(std::stop_token stop,SourceConfig config,LUID adapterId){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Source factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(adapterId,IID_PPV_ARGS(&adapter)),"Source adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Source D3D11 device");
        std::array<std::shared_ptr<StereoFrame>,3> pool;
        auto publish=[&](ID3D11Texture2D* input,unsigned width,unsigned height,double timestamp,Encoding encoding,unsigned left=0,unsigned top=0){
            if(!width || !height || (config.packing==Packing::SideBySide ? width%2 : height%2))throw std::runtime_error("Packed stereo dimensions must divide evenly into two eyes.");
            size_t index=3;for(size_t i=0;i<3;i++)if(!pool[i] || pool[i].use_count()==1){index=i;break;}
            if(index==3){std::lock_guard l(mutex_);status_.dropped++;return;}
            D3D11_TEXTURE2D_DESC in{};input->GetDesc(&in);
            auto& frame=pool[index];D3D11_TEXTURE2D_DESC old{};if(frame)frame->texture->GetDesc(&old);
            if(!frame || old.Width!=width || old.Height!=height || old.Format!=in.Format){
                frame=std::make_shared<StereoFrame>();D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=1;d.ArraySize=1;d.SampleDesc.Count=1;d.Format=in.Format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                check(device->CreateTexture2D(&d,nullptr,&frame->texture),"Create shared source frame");ComPtr<IDXGIResource> resource;check(frame->texture.As(&resource),"Source shared resource");check(resource->GetSharedHandle(&frame->sharedHandle),"Source shared handle");
            }
            ComPtr<IDXGIKeyedMutex> key;check(frame->texture.As(&key),"Source keyed mutex");HRESULT acquired=key->AcquireSync(0,0);if(acquired!=S_OK){std::lock_guard l(mutex_);status_.dropped++;return;}
            D3D11_BOX box{left,top,0,left+width,top+height,1};context->CopySubresourceRegion(frame->texture.Get(),0,0,0,0,input,0,&box);
            // Hand the texture over only after the GPU has finished the copy. Released while the copy was
            // still queued, the presenter's draw of this texture waited on it, and with a focused game
            // loading the GPU that wait pushed the output past its refresh: an eye repeated and the image
            // jumped every few seconds, as the game's frame rate beat against the output's pair rate.
            {D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> done;
             if(SUCCEEDED(device->CreateQuery(&qd,&done))){context->End(done.Get());context->Flush();
                 double giveUp=qpc()+.1;BOOL finished=FALSE;
                 while(context->GetData(done.Get(),&finished,sizeof(finished),0)==S_FALSE&&qpc()<giveUp)std::this_thread::yield();}
             else context->Flush();}
            check(key->ReleaseSync(0),"Release source mutex");
            frame->width=width;frame->height=height;frame->packing=config.packing;frame->encoding=encoding;frame->timestamp=timestamp;
            std::lock_guard l(mutex_);frame->pairId=++status_.frames;latest_=frame;status_.lastFrame=qpc();status_.message="Receiving complete stereo pairs";
        };
        if(config.kind==SourceKind::Image){
            ComPtr<IWICImagingFactory> wic;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"WIC factory");
            ComPtr<IWICBitmapDecoder> decoder;check(wic->CreateDecoderFromFilename(config.file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Open stereo image");
            ComPtr<IWICBitmapFrameDecode> frame;check(decoder->GetFrame(0,&frame),"Image frame");ComPtr<IWICFormatConverter> converter;check(wic->CreateFormatConverter(&converter),"Image converter");
            check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Decode SDR image");UINT w=0,h=0;converter->GetSize(&w,&h);if(!w||!h||w>16384||h>16384||uint64_t(w)*h>64000000)throw std::runtime_error("Image exceeds supported size.");
            std::vector<uint8_t> pixels(size_t(w)*h*4);check(converter->CopyPixels(nullptr,w*4,UINT(pixels.size()),pixels.data()),"Decode pixels");
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&data,&texture),"Image texture");publish(texture.Get(),w,h,qpc(),Encoding::SRGB);
            {std::lock_guard l(mutex_);status_.message="Static stereo image (SDR)";}
            while(!stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }else if(config.kind==SourceKind::Window){
            namespace cap=winrt::Windows::Graphics::Capture;namespace dx=winrt::Windows::Graphics::DirectX;
            if(!cap::GraphicsCaptureSession::IsSupported())throw std::runtime_error("Windows Graphics Capture is unavailable.");
            auto interop=winrt::get_activation_factory<cap::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();cap::GraphicsCaptureItem item{nullptr};
            check(interop->CreateForWindow(config.window,winrt::guid_of<cap::GraphicsCaptureItem>(),winrt::put_abi(item)),"Capture selected window");
            // The captured game keeps focus (the pad follows focus), and Windows then favours its GPU work
            // over the unfocused stereo output: the output missed refreshes in bursts every few seconds,
            // each one a wrong-eye flash in the glasses. Drop only the game's GPU scheduling class while it
            // is captured (its CPU priority is untouched) and restore it when capture ends.
            struct GpuClassGuard {HANDLE process=nullptr;D3DKMT_SCHEDULINGPRIORITYCLASS original=D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL;
                ~GpuClassGuard(){if(process){D3DKMTSetProcessSchedulingPriorityClass(process,original);CloseHandle(process);}}} gameGpu;
            {DWORD pid=0;GetWindowThreadProcessId(config.window,&pid);
             if(pid&&pid!=GetCurrentProcessId())if(HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_SET_INFORMATION,FALSE,pid)){
                 D3DKMT_SCHEDULINGPRIORITYCLASS current{};
                 if(D3DKMTGetProcessSchedulingPriorityClass(p,&current)==0&&D3DKMTSetProcessSchedulingPriorityClass(p,D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL)==0){gameGpu.process=p;gameGpu.original=current;}
                 else CloseHandle(p);}}
            ComPtr<IDXGIDevice> dxDevice;device.As(&dxDevice);winrt::com_ptr<IInspectable> inspectable;check(CreateDirect3D11DeviceFromDXGIDevice(dxDevice.Get(),inspectable.put()),"Capture WinRT device");
            auto rtDevice=inspectable.as<dx::Direct3D11::IDirect3DDevice>();auto size=item.Size();
            auto format=config.captureHDR?dx::DirectXPixelFormat::R16G16B16A16Float:dx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
            auto frames=cap::Direct3D11CaptureFramePool::CreateFreeThreaded(rtDevice,format,3,size);auto session=frames.CreateCaptureSession(item);session.IsCursorCaptureEnabled(false);session.StartCapture();
            while(!stop.stop_requested()){
                if(!IsWindow(config.window)){std::lock_guard l(mutex_);status_.message="Source window closed; holding last complete pair";status_.running=false;break;}
                auto frame=frames.TryGetNextFrame();if(!frame){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
                // Drain to the most recent complete packed pair.
                while(auto newer=frames.TryGetNextFrame()){frame.Close();frame=newer;}
                auto content=frame.ContentSize();
                if(content.Width!=size.Width || content.Height!=size.Height){frame.Close();if(content.Width>0 && content.Height>0){size=content;frames.Recreate(rtDevice,format,3,size);}continue;}
                auto access=frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();ComPtr<ID3D11Texture2D> texture;check(access->GetInterface(IID_PPV_ARGS(&texture)),"Capture texture");
                // Window capture returns the window's visible frame, title bar and borders included whenever the
                // game shows them (2562x1462 around a 2560x1440 picture on 2026-09-14). The border moved the
                // side-by-side split and shifted both eyes until the game dropped it. Crop to the client area
                // every frame so the packed pair is the game's picture only, from the first frame.
                unsigned cropLeft=0,cropTop=0,cropWidth=unsigned(size.Width),cropHeight=unsigned(size.Height);
                {RECT bounds{},client{};POINT origin{0,0};
                 if(SUCCEEDED(DwmGetWindowAttribute(config.window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))&&GetClientRect(config.window,&client)&&ClientToScreen(config.window,&origin)){
                     long l=origin.x-bounds.left,t=origin.y-bounds.top;
                     if(l>=0&&t>=0&&client.right>0&&client.bottom>0&&l+client.right<=size.Width&&t+client.bottom<=size.Height){cropLeft=unsigned(l);cropTop=unsigned(t);cropWidth=unsigned(client.right);cropHeight=unsigned(client.bottom);}}}
                if(config.packing==Packing::SideBySide)cropWidth&=~1u;else cropHeight&=~1u;
                publish(texture.Get(),cropWidth,cropHeight,double(frame.SystemRelativeTime().count())/10000000.0,config.captureHDR?Encoding::LinearScRGB:Encoding::SRGB,cropLeft,cropTop);frame.Close();
            }session.Close();frames.Close();
        }else if(config.kind==SourceKind::Spout){
            spoutDX receiver;if(!receiver.OpenDirectX11(device.Get()))throw std::runtime_error("Spout initialization failed.");receiver.SetReceiverName(config.sender.empty()?nullptr:config.sender.c_str());
            while(!stop.stop_requested()){
                if(receiver.ReceiveTexture() && receiver.IsFrameNew()){
                    auto* t=receiver.GetSenderTexture();if(t)publish(t,receiver.GetSenderWidth(),receiver.GetSenderHeight(),qpc(),config.encoding);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }receiver.ReleaseReceiver();receiver.CloseDirectX11();
        }else if(config.kind==SourceKind::SharedFrames){
            FrameChannelReader channel;ComPtr<ID3D11Texture2D> upload;unsigned uploadWidth=0,uploadHeight=0;uint64_t lastFrame=0;
            {std::lock_guard l(mutex_);status_.message="Waiting for Blender to publish frames";}
            while(!stop.stop_requested()){
                if(!channel.openHeader()){std::this_thread::sleep_for(std::chrono::milliseconds(250));continue;}
                if(!channel.event)channel.event=OpenEventW(SYNCHRONIZE,FALSE,frames::frameEventName);
                if(channel.event)WaitForSingleObject(channel.event,50);else std::this_thread::sleep_for(std::chrono::milliseconds(4));
                auto* h=channel.header;
                if(h->magic!=frames::magic||h->version!=frames::version||h->format!=frames::FormatRGBA8BottomUp)continue;
                int32_t latest=h->latestSlot;uint32_t pid=h->senderPid,generation=h->generation;uint64_t slotBytes=h->slotBytes;
                if(latest<0||latest>=int32_t(frames::slotCount)||!slotBytes||slotBytes>uint64_t(16384)*16384*4)continue;
                if(!channel.openData(pid,generation,slotBytes))continue;
                auto& slot=h->slots[latest];uint32_t seq=slot.seq;uint64_t frameId=slot.frameId;
                if((seq&1)||frameId==lastFrame)continue;
                unsigned width=slot.width,height=slot.height;
                if(!width||!height||width%2||uint64_t(width)*height*4>slotBytes)continue;
                if(!upload||uploadWidth!=width||uploadHeight!=height){
                    upload.Reset();D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
                    check(device->CreateTexture2D(&d,nullptr,&upload),"Frame channel texture");uploadWidth=width;uploadHeight=height;
                }
                {
                    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(upload.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map frame channel texture");
                    const uint8_t* rows=channel.data+uint64_t(latest)*slotBytes;auto* target=static_cast<uint8_t*>(mapped.pData);
                    for(unsigned y=0;y<height;y++)memcpy(target+uint64_t(y)*mapped.RowPitch,rows+uint64_t(height-1-y)*width*4,size_t(width)*4); // OpenGL rows are bottom-up
                    context->Unmap(upload.Get(),0);
                }
                // The sender may have reused the slot during the upload; that texture is overwritten next time.
                if(slot.seq!=seq||h->generation!=generation||h->senderPid!=pid){h->framesTorn++;continue;}
                lastFrame=frameId;publish(upload.Get(),width,height,qpc(),Encoding::SRGB);h->framesReceived++;
                std::lock_guard l(mutex_);status_.message="Receiving stereo frames from "+senderName(h);
            }
        }
    }catch(const winrt::hresult_error& e){std::lock_guard l(mutex_);status_.message=utf8(e.message().c_str());status_.running=false;}
    catch(const std::exception& e){std::lock_guard l(mutex_);status_.message=e.what();status_.running=false;}
    winrt::uninit_apartment();
}
}
