// Stereo transport only: enable native SBS/TB or a stereo shader before this add-on.
#include <windows.h>
#include <d3d11.h>
#include <reshade.hpp>
#include <SpoutDX.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
static std::mutex sendersMutex;
static std::unordered_map<reshade::api::effect_runtime*,std::unique_ptr<spoutDX>> senders;
static void finished(reshade::api::effect_runtime* runtime,reshade::api::command_list*,reshade::api::resource_view rtv,reshade::api::resource_view){
    auto* device=runtime->get_device();if(device->get_api()!=reshade::api::device_api::d3d11)return;
    auto resource=device->get_resource_from_view(rtv);if(!resource.handle)return;
    std::lock_guard lock(sendersMutex);
    auto& sender=senders[runtime];if(!sender){
        sender=std::make_unique<spoutDX>();
        if(!sender->OpenDirectX11(reinterpret_cast<ID3D11Device*>(device->get_native()))){sender.reset();return;}
        auto name="VisionStereo-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(reinterpret_cast<uintptr_t>(runtime));sender->SetSenderName(name.c_str());
    }
    sender->SendTexture(reinterpret_cast<ID3D11Texture2D*>(resource.handle));
}
static void destroyed(reshade::api::effect_runtime* runtime){std::lock_guard lock(sendersMutex);senders.erase(runtime);}
extern "C" __declspec(dllexport) const char* NAME="Vision Stereo Spout";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Exports finished DX11 SBS/top-bottom frames to Vision Restoration. Does not generate stereo or control the emitter.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){if(!reshade::register_addon(module))return FALSE;reshade::register_event<reshade::addon_event::reshade_finish_effects>(finished);reshade::register_event<reshade::addon_event::destroy_effect_runtime>(destroyed);}
    else if(reason==DLL_PROCESS_DETACH){reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(finished);reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(destroyed);reshade::unregister_addon(module);}
    return TRUE;
}
