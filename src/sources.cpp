#include "sources.h"
#include "gpu_capture.h"
#include "gpu_completion.h"
#include "direct_eyes.h"
#include "wibble_source.h"
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
std::vector<std::pair<HWND,std::string>> captureWindows(HWND exclude,HWND selected){
    std::vector<std::pair<HWND,std::string>> result;
    EnumWindows([](HWND h,LPARAM p)->BOOL{auto& r=*reinterpret_cast<std::vector<std::pair<HWND,std::string>>*>(p);if(!IsWindowVisible(h) || GetWindow(h,GW_OWNER))return TRUE;wchar_t title[512]{};GetWindowTextW(h,title,512);if(title[0])r.emplace_back(h,utf8(title));return TRUE;},reinterpret_cast<LPARAM>(&result));
    std::erase_if(result,[&](auto& w){return w.first==exclude;});
    // A capture worker retains its target handle. Keep that selection visible
    // when the target hides or closes, instead of selecting another application.
    if(selected&&selected!=exclude&&std::none_of(result.begin(),result.end(),[&](const auto& w){return w.first==selected;})){
        wchar_t title[512]{};GetWindowTextW(selected,title,512);
        result.emplace_back(selected,IsWindow(selected)?(title[0]?utf8(title):std::string("Selected window"))+" (hidden)":"Selected window closed - choose another window");
    }
    return result;
}
void StereoSource::start(const SourceConfig& c,LUID adapter){stop();if(c.kind==SourceKind::Patterns)return;if(c.kind==SourceKind::Default){std::lock_guard l(mutex_);status_.message="Choose an input source.";return;}{std::lock_guard l(mutex_);status_={"Starting source...",0,0,0,true};screen_=c.screen;++screenRevision_;mediaPaused_=false;mediaSeekMs_=-1;}thread_=std::jthread([this,c,adapter](std::stop_token s){run(s,c,adapter);});}
void StereoSource::pauseMedia(bool paused){std::lock_guard l(mutex_);mediaPaused_=paused;}
void StereoSource::seekMedia(int64_t ms){std::lock_guard l(mutex_);mediaSeekMs_=std::max(int64_t(0),ms);}
void StereoSource::volumeMedia(int percent){std::lock_guard l(mutex_);mediaVolume_=std::clamp(percent,0,100);}
void StereoSource::configureScreen(const ScreenSettings& s){std::lock_guard l(mutex_);screen_=s;++screenRevision_;}
void StereoSource::stop(){if(thread_.joinable()){thread_.request_stop();thread_.join();}std::lock_guard l(mutex_);latest_.reset();directQueue_.clear();status_={};}
std::shared_ptr<StereoFrame> StereoSource::latest()const{std::lock_guard l(mutex_);return latest_;}
std::shared_ptr<StereoFrame> StereoSource::forPresentation()const{std::lock_guard l(mutex_);return directQueue_.empty()?latest_:directQueue_.front();}
void StereoSource::presented(const std::shared_ptr<StereoFrame>& frame){std::lock_guard l(mutex_);if(!directQueue_.empty()&&directQueue_.front()==frame)directQueue_.pop_front();}
SourceStatus StereoSource::status()const{std::lock_guard l(mutex_);return status_;}
void StereoSource::run(std::stop_token stop,SourceConfig config,LUID adapterId){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        // Capture remains latency-sensitive when the app is covered or minimized,
        // before the output renderer applies the same process scheduling policy.
        PROCESS_POWER_THROTTLING_STATE power{};power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;power.StateMask=0;
        SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power));
        captureAcquiredAt_=0;
        // VRto3D's packed desktop window must remain independent of the
        // fullscreen output and SteamVR windows covering it.
        if(config.kind==SourceKind::VrTo3D){
            config.captureMethod=CaptureMethod::WindowsGraphicsCapture;
            if(!IsWindow(config.window))throw std::runtime_error("Select the VRto3D / SteamVR Headset window first.");
            if(config.packing!=Packing::SideBySide&&config.packing!=Packing::TopBottom)
                throw std::runtime_error("VRto3D capture requires SBS or top/bottom output.");
        }
        if(config.kind==SourceKind::Screen)config.packing=Packing::SideBySide;
        if(config.kind==SourceKind::DirectEyes||config.kind==SourceKind::WibbleWobble)config.packing=Packing::SeparateEyes;
        if(isWindowCapture(config.kind)||config.kind==SourceKind::Screen){
            // Request HDR precision; the capture backend reports each frame's actual encoding.
            config.captureHDR=true;
            const HMONITOR monitor=config.kind==SourceKind::Screen?config.monitor:MonitorFromWindow(config.window,MONITOR_DEFAULTTONEAREST);
            for(const auto& display:enumerateDisplays())if(display.monitor==monitor){config.sdrWhiteLevel=display.sdrWhiteNits/80.f;break;}
        }
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Source factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(adapterId,IID_PPV_ARGS(&adapter)),"Source adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Source D3D11 device");
        std::array<std::shared_ptr<StereoFrame>,3> pool;
        std::array<ComPtr<IDXGIKeyedMutex>,3> poolKeys;
        // Publication is serialized and waits for completion before reusing this query.
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> done;
        check(device->CreateQuery(&qd,&done),"Create source completion query");
        auto publish=[&](ID3D11Texture2D* input,unsigned width,unsigned height,double timestamp,Encoding encoding,unsigned left=0,unsigned top=0,bool duplicateEyes=false){
            const bool separate=config.packing==Packing::SeparateEyes;
            const bool queued=config.kind==SourceKind::DirectEyes;
            const unsigned outputWidth=duplicateEyes?width*2:width;
            if(!width || !height || (!separate&&(config.packing==Packing::SideBySide ? outputWidth%2 : height%2)))throw std::runtime_error("Packed stereo dimensions must divide evenly into two eyes.");
            size_t index=3;for(size_t i=0;i<3;i++)if(!pool[i] || pool[i].use_count()==1){index=i;break;}
            // Direct providers retain ownership and retry until space exists;
            // never acknowledge a sequential eye pair that was not copied.
            while(index==3&&queued&&!stop.stop_requested()){
                shortWait();
                for(size_t i=0;i<3;i++)if(!pool[i]||pool[i].use_count()==1){index=i;break;}
            }
            if(stop.stop_requested())return;
            if(index==3){std::lock_guard l(mutex_);status_.dropped++;return;}
            D3D11_TEXTURE2D_DESC in{};input->GetDesc(&in);
            auto& frame=pool[index];D3D11_TEXTURE2D_DESC old{};if(frame)frame->texture->GetDesc(&old);
            if(!frame || old.Width!=outputWidth || old.Height!=height || old.Format!=in.Format){
                frame=std::make_shared<StereoFrame>();D3D11_TEXTURE2D_DESC d{};d.Width=outputWidth;d.Height=height;d.MipLevels=1;d.ArraySize=separate?2:1;d.SampleDesc.Count=1;d.Format=in.Format;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                check(device->CreateTexture2D(&d,nullptr,&frame->texture),"Create shared source frame");ComPtr<IDXGIResource> resource;check(frame->texture.As(&resource),"Source shared resource");check(resource->GetSharedHandle(&frame->sharedHandle),"Source shared handle");
                check(frame->texture.As(&poolKeys[index]),"Source keyed mutex");
            }
            auto& key=poolKeys[index];HRESULT acquired=key->AcquireSync(0,0);
            while(queued&&acquired==HRESULT(WAIT_TIMEOUT)&&!stop.stop_requested()){
                shortWait();acquired=key->AcquireSync(0,0);
            }
            if(acquired!=S_OK){if(stop.stop_requested())return;if(queued)throw std::runtime_error("Direct eye snapshot mutex failed; holding last complete pair");std::lock_guard l(mutex_);status_.dropped++;return;}
            D3D11_BOX box{left,top,0,left+width,top+height,1};context->CopySubresourceRegion(frame->texture.Get(),0,0,0,0,input,0,&box);
            if(separate)context->CopySubresourceRegion(frame->texture.Get(),1,0,0,0,input,1,&box);
            if(duplicateEyes)context->CopySubresourceRegion(frame->texture.Get(),0,width,0,0,input,0,&box);
            // Hand the texture over only after the GPU has finished the copy. Released while the copy was
            // still queued, the presenter's draw of this texture waited on it, and with a focused game
            // loading the GPU that wait pushed the output past its refresh: an eye repeated and the image
            // jumped every few seconds, as the game's frame rate beat against the output's pair rate.
            context->End(done.Get());context->Flush();
            const HRESULT completed=waitForGpuCompletion([&](BOOL* finished){return context->GetData(done.Get(),finished,sizeof(*finished),D3D11_ASYNC_GETDATA_DONOTFLUSH);},stop,std::chrono::seconds(5),
                std::chrono::microseconds(isWindowCapture(config.kind)?500:150));
            check(key->ReleaseSync(0),"Release source mutex");
            if(stop.stop_requested())return;
            check(completed,"Source GPU copy did not complete; holding last complete pair");
            frame->width=outputWidth;frame->height=height;frame->packing=config.kind==SourceKind::Screen?Packing::SideBySide:config.packing;frame->encoding=encoding;frame->timestamp=timestamp;
            frame->alignmentApplied=config.kind==SourceKind::Screen;frame->sdrWhiteLevel=config.sdrWhiteLevel;
            frame->eyeAspect=config.packing==Packing::SideBySide&&
                (config.kind==SourceKind::Image||config.kind==SourceKind::Window||config.kind==SourceKind::Vlc)
                ?float(width)/float(height)/(config.sbsFormat==SbsFormat::Full?2.f:1.f):config.kind==SourceKind::WibbleWobble?float(width)/float(height):0.f;
            if(config.kind==SourceKind::VrTo3D)
                frame->eyeAspect=float(width)/float(height)*(config.sbsFormat==SbsFormat::Half?1.f:config.packing==Packing::TopBottom?2.f:.5f);
            std::lock_guard l(mutex_);frame->captureAcquiredAt=captureAcquiredAt_;frame->publishedAt=qpc();frame->pairId=++status_.frames;latest_=frame;if(queued)directQueue_.push_back(frame);status_.lastFrame=frame->publishedAt;if(config.kind!=SourceKind::Screen)status_.message="Receiving complete stereo pairs"; // the screen conversion reports its own state
        };
        if(config.kind==SourceKind::WibbleWobble){
            wibble::SourceReader reader;reader.open(device.Get(),context.Get());
            while(!stop.stop_requested()){
                wibble::Picture picture;
                if(reader.next(stop,picture)){
                    captureAcquiredAt_=picture.acquiredAt;
                    publish(picture.texture.Get(),picture.width,picture.height,picture.acquiredAt,picture.encoding);
                }
                {std::lock_guard l(mutex_);status_.message=reader.status();status_.captureBackend="Wibble Wobble / SteamVR stereo input";status_.requiresElevation=reader.requiresElevation();}
                shortWait();
            }
        }else if(config.kind==SourceKind::DirectEyes){
            direct::Reader reader;check(reader.open(device.Get(),config.directChannel),"Open direct stereo provider (the provider must be running)");
            while(!stop.stop_requested()){
                reader.announce(directCovering_,directPairPeriodUs_);
                const HRESULT hr=reader.acquire();
                // Not a 1 ms sleep: that lasts a 15.6 ms tick without a raised timer resolution, which
                // held back the provider's next pair (and the game's frame rate) by the same amount.
                if(hr==S_FALSE||hr==DXGI_ERROR_WAS_STILL_DRAWING){shortWait();continue;}
                check(hr,"Direct stereo provider disconnected; holding last complete pair");
                D3D11_TEXTURE2D_DESC desc{};reader.texture()->GetDesc(&desc);
                publish(reader.texture(),desc.Width,desc.Height,qpc(),Encoding(reader.encoding()));
                reader.release();
                {std::lock_guard l(mutex_);status_.message="Receiving separate full-resolution eyes";}
            }
        }else if(config.kind==SourceKind::Image){
            ComPtr<IWICImagingFactory> wic;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"WIC factory");
            ComPtr<IWICBitmapDecoder> decoder;check(wic->CreateDecoderFromFilename(config.file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Open stereo image");
            ComPtr<IWICBitmapFrameDecode> frame;check(decoder->GetFrame(0,&frame),"Image frame");ComPtr<IWICFormatConverter> converter;check(wic->CreateFormatConverter(&converter),"Image converter");
            check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Decode SDR image");UINT w=0,h=0;converter->GetSize(&w,&h);if(!w||!h||w>16384||h>16384||uint64_t(w)*h>64000000)throw std::runtime_error("Image exceeds supported size.");
            std::vector<uint8_t> pixels(size_t(w)*h*4);check(converter->CopyPixels(nullptr,w*4,UINT(pixels.size()),pixels.data()),"Decode pixels");
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,&data,&texture),"Image texture");publish(texture.Get(),w,h,qpc(),Encoding::SRGB);
            {std::lock_guard l(mutex_);status_.message="Static stereo image (SDR)";}
            while(!stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }else if(isWindowCapture(config.kind)){
            // The captured game keeps focus (the pad follows focus), and Windows then favours its GPU work
            // over the unfocused stereo output: the output missed refreshes in bursts every few seconds,
            // each one a wrong-eye flash in the glasses. Drop only the game's GPU scheduling class while it
            // is captured (its CPU priority is untouched) and restore it when capture ends.
            struct GpuClassGuard {HANDLE process=nullptr;D3DKMT_SCHEDULINGPRIORITYCLASS original=D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL;
                ~GpuClassGuard(){if(process){D3DKMTSetProcessSchedulingPriorityClass(process,original);CloseHandle(process);}}} gameGpu;
            // A VRto3D window belongs to vrserver. This can also be selected
            // through generic Window capture; never lower the whole runtime.
            if(config.kind==SourceKind::Window){DWORD pid=0;GetWindowThreadProcessId(config.window,&pid);
             if(pid&&pid!=GetCurrentProcessId())if(HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_SET_INFORMATION,FALSE,pid)){
                 wchar_t executable[32768]{};DWORD length=DWORD(std::size(executable));
                 const bool known=QueryFullProcessImageNameW(p,0,executable,&length)!=FALSE;
                 const auto basename=known?std::filesystem::path(executable).filename().wstring():std::wstring{};
                 const bool runtime=_wcsicmp(basename.c_str(),L"vrserver.exe")==0||_wcsicmp(basename.c_str(),L"vrcompositor.exe")==0;
                 D3DKMT_SCHEDULINGPRIORITYCLASS current{};
                 if(known&&!runtime&&D3DKMTGetProcessSchedulingPriorityClass(p,&current)==0&&D3DKMTSetProcessSchedulingPriorityClass(p,D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL)==0){gameGpu.process=p;gameGpu.original=current;}
                 else CloseHandle(p);}}
            GpuCapture capture(device.Get(),config);
            while(!stop.stop_requested()){
                if(!IsWindow(config.window)){std::lock_guard l(mutex_);status_.message="Source window closed; holding last complete pair";status_.running=false;break;}
                CapturedPicture picture;
                const bool ready=capture.next(picture,2);
                {std::lock_guard l(mutex_);status_.captureBackend=capture.backend();}
                if(!ready){shortWait();continue;}
                captureAcquiredAt_=picture.acquiredAt;
                if(config.packing==Packing::SideBySide)picture.width&=~1u;else picture.height&=~1u;
                if(picture.width&&picture.height)publish(picture.texture.Get(),picture.width,picture.height,picture.timestamp,picture.encoding,picture.left,picture.top);
                capture.release();
            }
        }else if(config.kind==SourceKind::Vlc){
            runVlc(stop,config,device.Get(),context.Get(),publish);
        }else if(config.kind==SourceKind::Screen){
            runScreen(stop,config,adapterId,device.Get(),context.Get(),[&](ID3D11Texture2D* t,unsigned w,unsigned h,double ts,Encoding e,unsigned l,unsigned top,bool duplicate){publish(t,w,h,ts,e,l,top,duplicate);});
        }
    }catch(const winrt::hresult_error& e){std::lock_guard l(mutex_);status_.message=utf8(e.message().c_str());status_.running=false;}
    catch(const std::exception& e){std::lock_guard l(mutex_);status_.message=e.what();status_.running=false;}
    winrt::uninit_apartment();
}
}
