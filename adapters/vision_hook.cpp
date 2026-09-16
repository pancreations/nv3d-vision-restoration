// SPDX-License-Identifier: GPL-3.0-or-later
// Vision Restoration game hook: a ReShade add-on that shows a game in frame-sequential stereo on
// the game's own swap chain and reports every present to Vision Restoration, which drives the IR
// emitter. ReShade is only the injection framework.
//
// Stereo sources (ReShade.ini [VISION] StereoSource):
//   packed - the game renders both cameras side by side or top/bottom (Dolphin, geo-11);
//   depth  - the game renders one camera (GTA V Enhanced): each eye is resampled from the finished
//            frame with the game's own depth buffer (depth_tracker.h, depth_stereo_shader.h);
//   auto   - packed on Direct3D 11, depth on Direct3D 12 (the default).
//
// Every refresh shows one slot of the host's sequence (Left/Right, Left/Black/Right/Black or
// Left/Left/Right/Right). Present identifiers and DXGI frame statistics go to shared memory; the host
// maps them to refreshes and times the glasses. Submitted present IDs are timing evidence, not proof
// of optical synchronization.
//
// Direct3D 11: each game frame presents its whole sequence (every slot but the last from inside the
// game's Present), and a vblank thread fills refreshes the game leaves empty. Flip-model buffer 0 is
// always the current buffer, so the extra presents are invisible to the game.
//
// Direct3D 12: the game renders into GetCurrentBackBufferIndex(), which every extra present advances,
// and its flip queue holds three images (12.5 ms at 240 Hz), so a whole sequence per game frame leaves
// a gap whenever a frame takes longer than that: GTA V at 50 fps slipped every frame and the glasses
// flashed. Instead the game's own Present carries one slot, both eye images are rendered once per game
// frame, and the vblank thread presents the following slots from its own native command list on
// refreshes the game leaves empty. It never presents while the game is drawing into a back buffer (from
// the game's first draw, clear or copy into one until it presents). A game that still draws into a
// buffer other than the current one fetched the index before a filled refresh; filling is switched off
// for it, and a game that keeps doing so counts its own presents and is left in 2D.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <reshade.hpp>
#include "sync_protocol.h"
#include "stereo_blit_shader.h"
#include "depth_stereo_shader.h"
#include "depth_tracker.h"
#include "d3d11_state_scope.h"
#include "d3d11_pixel_probe.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace reshade::api;
namespace {
LONGLONG qpcRaw(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
double qpcFreq(){static double f=[]{LARGE_INTEGER v;QueryPerformanceFrequency(&v);return double(v.QuadPart);}();return f;}
double qpc(){return double(qpcRaw())/qpcFreq();}

struct Host {
    HANDLE mapping=nullptr,event=nullptr;vision::sync::Shared* shared=nullptr;double lastTry=0;
    void close(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(event)CloseHandle(event);shared=nullptr;mapping=nullptr;event=nullptr;}
    bool connect(){
        if(shared)return true;double now=qpc();if(now-lastTry<1.0)return false;lastTry=now;
        HANDLE m=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,vision::sync::mappingName);if(!m)return false;
        auto* s=static_cast<vision::sync::Shared*>(MapViewOfFile(m,FILE_MAP_ALL_ACCESS,0,0,sizeof(vision::sync::Shared)));
        if(!s||s->magic!=vision::sync::magic||s->version!=vision::sync::version){if(s)UnmapViewOfFile(s);CloseHandle(m);return false;}
        mapping=m;shared=s;event=OpenEventW(EVENT_MODIFY_STATE,FALSE,vision::sync::frameEventName);return true;
    }
    // The host recreates its mapping when it restarts; a stale heartbeat means our view is orphaned.
    bool alive(){if(!shared)return false;double age=double(qpcRaw()-shared->hostHeartbeatQpc)/qpcFreq();if(age>3.0){close();return false;}return true;}
};

// Depth-based stereo settings: ReShade.ini [VISION], changed live with hotkeys.
struct DepthSettings {
    int source=-1;          // StereoSource: -1 auto (packed on D3D11, depth on D3D12), 0 packed, 1 depth
    float separation=.03f;  // Separation: parallax at infinity, fraction of the width (both eyes together)
    float convergence=.03f; // Convergence: depth-buffer value (reversed) that sits at the screen
    float popOut=.5f;       // PopOut: how far in front of the screen, as a fraction of Separation
    int reversed=-1;        // DepthReversed: -1 detect from the game's depth clears, 0 standard, 1 reversed
    uint32_t width=0,height=0; // DepthWidth/DepthHeight: only use a depth buffer of this size (0 = automatic)
    bool enabled=true;      // Ctrl+Shift+Insert shows 2D with the glasses still running
};
DepthSettings g_depth;bool g_settingsLoaded=false;
void loadSettings(){
    if(g_settingsLoaded)return;g_settingsLoaded=true;
    char text[16]{};size_t size=sizeof(text)-1;
    if(reshade::get_config_value(nullptr,"VISION","StereoSource",text,&size)){const std::string value(text,strnlen(text,sizeof(text)));g_depth.source=value=="packed"?0:value=="depth"?1:-1;}
    float value=0;int number=0;
    if(reshade::get_config_value(nullptr,"VISION","Separation",value))g_depth.separation=std::clamp(value,0.f,.1f);
    if(reshade::get_config_value(nullptr,"VISION","Convergence",value))g_depth.convergence=std::clamp(value,.0005f,1.f);
    if(reshade::get_config_value(nullptr,"VISION","PopOut",value))g_depth.popOut=std::clamp(value,0.f,2.f);
    if(reshade::get_config_value(nullptr,"VISION","DepthReversed",number))g_depth.reversed=std::clamp(number,-1,1);
    if(reshade::get_config_value(nullptr,"VISION","DepthWidth",number))g_depth.width=uint32_t(std::max(number,0));
    if(reshade::get_config_value(nullptr,"VISION","DepthHeight",number))g_depth.height=uint32_t(std::max(number,0));
}
void saveSettings(){
    reshade::set_config_value(nullptr,"VISION","Separation",g_depth.separation);
    reshade::set_config_value(nullptr,"VISION","Convergence",g_depth.convergence);
    reshade::set_config_value(nullptr,"VISION","PopOut",g_depth.popOut);
}
// Eye and black images stay readable by shaders and copyable to the back buffer at the same time.
resource_usage eyeState(){return resource_usage::copy_source|resource_usage::shader_resource;}

struct SwapState {
    device* dev=nullptr;swapchain* chain=nullptr;bool d3d12=false;
    pipeline_layout layout{},depthLayout{};pipeline pipe{},depthPipe{};sampler samp{};resource copy{};resource_view copyView{};std::vector<resource_view> targets;
    uint32_t width=0,height=0;format fmt=format::unknown;bool ready=false;std::string failure;
    int pendingSlot=-1;UINT pendingAfter=0;
    uint64_t frames=0,nativePresents=0,errors=0;bool active=false,awaitingRuntime=false,blocked=false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> contextState;
    vision::hook::PixelProbe probe;uint64_t sourceFrames=0;int diagnosticMode=0;
    // Refresh filling: the display shows one image per refresh, so every refresh needs its own slot. A
    // 60 fps game on a 144 Hz panel only presents 120 times a second, leaving a quarter of the refreshes
    // showing the previous eye. Measured in experiments/vblank-filler.
    Microsoft::WRL::ComPtr<IDXGIOutput> output;Microsoft::WRL::ComPtr<ID3D11Multithread> lock;
    UINT lastPresentId=0;int lastSlot=-1;uint64_t fillerPresents=0,fillerErrors=0;bool fillerReady=false;
    // Last accepted draw parameters, so a filled refresh repeats the same geometry.
    command_list* cmd=nullptr;command_queue* queue=nullptr;int packing=0,sequence=0;bool swapHalves=false;float convergence=0,bandHeight=1,bandCenter=.5f;
    // Depth-based stereo: the game's depth, copied each frame, and whether this frame had any.
    bool depthMode=false,depthFresh=false;vision::hook::depth::Capture depth;uint64_t wrongBufferFrames=0;
    // Direct3D 12: both eyes of the latest game frame and a black image, which every slot copies to the
    // back buffer, and the filler's own native command list. ReShade's immediate command list is no use
    // to the filler thread: ReShade flushes it from whichever game thread executes command lists.
    resource eyes[2]{},black{};resource_view eyeViews[2]{},blackView{};bool eyesReady=false,blackCleared=false,fillerTried=false;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12;Microsoft::WRL::ComPtr<ID3D12CommandAllocator> fillAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> fillList;Microsoft::WRL::ComPtr<ID3D12Fence> fillFence;HANDLE fillEvent=nullptr;UINT64 fillFenceValue=0;
    // Refresh phase: (refresh - slot) mod cycle as first shown, and the last present queued before a
    // correction (its successors were already queued with the old phase).
    int phaseRef=-1;UINT correctedThrough=0;uint64_t realigned=0;
    void release(bool waitForGpu){
        if(!dev)return;
        if(waitForGpu&&d3d12&&queue)queue->wait_idle(); // nothing destroyed below may still be in flight
        if(waitForGpu&&fillFence&&fillEvent&&fillFenceValue&&fillFence->GetCompletedValue()<fillFenceValue&&SUCCEEDED(fillFence->SetEventOnCompletion(fillFenceValue,fillEvent)))WaitForSingleObject(fillEvent,1000);
        depth.release(dev);
        for(auto& t:targets)if(t.handle)dev->destroy_resource_view(t);targets.clear();
        for(int e=0;e<2;++e){if(eyeViews[e].handle)dev->destroy_resource_view(eyeViews[e]);if(eyes[e].handle)dev->destroy_resource(eyes[e]);eyeViews[e]={};eyes[e]={};}
        if(blackView.handle)dev->destroy_resource_view(blackView);if(black.handle)dev->destroy_resource(black);blackView={};black={};
        if(copyView.handle)dev->destroy_resource_view(copyView);if(copy.handle)dev->destroy_resource(copy);if(samp.handle)dev->destroy_sampler(samp);
        if(pipe.handle)dev->destroy_pipeline(pipe);if(layout.handle)dev->destroy_pipeline_layout(layout);
        if(depthPipe.handle)dev->destroy_pipeline(depthPipe);if(depthLayout.handle)dev->destroy_pipeline_layout(depthLayout);
        copyView={};copy={};samp={};pipe={};layout={};depthPipe={};depthLayout={};context.Reset();contextState.Reset();output.Reset();lock.Reset();
        fillList.Reset();fillAllocator.Reset();fillFence.Reset();queue12.Reset();if(fillEvent){CloseHandle(fillEvent);fillEvent=nullptr;}
        ready=false;fillerReady=false;lastSlot=-1;eyesReady=false;
    }
};
std::mutex g_mutex;std::unordered_map<swapchain*,SwapState> g_states;Host g_host;std::vector<uint8_t> g_vs,g_ps,g_depthPs;
uint32_t g_ringHead=0;
std::array<vision::sync::FrameRecord,vision::sync::ringSize> g_ring{};
swapchain* g_owner=nullptr;
thread_local swapchain* g_presentChain=nullptr;
std::atomic<bool> g_fillerPaused{false}; // the regression harness pauses filling while it reads buffers back
void resetRecords(){g_ringHead=0;g_ring={};}
void fail(SwapState& st,const std::string& message,bool permanent=true){
    const bool changed=st.failure!=message;
    st.active=false;st.pendingSlot=-1;st.blocked=permanent;st.failure=message;
    if(changed)reshade::log::message(reshade::log::level::warning,("[Vision] "+message).c_str());
}

// Our own commands: the Direct3D 11 state is isolated from the game's, and nothing we record
// counts as the game's rendering in the depth tracker.
struct DrawScope {
    std::optional<vision::hook::ContextScope> context;
    explicit DrawScope(SwapState& st){vision::hook::depth::t_own=true;if(st.context)context.emplace(st.context.Get(),st.contextState.Get());}
    ~DrawScope(){context.reset();vision::hook::depth::t_own=false;}
    DrawScope(const DrawScope&)=delete;DrawScope& operator=(const DrawScope&)=delete;
};

bool compile(const char* source,const char* entry,const char* profile,std::vector<uint8_t>& out,std::string& error){
    ID3DBlob* blob=nullptr;ID3DBlob* messages=nullptr;
    const HRESULT h=D3DCompile(source,strlen(source),"vision_hook.hlsl",nullptr,nullptr,entry,profile,0,0,&blob,&messages);
    if(FAILED(h)){error=messages?static_cast<char*>(messages->GetBufferPointer()):entry;if(messages)messages->Release();if(blob)blob->Release();return false;}
    out.assign(static_cast<uint8_t*>(blob->GetBufferPointer()),static_cast<uint8_t*>(blob->GetBufferPointer())+blob->GetBufferSize());blob->Release();if(messages)messages->Release();return true;
}
bool compileShaders(std::string& error){
    if(!g_depthPs.empty())return true;
    return compile(vision::stereoBlitShader,"vs","vs_5_0",g_vs,error)&&compile(vision::stereoBlitShader,"ps","ps_5_0",g_ps,error)&&compile(vision::depthStereoShader,"ps","ps_5_0",g_depthPs,error);
}
// A full-screen triangle pipeline: sampler s0, textures t0..t(n-1), constants b0.
bool createPipeline(device* dev,format target,const std::vector<uint8_t>& ps,uint32_t textures,uint32_t constants,pipeline_layout& layout,pipeline& pipe){
    std::vector<pipeline_layout_param> params;
    params.emplace_back(descriptor_range{0,0,0,1,shader_stage::pixel,1,descriptor_type::sampler});
    for(uint32_t t=0;t<textures;++t)params.emplace_back(descriptor_range{0,t,0,1,shader_stage::pixel,1,descriptor_type::shader_resource_view});
    params.emplace_back(constant_range{0,0,0,constants,shader_stage::pixel});
    if(!dev->create_pipeline_layout(uint32_t(params.size()),params.data(),&layout))return false;
    shader_desc vsd{g_vs.data(),g_vs.size()},psd{ps.data(),ps.size()};primitive_topology topo=primitive_topology::triangle_list;
    rasterizer_desc rs{};rs.cull_mode=cull_mode::none;depth_stencil_desc dsd{};dsd.depth_enable=false;dsd.stencil_enable=false;blend_desc bs{};
    pipeline_subobject subs[]{{pipeline_subobject_type::vertex_shader,1,&vsd},{pipeline_subobject_type::pixel_shader,1,&psd},{pipeline_subobject_type::render_target_formats,1,&target},{pipeline_subobject_type::primitive_topology,1,&topo},{pipeline_subobject_type::rasterizer_state,1,&rs},{pipeline_subobject_type::depth_stencil_state,1,&dsd},{pipeline_subobject_type::blend_state,1,&bs}};
    return dev->create_pipeline(layout,uint32_t(std::size(subs)),subs,&pipe);
}

void publish(SwapState& st,IDXGISwapChain* native,bool signal){
    auto* sh=g_host.shared;if(!sh)return;
    InterlockedIncrement(&sh->seq);MemoryBarrier();
    sh->gamePid=GetCurrentProcessId();sh->gameHeartbeatQpc=qpcRaw();sh->hwnd=reinterpret_cast<uint64_t>(st.chain->get_hwnd());
    sh->api=st.dev->get_api()==device_api::d3d11?vision::sync::ApiD3D11:st.dev->get_api()==device_api::d3d12?vision::sync::ApiD3D12:vision::sync::ApiUnknown;
    sh->width=st.width;sh->height=st.height;sh->backBuffers=st.chain->get_back_buffer_count();sh->format=uint32_t(st.fmt);sh->active=st.active?1:0;
    sh->frames=st.frames;sh->nativePresents=st.nativePresents;sh->presentErrors=st.errors;sh->hookFlags=uint32_t(vision::sync::HookSequences)|uint32_t(vision::sync::HookPatterns)|(st.d3d12?uint32_t(vision::sync::HookPhaseLocked):0u);
    DXGI_FRAME_STATISTICS stats{};if(native&&SUCCEEDED(native->GetFrameStatistics(&stats))){
        sh->presentCount=stats.PresentCount;sh->presentRefreshCount=stats.PresentRefreshCount;sh->syncRefreshCount=stats.SyncRefreshCount;sh->syncQpc=stats.SyncQPCTime.QuadPart;sh->statsValid=1;
        IDXGISwapChainMedia* media=nullptr;if(SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&media)))){DXGI_FRAME_STATISTICS_MEDIA fm{};if(SUCCEEDED(media->GetFrameStatisticsMedia(&fm)))sh->composed=(fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSED||fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE)?1:0;media->Release();}
    }else sh->statsValid=0;
    std::copy(g_ring.begin(),g_ring.end(),sh->ring);sh->ringHead=g_ringHead;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);const wchar_t* base=wcsrchr(path,L'\\');base=base?base+1:path;size_t n=0;for(;base[n]&&n<63;n++)sh->gameName[n]=char(base[n]<128?base[n]:'?');sh->gameName[n]=0;
    std::string status;
    if(!st.failure.empty())status=st.failure;
    else if(!st.active)status="Passing the game through";
    else if(st.depthMode){
        char text[192];sprintf_s(text,"Depth 3D %s: separation %.1f%%, convergence %.4f, %s, filled %llu",g_depth.enabled?"on":"off (2D)",g_depth.separation*100,g_depth.convergence,st.depthFresh?st.depth.describe().c_str():"no scene depth this frame (2D)",static_cast<unsigned long long>(st.fillerPresents));
        status=text;
    }else status="Splitting stereo frames; filled "+std::to_string(st.fillerPresents)+" refreshes the game did not";
    strncpy_s(sh->message,status.c_str(),_TRUNCATE);
    MemoryBarrier();InterlockedIncrement(&sh->seq);
    if(signal&&g_host.event)SetEvent(g_host.event);
}
void record(uint32_t presentId,int slot){g_ring[g_ringHead%vision::sync::ringSize]={presentId,slot};++g_ringHead;}
int slotOfPresent(UINT presentId){
    const uint32_t count=std::min<uint32_t>(g_ringHead,vision::sync::ringSize);
    for(uint32_t k=0;k<count;++k){const auto& r=g_ring[(g_ringHead-1-k)%vision::sync::ringSize];if(r.presentId==presentId)return r.eye;if(r.presentId<presentId)break;}
    return -1;
}
// Keeps every slot on its refresh phase (Direct3D 12). A refresh no present reached (a slip) repeats
// the previous slot, and every later slot would land a refresh late while the glasses keep the old
// phase. DXGI reports on which refresh each present was shown; when that phase moves, the next slot
// skips ahead by the slip. Returns the slots to skip.
int trackPhase(SwapState& st,IDXGISwapChain* native,unsigned slots){
    if(st.lastSlot<0){st.phaseRef=-1;st.correctedThrough=0;}
    DXGI_FRAME_STATISTICS stats{};
    if(slots<2||FAILED(native->GetFrameStatistics(&stats))||!stats.PresentRefreshCount||stats.PresentCount<=st.correctedThrough)return 0;
    const int shown=slotOfPresent(stats.PresentCount);if(shown<0)return 0;
    const int phase=int((uint64_t(stats.PresentRefreshCount)+slots-unsigned(shown)%slots)%slots);
    if(st.phaseRef<0){st.phaseRef=phase;return 0;}
    if(phase==st.phaseRef)return 0;
    st.correctedThrough=st.lastPresentId;++st.realigned;
    return (phase-st.phaseRef+int(slots))%int(slots);
}
// Every change in what the host lets us do goes to ReShade.log, so a run that shows the
// packed image on screen explains itself instead of ending with "Hook ready" alone.
std::string g_hostNote;
void noteHost(const std::string& note,bool warning){
    if(g_hostNote==note)return;g_hostNote=note;
    reshade::log::message(warning?reshade::log::level::warning:reshade::log::level::info,("[Vision] "+note).c_str());
}
std::string hostPauseReason(const vision::sync::Shared* sh){
    switch(sh->hostState){
    case vision::sync::HostPausedByOutput:return "Vision Restoration paused the hook while its own stereo output or live preview is running: close that output in the app, then the game switches to stereo automatically";
    case vision::sync::HostEmitterNotReady:return "Vision Restoration has no ready emitter: connect the emitter in the app, then the game switches to stereo automatically";
    default:return "Vision Restoration has disabled the hook: passing the game image through";
    }
}

// Copies the game's finished frame (and, for depth-based stereo, its depth) for this frame's slots.
void captureFrame(command_list* cmd,SwapState& st,uint32_t index){
    const resource back=st.chain->get_back_buffer(index);
    if(st.d3d12){cmd->barrier(back,resource_usage::present,resource_usage::copy_source);cmd->barrier(st.copy,resource_usage::shader_resource,resource_usage::copy_dest);}
    cmd->copy_resource(back,st.copy);
    if(st.d3d12){cmd->barrier(st.copy,resource_usage::copy_dest,resource_usage::shader_resource);cmd->barrier(back,resource_usage::copy_source,resource_usage::present);}
    st.depthFresh=st.depthMode&&st.depth.update(st.dev,cmd,st.width,st.height);
}
// Puts the finished frame back for the game's own Present, instead of a black or one-eye image (Direct3D 11).
void restoreFrame(command_list* cmd,SwapState& st){
    if(st.context)st.context->ClearState();
    cmd->copy_resource(st.copy,st.chain->get_current_back_buffer());
}
// Draws one eye into `target`: the depth-based eye, or a packed half (the whole image for a
// single-camera game without depth this frame: menus, loading). depthView shows the depth buffer
// instead (grey = screen depth, brighter = nearer).
void drawEye(command_list* cmd,SwapState& st,resource_view target,int eye,bool depthView){
    const bool depthDraw=st.depthMode&&st.depth.view.handle&&(st.depthFresh||depthView);
    const viewport vp{0,0,float(st.width),float(st.height),0,1};const rect sr{0,0,int32_t(st.width),int32_t(st.height)};
    const uint32_t triangles=uint32_t(primitive_topology::triangle_list);const dynamic_state topologyState=dynamic_state::primitive_topology;
    if(depthDraw){
        const auto& d=st.depth.desc.texture;
        const float values[16]{eye==0?-1.f:1.f,g_depth.enabled?g_depth.separation:0.f,g_depth.convergence,g_depth.popOut,
                               st.depth.isReversed()?1.f:0.f,st.depthFresh?1.f:0.f,float(d.width),float(d.height),
                               depthView?1.f:0.f,st.convergence,1.f/float(st.width),1.f/float(st.height),
                               st.bandHeight,st.bandCenter,0,0};
        cmd->bind_pipeline(pipeline_stage::all_graphics,st.depthPipe);cmd->bind_pipeline_states(1,&topologyState,&triangles);
        cmd->push_descriptors(shader_stage::pixel,st.depthLayout,0,descriptor_table_update{{},0,0,1,descriptor_type::sampler,&st.samp});
        cmd->push_descriptors(shader_stage::pixel,st.depthLayout,1,descriptor_table_update{{},0,0,1,descriptor_type::shader_resource_view,&st.copyView});
        cmd->push_descriptors(shader_stage::pixel,st.depthLayout,2,descriptor_table_update{{},0,0,1,descriptor_type::shader_resource_view,&st.depth.view});
        cmd->push_constants(shader_stage::pixel,st.depthLayout,3,0,16,values);
    }else{
        const bool mono=st.depthMode;const int half=eye^(st.swapHalves?1:0);const int packing=st.packing;
        const float values[12]{mono?0.f:packing==0?half*.5f:0.f,mono?0.f:packing==0?0.f:half*.5f,mono||packing!=0?1.f:.5f,mono||packing==0?1.f:.5f,
                               (eye==0?1.f:-1.f)*st.convergence,.5f/float(st.width),.5f/float(st.height),0,st.bandHeight,st.bandCenter,0,0};
        cmd->bind_pipeline(pipeline_stage::all_graphics,st.pipe);cmd->bind_pipeline_states(1,&topologyState,&triangles);
        cmd->push_descriptors(shader_stage::pixel,st.layout,0,descriptor_table_update{{},0,0,1,descriptor_type::sampler,&st.samp});
        cmd->push_descriptors(shader_stage::pixel,st.layout,1,descriptor_table_update{{},0,0,1,descriptor_type::shader_resource_view,&st.copyView});
        cmd->push_constants(shader_stage::pixel,st.layout,2,0,12,values);
    }
    cmd->bind_viewports(0,1,&vp);cmd->bind_scissor_rects(0,1,&sr);
    cmd->bind_render_targets_and_depth_stencil(1,&target);
    cmd->draw(3,1,0,0);
}
// Draws one slot into back buffer `index`: content 0 left eye, 1 right eye, 2 black.
void drawSlot(command_list* cmd,SwapState& st,uint32_t index,int content,bool depthView=false){
    const resource target=st.chain->get_back_buffer(index);
    if(st.d3d12)cmd->barrier(target,resource_usage::present,resource_usage::render_target);
    if(content==2){const float black[4]{0,0,0,1};cmd->clear_render_target_view(st.targets[index],black);}
    else drawEye(cmd,st,st.targets[index],content,depthView);
    if(st.d3d12)cmd->barrier(target,resource_usage::render_target,resource_usage::present);
}
// Direct3D 12: both eyes of this game frame, drawn once; every slot copies one of them.
void renderEyes(command_list* cmd,SwapState& st,bool depthView=false){
    for(int eye=0;eye<2;++eye){
        cmd->barrier(st.eyes[eye],eyeState(),resource_usage::render_target);
        drawEye(cmd,st,st.eyeViews[eye],eye,depthView);
        cmd->barrier(st.eyes[eye],resource_usage::render_target,eyeState());
    }
}
void ensureBlack(command_list* cmd,SwapState& st){
    if(st.blackCleared)return;
    const float black[4]{0,0,0,1};
    cmd->barrier(st.black,eyeState(),resource_usage::render_target);cmd->clear_render_target_view(st.blackView,black);cmd->barrier(st.black,resource_usage::render_target,eyeState());
    st.blackCleared=true;
}
void showSlot12(command_list* cmd,SwapState& st,uint32_t index,int content){
    const resource back=st.chain->get_back_buffer(index);
    cmd->barrier(back,resource_usage::present,resource_usage::copy_dest);
    cmd->copy_resource(content==2?st.black:st.eyes[content],back);
    cmd->barrier(back,resource_usage::copy_dest,resource_usage::present);
}
// Direct3D 11 slot presents from inside the game's Present: bounded queue-pressure retries, so an
// emulator's speed limiter never sees extra VSync blocking.
HRESULT presentSlot(IDXGISwapChain* native){
    HRESULT result=S_OK;const double deadline=qpc()+0.100;
    do{result=native->Present(1,DXGI_PRESENT_DO_NOT_WAIT);if(result!=DXGI_ERROR_WAS_STILL_DRAWING)break;Sleep(1);}while(qpc()<deadline);
    return result;
}

// The Direct3D 12 filler's native objects, created on the first stereo frame (the queue is only known then).
bool ensureFiller12(SwapState& st,command_queue* queue){
    if(st.fillerTried)return st.fillerReady;
    st.fillerTried=true;
    auto* device=reinterpret_cast<ID3D12Device*>(st.dev->get_native());
    auto* nativeChain=reinterpret_cast<IDXGISwapChain*>(st.chain->get_native());
    st.queue12=reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
    st.fillEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    st.fillerReady=device&&st.queue12&&st.fillEvent&&nativeChain&&SUCCEEDED(nativeChain->GetContainingOutput(&st.output))
        &&SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&st.fillAllocator)))
        &&SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,st.fillAllocator.Get(),nullptr,IID_PPV_ARGS(&st.fillList)))
        &&SUCCEEDED(st.fillList->Close())&&SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&st.fillFence)));
    if(!st.fillerReady)reshade::log::message(reshade::log::level::warning,"[Vision] Refresh filling is unavailable on this Direct3D 12 swap chain: each game frame covers one refresh, so a game below the display rate repeats slots.");
    return st.fillerReady;
}
// Records and executes one filled slot: the eye or black image copied to back buffer `index`.
bool recordCopy12(SwapState& st,uint32_t index,int content){
    if(st.fillFenceValue&&st.fillFence->GetCompletedValue()<st.fillFenceValue){
        if(FAILED(st.fillFence->SetEventOnCompletion(st.fillFenceValue,st.fillEvent))||WaitForSingleObject(st.fillEvent,50)!=WAIT_OBJECT_0)return false;
    }
    if(FAILED(st.fillAllocator->Reset())||FAILED(st.fillList->Reset(st.fillAllocator.Get(),nullptr)))return false;
    auto* back=reinterpret_cast<ID3D12Resource*>(st.chain->get_back_buffer(index).handle);
    auto* source=reinterpret_cast<ID3D12Resource*>((content==2?st.black:st.eyes[content]).handle);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=back;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    st.fillList->ResourceBarrier(1,&barrier);
    st.fillList->CopyResource(back,source);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    st.fillList->ResourceBarrier(1,&barrier);
    if(FAILED(st.fillList->Close()))return false;
    ID3D12CommandList* lists[]{st.fillList.Get()};st.queue12->ExecuteCommandLists(1,lists);
    return SUCCEEDED(st.queue12->Signal(st.fillFence.Get(),++st.fillFenceValue));
}
// One Direct3D 12 refresh the game left empty. Presents only when nothing else is presenting, the game
// is not drawing into a back buffer, the flip queue is empty and no game slot is pending.
void fillRefresh12(){
    using vision::hook::depth::g_presenting;using vision::hook::depth::g_composing;
    if(g_presenting.load()||g_composing.load()||g_fillerPaused.load())return;
    std::unique_lock lock(g_mutex,std::try_to_lock);if(!lock.owns_lock())return;
    auto it=g_states.find(g_owner);if(it==g_states.end())return;
    auto& st=it->second;
    auto* native=st.ready?reinterpret_cast<IDXGISwapChain*>(st.chain->get_native()):nullptr;
    if(!native||!st.d3d12||!st.active||st.blocked||!st.fillerReady||!st.eyesReady||st.pendingSlot>=0||st.lastSlot<0)return;
    if(g_presenting.load()||g_composing.load()||g_fillerPaused.load())return;
    // The flip that just happened may not be visible in the statistics yet (see the Direct3D 11 filler).
    DXGI_FRAME_STATISTICS stats{};const double deadline=qpc()+0.0025;bool statistics=true;
    for(;;){
        if(FAILED(native->GetFrameStatistics(&stats))){statistics=false;break;}
        if(stats.PresentCount==st.lastPresentId||qpc()>deadline)break;
        YieldProcessor();
    }
    UINT before=0;
    if(!statistics||stats.PresentCount!=st.lastPresentId||FAILED(native->GetLastPresentCount(&before))||before!=st.lastPresentId||g_composing.load())return;
    const unsigned slots=vision::sync::slotsPerFrame(st.sequence);
    const int slot=int((unsigned(st.lastSlot)+1u+unsigned(trackPhase(st,native,slots)))%slots);
    if(!recordCopy12(st,st.chain->get_current_back_buffer_index(),vision::sync::slotEye(st.sequence,unsigned(slot)))){++st.fillerErrors;return;}
    const HRESULT result=native->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
    UINT after=0;
    if(result==S_OK&&SUCCEEDED(native->GetLastPresentCount(&after))&&uint32_t(after-before)==1){
        record(after,slot);st.lastPresentId=after;st.lastSlot=slot;++st.fillerPresents;
        publish(st,native,true);
    }else if(result!=DXGI_ERROR_WAS_STILL_DRAWING&&result!=DXGI_STATUS_OCCLUDED)++st.fillerErrors;
}

// One image per refresh. The display holds the last flipped image until the next flip, so a refresh
// the game does not fill still shows the previous slot while the glasses have already switched: that
// mismatch is the flicker. This thread waits for each vblank and, when the flip queue is empty,
// presents the next slot, so every refresh carries a known slot and the host's refresh-to-present
// mapping is exact. It never blocks the game: fills use DO_NOT_WAIT, and the game's own presents take
// priority because a non-empty queue is left alone. Measured in experiments/vblank-filler (Direct3D
// 11): 866 of 867 refreshes filled, perfect alternation, and the producer held its exact frame rate.
std::jthread g_filler;
void runFiller(std::stop_token stop){
    while(!stop.stop_requested()){
        // Re-read the owner every refresh: it changes when the host pauses the hook, when the
        // game resizes, and when a swap chain is recreated.
        Microsoft::WRL::ComPtr<IDXGIOutput> output;Microsoft::WRL::ComPtr<ID3D11Multithread> guard;bool direct3d12=false;
        {std::lock_guard lock(g_mutex);auto it=g_states.find(g_owner);
         if(it!=g_states.end()&&it->second.active&&it->second.fillerReady){output=it->second.output;guard=it->second.lock;direct3d12=it->second.d3d12;}}
        if(!output||(!direct3d12&&!guard)||g_fillerPaused.load()){Sleep(8);continue;}
        if(FAILED(output->WaitForVBlank())){Sleep(4);continue;}
        if(stop.stop_requested())break;
        if(direct3d12){fillRefresh12();continue;}
        // Direct3D 11 lock discipline: take the device lock first (holding nothing), then try for our
        // own state without ever waiting. The game's present path holds our state lock while it
        // presents, and presenting takes the device lock, so a filler that waited here would deadlock
        // the game. A refresh we cannot claim instantly is simply left to the game.
        guard->Enter();
        {
            std::unique_lock lock(g_mutex,std::try_to_lock);
            auto it=lock.owns_lock()?g_states.find(g_owner):g_states.end();
            if(it!=g_states.end()){
                auto& st=it->second;
                // Only continue a live, unblocked stereo sequence; never touch a paused or
                // passed-through game, and never inject between our own slots and the game's
                // last slot present.
                auto* native=st.ready?reinterpret_cast<IDXGISwapChain*>(st.chain->get_native()):nullptr;
                if(native&&!st.d3d12&&st.active&&!st.blocked&&st.fillerReady&&st.cmd&&st.pendingSlot<0&&st.lastSlot>=0&&st.lock.Get()==guard.Get()){
                    // The flip that just happened may not be visible in the statistics yet.
                    // Measured lag is 5 us at the median and 1.4 ms at the 99th percentile, so
                    // poll briefly rather than filling a refresh the game has already claimed.
                    DXGI_FRAME_STATISTICS stats{};const double deadline=qpc()+0.0025;bool statistics=true;
                    for(;;){
                        if(FAILED(native->GetFrameStatistics(&stats))){statistics=false;break;}
                        if(stats.PresentCount==st.lastPresentId||qpc()>deadline)break;
                        YieldProcessor();
                    }
                    UINT before=0;
                    if(statistics&&stats.PresentCount==st.lastPresentId&&SUCCEEDED(native->GetLastPresentCount(&before))&&before==st.lastPresentId){
                        const unsigned slots=vision::sync::slotsPerFrame(st.sequence);
                        const int slot=(st.lastSlot+1)%int(slots);
                        HRESULT result;
                        {
                            DrawScope scope(st);
                            drawSlot(st.cmd,st,st.chain->get_current_back_buffer_index(),vision::sync::slotEye(st.sequence,unsigned(slot)));
                            result=native->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
                        }
                        UINT after=0;
                        if(result==S_OK&&SUCCEEDED(native->GetLastPresentCount(&after))&&uint32_t(after-before)==1){
                            record(after,slot);st.lastPresentId=after;st.lastSlot=slot;++st.fillerPresents;
                            publish(st,native,true);
                        }else if(result!=DXGI_ERROR_WAS_STILL_DRAWING&&result!=DXGI_STATUS_OCCLUDED)++st.fillerErrors;
                    }
                }
            }
        }
        guard->Leave();
    }
}
void startFiller(){if(!g_filler.joinable())g_filler=std::jthread(runFiller);}
void stopFiller(){if(g_filler.joinable()){g_filler.request_stop();g_filler.join();}}

bool onCreateSwapchain(device_api api,swapchain_desc& desc,void*){
    if(api!=device_api::d3d11&&api!=device_api::d3d12)return false;
    desc.sync_interval=1; // every slot must occupy exactly one refresh; tearing or interval 0 would drop slots
    // Preserve the game's buffer count and waitable-swap-chain configuration.
    return true;
}
void onInitSwapchain(swapchain* sc,bool){
    device* dev=sc->get_device();const device_api api=dev->get_api();if(api!=device_api::d3d11&&api!=device_api::d3d12)return;
    std::lock_guard lock(g_mutex);loadSettings();
    auto& st=g_states[sc];st.release(false);st=SwapState{};st.dev=dev;st.chain=sc;st.d3d12=api==device_api::d3d12;
    st.depthMode=g_depth.source<0?st.d3d12:g_depth.source==1;st.depth.reversedOverride=g_depth.reversed;st.depth.filterWidth=g_depth.width;st.depth.filterHeight=g_depth.height;
    resource_desc bd=dev->get_resource_desc(sc->get_back_buffer(0));st.width=bd.texture.width;st.height=bd.texture.height;st.fmt=format_to_default_typed(bd.texture.format);
    if(bd.texture.samples!=1 || bd.texture.depth_or_layers!=1){fail(st,"Multisampled or array back buffers are not supported by this hook");return;}
    ID3D11Device* nativeDevice=nullptr;Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if(!st.d3d12){
        nativeDevice=reinterpret_cast<ID3D11Device*>(dev->get_native());
        nativeDevice->GetImmediateContext(&context);
        if(FAILED(context.As(&st.context)) || FAILED(vision::hook::createContextState(nativeDevice,&st.contextState))){
            fail(st,"D3D11.1 context-state isolation unavailable; stereo disabled");return;
        }
    }
    std::string error;if(!compileShaders(error)){st.failure="Shader compile failed: "+error;return;}
    if(!createPipeline(dev,st.fmt,g_ps,1,12,st.layout,st.pipe)){st.failure="Pipeline creation failed";return;}
    if(!createPipeline(dev,st.fmt,g_depthPs,2,16,st.depthLayout,st.depthPipe)){st.failure="Depth stereo pipeline creation failed";return;}
    sampler_desc sd{};sd.filter=filter_mode::min_mag_mip_linear;sd.address_u=sd.address_v=sd.address_w=texture_address_mode::clamp;if(!dev->create_sampler(sd,&st.samp)){st.failure="Sampler failed";return;}
    resource_desc cd(st.width,st.height,1,1,st.fmt,1,memory_heap::default_,resource_usage::shader_resource|resource_usage::copy_dest);
    if(!dev->create_resource(cd,nullptr,resource_usage::shader_resource,&st.copy)){st.failure="Copy texture failed";return;}
    if(!dev->create_resource_view(st.copy,resource_usage::shader_resource,resource_view_desc(st.fmt),&st.copyView)){st.failure="Copy view failed";return;}
    if(st.d3d12){
        const resource_desc ed(st.width,st.height,1,1,st.fmt,1,memory_heap::default_,resource_usage::render_target|eyeState());
        for(int e=0;e<2;++e)if(!dev->create_resource(ed,nullptr,eyeState(),&st.eyes[e])||!dev->create_resource_view(st.eyes[e],resource_usage::render_target,resource_view_desc(st.fmt),&st.eyeViews[e])){st.failure="Eye image creation failed";return;}
        if(!dev->create_resource(ed,nullptr,eyeState(),&st.black)||!dev->create_resource_view(st.black,resource_usage::render_target,resource_view_desc(st.fmt),&st.blackView)){st.failure="Black image creation failed";return;}
    }else{
        st.probe.initialize(nativeDevice,static_cast<DXGI_FORMAT>(st.fmt));
        // Refresh filling needs the swap chain's output (to wait for vblank) and a context lock that
        // makes our draw+Present sequence atomic against the game's own rendering thread.
        auto* nativeChain=reinterpret_cast<IDXGISwapChain*>(sc->get_native());
        st.fillerReady=nativeChain&&SUCCEEDED(nativeChain->GetContainingOutput(&st.output))&&SUCCEEDED(context.As(&st.lock));
        if(st.fillerReady)st.lock->SetMultithreadProtected(TRUE);
        else reshade::log::message(reshade::log::level::warning,"[Vision] No output or context lock: each game frame covers its own refreshes only, so slots repeat on refreshes the game does not fill.");
    }
    for(uint32_t i=0;i<sc->get_back_buffer_count();i++){resource_view v{};if(!dev->create_resource_view(sc->get_back_buffer(i),resource_usage::render_target,resource_view_desc(st.fmt),&v)){st.failure="Render target view failed";return;}st.targets.push_back(v);}
    st.ready=true;st.failure.clear();
    reshade::log::message(reshade::log::level::info,("[Vision] Hook ready on "+std::to_string(st.width)+"x"+std::to_string(st.height)+(st.d3d12?" Direct3D 12":" Direct3D 11")+" swap chain: "+(st.depthMode?"depth-based stereo from the game's own depth buffer":"splitting side-by-side stereo")).c_str());
}
void onDestroySwapchain(swapchain* sc,bool){
    // Stop the filler before taking the lock: it takes the same lock every refresh, and stopping
    // here rather than in DllMain keeps the join away from the Windows loader lock.
    stopFiller();
    std::lock_guard lock(g_mutex);auto it=g_states.find(sc);if(it==g_states.end())return;
    if(g_owner==sc){it->second.active=false;if(g_host.shared)publish(it->second,nullptr,true);g_owner=nullptr;resetRecords();vision::hook::depth::stopWatching();}
    if(g_presentChain==sc)g_presentChain=nullptr;
    it->second.release(true);g_states.erase(it);
}

// Capture only the presentation identity here. Work after effects/overlay (reshade_present) so
// every slot comes from the same completed image.
void onPresent(command_queue*,swapchain* sc,const rect* src,const rect* dst,uint32_t dirtyCount,const rect*){
    vision::hook::depth::g_presenting.store(true);vision::hook::depth::g_composing.store(false);
    std::lock_guard lock(g_mutex);g_presentChain=sc;
    auto it=g_states.find(sc);if(it==g_states.end())return;auto& st=it->second;
    st.awaitingRuntime=true;
    if(st.pendingSlot>=0)fail(st,"Game Present did not complete; stereo paused until disabled/re-enabled");
    if(src||dst||dirtyCount)fail(st,"Partial presents are unsupported; stereo disabled to preserve the game image");
}
// Ctrl+Shift chords while the game has focus. Depth changes are saved to ReShade.ini at once.
void handleKeys(effect_runtime* runtime,SwapState& st){
    if(!runtime->is_key_down(VK_CONTROL)||!runtime->is_key_down(VK_SHIFT))return;
    if(runtime->is_key_pressed(VK_F8)){
        st.diagnosticMode=(st.diagnosticMode+1)%(st.depthMode?5:4);st.blocked=false;st.failure.clear();st.pendingSlot=-1;st.lastSlot=-1;resetRecords();
        reshade::log::message(reshade::log::level::info,("[Vision] Diagnostic mode "+std::to_string(st.diagnosticMode)+" (0=sequential, 1=original image, 2=left only, 3=right only"+(st.depthMode?", 4=depth view)":")")).c_str());
    }
    if(!st.depthMode)return;
    bool changed=false;
    if(runtime->is_key_pressed(VK_PRIOR)){g_depth.separation=std::min(std::max(g_depth.separation,.002f)*1.2f,.1f);changed=true;}
    if(runtime->is_key_pressed(VK_NEXT)){g_depth.separation=std::max(g_depth.separation/1.2f,.002f);changed=true;}
    if(runtime->is_key_pressed(VK_HOME)){g_depth.convergence=std::min(g_depth.convergence*1.25f,1.f);changed=true;}
    if(runtime->is_key_pressed(VK_END)){g_depth.convergence=std::max(g_depth.convergence/1.25f,.0005f);changed=true;}
    if(runtime->is_key_pressed(VK_INSERT)){g_depth.enabled=!g_depth.enabled;changed=true;}
    if(runtime->is_key_pressed(VK_DELETE)){const DepthSettings defaults;g_depth.separation=defaults.separation;g_depth.convergence=defaults.convergence;g_depth.popOut=defaults.popOut;g_depth.enabled=true;changed=true;}
    if(changed){
        saveSettings();
        char text[160];sprintf_s(text,"[Vision] Depth 3D %s: separation %.2f%%, convergence %.4f, pop-out %.2f",g_depth.enabled?"on":"off",g_depth.separation*100,g_depth.convergence,g_depth.popOut);
        reshade::log::message(reshade::log::level::info,text);
    }
}
// Direct3D 12 game frame: the game's own Present carries the next slot; the filler presents the rest.
void presentFrame12(command_list* cmd,command_queue* queue,SwapState& st,IDXGISwapChain* native,int sequence,unsigned slots,UINT before){
    const bool fill=ensureFiller12(st,queue);
    // The game normally drew into the current back buffer. If it drew into another one, it fetched the
    // index before a filled refresh advanced it: take its image from where it drew, and stop filling
    // after three such frames.
    const uint32_t index=st.chain->get_current_back_buffer_index();uint32_t source=index;
    if(const uint64_t written=vision::hook::depth::takeWritten();written&&written!=st.chain->get_back_buffer(index).handle){
        for(uint32_t i=0;i<st.chain->get_back_buffer_count();++i)if(st.chain->get_back_buffer(i).handle==written)source=i;
        const bool stopFilling=st.fillerReady&&++st.wrongBufferFrames>=3;
        if(!st.fillerReady)++st.wrongBufferFrames;
        if(st.wrongBufferFrames<=10){
            char note[192];sprintf_s(note,"[Vision] Frame %llu was drawn into back buffer %u while buffer %u is current%s",static_cast<unsigned long long>(st.sourceFrames),source,index,stopFilling?"; refresh filling is now off for this game":"");
            reshade::log::message(reshade::log::level::warning,note);
        }
        if(stopFilling)st.fillerReady=false;
    }
    {
        DrawScope scope(st);
        captureFrame(cmd,st,source);
        ensureBlack(cmd,st);
        renderEyes(cmd,st);
        if(st.lastSlot<0){st.phaseRef=-1;st.correctedThrough=0;}
        const int slot=st.lastSlot>=0?int((unsigned(st.lastSlot)+1u+unsigned(trackPhase(st,native,slots)))%slots):0;
        showSlot12(cmd,st,index,vision::sync::slotEye(sequence,unsigned(slot)));
        queue->flush_immediate_command_list();
        st.eyesReady=true;
        st.pendingSlot=slot;st.pendingAfter=before;st.active=true;st.failure.clear();
    }
    if(fill&&st.fillerReady)startFiller();
}
void onStereoPresent(effect_runtime* runtime){
    std::lock_guard lock(g_mutex);
    auto it=g_states.find(g_presentChain);if(it==g_states.end())return;auto& st=it->second;
    if(!st.awaitingRuntime || runtime->get_hwnd()!=st.chain->get_hwnd() || runtime->get_device()!=st.dev)return;
    st.awaitingRuntime=false;
    auto* native=reinterpret_cast<IDXGISwapChain*>(st.chain->get_native());
    if(st.ready){
        std::string probeResult;if(st.context&&st.probe.poll(st.context.Get(),probeResult))reshade::log::message(reshade::log::level::info,probeResult.c_str());
        handleKeys(runtime,st);
    }
    const bool connected=g_host.shared!=nullptr;
    if(!g_host.connect()||!g_host.alive()){
        noteHost(connected?"Lost the connection to Vision Restoration (no heartbeat for 3 s): passing the game through until the app is back":"Vision Restoration is not running, so the game image is passed through unchanged. Start the app, connect the emitter and keep its own output closed; the game then switches to stereo automatically",true);
        st.active=false;g_owner=nullptr;resetRecords();return;
    }
    if(!connected){resetRecords();noteHost("Connected to Vision Restoration (pid "+std::to_string(g_host.shared->hostPid)+")",false);}
    // One swap chain owns this v1 channel. Independent windows must never mix IDs.
    if(g_owner&&g_owner!=st.chain)return;
    auto* sh=g_host.shared;
    if(!st.ready||!sh->enabled){
        st.active=false;st.pendingSlot=-1;st.lastSlot=-1;
        if(!sh->enabled){st.blocked=false;if(st.ready)st.failure.clear();resetRecords();g_owner=nullptr;noteHost(hostPauseReason(sh),true);}
        else noteHost("Vision Restoration enabled the hook, but this swap chain is unusable: "+st.failure,true);
        publish(st,native,false);return;
    }
    noteHost(st.depthMode?"Vision Restoration enabled the hook: depth-based stereo from the game's single camera":"Vision Restoration enabled the hook: splitting the side-by-side image into left/right frames",false);
    if(st.blocked){publish(st,native,false);return;}
    const int packing=sh->packing;const bool swapHalves=sh->swapHalves!=0;
    if(!st.depthMode&&packing!=0&&packing!=1){fail(st,"Unsupported stereo packing; choose side-by-side or top/bottom");publish(st,native,true);return;}
    const int sequence=vision::sync::sequenceKnown(sh->sequence)?sh->sequence:0;const unsigned slots=vision::sync::slotsPerFrame(sequence);
    if(g_owner!=st.chain){g_owner=st.chain;if(st.d3d12)vision::hook::depth::watchBackBuffers(st.chain);}
    if(st.fillerReady&&!st.d3d12)startFiller();
    const float convergence=std::clamp(sh->convergenceMicrounits,-50000,50000)/1000000.f;
    // Stereo area from the host; zero means an older host and the whole output.
    const float bandHeight=sh->bandHeightMicrounits>0?std::clamp(sh->bandHeightMicrounits,100000,1000000)/1000000.f:1.f;
    const float bandCenter=sh->bandHeightMicrounits>0?std::clamp(sh->bandCenterMicrounits,0,1000000)/1000000.f:.5f;
    auto* queue=runtime->get_command_queue();auto* cmd=queue->get_immediate_command_list();
    // A new sequence starts a new cycle, so slot numbers never mix two sequences.
    if(sequence!=st.sequence){st.sequence=sequence;st.lastSlot=-1;}
    st.cmd=cmd;st.queue=queue;st.packing=packing;st.swapHalves=swapHalves;st.convergence=convergence;st.bandHeight=bandHeight;st.bandCenter=bandCenter;
    if(st.diagnosticMode){
        if(st.diagnosticMode>=2){
            DrawScope scope(st);
            const uint32_t index=st.chain->get_current_back_buffer_index();
            captureFrame(cmd,st,index);
            if(st.d3d12){ensureBlack(cmd,st);renderEyes(cmd,st,st.diagnosticMode==4);showSlot12(cmd,st,index,st.diagnosticMode==3?1:0);queue->flush_immediate_command_list();}
            else drawSlot(cmd,st,index,st.diagnosticMode==3?1:0,st.diagnosticMode==4);
        }
        st.active=false;st.pendingSlot=-1;
        st.failure=st.diagnosticMode==1?"Diagnostic: original image; glasses paused":st.diagnosticMode==2?"Diagnostic: left eye only; glasses paused":st.diagnosticMode==3?"Diagnostic: right eye only; glasses paused":"Diagnostic: depth view (mid grey = screen depth, brighter = nearer); glasses paused";
        publish(st,native,true);return;
    }
    ++st.sourceFrames;
    const bool probeFrame=st.context&&!st.probe.pending()&&(st.sourceFrames==30||st.sourceFrames==120||st.sourceFrames==600);
    UINT before=0;
    if(FAILED(native->GetLastPresentCount(&before))){fail(st,"Present IDs unavailable; stereo disabled");publish(st,native,true);return;}
    if(st.d3d12){
        if(st.wrongBufferFrames>=30){
            fail(st,"This Direct3D 12 game keeps drawing into a back buffer other than the current one (it counts its own presents), so stereo is off");
            publish(st,native,true);return;
        }
        presentFrame12(cmd,queue,st,native,sequence,slots,before);
        publish(st,native,true);return;
    }
    // Continue the sequence across game frames. A filled refresh may have shown any slot, so a
    // frame that always started at slot 0 would repeat a slot at every frame boundary.
    const int firstSlot=st.lastSlot>=0?(st.lastSlot+1)%int(slots):0;
    HRESULT result=S_OK;bool complete=true;
    {
        DrawScope scope(st);
        uint32_t index=st.chain->get_current_back_buffer_index();
        captureFrame(cmd,st,index);
        if(probeFrame)st.probe.capture(st.context.Get(),reinterpret_cast<ID3D11Resource*>(st.copy.handle),st.width,st.height,0);
        for(unsigned k=0;k+1<slots;++k){
            const int slot=(firstSlot+int(k))%int(slots);
            drawSlot(cmd,st,index,vision::sync::slotEye(sequence,unsigned(slot)));
            if(probeFrame&&k==0)st.probe.capture(st.context.Get(),reinterpret_cast<ID3D11Resource*>(st.chain->get_back_buffer(index).handle),st.width,st.height,1);
            result=presentSlot(native);
            UINT id=0;
            if(result==S_OK&&SUCCEEDED(native->GetLastPresentCount(&id))&&uint32_t(id-before)==1){
                ++st.nativePresents;record(id,slot);st.lastPresentId=id;st.lastSlot=slot;before=id;
                index=st.chain->get_current_back_buffer_index();
            }else{complete=false;break;}
        }
        if(complete){
            const int slot=(firstSlot+int(slots)-1)%int(slots);
            drawSlot(cmd,st,index,vision::sync::slotEye(sequence,unsigned(slot)));
            if(probeFrame)st.probe.capture(st.context.Get(),reinterpret_cast<ID3D11Resource*>(st.chain->get_back_buffer(index).handle),st.width,st.height,2);
            st.pendingSlot=slot;st.pendingAfter=before;st.active=true;st.failure.clear();
        }else{
            restoreFrame(cmd,st);
            if(result==DXGI_STATUS_OCCLUDED){
                fail(st,"Game window is occluded; stereo will resume when visible",false);
            }else{
                ++st.errors;
                char message[128];sprintf_s(message,"Slot Present failed/stalled (0x%08lX); stereo paused until disabled/re-enabled",static_cast<unsigned long>(result));
                fail(st,message);
            }
        }
    }
    // Publish the accepted slots immediately, before the game's last-slot Present blocks. Every
    // ring write is covered by publish's seqlock.
    publish(st,native,true);
}
void onFinishPresent(command_queue*,swapchain* sc){
    vision::hook::depth::g_presenting.store(false);
    std::lock_guard lock(g_mutex);if(g_presentChain==sc)g_presentChain=nullptr;
    auto it=g_states.find(sc);if(it==g_states.end())return;auto& st=it->second;
    if(g_owner&&g_owner!=sc)return;
    if(!g_host.shared)return;
    auto* native=reinterpret_cast<IDXGISwapChain*>(sc->get_native());
    if(st.pendingSlot>=0){
        UINT id=0;
        // finish_present does not expose HRESULT and can fire for TEST or
        // occlusion. A callback alone is not proof that a new slot was queued.
        if(SUCCEEDED(native->GetLastPresentCount(&id)) && uint32_t(id-st.pendingAfter)==1){
            record(id,st.pendingSlot);st.lastPresentId=id;st.lastSlot=st.pendingSlot;++st.frames;
            if(st.frames==1||st.frames%300==0)reshade::log::message(reshade::log::level::info,("[Vision] Completed stereo frames: "+std::to_string(st.frames)+", present errors: "+std::to_string(st.errors)+", filled refreshes: "+std::to_string(st.fillerPresents)+", phase corrections: "+std::to_string(st.realigned)+(st.depthMode?", depth: "+st.depth.describe():std::string())).c_str());
        }else{++st.errors;fail(st,"The game's Present did not advance by one; stereo paused until disabled/re-enabled");}
        st.pendingSlot=-1;
    }else if(st.awaitingRuntime){
        st.awaitingRuntime=false;st.active=false;
        if(st.ready)st.failure="Waiting for ReShade's completed frame";
    }
    publish(st,native,true);
}
}
extern "C" __declspec(dllexport) const char* NAME="Vision Game Hook";
extern "C" __declspec(dllexport) const char* DESCRIPTION="Shows Direct3D 11 and 12 games in frame-sequential stereo on their own swap chain (side-by-side stereo from the game, or depth-based stereo from the game's depth buffer) and reports timing to Vision Restoration, which drives the NVIDIA 3D Vision emitter.";
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        if(!reshade::register_addon(module))return FALSE;
        // The depth tracker first: its present handler collects the frame's depth statistics before
        // this frame's slots are drawn.
        vision::hook::depth::registerEvents();
        reshade::register_event<reshade::addon_event::create_swapchain>(onCreateSwapchain);
        reshade::register_event<reshade::addon_event::init_swapchain>(onInitSwapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(onDestroySwapchain);
        reshade::register_event<reshade::addon_event::present>(onPresent);
        reshade::register_event<reshade::addon_event::reshade_present>(onStereoPresent);
        reshade::register_event<reshade::addon_event::finish_present>(onFinishPresent);
    }else if(reason==DLL_PROCESS_DETACH){
        reshade::unregister_event<reshade::addon_event::create_swapchain>(onCreateSwapchain);
        reshade::unregister_event<reshade::addon_event::init_swapchain>(onInitSwapchain);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(onDestroySwapchain);
        reshade::unregister_event<reshade::addon_event::present>(onPresent);
        reshade::unregister_event<reshade::addon_event::reshade_present>(onStereoPresent);
        reshade::unregister_event<reshade::addon_event::finish_present>(onFinishPresent);
        vision::hook::depth::unregisterEvents();
        stopFiller();
        {std::lock_guard lock(g_mutex);for(auto& [k,v]:g_states)v.release(false);g_states.clear();g_host.close();}
        reshade::unregister_addon(module);
    }
    return TRUE;
}
