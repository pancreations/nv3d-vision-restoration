#include "gpu_capture.h"
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace vision {
namespace cap=winrt::Windows::Graphics::Capture;
namespace dx=winrt::Windows::Graphics::DirectX;
struct GpuCapture::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> paddedClient;
    ComPtr<ID3D11RenderTargetView> paddedTarget;
    SourceConfig config;
    ComPtr<IDXGIOutputDuplication> duplication;
    RECT desktop{};
    HMONITOR duplicatedMonitor=nullptr;
    bool acquired=false,havePicture=false,delivered=false;
    double nextRetry=0;
    LARGE_INTEGER frequency{};
    struct FrameSignal {HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);~FrameSignal(){if(event)CloseHandle(event);}};
    std::shared_ptr<FrameSignal> signal;
    winrt::event_token arrivalToken{};
    bool arrivalRegistered=false;
    std::string waiting,note;
    dx::Direct3D11::IDirect3DDevice rt{nullptr};
    cap::Direct3D11CaptureFramePool pool{nullptr};
    cap::GraphicsCaptureSession session{nullptr};
    cap::Direct3D11CaptureFrame frame{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
    ~Impl(){release();if(pool&&arrivalRegistered)pool.FrameArrived(arrivalToken);if(session)session.Close();if(pool)pool.Close();}
    void release(){if(frame){frame.Close();frame=nullptr;}if(acquired){
        const HRESULT hr=duplication->ReleaseFrame();acquired=false;
        if(hr==DXGI_ERROR_ACCESS_LOST){duplication.Reset();nextRetry=0;}
    }}
    RECT client(){RECT r{};POINT origin{};if(!GetClientRect(config.window,&r)||!ClientToScreen(config.window,&origin))throw std::runtime_error("Source window has no client area.");OffsetRect(&r,origin.x,origin.y);return r;}
    bool cropClient(CapturedPicture& picture,const D3D11_TEXTURE2D_DESC& desc){
        const RECT r=client();
        const RECT available{desktop.left,desktop.top,
            std::min(desktop.right,desktop.left+LONG(desc.Width)),std::min(desktop.bottom,desktop.top+LONG(desc.Height))};
        RECT intersection{};
        if(!IntersectRect(&intersection,&r,&available)){
            waiting="source client area is outside the capture display";return false;
        }
        picture.width=unsigned(r.right-r.left);picture.height=unsigned(r.bottom-r.top);
        if(EqualRect(&r,&intersection)){
            picture.left=unsigned(r.left-desktop.left);picture.top=unsigned(r.top-desktop.top);return true;
        }
        // A desktop crop cannot recover off-display pixels. Keep the original
        // client dimensions and eye boundary; clipping the SBS image would move
        // that boundary and feed a strip of one eye to the other.
        D3D11_TEXTURE2D_DESC old{};if(paddedClient)paddedClient->GetDesc(&old);
        if(!paddedClient||old.Width!=picture.width||old.Height!=picture.height||old.Format!=desc.Format){
            paddedTarget.Reset();paddedClient.Reset();
            D3D11_TEXTURE2D_DESC d{};d.Width=picture.width;d.Height=picture.height;
            d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=desc.Format;d.BindFlags=D3D11_BIND_RENDER_TARGET;
            check(device->CreateTexture2D(&d,nullptr,&paddedClient),"Create padded client capture");
            check(device->CreateRenderTargetView(paddedClient.Get(),nullptr,&paddedTarget),"Client capture target");
        }
        const float black[]{0,0,0,1};context->ClearRenderTargetView(paddedTarget.Get(),black);
        const D3D11_BOX box{UINT(intersection.left-desktop.left),UINT(intersection.top-desktop.top),0,
            UINT(intersection.right-desktop.left),UINT(intersection.bottom-desktop.top),1};
        context->CopySubresourceRegion(paddedClient.Get(),0,UINT(intersection.left-r.left),UINT(intersection.top-r.top),0,picture.texture.Get(),0,&box);
        picture.texture=paddedClient;picture.left=picture.top=0;
        note="pixels outside capture display padded black";return true;
    }
    void windowsCapture(){
        duplication.Reset();
        if(!cap::GraphicsCaptureSession::IsSupported())throw std::runtime_error("Windows Graphics Capture is unavailable.");
        auto interop=winrt::get_activation_factory<cap::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();cap::GraphicsCaptureItem item{nullptr};
        if(config.window)check(interop->CreateForWindow(config.window,winrt::guid_of<cap::GraphicsCaptureItem>(),winrt::put_abi(item)),"Capture window");
        else check(interop->CreateForMonitor(config.monitor,winrt::guid_of<cap::GraphicsCaptureItem>(),winrt::put_abi(item)),"Capture monitor");
        ComPtr<IDXGIDevice> dev;check(device.As(&dev),"Capture device");winrt::com_ptr<IInspectable> inspectable;
        check(CreateDirect3D11DeviceFromDXGIDevice(dev.Get(),inspectable.put()),"Capture WinRT device");rt=inspectable.as<dx::Direct3D11::IDirect3DDevice>();size=item.Size();
        pool=cap::Direct3D11CaptureFramePool::CreateFreeThreaded(rt,dx::DirectXPixelFormat::R16G16B16A16Float,2,size);signal=std::make_shared<FrameSignal>();check(signal->event?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Capture frame event");
        arrivalToken=pool.FrameArrived([ready=signal](auto&&,auto&&){SetEvent(ready->event);});arrivalRegistered=true;
        session=pool.CreateCaptureSession(item);session.IsCursorCaptureEnabled(false);
        try{session.IsBorderRequired(false);}catch(...){}
        session.StartCapture();
    }
    void duplicate(){
        HMONITOR monitor=config.window?MonitorFromWindow(config.window,MONITOR_DEFAULTTONEAREST):config.monitor;
        duplicatedMonitor=monitor;
        ComPtr<IDXGIDevice> dev;check(device.As(&dev),"Duplication device");ComPtr<IDXGIAdapter> adapter;check(dev->GetAdapter(&adapter),"Duplication adapter");
        for(UINT i=0;;++i){
            ComPtr<IDXGIOutput> output;HRESULT hr=adapter->EnumOutputs(i,&output);if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Enumerate capture outputs");
            DXGI_OUTPUT_DESC desc{};check(output->GetDesc(&desc),"Capture output description");if(desc.Monitor!=monitor)continue;
            if(desc.Rotation!=DXGI_MODE_ROTATION_IDENTITY)throw std::runtime_error("Rotated display requires Windows capture");desktop=desc.DesktopCoordinates;
            ComPtr<IDXGIOutput5> modern;
            // DuplicateOutput1 can return an application's 8-bit scanout even
            // with HDR enabled. Interpret the acquired format, not display mode.
            // https://learn.microsoft.com/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1
            const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT};
            if(SUCCEEDED(output.As(&modern)))hr=modern->DuplicateOutput1(device.Get(),0,UINT(std::size(formats)),formats,&duplication);
            else {ComPtr<IDXGIOutput1> legacy;check(output.As(&legacy),"Desktop duplication output");hr=legacy->DuplicateOutput(device.Get(),&duplication);}
            if(hr==DXGI_ERROR_UNSUPPORTED||hr==E_ACCESSDENIED||hr==DXGI_ERROR_NOT_CURRENTLY_AVAILABLE||hr==DXGI_ERROR_SESSION_DISCONNECTED){
                waiting=hr==DXGI_ERROR_UNSUPPORTED?"waiting for a supported desktop mode":hr==E_ACCESSDENIED?"waiting for desktop access":hr==DXGI_ERROR_NOT_CURRENTLY_AVAILABLE?"waiting for a free duplication session":"waiting for the desktop session";
                nextRetry=qpc()+.25;return;
            }
            check(hr,"Start desktop duplication");havePicture=false;waiting.clear();note.clear();return;
        }
        throw std::runtime_error("DXGI capture display belongs to another GPU. Choose Windows Graphics Capture, or move the source to the stereo output GPU.");
    }
};
GpuCapture::GpuCapture(ID3D11Device* device,const SourceConfig& config):impl_(std::make_unique<Impl>()){
    impl_->device=device;device->GetImmediateContext(&impl_->context);impl_->config=config;QueryPerformanceFrequency(&impl_->frequency);
    if(config.kind==SourceKind::VrTo3D){
        if(!IsWindow(config.window))throw std::runtime_error("Select the VRto3D / SteamVR Headset window first.");
        impl_->config.captureMethod=CaptureMethod::WindowsGraphicsCapture;
    }
    // A screen source may retain the last window selection in the UI. Only
    // window sources may use it to choose a capture target or crop geometry.
    if(config.kind==SourceKind::Screen)impl_->config.window=nullptr;
    if(impl_->config.captureMethod==CaptureMethod::WindowsGraphicsCapture)impl_->windowsCapture();
    else impl_->duplicate();
}
GpuCapture::~GpuCapture()=default;
void GpuCapture::release(){impl_->release();}
std::string GpuCapture::backend()const{
    const auto& s=*impl_;
    const std::string name=s.config.captureMethod==CaptureMethod::DesktopDuplication?"DXGI Desktop Duplication":"Windows Graphics Capture";
    if(!s.waiting.empty())return name+" ("+s.waiting+(s.delivered?"; holding last frame)":"; no frame received)");
    return name+(s.note.empty()?std::string{}:" ("+s.note+")");
}
bool GpuCapture::next(CapturedPicture& picture,unsigned timeoutMs){
    auto& s=*impl_;s.release();picture={};
    if(s.config.window){
        if(!IsWindow(s.config.window)){s.waiting="source window closed; select a running window";return false;}
        if(IsIconic(s.config.window)){s.waiting="source window is minimized; restore it to resume capture";return false;}
        if(!IsWindowVisible(s.config.window)){s.waiting="source window is hidden; show it to resume capture";return false;}
    }
    if(s.config.captureMethod==CaptureMethod::DesktopDuplication){
            // Follow a window moved to another display instead of retaining a
            // crop on the original monitor indefinitely.
            if(s.config.window&&MonitorFromWindow(s.config.window,MONITOR_DEFAULTTONEAREST)!=s.duplicatedMonitor){s.duplication.Reset();s.nextRetry=0;}
            if(!s.duplication){if(qpc()<s.nextRetry)return false;s.duplicate();if(!s.duplication)return false;}
            DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
            HRESULT hr=s.duplication->AcquireNextFrame(timeoutMs,&info,&resource);if(hr==DXGI_ERROR_WAIT_TIMEOUT)return false;
            if(hr==DXGI_ERROR_ACCESS_LOST||hr==E_ACCESSDENIED||hr==DXGI_ERROR_SESSION_DISCONNECTED){s.duplication.Reset();s.nextRetry=qpc()+.25;s.waiting="waiting for desktop capture to resume";return false;}
            check(hr,"Acquire desktop frame");s.acquired=true;picture.acquiredAt=qpc();
            if(!info.LastPresentTime.QuadPart&&s.havePicture){s.release();return false;}
            s.waiting.clear();s.note.clear();
            check(resource.As(&picture.texture),"Desktop texture");D3D11_TEXTURE2D_DESC desc{};picture.texture->GetDesc(&desc);
            const UINT desktopWidth=UINT(s.desktop.right-s.desktop.left),desktopHeight=UINT(s.desktop.bottom-s.desktop.top);
            if(desc.Width!=desktopWidth||desc.Height!=desktopHeight){
                // Enumeration and acquisition can straddle a mode change. The
                // old desktop coordinates cannot safely address this surface.
                s.release();picture={};s.duplication.Reset();s.nextRetry=qpc()+.25;
                s.waiting="capture size changed ("+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+
                    ", expected "+std::to_string(desktopWidth)+"x"+std::to_string(desktopHeight)+
                    "); refreshing display geometry";return false;
            }
            picture.width=desc.Width;picture.height=desc.Height;picture.encoding=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?Encoding::LinearScRGB:Encoding::SRGB;
            picture.timestamp=info.LastPresentTime.QuadPart?double(info.LastPresentTime.QuadPart)/double(s.frequency.QuadPart):qpc();
            if(s.config.window&&!s.cropClient(picture,desc)){s.release();picture={};return false;}
            if(info.ProtectedContentMaskedOut)s.note+=(s.note.empty()?"":"; ")+std::string("Windows masked protected content");
            s.havePicture=true;s.delivered=true;return true;
    }
    s.waiting.clear();
    s.frame=s.pool.TryGetNextFrame();
    if(!s.frame&&timeoutMs){WaitForSingleObject(s.signal->event,timeoutMs);s.frame=s.pool.TryGetNextFrame();}
    if(!s.frame)return false;
    picture.acquiredAt=qpc();
    if(auto newer=s.pool.TryGetNextFrame()){s.frame.Close();s.frame=newer;}
    auto content=s.frame.ContentSize();if(content.Width!=s.size.Width||content.Height!=s.size.Height){s.release();if(content.Width>0&&content.Height>0){s.size=content;s.pool.Recreate(s.rt,dx::DirectXPixelFormat::R16G16B16A16Float,2,s.size);}return false;}
    auto access=s.frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();check(access->GetInterface(IID_PPV_ARGS(&picture.texture)),"Windows capture texture");
    picture.width=unsigned(s.size.Width);picture.height=unsigned(s.size.Height);picture.timestamp=double(s.frame.SystemRelativeTime().count())/1e7;picture.encoding=Encoding::LinearScRGB;
    if(s.config.window){RECT bounds{},r=s.client();if(SUCCEEDED(DwmGetWindowAttribute(s.config.window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))){
        long l=r.left-bounds.left,t=r.top-bounds.top;if(l>=0&&t>=0&&r.right-bounds.left<=s.size.Width&&r.bottom-bounds.top<=s.size.Height){picture.left=unsigned(l);picture.top=unsigned(t);picture.width=unsigned(r.right-r.left);picture.height=unsigned(r.bottom-r.top);}}}
    s.delivered=true;return true;
}
}
