// SPDX-License-Identifier: GPL-3.0-or-later
// Shared capture adapter. ReShade supplies graphics API hooks; this module
// publishes eyes to the independent app presenter, never extra game Presents.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <reshade.hpp>
#include "direct_eyes.h"
#include "capture_layout.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <filesystem>
using Microsoft::WRL::ComPtr;
namespace api=reshade::api;
namespace direct=vision::direct;
namespace capture=vision::capture;
namespace {
struct State {
    api::device* device=nullptr;api::resource bridge{};api::fence fence{};uint64_t fenceValue=0;
    ComPtr<ID3D11Device> native;ComPtr<ID3D11Texture2D> readable;
    std::unique_ptr<direct::Producer> producer;
    capture::Mode mode=capture::Mode::Disabled;capture::SequentialClock sequence;
    bool rightFirst=false,connected=false,failed=false,presentArmed=false,submitted=false;UINT presentCount=0;
    uint32_t width=0,height=0,layers=0;api::format format=api::format::unknown;
    HANDLE katangaMapping=nullptr;const uint32_t* katangaView=nullptr;uint32_t katangaHandle=0;
    ComPtr<ID3D11Texture2D> katanga;
    ~State(){if(bridge.handle)device->destroy_resource(bridge);if(fence.handle)device->destroy_fence(fence);if(katangaView)UnmapViewOfFile(katangaView);if(katangaMapping)CloseHandle(katangaMapping);}
};
std::mutex mutex;std::unordered_map<api::swapchain*,std::unique_ptr<State>> states;api::swapchain* owner=nullptr;std::wstring configPath;
void failure(State& state,const char* text){state.failed=true;reshade::log::message(reshade::log::level::error,text);}
DXGI_FORMAT nativeFormat(api::format format){
    switch(format){case api::format::r8g8b8a8_unorm:case api::format::r8g8b8a8_unorm_srgb:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case api::format::b8g8r8a8_unorm:case api::format::b8g8r8a8_unorm_srgb:return DXGI_FORMAT_B8G8R8A8_UNORM;
    case api::format::r10g10b10a2_unorm:return DXGI_FORMAT_R10G10B10A2_UNORM;
    case api::format::r16g16b16a16_float:return DXGI_FORMAT_R16G16B16A16_FLOAT;default:return DXGI_FORMAT_UNKNOWN;}
}
bool initialize(State& state){
    ComPtr<IDXGIAdapter> adapter;
    if(state.device->get_api()==api::device_api::d3d11){
        // A legacy provider can still be inside its factory hook here.
        // Query the already created device instead of re-entering factory creation.
        auto* device=reinterpret_cast<ID3D11Device*>(state.device->get_native());
        ComPtr<IDXGIDevice> dxgi;
        if(!device||FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter)))return false;
    }else{
        LUID luid{};if(!state.device->get_property(api::device_properties::adapter_luid,&luid))return false;
        ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
        for(UINT n=0;;n++){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(n,&candidate)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};if(candidate&&SUCCEEDED(candidate->GetDesc1(&d))&&d.AdapterLuid.LowPart==luid.LowPart&&d.AdapterLuid.HighPart==luid.HighPart){candidate.As(&adapter);break;}}
    }
    if(!adapter)return false;
    // Use the Windows runtime for private transport allocations, even when a
    // game's d3d11.dll is a stereo renderer that modifies resource creation.
    wchar_t path[MAX_PATH]{};if(!GetSystemDirectoryW(path,MAX_PATH))return false;
    const auto systemPath=std::wstring(path)+L"\\d3d11.dll";
    // Stereo wrappers redirect LoadLibrary even for absolute system paths.
    // Reuse the already loaded Windows module to avoid creating another
    // renderer-owned device (and changing the provider's global state).
    auto dll=GetModuleHandleW(systemPath.c_str());const bool owned=!dll;
    if(!dll)dll=LoadLibraryExW(systemPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)return false;
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(dll,"D3D11CreateDevice"));
    const HRESULT hr=create?create(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&state.native,nullptr,nullptr):E_NOINTERFACE;
    if(owned)FreeLibrary(dll);if(FAILED(hr))return false;
    state.producer=std::make_unique<direct::Producer>();
    return SUCCEEDED(state.producer->open(state.native.Get()));
}
bool makeBridge(State& state,const api::resource_desc& source){
    if(state.readable&&state.width==source.texture.width&&state.height==source.texture.height&&state.layers==source.texture.depth_or_layers&&state.format==source.texture.format)return true;
    if(source.texture.samples!=1||source.texture.levels!=1||nativeFormat(source.texture.format)==DXGI_FORMAT_UNKNOWN)return false;
    D3D11_TEXTURE2D_DESC d{};d.Width=source.texture.width;d.Height=source.texture.height;d.ArraySize=source.texture.depth_or_layers;d.MipLevels=d.SampleDesc.Count=1;
    d.Format=nativeFormat(source.texture.format);d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> texture;ComPtr<IDXGIResource1> shared;HANDLE handle=nullptr;
    if(FAILED(state.native->CreateTexture2D(&d,nullptr,&texture))||FAILED(texture.As(&shared))||FAILED(shared->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle)))return false;
    auto description=source;description.heap=api::memory_heap::default_;description.flags=api::resource_flags::shared|api::resource_flags::shared_nt_handle;
    description.usage=api::resource_usage::copy_dest|api::resource_usage::copy_source|api::resource_usage::shader_resource;
    api::resource imported{};const bool ok=state.device->create_resource(description,nullptr,api::resource_usage::general,&imported,&handle);CloseHandle(handle);if(!ok)return false;
    if(state.bridge.handle)state.device->destroy_resource(state.bridge);
    state.bridge=imported;state.readable=std::move(texture);state.width=d.Width;state.height=d.Height;state.layers=d.ArraySize;state.format=source.texture.format;return true;
}
bool completed(State& state,api::command_queue* queue){
    queue->flush_immediate_command_list();
    if(!state.fence.handle&&!state.device->create_fence(0,api::fence_flags::none,&state.fence))return false;
    if(!queue->signal(state.fence,++state.fenceValue))return false;
    return state.device->wait(state.fence,state.fenceValue,5000000000ull);
}
HRESULT submit(State& state,ID3D11Texture2D* texture,const capture::EyeRegion& region,unsigned eye,uint64_t pair,direct::Encoding encoding){
    D3D11_BOX box{region.left,region.top,0,region.right,region.bottom,1};const auto start=GetTickCount64();
    for(;;){
        if(!state.producer->connected())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const auto hr=state.producer->submit(texture,region.subresource,direct::Eye(eye),pair,encoding,{},&box,true);
        if(hr!=DXGI_ERROR_WAS_STILL_DRAWING)return hr;
        if(GetTickCount64()-start>=5000)return DXGI_ERROR_WAIT_TIMEOUT;
        Sleep(1);
    }
}
void init(api::swapchain* chain,bool){
    std::lock_guard lock(mutex);if(states.contains(chain))return;if(owner&&owner!=chain)return;
    wchar_t configured[32]{};GetPrivateProfileStringW(L"VISION_CAPTURE",L"Mode",L"disabled",configured,32,configPath.c_str());std::string mode;for(wchar_t c:std::wstring(configured))mode+=char(c);
    auto state=std::make_unique<State>();state->mode=capture::mode(mode);if(state->mode==capture::Mode::Disabled)return;
    state->device=chain->get_device();
    // Katanga is ordered on the provider's D3D11 queue, never polled by the app.
    if(state->mode==capture::Mode::Katanga&&state->device->get_api()!=api::device_api::d3d11)return;
    if(state->device->get_api()!=api::device_api::d3d11&&state->device->get_api()!=api::device_api::d3d12)return;
    state->rightFirst=GetPrivateProfileIntW(L"VISION_CAPTURE",L"RightFirst",0,configPath.c_str())!=0;
    if(!initialize(*state)){failure(*state,"[VisionCapture] Cannot create direct-eye channel on this adapter");return;}
    owner=chain;states[chain]=std::move(state);
    reshade::log::message(reshade::log::level::info,"[VisionCapture] Direct-eye provider ready; select this game in the app's Direct 3D eyes input");
}
void destroy(api::swapchain* chain,bool resizing){std::lock_guard lock(mutex);
    if(resizing){auto it=states.find(chain);if(it!=states.end()&&it->second->mode==capture::Mode::Sequential&&!it->second->sequence.boundary())failure(*it->second,"[VisionCapture] Resize interrupted an eye pair; restart the game to establish eye phase");return;}
    states.erase(chain);if(owner==chain)owner=nullptr;
}
void present(api::command_queue* queue,api::swapchain* chain,const api::rect* src,const api::rect* dst,uint32_t count,const api::rect*){
    std::lock_guard lock(mutex);auto it=states.find(chain);if(it==states.end())return;auto& state=*it->second;if(state.failed)return;
    auto* native=reinterpret_cast<IDXGISwapChain*>(chain->get_native());if(!native||FAILED(native->GetLastPresentCount(&state.presentCount))){failure(state,"[VisionCapture] Present identity unavailable");return;}
    state.presentArmed=true;state.submitted=false;
    const bool connected=state.producer->connected();
    if(!connected){state.connected=false;state.producer->reset(true);return;}
    if(!state.connected){
        // A new consumer starts only at a declared complete sequence boundary.
        if(state.mode==capture::Mode::Sequential&&!state.sequence.boundary())return;
        if(FAILED(state.producer->reset())){failure(state,"[VisionCapture] Restart both capture endpoints after disconnect");return;}
        state.connected=true;
    }
    if(src||dst||count){failure(state,"[VisionCapture] Partial Present is unsupported; capture stopped without changing the game");return;}
    ID3D11Texture2D* input=nullptr;api::resource resource=chain->get_current_back_buffer();auto desc=state.device->get_resource_desc(resource);
    if(state.mode==capture::Mode::Katanga){
        if(!state.katangaView){state.katangaMapping=OpenFileMappingW(FILE_MAP_READ,FALSE,L"Local\\KatangaMappedFile");if(!state.katangaMapping)return;
            state.katangaView=static_cast<const uint32_t*>(MapViewOfFile(state.katangaMapping,FILE_MAP_READ,0,0,4));if(!state.katangaView){failure(state,"[VisionCapture] Cannot map Katanga producer");return;}}
        const auto handle=*static_cast<const volatile uint32_t*>(state.katangaView);if(!handle)return;
        if(handle!=state.katangaHandle){ComPtr<ID3D11Texture2D> texture;if(FAILED(state.native->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(handle)),IID_PPV_ARGS(&texture)))){failure(state,"[VisionCapture] Cannot open Katanga eyes");return;}state.katanga=std::move(texture);state.katangaHandle=handle;}
        // Finish the provider's writes before copying on our native device.
        if(!completed(state,queue)){failure(state,"[VisionCapture] Katanga producer GPU completion failed");return;}
        input=state.katanga.Get();D3D11_TEXTURE2D_DESC d{};input->GetDesc(&d);desc.texture.width=d.Width;desc.texture.height=d.Height;desc.texture.depth_or_layers=uint16_t(d.ArraySize);desc.texture.levels=uint16_t(d.MipLevels);desc.texture.samples=uint16_t(d.SampleDesc.Count);desc.texture.format=api::format(d.Format);
    }else{
        if(!makeBridge(state,desc)){failure(state,"[VisionCapture] Unsupported source format or shared GPU transport");return;}
        auto* commands=queue->get_immediate_command_list();if(!commands){failure(state,"[VisionCapture] No graphics command list");return;}
        commands->barrier(resource,api::resource_usage::present,api::resource_usage::copy_source);
        commands->barrier(state.bridge,api::resource_usage::general,api::resource_usage::copy_dest);
        commands->copy_resource(resource,state.bridge);
        commands->barrier(state.bridge,api::resource_usage::copy_dest,api::resource_usage::general);
        commands->barrier(resource,api::resource_usage::copy_source,api::resource_usage::present);
        if(!completed(state,queue)){failure(state,"[VisionCapture] Source GPU completion failed");return;}input=state.readable.Get();
    }
    capture::EyeRegion regions[2];if(!capture::regions(state.mode,desc.texture.width,desc.texture.height,desc.texture.depth_or_layers,regions)){failure(state,"[VisionCapture] Source dimensions do not match the declared stereo layout");return;}
    const auto encoding=desc.texture.format==api::format::r16g16b16a16_float?direct::Encoding::LinearScRGB:direct::Encoding::SRGB;
    HRESULT hr;
    if(state.mode==capture::Mode::Sequential)hr=submit(state,input,regions[0],state.sequence.eye(state.rightFirst),state.sequence.pair(),encoding);
    else{const auto id=state.sequence.pair();hr=submit(state,input,regions[0],state.rightFirst?1:0,id,encoding);if(SUCCEEDED(hr))hr=submit(state,input,regions[1],state.rightFirst?0:1,id,encoding);}
    if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)){state.connected=false;state.producer->reset(true);}
    else if(FAILED(hr)){char message[128];sprintf_s(message,"[VisionCapture] Eye transfer stopped: 0x%08lx; last complete pair retained",hr);failure(state,message);}
    else state.submitted=true;
}
void finish(api::command_queue*,api::swapchain* chain){
    std::lock_guard lock(mutex);auto it=states.find(chain);if(it==states.end())return;auto& state=*it->second;
    if(!state.presentArmed)return;state.presentArmed=false;
    UINT after=0;auto* native=reinterpret_cast<IDXGISwapChain*>(chain->get_native());
    if(native&&SUCCEEDED(native->GetLastPresentCount(&after))&&after-state.presentCount==1){
        if(state.submitted&&!state.failed&&FAILED(state.producer->commit()))failure(state,"[VisionCapture] Cannot commit the completed Present");
        state.sequence.presented();if(state.mode!=capture::Mode::Sequential)state.sequence.presented();}
    else if(state.connected)failure(state,"[VisionCapture] Present did not advance; capture stopped to preserve eye identity");
}
}
extern "C" __declspec(dllexport) const char* NAME="Vision Stereo Capture";
extern "C" __declspec(dllexport) const char* VISION_CAPTURE_ABI="VisionStereoCapture.v1";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Direct stereo capture for declared sequential frames, stereo arrays and Katanga; independent frame-sequential output in Vision Restoration.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){std::wstring path(32768,L'\0');const auto length=GetModuleFileNameW(module,path.data(),DWORD(path.size()));if(!length||length>=path.size())return FALSE;path.resize(length);configPath=(std::filesystem::path(path).parent_path()/L"VisionStereoCapture.ini").wstring();if(!reshade::register_addon(module))return FALSE;reshade::register_event<reshade::addon_event::init_swapchain>(init);reshade::register_event<reshade::addon_event::destroy_swapchain>(destroy);reshade::register_event<reshade::addon_event::present>(present);reshade::register_event<reshade::addon_event::finish_present>(finish);}
    else if(reason==DLL_PROCESS_DETACH){reshade::unregister_event<reshade::addon_event::init_swapchain>(init);reshade::unregister_event<reshade::addon_event::destroy_swapchain>(destroy);reshade::unregister_event<reshade::addon_event::present>(present);reshade::unregister_event<reshade::addon_event::finish_present>(finish);reshade::unregister_addon(module);}
    return TRUE;
}
