#include "gpu_capture.h"
#include <winrt/base.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
using namespace vision;
namespace {
constexpr unsigned fixtureWidth=320,fixtureHeight=180;
enum class Color { Red,Green,Blue,White,Magenta,Black };
using Pattern=std::array<Color,4>;
Pattern palette(unsigned version){
    constexpr Pattern colors{Color::Red,Color::Green,Color::Blue,Color::White};
    Pattern result{};for(unsigned i=0;i<4;++i)result[i]=colors[(i+version)%4];return result;
}
COLORREF colorRef(Color color){
    switch(color){case Color::Red:return RGB(255,0,0);case Color::Green:return RGB(0,255,0);
    case Color::Blue:return RGB(0,0,255);case Color::White:return RGB(255,255,255);
    case Color::Magenta:return RGB(255,0,255);default:return RGB(0,0,0);}
}
void pumpMessages(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
struct Fixture {
    HWND window=nullptr;Pattern pattern=palette(0);
    ~Fixture(){if(window)DestroyWindow(window);}
    static LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM w,LPARAM l){
        if(message==WM_NCCREATE)SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* fixture=reinterpret_cast<Fixture*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(message==WM_PAINT&&fixture){
            PAINTSTRUCT paint{};HDC dc=BeginPaint(h,&paint);RECT client{};GetClientRect(h,&client);
            for(unsigned i=0;i<4;++i){RECT r{LONG((i%2)*client.right/2),LONG((i/2)*client.bottom/2),LONG((i%2+1)*client.right/2),LONG((i/2+1)*client.bottom/2)};
                HBRUSH brush=CreateSolidBrush(colorRef(fixture->pattern[i]));FillRect(dc,&r,brush);DeleteObject(brush);}
            EndPaint(h,&paint);return 0;
        }
        return DefWindowProcW(h,message,w,l);
    }
    void create(const wchar_t* title,LONG left,LONG top){
        WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"VisionCapturePixelFixture";
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Register capture fixture failed");
        window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,title,WS_POPUP|WS_VISIBLE,left,top,fixtureWidth,fixtureHeight,nullptr,nullptr,cls.hInstance,this);
        if(!window)throw std::runtime_error("Create capture fixture failed");paint(pattern);
    }
    void paint(Pattern value){pattern=value;InvalidateRect(window,nullptr,FALSE);UpdateWindow(window);pumpMessages();check(DwmFlush(),"Flush fixture paint");}
    void close(){if(window){DestroyWindow(window);window=nullptr;}}
};
float decodeHalf(uint16_t bits){
    const unsigned exponent=(bits>>10)&31,mantissa=bits&1023;
    float value=exponent==31?(mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity()):
        exponent?std::ldexp(float(1024+mantissa),int(exponent)-25):std::ldexp(float(mantissa),-24);
    return bits&0x8000?-value:value;
}
using Pixel=std::array<float,3>;
using Samples=std::array<Pixel,4>;
std::string describeRect(const RECT& rect){
    return std::to_string(rect.left)+","+std::to_string(rect.top)+","+std::to_string(rect.right)+","+std::to_string(rect.bottom);
}
std::string describeGeometry(const CapturedPicture& picture){
    D3D11_TEXTURE2D_DESC desc{};if(picture.texture)picture.texture->GetDesc(&desc);
    return "actual crop="+std::to_string(picture.width)+"x"+std::to_string(picture.height)+" origin="+std::to_string(picture.left)+","+std::to_string(picture.top)+
        " texture="+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+" format="+std::to_string(desc.Format);
}
std::string describeTarget(const Display& display,HWND source){
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(display.monitor,&monitor);
    RECT client{};POINT origin{};GetClientRect(source,&client);ClientToScreen(source,&origin);OffsetRect(&client,origin.x,origin.y);
    return "selected="+utf8(display.gdiName)+" enumerated rect="+describeRect(display.rect)+" mode="+std::to_string(display.width)+"x"+std::to_string(display.height)+
        " current monitor rect="+describeRect(monitor.rcMonitor)+" source client="+describeRect(client)+" source DPI="+std::to_string(GetDpiForWindow(source))+
        " thread DPI awareness="+std::to_string(int(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext())));
}
Samples samplePicture(ID3D11Device* device,ID3D11DeviceContext* context,const CapturedPicture& picture){
    if(!picture.texture||!picture.width||!picture.height)throw std::runtime_error("Missing captured texture or geometry; "+describeGeometry(picture));
    D3D11_TEXTURE2D_DESC desc{};picture.texture->GetDesc(&desc);
    if(picture.left+picture.width>desc.Width||picture.top+picture.height>desc.Height)throw std::runtime_error("Captured crop exceeds texture bounds; "+describeGeometry(picture));
    if(desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)
        throw std::runtime_error("Unexpected capture format: "+std::to_string(desc.Format));
    desc.Width=picture.width;desc.Height=picture.height;desc.MipLevels=desc.ArraySize=1;desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"Create pixel staging");
    D3D11_BOX box{picture.left,picture.top,0,picture.left+picture.width,picture.top+picture.height,1};context->CopySubresourceRegion(staging.Get(),0,0,0,0,picture.texture.Get(),0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read capture pixels");
    Samples result{};
    for(unsigned i=0;i<4;++i){const unsigned x=picture.width*(1+2*(i%2))/4,y=picture.height*(1+2*(i/2))/4;
        const auto* row=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch;
        if(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){const auto* pixel=reinterpret_cast<const uint16_t*>(row)+size_t(x)*4;for(unsigned c=0;c<3;++c)result[i][c]=decodeHalf(pixel[c]);}
        else {const auto* pixel=row+size_t(x)*4;const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;result[i]={pixel[bgra?2:0]/255.f,pixel[1]/255.f,pixel[bgra?0:2]/255.f};}
    }
    context->Unmap(staging.Get(),0);return result;
}
bool matches(const Pixel& pixel,Color expected){
    if(!std::all_of(pixel.begin(),pixel.end(),[](float value){return std::isfinite(value);}))return false;
    const float peak=*std::max_element(pixel.begin(),pixel.end());
    if(expected==Color::Black)return std::all_of(pixel.begin(),pixel.end(),[](float value){return std::abs(value)<.04f;});
    if(peak<.15f)return false;
    const std::array<bool,3> high{expected==Color::Red||expected==Color::White||expected==Color::Magenta,
        expected==Color::Green||expected==Color::White,expected==Color::Blue||expected==Color::White||expected==Color::Magenta};
    for(unsigned c=0;c<3;++c)if(high[c]?pixel[c]<peak*.7f:std::abs(pixel[c])>peak*.2f+.03f)return false;
    return true;
}
bool matches(const Samples& samples,const Pattern& expected){for(unsigned i=0;i<4;++i)if(!matches(samples[i],expected[i]))return false;return true;}
std::string describe(const Samples& samples){std::ostringstream out;for(const auto& p:samples)out<<" ["<<p[0]<<","<<p[1]<<","<<p[2]<<"]";return out.str();}
void testPixelAssertions(){
    if(decodeHalf(0x3c00)!=1.f||decodeHalf(0xbc00)!=-1.f||decodeHalf(0)!=0.f||!std::isnan(decodeHalf(0x7e00))||!std::isinf(decodeHalf(0x7c00)))throw std::runtime_error("FP16 decoder failed");
    if(matches(Pixel{0,0,0},Color::White)||matches(Pixel{1,1,1},Color::Red)||matches(Pixel{-1,-1,-1},Color::White)||matches(Pixel{decodeHalf(0x7e00),1,1},Color::White))throw std::runtime_error("Pixel assertions accepted invalid source content");
    if(!matches(Pixel{3,0,0},Color::Red)||!matches(Pixel{2,2,2},Color::White))throw std::runtime_error("Pixel assertions rejected HDR-scaled source content");
}
double awaitPattern(GpuCapture& capture,ID3D11Device* device,ID3D11DeviceContext* context,const Pattern& expected,
    const Display& display,HWND source,bool screen,double newerThan,const char* scenario){
    const double deadline=qpc()+5;Samples last{};bool sawFrame=false;
    while(qpc()<deadline){
        pumpMessages();CapturedPicture picture;if(!capture.next(picture,10)){Sleep(2);continue;}
        const std::string geometry=std::string(scenario)+": "+capture.backend()+"; "+describeGeometry(picture)+"; "+describeTarget(display,source);
        if(!picture.texture)throw std::runtime_error("Capture returned a null texture; "+geometry);
        const double timestamp=picture.timestamp;
        if(timestamp<=0||picture.acquiredAt<=0||picture.acquiredAt>qpc())throw std::runtime_error("Invalid capture timestamps");
        D3D11_TEXTURE2D_DESC desc{};picture.texture->GetDesc(&desc);
        const Encoding expectedEncoding=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?Encoding::LinearScRGB:Encoding::SRGB;
        if(picture.encoding!=expectedEncoding)throw std::runtime_error("Capture encoding disagrees with the acquired pixel format");
        if(capture.backend().find("Windows Graphics Capture")==0&&desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)throw std::runtime_error("Windows Graphics Capture lost its requested FP16 format");
        if(screen){
            if(picture.width!=unsigned(display.rect.right-display.rect.left)||picture.height!=unsigned(display.rect.bottom-display.rect.top)||picture.left||picture.top)
                throw std::runtime_error("Screen capture did not cover selected monitor; expected="+std::to_string(display.rect.right-display.rect.left)+"x"+std::to_string(display.rect.bottom-display.rect.top)+" origin=0,0; "+geometry);
            POINT origin{};check(ClientToScreen(source,&origin)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Locate screen fixture");
            picture.left=unsigned(origin.x-display.rect.left);picture.top=unsigned(origin.y-display.rect.top);picture.width=fixtureWidth;picture.height=fixtureHeight;
        }else if(picture.width!=fixtureWidth||picture.height!=fixtureHeight)throw std::runtime_error("Window capture changed client dimensions; expected="+std::to_string(fixtureWidth)+"x"+std::to_string(fixtureHeight)+"; "+geometry);
        try{last=samplePicture(device,context,picture);}catch(const std::exception& e){throw std::runtime_error(std::string(e.what())+"; "+geometry);}sawFrame=true;capture.release();
        if(timestamp>newerThan&&matches(last,expected))return timestamp;
        Sleep(2);
    }
    throw std::runtime_error(std::string(scenario)+": "+capture.backend()+"; "+(sawFrame?"last RGB samples"+describe(last):"no frame delivered"));
}
double awaitPublishedPattern(StereoSource& source,ID3D11Device* device,ID3D11DeviceContext* context,const Pattern& expected,uint64_t newerThan,uint64_t& pairId,
    Packing expectedPacking=Packing::SideBySide,float expectedAspect=-1.f){
    const double deadline=qpc()+5;Samples last{};bool sawFrame=false;
    while(qpc()<deadline){
        pumpMessages();auto frame=source.latest();if(!frame||frame->pairId<=newerThan){if(!source.status().running)break;Sleep(2);continue;}
        if(frame->width!=fixtureWidth||frame->height!=fixtureHeight){CapturedPicture published;published.texture=frame->texture;published.width=frame->width;published.height=frame->height;
            throw std::runtime_error("Published capture has wrong dimensions; expected="+std::to_string(fixtureWidth)+"x"+std::to_string(fixtureHeight)+"; "+source.status().captureBackend+"; "+describeGeometry(published));}
        if(frame->packing!=expectedPacking)throw std::runtime_error("Published capture lost the selected stereo packing");
        if(expectedAspect>=0&&(!std::isfinite(frame->eyeAspect)||std::abs(frame->eyeAspect-expectedAspect)>.0001f))
            throw std::runtime_error("Published capture has wrong eye aspect; expected="+std::to_string(expectedAspect)+" actual="+std::to_string(frame->eyeAspect));
        if(frame->captureAcquiredAt<=0||frame->publishedAt<frame->captureAcquiredAt||frame->publishedAt>qpc())throw std::runtime_error("Invalid capture publication timestamps");
        CapturedPicture picture;check(device->OpenSharedResource(frame->sharedHandle,IID_PPV_ARGS(&picture.texture)),"Open published capture");picture.width=frame->width;picture.height=frame->height;
        ComPtr<IDXGIKeyedMutex> key;check(picture.texture.As(&key),"Published capture mutex");
        const HRESULT acquired=key->AcquireSync(0,100);if(acquired==HRESULT(WAIT_TIMEOUT)){Sleep(2);continue;}if(acquired!=S_OK)throw std::runtime_error("Acquire published capture mutex failed");
        try{last=samplePicture(device,context,picture);}catch(const std::exception& e){key->ReleaseSync(0);throw std::runtime_error(std::string(e.what())+"; "+source.status().captureBackend);}check(key->ReleaseSync(0),"Release published capture mutex");sawFrame=true;
        if(matches(last,expected)){pairId=frame->pairId;return frame->timestamp;}Sleep(2);
    }
    const auto status=source.status();throw std::runtime_error("Published source pixels did not update: "+status.message+" "+status.captureBackend+(sawFrame?describe(last):"; no published frame"));
}
void testVrTo3DCapture(ID3D11Device* device,ID3D11DeviceContext* context,const Display& display,Fixture& sourceWindow){
    SourceConfig config;config.kind=SourceKind::VrTo3D;config.window=sourceWindow.window;
    // The VR route must stay isolated even when the general capture preference
    // uses desktop duplication. An ordinary opaque cover exposes a regression:
    // desktop capture would publish its magenta pixels instead of either eye.
    config.captureMethod=CaptureMethod::DesktopDuplication;
    RECT area{};check(GetWindowRect(sourceWindow.window,&area)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Locate VR capture fixture");
    Fixture cover;cover.pattern.fill(Color::Magenta);cover.create(L"VRto3D opaque overlay fixture",area.left,area.top);
    {
        GpuCapture capture(device,config);
        if(capture.backend().find("Windows Graphics Capture")!=0)throw std::runtime_error("VRto3D raw capture did not enforce isolated window capture");
        sourceWindow.paint(palette(0));
        const double first=awaitPattern(capture,device,context,palette(0),display,sourceWindow.window,false,0,"Covered VRto3D initial pixels");
        sourceWindow.paint(palette(1));
        awaitPattern(capture,device,context,palette(1),display,sourceWindow.window,false,first,"Covered VRto3D changed pixels");
    }
    for(auto packing:{Packing::SideBySide,Packing::TopBottom})for(auto format:{SbsFormat::Half,SbsFormat::Full}){
        config.packing=packing;config.sbsFormat=format;
        const float packedAspect=float(fixtureWidth)/float(fixtureHeight);
        const float eyeAspect=format==SbsFormat::Half?packedAspect:packing==Packing::SideBySide?packedAspect/2.f:packedAspect*2.f;
        sourceWindow.paint(palette(2));StereoSource source;source.start(config,display.adapterLuid);uint64_t pairId=0;
        const double timestamp=awaitPublishedPattern(source,device,context,palette(2),0,pairId,packing,eyeAspect);
        sourceWindow.paint(palette(3));
        const double updated=awaitPublishedPattern(source,device,context,palette(3),pairId,pairId,packing,eyeAspect);
        if(updated<=timestamp)throw std::runtime_error("Covered VRto3D source timestamp did not advance with changed pixels");
        if(source.status().captureBackend.find("Windows Graphics Capture")!=0)throw std::runtime_error("VRto3D source publication did not enforce isolated window capture");
        std::cout<<"VRto3D "<<(packing==Packing::SideBySide?"SBS":"TAB")<<" "<<(format==SbsFormat::Half?"Half":"Full")
            <<": changing pixels beneath an opaque cover, packing and eye aspect "<<eyeAspect<<" verified"<<std::endl;
        const double stopStart=qpc();source.stop();if(qpc()-stopStart>.5)throw std::runtime_error("VRto3D capture worker did not stop promptly");
    }
}
} // namespace
static void diagnoseWindows(const std::string& titleFilter){
    std::vector<HWND> windows;
    EnumWindows([](HWND h,LPARAM p)->BOOL{reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);return TRUE;},reinterpret_cast<LPARAM>(&windows));
    auto describe=[](HWND h,const char* role){
        wchar_t title[512]{},className[256]{},exe[32768]{};GetWindowTextW(h,title,512);GetClassNameW(h,className,256);
        DWORD pid=0;GetWindowThreadProcessId(h,&pid);HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(process){DWORD chars=DWORD(std::size(exe));QueryFullProcessImageNameW(process,0,exe,&chars);CloseHandle(process);}
        RECT bounds{};if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))))GetWindowRect(h,&bounds);
        RECT client{};POINT origin{};GetClientRect(h,&client);ClientToScreen(h,&origin);OffsetRect(&client,origin.x,origin.y);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&monitor);
        DWORD cloaked=0,affinity=0,layerFlags=0;BYTE alpha=255;COLORREF key=0;
        DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));GetWindowDisplayAffinity(h,&affinity);
        const bool layerKnown=GetLayeredWindowAttributes(h,&key,&alpha,&layerFlags)!=FALSE;
        HRGN region=CreateRectRgn(0,0,0,0);const int regionType=GetWindowRgn(h,region);DeleteObject(region);
        std::cout<<role<<" hwnd=0x"<<std::hex<<uintptr_t(h)<<" style=0x"<<GetWindowLongPtrW(h,GWL_STYLE)<<" exstyle=0x"<<GetWindowLongPtrW(h,GWL_EXSTYLE)<<std::dec
            <<" pid="<<pid<<" visible="<<IsWindowVisible(h)<<" iconic="<<IsIconic(h)<<" cloaked="<<cloaked<<" affinity="<<affinity
            <<" bounds="<<bounds.left<<","<<bounds.top<<","<<bounds.right<<","<<bounds.bottom
            <<" client="<<client.left<<","<<client.top<<","<<client.right<<","<<client.bottom
            <<" monitor="<<monitor.rcMonitor.left<<","<<monitor.rcMonitor.top<<","<<monitor.rcMonitor.right<<","<<monitor.rcMonitor.bottom
            <<" layerKnown="<<layerKnown<<" layerAlpha="<<unsigned(alpha)<<" layerFlags="<<layerFlags<<" regionType="<<regionType
            <<" class=\""<<utf8(className)<<"\" title=\""<<utf8(title)<<"\" exe=\""<<utf8(exe)<<"\"\n";
    };
    if(titleFilter.empty()){for(HWND h:windows)if(IsWindowVisible(h))describe(h,"window");return;}
    for(HWND target:windows){wchar_t title[512]{};GetWindowTextW(target,title,512);if(utf8(title).find(titleFilter)==std::string::npos)continue;
        describe(target,"target");RECT client{};POINT origin{};GetClientRect(target,&client);ClientToScreen(target,&origin);OffsetRect(&client,origin.x,origin.y);
        for(HWND h=GetWindow(target,GW_HWNDPREV);h;h=GetWindow(h,GW_HWNDPREV)){
            if(!IsWindowVisible(h)||IsIconic(h))continue;
            RECT bounds{},intersection{};if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))))GetWindowRect(h,&bounds);
            if(IntersectRect(&intersection,&client,&bounds))describe(h,"above");
        }
    }
}
int main(int argc,char** argv){
    const BOOL dpiSet=SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD dpiError=dpiSet?ERROR_SUCCESS:GetLastError();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        testPixelAssertions();
        std::vector<CaptureMethod> methods{CaptureMethod::DesktopDuplication,CaptureMethod::WindowsGraphicsCapture};
        unsigned displayIndex=0;bool screenOnly=false,vrTo3DOnly=false;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];
            if(arg=="--dxgi-only")methods={CaptureMethod::DesktopDuplication};
            else if(arg=="--wgc-only")methods={CaptureMethod::WindowsGraphicsCapture};
            else if(arg=="--display"&&i+1<argc)displayIndex=unsigned(std::stoul(argv[++i]));
            else if(arg=="--screen-target-only")screenOnly=true;
            else if(arg=="--vrto3d-only")vrTo3DOnly=true;
            else if(arg=="--pixel-assertions-only"){std::cout<<"Capture pixel assertions passed\n";return 0;}
            else if(arg=="--diagnose-windows"){diagnoseWindows(i+1<argc?argv[i+1]:"");return 0;}
            else throw std::runtime_error("Unknown capture test option: "+arg);
        }
        const auto displays=enumerateDisplays();if(displays.empty()){std::cout<<"No display available; integration skipped\n";return 77;}
        if(displayIndex>=displays.size())throw std::runtime_error("Capture display index is out of range");
        const auto& display=displays[displayIndex];const auto& area=display.rect;
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
        ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(display.adapterLuid,IID_PPV_ARGS(&adapter)),"Adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Device");
        std::cout<<"Capture display "<<displayIndex<<": "<<display.name<<" GPU "<<display.gpu<<" HDR "<<display.hdrEnabled
            <<" rect="<<describeRect(display.rect)<<" mode="<<display.width<<"x"<<display.height
            <<" SetProcessDpiAwarenessContext="<<dpiSet<<" error="<<dpiError<<" thread awareness="<<int(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()))<<std::endl;
        Fixture sourceWindow;sourceWindow.create(L"Capture changing pixel fixture",area.left+30,area.top+30);
        if(vrTo3DOnly){testVrTo3DCapture(device.Get(),context.Get(),display,sourceWindow);std::cout<<"VRto3D capture integration passed\n";return 0;}
        SourceConfig config;config.kind=SourceKind::Screen;config.monitor=display.monitor;
        // Screen capture must ignore a stale window selection for every backend.
        config.window=reinterpret_cast<HWND>(uintptr_t(1));
        for(auto method:methods){
            config.captureMethod=method;sourceWindow.paint(palette(0));GpuCapture capture(device.Get(),config);
            const double first=awaitPattern(capture,device.Get(),context.Get(),palette(0),display,sourceWindow.window,true,0,"Initial screen pixels");
            sourceWindow.paint(palette(1));awaitPattern(capture,device.Get(),context.Get(),palette(1),display,sourceWindow.window,true,first,"Screen pixels did not change");
            std::cout<<capture.backend()<<": selected monitor pixels and changed frame verified despite stale window handle"<<std::endl;
        }
        if(screenOnly)return 0;
        config.kind=SourceKind::Window;config.window=sourceWindow.window;
        for(auto method:methods){
            config.captureMethod=method;sourceWindow.paint(palette(0));GpuCapture capture(device.Get(),config);
            double timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(0),display,sourceWindow.window,false,0,"Initial window pixels");
            sourceWindow.paint(palette(1));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(1),display,sourceWindow.window,false,timestamp,"Uncovered source update");
            std::cout<<capture.backend()<<": changing client pixels verified"<<std::endl;
            Fixture cover;cover.pattern.fill(Color::Magenta);cover.create(L"Capture magenta cover fixture",area.left+30,area.top+30);
            if(method==CaptureMethod::DesktopDuplication){
                // Desktop duplication contains ordinary covering windows. It must
                // publish those real desktop pixels instead of freezing a cached crop.
                timestamp=awaitPattern(capture,device.Get(),context.Get(),cover.pattern,display,sourceWindow.window,false,timestamp,"Ordinary cover pixels");
                cover.paint(Pattern{Color::Blue,Color::Blue,Color::Blue,Color::Blue});
                timestamp=awaitPattern(capture,device.Get(),context.Get(),cover.pattern,display,sourceWindow.window,false,timestamp,"Ordinary cover update");
                std::cout<<capture.backend()<<": ordinary cover pixels and updates verified"<<std::endl;
                check(SetWindowDisplayAffinity(cover.window,WDA_EXCLUDEFROMCAPTURE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Exclude cover fixture from capture");
                // Change the source while the excluded overlay stays in place.
                // Receiving its old pixels or a new timestamp alone cannot pass.
                sourceWindow.paint(palette(2));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(2),display,sourceWindow.window,false,timestamp,"Source update beneath excluded cover");
                sourceWindow.paint(palette(3));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(3),display,sourceWindow.window,false,timestamp,"Second source update beneath excluded cover");
                std::cout<<capture.backend()<<": two changed source patterns beneath excluded cover verified"<<std::endl;
            }else{
                sourceWindow.paint(palette(2));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(2),display,sourceWindow.window,false,timestamp,"Covered WGC source update");
                std::cout<<capture.backend()<<": changed source pixels beneath ordinary cover verified"<<std::endl;
            }
            cover.close();sourceWindow.paint(palette(0));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(0),display,sourceWindow.window,false,timestamp,"Cover removal recovery");
            if(method==CaptureMethod::DesktopDuplication){
                Fixture emptyHost;
                emptyHost.window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,L"STATIC",L"Capture empty composition host fixture",WS_POPUP|WS_VISIBLE,area.left+30,area.top+30,fixtureWidth,fixtureHeight,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!emptyHost.window)throw std::runtime_error("Create empty composition host failed");
                sourceWindow.paint(palette(1));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(1),display,sourceWindow.window,false,timestamp,"Empty composition host blocked changed pixels");
                sourceWindow.paint(palette(2));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(2),display,sourceWindow.window,false,timestamp,"Empty composition host froze changed pixels");
                emptyHost.close();
                std::cout<<capture.backend()<<": empty composition host did not suppress changed source pixels"<<std::endl;
                // Preserve the complete source client coordinate space: clipping
                // must not shift the center separating left and right stereo eyes.
                check(SetWindowPos(sourceWindow.window,HWND_TOPMOST,area.left-120,area.top+30,0,0,SWP_NOSIZE|SWP_NOACTIVATE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Move source across display edge");
                sourceWindow.paint(palette(1));auto padded=palette(1);padded[0]=padded[2]=Color::Black;
                timestamp=awaitPattern(capture,device.Get(),context.Get(),padded,display,sourceWindow.window,false,timestamp,"Partial monitor source padding");
                check(SetWindowPos(sourceWindow.window,HWND_TOPMOST,area.left+30,area.top+30,0,0,SWP_NOSIZE|SWP_NOACTIVATE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Restore source position");
                sourceWindow.paint(palette(2));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(2),display,sourceWindow.window,false,timestamp,"Display edge recovery");
                check(SetWindowPos(sourceWindow.window,HWND_TOPMOST,area.right-LONG(fixtureWidth)+120,area.bottom-LONG(fixtureHeight)+60,0,0,SWP_NOSIZE|SWP_NOACTIVATE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Move source across right and bottom display edges");
                sourceWindow.paint(palette(3));auto bottomRightPadded=palette(3);bottomRightPadded[1]=bottomRightPadded[2]=bottomRightPadded[3]=Color::Black;
                timestamp=awaitPattern(capture,device.Get(),context.Get(),bottomRightPadded,display,sourceWindow.window,false,timestamp,"Right and bottom monitor source padding");
                check(SetWindowPos(sourceWindow.window,HWND_TOPMOST,area.left+30,area.top+30,0,0,SWP_NOSIZE|SWP_NOACTIVATE)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Restore source after right and bottom edge overhang");
                sourceWindow.paint(palette(0));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(0),display,sourceWindow.window,false,timestamp,"Right and bottom display edge recovery");
                std::cout<<capture.backend()<<": pixels outside capture display padded black; client coordinates and dimensions preserved"<<std::endl;
            }
            CapturedPicture picture;
            ShowWindow(sourceWindow.window,SW_MINIMIZE);if(capture.next(picture))throw std::runtime_error("Minimized source published a frame");
            if(capture.backend().find("source window is minimized")==std::string::npos)throw std::runtime_error("Capture did not identify a minimized source");
            ShowWindow(sourceWindow.window,SW_SHOWNOACTIVATE);SetWindowPos(sourceWindow.window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            sourceWindow.paint(palette(3));timestamp=awaitPattern(capture,device.Get(),context.Get(),palette(3),display,sourceWindow.window,false,timestamp,"Minimized source recovery");
            ShowWindow(sourceWindow.window,SW_HIDE);if(capture.next(picture))throw std::runtime_error("Hidden source published a frame");
            if(capture.backend().find("source window is hidden")==std::string::npos)throw std::runtime_error("Capture did not identify a hidden source");
            const auto visibleWindows=captureWindows(nullptr);
            if(std::any_of(visibleWindows.begin(),visibleWindows.end(),[&](const auto& w){return w.first==sourceWindow.window;}))throw std::runtime_error("Default window enumeration included a hidden source");
            const auto selectedWindows=captureWindows(nullptr,sourceWindow.window);
            if(std::none_of(selectedWindows.begin(),selectedWindows.end(),[&](const auto& w){return w.first==sourceWindow.window&&w.second.find("(hidden)")!=std::string::npos;}))throw std::runtime_error("Hidden capture target was lost from the selection list");
            const auto excludedWindows=captureWindows(sourceWindow.window,sourceWindow.window);
            if(std::any_of(excludedWindows.begin(),excludedWindows.end(),[&](const auto& w){return w.first==sourceWindow.window;}))throw std::runtime_error("Selected capture target bypassed window exclusion");
            ShowWindow(sourceWindow.window,SW_SHOWNOACTIVATE);SetWindowPos(sourceWindow.window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            sourceWindow.paint(palette(0));awaitPattern(capture,device.Get(),context.Get(),palette(0),display,sourceWindow.window,false,timestamp,"Hidden source recovery");
            std::cout<<capture.backend()<<": minimize/hide recovery pixels verified"<<std::endl;
        }
        for(auto method:methods){
            config.captureMethod=method;sourceWindow.paint(palette(1));StereoSource source;source.start(config,display.adapterLuid);uint64_t pairId=0;
            const double timestamp=awaitPublishedPattern(source,device.Get(),context.Get(),palette(1),0,pairId);
            sourceWindow.paint(palette(2));const double updated=awaitPublishedPattern(source,device.Get(),context.Get(),palette(2),pairId,pairId);
            if(updated<=timestamp)throw std::runtime_error("Published source timestamp did not advance with changed pixels");
            std::cout<<source.status().captureBackend<<": changed pixels reached shared stereo source texture, pair "<<pairId<<std::endl;
            const double stopStart=qpc();source.stop();if(qpc()-stopStart>.5)throw std::runtime_error("Capture worker did not stop promptly");
        }
        if(std::find(methods.begin(),methods.end(),CaptureMethod::WindowsGraphicsCapture)!=methods.end())
            testVrTo3DCapture(device.Get(),context.Get(),display,sourceWindow);
        const HWND closedWindow=sourceWindow.window;sourceWindow.close();
        const auto closedSelection=captureWindows(nullptr,closedWindow);
        if(std::none_of(closedSelection.begin(),closedSelection.end(),[&](const auto& w){return w.first==closedWindow&&w.second.find("closed")!=std::string::npos;}))throw std::runtime_error("Closed capture target was silently replaced in the selection list");
        std::cout<<"Capture pixel integration passed for display "<<displayIndex<<"\n";return 0;
    }catch(const winrt::hresult_error& e){std::cerr<<utf8(e.message().c_str())<<std::endl;return 1;}
    catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
