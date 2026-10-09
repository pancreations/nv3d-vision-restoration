// SPDX-License-Identifier: GPL-3.0-or-later
#include "wibble_source.h"
#include "gpu_completion.h"
#include <algorithm>
#include <filesystem>
#include <chrono>
#if defined(VISION_HAS_OPENVR) && VISION_HAS_OPENVR
#define OPENVR_INTERFACE_INTERNAL
#include <openvr.h>
#include <tlhelp32.h>
#endif

namespace vision::wibble {
namespace {
DXGI_FORMAT copyFormat(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:case DXGI_FORMAT_R16G16B16A16_FLOAT:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:case DXGI_FORMAT_R32G32B32A32_FLOAT:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
bool concreteView(DXGI_FORMAT format){
    return format==DXGI_FORMAT_R8G8B8A8_UNORM||format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
        format==DXGI_FORMAT_B8G8R8A8_UNORM||format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||
        format==DXGI_FORMAT_B8G8R8X8_UNORM||format==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB||
        format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R32G32B32A32_FLOAT;
}
}
MirrorLayout describeMirror(const D3D11_TEXTURE2D_DESC& texture,const D3D11_SHADER_RESOURCE_VIEW_DESC& view){
    if(!texture.Width||!texture.Height||!texture.MipLevels||!texture.ArraySize||texture.SampleDesc.Count!=1)
        throw std::runtime_error("Wibble mirror must be a nonempty, single-sampled 2D texture.");
    unsigned mip=0,slice=0,mipCount=0;
    if(view.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D){
        mip=view.Texture2D.MostDetailedMip;mipCount=view.Texture2D.MipLevels;
        if(texture.ArraySize!=1||!view.Texture2D.MipLevels)throw std::runtime_error("Wibble mirror 2D view is invalid.");
    }else if(view.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY){
        mip=view.Texture2DArray.MostDetailedMip;slice=view.Texture2DArray.FirstArraySlice;mipCount=view.Texture2DArray.MipLevels;
        if(view.Texture2DArray.ArraySize!=1||!view.Texture2DArray.MipLevels)throw std::runtime_error("Wibble mirror eye must select one array slice.");
    }else throw std::runtime_error("Wibble mirror uses an unsupported texture view.");
    if(mip>=texture.MipLevels||mip>=32||slice>=texture.ArraySize)throw std::runtime_error("Wibble mirror mip or slice is out of bounds.");
    if(mipCount!=UINT(-1)&&mipCount>texture.MipLevels-mip)throw std::runtime_error("Wibble mirror view mip range is out of bounds.");
    const auto format=copyFormat(view.Format);
    if(!concreteView(view.Format)||format==DXGI_FORMAT_UNKNOWN||copyFormat(texture.Format)!=format)
        throw std::runtime_error("Wibble mirror color format cannot be copied safely.");
    MirrorLayout result;result.width=std::max(1u,texture.Width>>mip);result.height=std::max(1u,texture.Height>>mip);
    result.subresource=D3D11CalcSubresource(mip,slice,texture.MipLevels);result.format=format;
    result.encoding=format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_R32G32B32A32_FLOAT?Encoding::LinearScRGB:Encoding::SRGB;
    return result;
}
bool compatibleMirrors(const MirrorLayout& left,const MirrorLayout& right){
    return left.width==right.width&&left.height==right.height&&left.format==right.format&&left.encoding==right.encoding;
}
bool MirrorCadence::begin(double now,bool timed,uint32_t frame){
    const bool changed=timed&&(!timedCopy_||frame!=frame_);
    if(haveCopy_&&!changed&&now-copiedAt_<1./120)return false;
    haveCopy_=true;timedCopy_=timed;copiedAt_=now;frame_=frame;return true;
}

struct SourceReader::Impl {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Texture2D> snapshot;ComPtr<ID3D11Query> complete;
    std::string message="Waiting for Wibble Wobble / SteamVR";double retryAt=0;uint64_t published=0;bool elevationRequired=false;
#if defined(VISION_HAS_OPENVR) && VISION_HAS_OPENVR
    HMODULE module=nullptr;HANDLE server=nullptr;bool initialized=false;
    using Shutdown=void(VR_CALLTYPE*)();Shutdown shutdown=nullptr;
    vr::IVRSystem* system=nullptr;vr::IVRCompositor* compositor=nullptr;
    struct Eye {ID3D11ShaderResourceView* view=nullptr;ComPtr<ID3D11Texture2D> texture;MirrorLayout layout;};
    Eye eyes[2];MirrorCadence cadence;
    ~Impl(){disconnect();if(module)FreeLibrary(module);}
    void disconnect(){
        for(auto& eye:eyes){eye.texture.Reset();if(eye.view&&compositor)compositor->ReleaseMirrorTextureD3D11(eye.view);eye.view=nullptr;}
        snapshot.Reset();if(initialized&&shutdown)shutdown();initialized=false;system=nullptr;compositor=nullptr;cadence={};
        if(server)CloseHandle(server);server=nullptr;
    }
    template<class T>T function(const char* name){
        const auto value=GetProcAddress(module,name);if(!value)throw std::runtime_error(std::string("OpenVR loader is missing ")+name);return reinterpret_cast<T>(value);
    }
    static DWORD integrity(HANDLE process){
        HANDLE token=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&token))return 0;
        DWORD bytes=0;GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&bytes);std::vector<unsigned char> storage(bytes);DWORD result=0;
        if(bytes&&GetTokenInformation(token,TokenIntegrityLevel,storage.data(),bytes,&bytes)){
            const auto* label=reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
            if(IsValidSid(label->Label.Sid)){const auto count=*GetSidSubAuthorityCount(label->Label.Sid);if(count)result=*GetSidSubAuthority(label->Label.Sid,count-1);}
        }
        CloseHandle(token);return result;
    }
    bool runtimeHasHigherIntegrity(){
        const DWORD ownIntegrity=integrity(GetCurrentProcess());if(!ownIntegrity)return false;
        DWORD ownSession=0;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);bool higher=false;
        HANDLE processes=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(processes==INVALID_HANDLE_VALUE)return false;
        PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
        if(Process32FirstW(processes,&entry))do{
            DWORD session=0;if(_wcsicmp(entry.szExeFile,L"vrserver.exe")!=0||!ProcessIdToSessionId(entry.th32ProcessID,&session)||session!=ownSession)continue;
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
            if(process){higher=integrity(process)>ownIntegrity;CloseHandle(process);if(higher)break;}
        }while(Process32NextW(processes,&entry));CloseHandle(processes);return higher;
    }
    void load(){
        if(module)return;
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));const auto folder=std::filesystem::path(exe).parent_path();
        std::vector<std::filesystem::path> candidates{folder/L"openvr_api.dll",folder/L"openvr/openvr_api.dll"};
        wchar_t steam[32768]{};DWORD size=sizeof(steam);
        if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steam,&size)==ERROR_SUCCESS)
            candidates.push_back(std::filesystem::path(steam)/L"steamapps/common/SteamVR/bin/win64/openvr_api.dll");
        for(const auto& file:candidates){module=LoadLibraryExW(file.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(module)break;}
        if(!module)throw std::runtime_error("OpenVR loader is missing. Restore openvr_api.dll beside Vision Restoration.");
    }
    void connect(){
        elevationRequired=runtimeHasHigherIntegrity();
        if(elevationRequired)throw std::runtime_error("SteamVR is running as administrator. Connect to administrator SteamVR to display Wibble Wobble's eyes.");
        load();const auto init=function<uint32_t(VR_CALLTYPE*)(vr::EVRInitError*,vr::EVRApplicationType,const char*)>("VR_InitInternal2");
        shutdown=function<Shutdown>("VR_ShutdownInternal");
        const auto generic=function<void*(VR_CALLTYPE*)(const char*,vr::EVRInitError*)>("VR_GetGenericInterface");
        const auto errorText=function<const char*(VR_CALLTYPE*)(vr::EVRInitError)>("VR_GetVRInitErrorAsEnglishDescription");
        vr::EVRInitError error=vr::VRInitError_None;init(&error,vr::VRApplication_Background,nullptr);
        if(error!=vr::VRInitError_None)throw std::runtime_error(std::string("Start Wibble Wobble and SteamVR: ")+errorText(error));initialized=true;
        system=static_cast<vr::IVRSystem*>(generic(vr::IVRSystem_Version,&error));
        if(!system||error!=vr::VRInitError_None)throw std::runtime_error("SteamVR system interface unavailable; update SteamVR.");
        error=system->SetSDKVersion(vr::k_nSteamVRVersionMajor,vr::k_nSteamVRVersionMinor,vr::k_nSteamVRVersionBuild);
        if(error!=vr::VRInitError_None)throw std::runtime_error(errorText(error));
        compositor=static_cast<vr::IVRCompositor*>(generic(vr::IVRCompositor_Version,&error));
        if(!compositor||error!=vr::VRInitError_None)throw std::runtime_error("SteamVR compositor is not ready.");
        int32_t index=-1;system->GetDXGIOutputInfo(&index);if(index<0)throw std::runtime_error("SteamVR did not identify its rendering GPU.");
        ComPtr<IDXGIFactory> factory;check(CreateDXGIFactory(IID_PPV_ARGS(&factory)),"Wibble adapter factory");ComPtr<IDXGIAdapter> expected;
        check(factory->EnumAdapters(UINT(index),&expected),"SteamVR adapter");DXGI_ADAPTER_DESC expectedDesc{};check(expected->GetDesc(&expectedDesc),"SteamVR adapter identity");
        ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi),"Wibble source device");ComPtr<IDXGIAdapter> actual;check(dxgi->GetAdapter(&actual),"Wibble output adapter");
        DXGI_ADAPTER_DESC actualDesc{};check(actual->GetDesc(&actualDesc),"Wibble output adapter identity");
        if(expectedDesc.AdapterLuid.LowPart!=actualDesc.AdapterLuid.LowPart||expectedDesc.AdapterLuid.HighPart!=actualDesc.AdapterLuid.HighPart)
            throw std::runtime_error("Select an output display on the same GPU as Wibble Wobble / SteamVR.");
        for(unsigned i=0;i<2;i++){
            void* raw=nullptr;const auto result=compositor->GetMirrorTextureD3D11(i?vr::Eye_Right:vr::Eye_Left,device.Get(),&raw);eyes[i].view=static_cast<ID3D11ShaderResourceView*>(raw);
            if(result!=vr::VRCompositorError_None||!raw)throw std::runtime_error("Wibble eye mirror is unavailable (OpenVR error "+std::to_string(int(result))+").");
            ComPtr<ID3D11Resource> resource;eyes[i].view->GetResource(&resource);check(resource.As(&eyes[i].texture),"Wibble eye texture");
            D3D11_TEXTURE2D_DESC desc{};D3D11_SHADER_RESOURCE_VIEW_DESC view{};eyes[i].texture->GetDesc(&desc);eyes[i].view->GetDesc(&view);eyes[i].layout=describeMirror(desc,view);
        }
        if(!compatibleMirrors(eyes[0].layout,eyes[1].layout))throw std::runtime_error("Wibble left and right eye mirrors have incompatible sizes or formats.");
        const auto& layout=eyes[0].layout;D3D11_TEXTURE2D_DESC desc{};desc.Width=layout.width;desc.Height=layout.height;desc.MipLevels=1;desc.ArraySize=2;desc.Format=layout.format;
        desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc,nullptr,&snapshot),"Wibble stereo snapshot");
        HANDLE processes=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(processes!=INVALID_HANDLE_VALUE){PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);if(Process32FirstW(processes,&entry))do{
            if(_wcsicmp(entry.szExeFile,L"vrserver.exe")==0){server=OpenProcess(SYNCHRONIZE,FALSE,entry.th32ProcessID);break;}
        }while(Process32NextW(processes,&entry));CloseHandle(processes);}
        message="Receiving Wibble Wobble left and right images through SteamVR";
        cadence={};
    }
    bool frame(uint32_t& index){vr::Compositor_FrameTiming timing{};timing.m_nSize=sizeof(timing);if(!compositor->GetFrameTiming(&timing))return false;index=timing.m_nFrameIndex;return true;}
    bool receive(std::stop_token stop,Picture& picture){
        if(server&&WaitForSingleObject(server,0)==WAIT_OBJECT_0)throw std::runtime_error("SteamVR stopped; waiting to reconnect.");
        vr::VREvent_t event{};while(system->PollNextEvent(&event,sizeof(event)))if(event.eventType==vr::VREvent_Quit||event.eventType==vr::VREvent_DriverRequestedQuit){
            system->AcknowledgeQuit_Exiting();throw std::runtime_error("SteamVR is closing; waiting to reconnect.");
        }
        uint32_t before=0;const bool timed=frame(before);
        const double acquired=qpc();
        if(!cadence.begin(acquired,timed,before))return false;
        for(unsigned i=0;i<2;i++)context->CopySubresourceRegion(snapshot.Get(),i,0,0,0,eyes[i].texture.Get(),eyes[i].layout.subresource,nullptr);
        context->End(complete.Get());context->Flush();
        const auto result=waitForGpuCompletion([&](BOOL* ready){return context->GetData(complete.Get(),ready,sizeof(*ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);},stop);
        if(stop.stop_requested())return false;check(result,"Wibble eye copy did not complete");
        // GetFrameTiming describes compositor presentation, not either mirror's
        // generation. Its index can advance while we wait for an already valid
        // GPU copy, especially with a 240 Hz compositor. Rejecting on that
        // change can freeze capture indefinitely. Publish every completed copy
        // of the explicitly selected eyes; OpenVR has no atomic stereo-pair lock.
        const auto& layout=eyes[0].layout;picture={snapshot,layout.width,layout.height,layout.encoding,acquired,++published};
        message="Receiving Wibble Wobble left and right images through SteamVR";return true;
    }
#endif
};
SourceReader::SourceReader()=default;SourceReader::~SourceReader()=default;
void SourceReader::open(ID3D11Device* device,ID3D11DeviceContext* context){
    if(!device||!context)throw std::runtime_error("Wibble source needs a D3D11 device and context.");
    impl_=std::make_unique<Impl>();impl_->device=device;impl_->context=context;
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};check(device->CreateQuery(&query,&impl_->complete),"Wibble completion query");
}
bool SourceReader::next(std::stop_token stop,Picture& picture){
    if(!impl_)throw std::runtime_error("Open the Wibble source before reading images.");picture={};if(stop.stop_requested())return false;
#if defined(VISION_HAS_OPENVR) && VISION_HAS_OPENVR
    if(qpc()<impl_->retryAt)return false;
    try{if(!impl_->initialized)impl_->connect();return impl_->receive(stop,picture);}
    catch(const std::exception& error){impl_->message=error.what();impl_->disconnect();impl_->retryAt=qpc()+2;return false;}
#else
    impl_->message="This build needs the OpenVR SDK for Wibble Wobble input.";return false;
#endif
}
std::string SourceReader::status()const{return impl_?impl_->message:"Wibble Wobble input is stopped";}
bool SourceReader::requiresElevation()const{return impl_&&impl_->elevationRequired;}
}
