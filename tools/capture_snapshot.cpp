#include "gpu_capture.h"
#include "gpu_completion.h"
#include "renderer.h"
#include <winrt/base.h>
#include <dwmapi.h>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace vision;

int main(int argc,char** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        DWORD wantedPid=0;std::string titleFilter;std::filesystem::path output;
        CaptureMethod method=CaptureMethod::WindowsGraphicsCapture;
        for(int i=1;i<argc;++i){
            const std::string arg=argv[i];
            if(arg=="--pid"&&i+1<argc)wantedPid=DWORD(std::stoul(argv[++i]));
            else if(arg=="--title"&&i+1<argc)titleFilter=argv[++i];
            else if(arg=="--output"&&i+1<argc)output=wide(argv[++i]);
            else if(arg=="--backend"&&i+1<argc){
                const std::string backend=argv[++i];
                if(backend=="dxgi")method=CaptureMethod::DesktopDuplication;
                else if(backend=="windows")method=CaptureMethod::WindowsGraphicsCapture;
                else throw std::runtime_error("Capture backend must be dxgi or windows.");
            }
            else throw std::runtime_error("Usage: vision_capture_snapshot --pid PID [--title TEXT] [--backend windows|dxgi] --output DIRECTORY");
        }
        if((!wantedPid&&titleFilter.empty())||output.empty())throw std::runtime_error("Specify a PID or title and an output directory.");
        struct Search {DWORD pid;const std::string& title;HWND window=nullptr;uint64_t area=0;} search{wantedPid,titleFilter};
        EnumWindows([](HWND h,LPARAM data)->BOOL{
            auto& found=*reinterpret_cast<Search*>(data);DWORD pid=0;GetWindowThreadProcessId(h,&pid);
            if((found.pid&&pid!=found.pid)||!IsWindowVisible(h)||IsIconic(h)||GetWindow(h,GW_OWNER))return TRUE;
            wchar_t title[512]{};GetWindowTextW(h,title,512);if(!found.title.empty()&&utf8(title).find(found.title)==std::string::npos)return TRUE;
            RECT client{};if(!GetClientRect(h,&client)||client.right<=0||client.bottom<=0)return TRUE;
            const uint64_t area=uint64_t(client.right)*uint64_t(client.bottom);
            if(area>found.area){found.window=h;found.area=area;}return TRUE;
        },reinterpret_cast<LPARAM>(&search));
        if(!search.window)throw std::runtime_error("No visible, restored window matches the requested source. No window was changed.");
        const auto displays=enumerateDisplays();const HMONITOR monitor=MonitorFromWindow(search.window,MONITOR_DEFAULTTONEAREST);
        const auto display=std::find_if(displays.begin(),displays.end(),[&](const auto& d){return d.monitor==monitor;});
        if(display==displays.end())throw std::runtime_error("Source monitor was not enumerated.");
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Snapshot factory");
        ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(display->adapterLuid,IID_PPV_ARGS(&adapter)),"Snapshot adapter");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Snapshot device");
        SourceConfig config;config.kind=SourceKind::Window;config.window=search.window;config.captureMethod=method;
        GpuCapture capture(device.Get(),config);CapturedPicture picture;const double deadline=qpc()+5;
        while(qpc()<deadline&&!capture.next(picture,20)){}
        if(!picture.texture)throw std::runtime_error("No captured frame: "+capture.backend());
        D3D11_TEXTURE2D_DESC desc{};picture.texture->GetDesc(&desc);
        RECT bounds{},client{},outer{};POINT origin{};GetWindowRect(search.window,&outer);
        check(DwmGetWindowAttribute(search.window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)),"DWM capture bounds");
        if(!GetClientRect(search.window,&client)||!ClientToScreen(search.window,&origin))throw std::runtime_error("Client geometry became unavailable.");
        OffsetRect(&client,origin.x,origin.y);
        const UINT rawWidth=desc.Width,rawHeight=desc.Height;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=desc.MiscFlags=desc.CPUAccessFlags=0;
        ComPtr<ID3D11Texture2D> raw,cropped;check(device->CreateTexture2D(&desc,nullptr,&raw),"Raw snapshot texture");
        context->CopyResource(raw.Get(),picture.texture.Get());
        desc.Width=picture.width;desc.Height=picture.height;
        check(device->CreateTexture2D(&desc,nullptr,&cropped),"Cropped snapshot texture");
        D3D11_BOX box{picture.left,picture.top,0,picture.left+picture.width,picture.top+picture.height,1};
        context->CopySubresourceRegion(cropped.Get(),0,0,0,0,picture.texture.Get(),0,&box);
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> done;check(device->CreateQuery(&qd,&done),"Snapshot completion");
        context->End(done.Get());context->Flush();
        check(waitForGpuCompletion([&](BOOL* ready){return context->GetData(done.Get(),ready,sizeof(*ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);},std::stop_token{}),"Snapshot copy completion");
        capture.release();
        std::filesystem::create_directories(output);
        std::ofstream metadata(output/L"capture-metadata.txt");
        auto rect=[&](const char* name,const RECT& r){metadata<<name<<'='<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<'\n';};
        wchar_t title[512]{};GetWindowTextW(search.window,title,512);DWORD pid=0;GetWindowThreadProcessId(search.window,&pid);
        metadata<<"backend="<<capture.backend()<<"\npid="<<pid<<"\nhwnd=0x"<<std::hex<<uintptr_t(search.window)<<std::dec<<"\ntitle="<<utf8(title)<<'\n';
        rect("window",outer);rect("dwm",bounds);rect("client",client);rect("monitor",display->rect);
        metadata<<"dpi="<<GetDpiForWindow(search.window)<<"\nraw_texture="<<rawWidth<<'x'<<rawHeight<<"\nformat="<<int(desc.Format)
            <<"\ncrop="<<picture.left<<','<<picture.top<<','<<picture.width<<','<<picture.height
            <<"\nencoding="<<int(picture.encoding)<<"\ntimestamp="<<picture.timestamp
            <<"\nPNG conversion: FP16 RGB clamped to [0,1] then sRGB encoded; geometry unchanged.\n";
        metadata.close();
        saveTexturePng(device.Get(),context.Get(),raw.Get(),output/L"capture-raw.png");
        saveTexturePng(device.Get(),context.Get(),cropped.Get(),output/L"capture-client.png");
        std::cout<<"Saved raw "<<rawWidth<<'x'<<rawHeight<<" and client "<<picture.width<<'x'<<picture.height<<" at "<<picture.left<<','<<picture.top<<" to "<<output.string()<<'\n';
        return 0;
    }catch(const winrt::hresult_error& e){std::cerr<<utf8(e.message().c_str())<<'\n';return 1;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
