#include "sources.h"
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
std::vector<std::pair<HWND,std::string>> captureWindows(HWND exclude){
    std::vector<std::pair<HWND,std::string>> result;
    EnumWindows([](HWND h,LPARAM p)->BOOL{auto& r=*reinterpret_cast<std::vector<std::pair<HWND,std::string>>*>(p);if(!IsWindowVisible(h) || GetWindow(h,GW_OWNER))return TRUE;wchar_t title[512]{};GetWindowTextW(h,title,512);if(title[0])r.emplace_back(h,utf8(title));return TRUE;},reinterpret_cast<LPARAM>(&result));
    std::erase_if(result,[&](auto& w){return w.first==exclude;});return result;
}
void StereoSource::start(const SourceConfig& c,LUID adapter){stop();if(c.kind==SourceKind::Patterns)return;{std::lock_guard l(mutex_);status_={"Starting source...",0,0,0,true};screen_=c.screen;++screenRevision_;}thread_=std::jthread([this,c,adapter](std::stop_token s){run(s,c,adapter);});}
void StereoSource::configureScreen(const ScreenSettings& s){std::lock_guard l(mutex_);screen_=s;++screenRevision_;}
void StereoSource::stop(){if(thread_.joinable()){thread_.request_stop();thread_.join();}std::lock_guard l(mutex_);latest_.reset();status_={};}
std::shared_ptr<StereoFrame> StereoSource::latest()const{std::lock_guard l(mutex_);return latest_;}
SourceStatus StereoSource::status()const{std::lock_guard l(mutex_);return status_;}
void StereoSource::run(std::stop_token stop,SourceConfig config,LUID adapterId){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        if(config.kind==SourceKind::Screen)config.packing=Packing::SideBySide;
        if(config.kind==SourceKind::Window||config.kind==SourceKind::Screen){
            // WGC's 8-bit path clips an HDR desktop before the presenter ever
            // receives it. Preserve scRGB for both HDR and SDR output modes.
            config.captureHDR=true;
            const HMONITOR monitor=config.kind==SourceKind::Screen?config.monitor:MonitorFromWindow(config.window,MONITOR_DEFAULTTONEAREST);
            for(const auto& display:enumerateDisplays())if(display.monitor==monitor){config.sdrWhiteLevel=display.sdrWhiteNits/80.f;break;}
        }
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
            frame->width=width;frame->height=height;frame->packing=config.kind==SourceKind::Screen?Packing::SideBySide:config.packing;frame->encoding=encoding;frame->timestamp=timestamp;
            frame->alignmentApplied=config.kind==SourceKind::Screen;frame->sdrWhiteLevel=config.sdrWhiteLevel;
            std::lock_guard l(mutex_);frame->pairId=++status_.frames;latest_=frame;status_.lastFrame=qpc();if(config.kind!=SourceKind::Screen)status_.message="Receiving complete stereo pairs"; // the screen conversion reports its own state
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
        }else if(config.kind==SourceKind::Screen){
            runScreen(stop,config,adapterId,device.Get(),context.Get(),[&](ID3D11Texture2D* t,unsigned w,unsigned h,double ts,Encoding e,unsigned l,unsigned top){publish(t,w,h,ts,e,l,top);});
        }
    }catch(const winrt::hresult_error& e){std::lock_guard l(mutex_);status_.message=utf8(e.message().c_str());status_.running=false;}
    catch(const std::exception& e){std::lock_guard l(mutex_);status_.message=e.what();status_.running=false;}
    winrt::uninit_apartment();
}
}
