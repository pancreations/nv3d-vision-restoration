// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include <MinHook.h>
#include "stereo.h"
#include "sbs_shader.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
namespace api=reshade::api;
using namespace vision::elden;
namespace {
std::mutex guard;
std::filesystem::path configPath,logPath;
HMODULE selfModule=nullptr;
bool hooked=false,attempted=false,enabled=true,reverseEyes=false,lockPairPose=true,reverseCameraOrder=true;
float separation=.065f,convergence=.0025f;
unsigned desiredEye=0,renderedEye=0;
uint64_t stamp=0,consumedStamp=0,cameraCalls=0,presents=0,invalidCameras=0;
Camera pairCenters[3]{};
void* pairOwner=nullptr;
bool haveCenters=false;
std::string failure;
using UpdateCamera=void(__fastcall*)(void*,void*,void*,void*);
UpdateCamera originalUpdate=nullptr;
void log(const std::string& text) {
    const auto message="[VisionEldenSBS] "+text;
    reshade::log::message(reshade::log::level::info,message.c_str());
    FILE* file=nullptr;if(_wfopen_s(&file,logPath.c_str(),L"ab")==0&&file){fprintf(file,"%llu %s\n",GetTickCount64(),text.c_str());fclose(file);}
}
void fail(const std::string& text){failure=text;enabled=false;desiredEye=0;haveCenters=false;log("STOPPED: "+text);}
// These narrow POD helpers keep SEH outside C++ unwinding scopes.
bool readMemory(const void* source,void* destination,size_t size) {
    __try {memcpy(destination,source,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool writePosition(Camera* target,const Camera* value) {
    __try {memcpy(target->basis,value->basis,sizeof(value->basis));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool applyCameras(void* owner,Camera** pointers) {
    Camera centers[3];
    for(int i=0;i<3;++i)if(!pointers[i]||!readMemory(pointers[i],&centers[i],sizeof(Camera))||!valid(centers[i])){++invalidCameras;return false;}
    if(!enabled)return false;
    if(desiredEye==0||!haveCenters||pairOwner!=owner){
        for(int i=0;i<3;++i)pairCenters[i]=centers[i];
        haveCenters=true;pairOwner=owner;
    }
    for(int i=0;i<3;++i){
        bool duplicate=false;for(int j=0;j<i;++j)duplicate|=pointers[j]==pointers[i];if(duplicate)continue;
        const auto camera=eyeCamera(lockPairPose?pairCenters[i]:centers[i],desiredEye,separation);
        if(!writePosition(pointers[i],&camera)){fail("Camera write failed; restart the game.");return false;}
    }
    renderedEye=desiredEye;++stamp;++cameraCalls;return true;
}
void __fastcall updateCamera(void* owner,void* a,void* b,void* c) {
    originalUpdate(owner,a,b,c);
    std::lock_guard lock(guard);
    if(!enabled||!owner)return;
    Camera* cameras[3]{};int debugCamera=0;
    if(!readMemory(static_cast<char*>(owner)+0x18,cameras,sizeof(cameras))||
       !readMemory(static_cast<char*>(owner)+0xc8,&debugCamera,sizeof(debugCamera))||debugCamera!=0)return;
    applyCameras(owner,cameras);
}
float number(const wchar_t* key,float value,float lo,float hi) {
    wchar_t buffer[64]{};GetPrivateProfileStringW(L"Stereo",key,L"",buffer,64,configPath.c_str());
    if(buffer[0]){wchar_t* end=nullptr;const float parsed=wcstof(buffer,&end);if(end!=buffer&&*end==0&&std::isfinite(parsed))value=parsed;}
    return std::clamp(value,lo,hi);
}
void loadSettings() {
    separation=number(L"EyeSeparation",.065f,0.f,.3f);
    convergence=number(L"ConvergenceShift",.0025f,-.03f,.03f);
    reverseEyes=GetPrivateProfileIntW(L"Stereo",L"SwapEyes",0,configPath.c_str())!=0;
    lockPairPose=GetPrivateProfileIntW(L"Stereo",L"LockPairPose",1,configPath.c_str())!=0;
    reverseCameraOrder=GetPrivateProfileIntW(L"Stereo",L"ReverseCameraOrder",1,configPath.c_str())!=0;
}
void initializeCamera() {
    if(attempted)return;attempted=true;
    loadSettings();
#ifdef VISION_ELDEN_TEST
    hooked=true;log("Synthetic test module; never install this build in a game.");return;
#else
    wchar_t offline[8]{};
    if(GetEnvironmentVariableW(L"VISION_ELDEN_SBS_OFFLINE",offline,8)!=1||offline[0]!=L'1'){fail("Use Launch Elden Ring SBS.cmd for the offline stereo session.");return;}
    if(GetModuleHandleW(L"EasyAntiCheat_x64.dll")||GetModuleHandleW(L"EasyAntiCheat_EOS.dll")){fail("Protected session detected.");return;}
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    if(_wcsicmp(std::filesystem::path(exe).filename().c_str(),L"eldenring.exe")!=0){fail("This module is only for Elden Ring.");return;}
    DWORD ignored=0;const DWORD length=GetFileVersionInfoSizeW(exe,&ignored);std::vector<unsigned char> version(length);
    VS_FIXEDFILEINFO* info=nullptr;UINT infoLength=0;
    if(!length||!GetFileVersionInfoW(exe,0,length,version.data())||!VerQueryValueW(version.data(),L"\\",reinterpret_cast<void**>(&info),&infoLength)||
       infoLength<sizeof(*info)||info->dwFileVersionMS!=MAKELONG(7,2)||info->dwFileVersionLS!=MAKELONG(0,1)){fail("Unsupported executable version (expected 2.7.1.0).");return;}
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    constexpr unsigned char signature[]{0x4c,0x8b,0x49,0x18,0x4c,0x8b,0xd1,0x8b,0x42,0x50,0x41,0x89,0x41,0x50,0x8b,0x42};
    void* target=nullptr;unsigned matches=0;auto* section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE){
        const size_t size=section[i].Misc.VirtualSize;
        if(section[i].VirtualAddress>=nt->OptionalHeader.SizeOfImage||size>nt->OptionalHeader.SizeOfImage-section[i].VirtualAddress)continue;
        for(size_t offset=0;offset+sizeof(signature)<=size;++offset){auto* p=base+section[i].VirtualAddress+offset;
            if(*p==signature[0]&&memcmp(p,signature,sizeof(signature))==0){target=p;++matches;}}
    }
    if(matches!=1||static_cast<unsigned char*>(target)-base!=0x681970){fail("Camera signature/RVA mismatch; no hook installed.");return;}
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED){fail("MinHook initialization failed.");return;}
    auto result=MH_CreateHook(target,reinterpret_cast<void*>(updateCamera),reinterpret_cast<void**>(&originalUpdate));
    if(result!=MH_OK){fail(std::string("Camera hook creation: ")+MH_StatusToString(result));return;}
    // The camera detour must never point into an unloaded add-on.
    HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(updateCamera),&pinned);
    result=MH_EnableHook(target);
    if(result!=MH_OK){MH_RemoveHook(target);fail(std::string("Camera hook activation: ")+MH_StatusToString(result));return;}
    hooked=true;log("Camera hook active: exe 2.7.1.0, unique signature at RVA 0x681970. Alternate-frame geometry stereo.");
#endif
}
struct Output {
    api::device* device=nullptr;api::command_queue* queue=nullptr;api::swapchain* chain=nullptr;
    uint32_t width=0,height=0;api::format format=api::format::unknown;
    // current mono, pending left, committed left, committed right
    std::array<api::resource,4> images{};std::array<api::resource_view,4> views{};
    std::vector<api::resource_view> targets;
    api::sampler sampler{};api::pipeline_layout layout{};api::pipeline pipeline{};
    PairClock clock;bool ready=false,armed=false;UINT before=0;uint64_t lastCameraStamp=0;
    ~Output(){release();}
    void release(){
        if(!device)return;
        if(queue)queue->wait_idle();
        for(auto view:targets)if(view.handle)device->destroy_resource_view(view);targets.clear();
        for(auto view:views)if(view.handle)device->destroy_resource_view(view);views={};
        for(auto image:images)if(image.handle)device->destroy_resource(image);images={};
        if(pipeline.handle)device->destroy_pipeline(pipeline);pipeline={};
        if(layout.handle)device->destroy_pipeline_layout(layout);layout={};
        if(sampler.handle)device->destroy_sampler(sampler);sampler={};ready=false;device=nullptr;
    }
} output;
bool compile(const char* entry,const char* profile,std::vector<unsigned char>& bytes) {
    ID3DBlob* code=nullptr;ID3DBlob* error=nullptr;
    const auto hr=D3DCompile(shader,strlen(shader),"VisionEldenSBS.hlsl",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)){log(error?static_cast<const char*>(error->GetBufferPointer()):"Shader compile failed");if(error)error->Release();return false;}
    bytes.assign(static_cast<unsigned char*>(code->GetBufferPointer()),static_cast<unsigned char*>(code->GetBufferPointer())+code->GetBufferSize());code->Release();if(error)error->Release();return true;
}
bool createOutput(api::swapchain* chain,api::command_queue* queue) {
    auto& s=output;s.device=chain->get_device();s.chain=chain;s.queue=queue;
    if(s.device->get_api()!=api::device_api::d3d12){log("Only Direct3D 12 is supported.");return false;}
    const auto desc=s.device->get_resource_desc(chain->get_back_buffer(0));
    s.width=desc.texture.width;s.height=desc.texture.height;s.format=api::format_to_default_typed(desc.texture.format);
    if(s.width%2||desc.texture.samples!=1||desc.texture.depth_or_layers!=1)return false;
    std::vector<unsigned char> vs,ps;if(!compile("vs","vs_5_0",vs)||!compile("ps","ps_5_0",ps))return false;
    api::pipeline_layout_param params[]{
        api::descriptor_range{0,0,0,1,api::shader_stage::pixel,1,api::descriptor_type::sampler},
        api::descriptor_range{0,0,0,1,api::shader_stage::pixel,1,api::descriptor_type::shader_resource_view},
        api::descriptor_range{0,1,0,1,api::shader_stage::pixel,1,api::descriptor_type::shader_resource_view},
        api::constant_range{0,0,0,4,api::shader_stage::pixel}};
    if(!s.device->create_pipeline_layout(4,params,&s.layout))return false;
    api::shader_desc v{vs.data(),vs.size()},p{ps.data(),ps.size()};api::primitive_topology topology=api::primitive_topology::triangle_list;
    api::rasterizer_desc raster{};raster.cull_mode=api::cull_mode::none;
    api::depth_stencil_desc depth{};depth.depth_enable=false;depth.stencil_enable=false;api::blend_desc blend{};
    api::pipeline_subobject objects[]{{api::pipeline_subobject_type::vertex_shader,1,&v},{api::pipeline_subobject_type::pixel_shader,1,&p},
        {api::pipeline_subobject_type::render_target_formats,1,&s.format},{api::pipeline_subobject_type::primitive_topology,1,&topology},
        {api::pipeline_subobject_type::rasterizer_state,1,&raster},{api::pipeline_subobject_type::depth_stencil_state,1,&depth},{api::pipeline_subobject_type::blend_state,1,&blend}};
    if(!s.device->create_pipeline(s.layout,uint32_t(std::size(objects)),objects,&s.pipeline))return false;
    api::sampler_desc sampler{};sampler.filter=api::filter_mode::min_mag_mip_linear;
    sampler.address_u=sampler.address_v=sampler.address_w=api::texture_address_mode::clamp;
    if(!s.device->create_sampler(sampler,&s.sampler))return false;
    const api::resource_desc image(s.width,s.height,1,1,s.format,1,api::memory_heap::default_,api::resource_usage::copy_source|api::resource_usage::copy_dest|api::resource_usage::shader_resource);
    for(int i=0;i<4;++i)if(!s.device->create_resource(image,nullptr,api::resource_usage::shader_resource,&s.images[i])||
        !s.device->create_resource_view(s.images[i],api::resource_usage::shader_resource,api::resource_view_desc(s.format),&s.views[i]))return false;
    s.targets.resize(chain->get_back_buffer_count());
    for(uint32_t i=0;i<s.targets.size();++i)if(!s.device->create_resource_view(chain->get_back_buffer(i),api::resource_usage::render_target,api::resource_view_desc(s.format),&s.targets[i]))return false;
    s.ready=true;log("SBS output ready: "+std::to_string(s.width)+"x"+std::to_string(s.height)+" (left | right).");return true;
}
void copy(api::command_list* cmd,api::resource source,api::resource destination) {
    cmd->barrier(source,api::resource_usage::shader_resource,api::resource_usage::copy_source);
    cmd->barrier(destination,api::resource_usage::shader_resource,api::resource_usage::copy_dest);
    cmd->copy_resource(source,destination);
    cmd->barrier(destination,api::resource_usage::copy_dest,api::resource_usage::shader_resource);
    cmd->barrier(source,api::resource_usage::copy_source,api::resource_usage::shader_resource);
}
void present(api::command_queue* queue,api::swapchain* chain,const api::rect*,const api::rect*,uint32_t,const api::rect*) {
    std::lock_guard lock(guard);initializeCamera();if(!hooked||!enabled)return;
    if(output.chain&&output.chain!=chain)return;
    if(!output.ready&&!createOutput(chain,queue)){output.release();fail("Cannot create stereo output resources.");return;}
    auto& s=output;++presents;
    auto* native=reinterpret_cast<IDXGISwapChain*>(chain->get_native());
    s.armed=SUCCEEDED(native->GetLastPresentCount(&s.before));
    if(!s.armed){fail("Present identity unavailable.");return;}
    const unsigned index=chain->get_current_back_buffer_index();auto back=chain->get_back_buffer(index);
    auto* cmd=queue->get_immediate_command_list();if(!cmd){fail("No graphics command list.");return;}
    cmd->barrier(back,api::resource_usage::present,api::resource_usage::copy_source);
    cmd->barrier(s.images[0],api::resource_usage::shader_resource,api::resource_usage::copy_dest);
    cmd->copy_resource(back,s.images[0]);
    cmd->barrier(s.images[0],api::resource_usage::copy_dest,api::resource_usage::shader_resource);
    cmd->barrier(back,api::resource_usage::copy_source,api::resource_usage::render_target);
    const bool fresh=stamp>consumedStamp;
    if(fresh){
        // More than one camera update between Presents is not a reliable eye
        // identity. Retry from a new left eye rather than pair unrelated frames.
        if(stamp-consumedStamp!=1){s.clock.reset();desiredEye=0;haveCenters=false;}
        else {
            const auto action=s.clock.accept(stamp,renderedEye);
            if(action==PairClock::Action::StageLeft)copy(cmd,s.images[0],s.images[1]);
            else if(action==PairClock::Action::Complete){copy(cmd,s.images[1],s.images[2]);copy(cmd,s.images[0],s.images[3]);}
            desiredEye=renderedEye^1;
        }
        consumedStamp=stamp;
    }else {s.clock.reset();desiredEye=0;haveCenters=false;}
    const bool stereo=s.clock.complete&&fresh;
    // The live 2.7.1.0 render has reversed camera identity relative to the
    // synthetic camera fixture. Calibrate source order BEFORE convergence;
    // swapping the final image would also reverse the screen-plane offset.
    api::resource_view left=s.views[stereo?(reverseCameraOrder?3:2):0],right=s.views[stereo?(reverseCameraOrder?2:3):0];
    const float constants[]{stereo?convergence:0.f,reverseEyes?1.f:0.f,0,0};
    cmd->bind_pipeline(api::pipeline_stage::all_graphics,s.pipeline);
    cmd->bind_pipeline_state(api::dynamic_state::primitive_topology,uint32_t(api::primitive_topology::triangle_list));
    cmd->push_descriptors(api::shader_stage::pixel,s.layout,0,api::descriptor_table_update{{},0,0,1,api::descriptor_type::sampler,&s.sampler});
    cmd->push_descriptors(api::shader_stage::pixel,s.layout,1,api::descriptor_table_update{{},0,0,1,api::descriptor_type::shader_resource_view,&left});
    cmd->push_descriptors(api::shader_stage::pixel,s.layout,2,api::descriptor_table_update{{},0,0,1,api::descriptor_type::shader_resource_view,&right});
    cmd->push_constants(api::shader_stage::pixel,s.layout,3,0,4,constants);
    const api::viewport viewport{0,0,float(s.width),float(s.height),0,1};const api::rect scissor{0,0,int(s.width),int(s.height)};
    cmd->bind_viewports(0,1,&viewport);cmd->bind_scissor_rects(0,1,&scissor);cmd->bind_render_targets_and_depth_stencil(1,&s.targets[index]);cmd->draw(3,1,0,0);
    cmd->barrier(back,api::resource_usage::render_target,api::resource_usage::present);
    if(presents%300==0)log("presents="+std::to_string(presents)+" camera_updates="+std::to_string(cameraCalls)+" pairs="+std::to_string(s.clock.pairs)+" invalid_cameras="+std::to_string(invalidCameras));
}
void finish(api::command_queue*,api::swapchain* chain) {
    std::lock_guard lock(guard);auto& s=output;if(s.chain!=chain||!s.armed)return;s.armed=false;
    UINT after=0;auto* native=reinterpret_cast<IDXGISwapChain*>(chain->get_native());
    if(FAILED(native->GetLastPresentCount(&after))||after-s.before!=1){s.clock.reset();desiredEye=0;haveCenters=false;log("Present did not advance: partial stereo pair discarded.");}
}
void destroy(api::swapchain* chain,bool) {
    std::lock_guard lock(guard);if(output.chain!=chain)return;output.release();output.chain=nullptr;output.clock.reset();
    consumedStamp=stamp;desiredEye=0;haveCenters=false;
}
void keyboard(api::effect_runtime* runtime) {
    std::lock_guard lock(guard);if(!hooked||!failure.empty())return;
    if(runtime->is_key_pressed(VK_F6)){enabled=!enabled;output.clock.reset();desiredEye=0;consumedStamp=stamp;haveCenters=false;log(enabled?"SBS enabled":"SBS disabled (mono)");}
    if(runtime->is_key_pressed(VK_F7)){reverseEyes=!reverseEyes;log(reverseEyes?"Eye order: right | left":"Eye order: left | right");}
    if(runtime->is_key_pressed(VK_F8)){
        loadSettings();output.clock.reset();desiredEye=0;consumedStamp=stamp;haveCenters=false;
        log("Settings reloaded. camera_updates="+std::to_string(cameraCalls)+" pairs="+std::to_string(output.clock.pairs)+" separation="+std::to_string(separation)+" convergence="+std::to_string(convergence)+" reverse_camera_order="+std::to_string(reverseCameraOrder));
    }
}
}
#ifdef VISION_ELDEN_TEST
extern "C" __declspec(dllexport) bool VisionEldenTestCamera(Camera* camera) {
    std::lock_guard lock(guard);initializeCamera();Camera* cameras[]{camera,camera,camera};return applyCameras(camera,cameras);
}
#endif
extern "C" __declspec(dllexport) const char* NAME="Vision Elden Ring SBS";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Experimental two-camera alternate-frame SBS for Elden Ring 2.7.1.0. F6 stereo, F7 swap, F8 log. Offline launcher required.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){
        selfModule=module;wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);
        configPath=std::filesystem::path(path).parent_path()/L"VisionEldenSBS.ini";logPath=configPath.parent_path()/L"VisionEldenSBS.log";
        if(!reshade::register_addon(module))return FALSE;
        reshade::register_event<reshade::addon_event::present>(present);
        reshade::register_event<reshade::addon_event::finish_present>(finish);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(destroy);
        reshade::register_event<reshade::addon_event::reshade_present>(keyboard);
    }else if(reason==DLL_PROCESS_DETACH){
        reshade::unregister_event<reshade::addon_event::present>(present);
        reshade::unregister_event<reshade::addon_event::finish_present>(finish);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(destroy);
        reshade::unregister_event<reshade::addon_event::reshade_present>(keyboard);
        reshade::unregister_addon(module);
    }return TRUE;
}
