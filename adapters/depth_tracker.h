// SPDX-License-Identifier: GPL-3.0-or-later
// Depth-buffer detection for the game hook's depth-based stereo (games that render one camera).
// Adapted from ReShade's Generic Depth example (examples/09-depth/generic_depth_addon.cpp,
// Copyright (C) 2021 Patrick Mours, BSD-3-Clause): draw counts are gathered per depth-stencil on
// every command list, merged into the queue when the list executes and collected at present.
// Two differences matter for upscaled games:
//  - a depth-stencil at any fraction of the output with the same aspect ratio qualifies, so an
//    upscaler's render resolution counts (DLSS Performance renders exactly half the width, which
//    Generic Depth's default aspect-ratio heuristic rejects);
//  - the choice is sticky: another buffer must be clearly busier for 30 frames before the depth
//    source changes, so a reflection pass cannot make the whole picture's depth jump.
#pragma once
#include <reshade.hpp>
#include <Unknwn.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vision::hook::depth {
using namespace reshade::api;

struct Counts {
    uint32_t draws=0; uint64_t vertices=0; bool reversed=false;
    void add(const Counts& other){draws+=other.draws;vertices+=other.vertices;reversed|=other.reversed;}
};
struct __declspec(uuid("9c5e2b7a-41d3-4f0e-8b6a-3d2f1e0c9a57")) ListState {
    bool queue; uint64_t current=0; std::unordered_map<uint64_t,Counts> perDepth;
    explicit ListState(bool isQueue):queue(isQueue){perDepth.reserve(16);}
};

inline std::shared_mutex g_lock;
inline std::unordered_map<uint64_t,resource_desc> g_resources; // live depth-stencil textures
inline std::vector<command_queue*> g_queues;
inline std::unordered_map<uint64_t,Counts> g_frame;           // collected at the last present
// Set while the hook records its own commands, which must never count as the game's.
inline thread_local bool t_own=false;
// Back buffers of the stereo swap chain. A Direct3D 12 game that renders into one that is not the
// current buffer tracks the index itself, so extra presents would desynchronize it.
inline std::atomic<uint64_t> g_watched[8]{};
inline std::atomic<uint64_t> g_written{0};
// Between a present and its completion, overlays and upscalers (OptiScaler) draw into the buffer
// being presented; those draws say nothing about which buffer the game renders its frames into.
inline std::atomic<bool> g_presenting{false};
// Set from the game's first draw, clear or copy into a back buffer until it presents: a refresh filled
// in that window would advance the buffer the game is drawing into.
inline std::atomic<bool> g_composing{false};

inline void noteTarget(uint64_t target){
    if(!target||!g_watched[0].load(std::memory_order_relaxed)||g_presenting.load(std::memory_order_relaxed))return;
    for(auto& watched:g_watched){
        const uint64_t value=watched.load(std::memory_order_relaxed);if(!value)break;
        if(value==target){g_written.store(target,std::memory_order_relaxed);g_composing.store(true,std::memory_order_relaxed);break;}
    }
}
inline void noteWrite(device* dev,resource_view rtv){
    if(!rtv.handle||!g_watched[0].load(std::memory_order_relaxed)||g_presenting.load(std::memory_order_relaxed))return;
    noteTarget(dev->get_resource_from_view(rtv).handle);
}
inline bool onCopyResource(command_list*,resource,resource dest){if(!t_own)noteTarget(dest.handle);return false;}
inline bool onCopyTextureRegion(command_list*,resource,uint32_t,const subresource_box*,resource dest,uint32_t,const subresource_box*,filter_mode){if(!t_own)noteTarget(dest.handle);return false;}
inline bool onResolveTextureRegion(command_list*,resource,uint32_t,const subresource_box*,resource dest,uint32_t,uint32_t,uint32_t,uint32_t,format){if(!t_own)noteTarget(dest.handle);return false;}
inline bool onClearRenderTarget(command_list* cmd,resource_view rtv,const float*,uint32_t,const rect*){if(!t_own)noteWrite(cmd->get_device(),rtv);return false;}
inline void onInitList(command_list* cmd){cmd->create_private_data<ListState>(false);}
inline void onDestroyList(command_list* cmd){cmd->destroy_private_data<ListState>();}
inline void onInitQueue(command_queue* queue){
    queue->create_private_data<ListState>(true);
    if((queue->get_type()&command_queue_type::graphics)!=0){std::unique_lock lock(g_lock);g_queues.push_back(queue);}
}
inline void onDestroyQueue(command_queue* queue){
    {std::unique_lock lock(g_lock);std::erase(g_queues,queue);}
    queue->destroy_private_data<ListState>();
}
// Direct3D 12 needs no resource flag to copy a texture, but the declared usage keeps ReShade's
// state conversion honest for the copy_source barrier.
inline bool onCreateResource(device* dev,resource_desc& desc,subresource_data*,resource_usage){
    if(dev->get_api()!=device_api::d3d12||desc.type!=resource_type::texture_2d||(desc.usage&resource_usage::depth_stencil)==0||desc.texture.samples>1||(desc.usage&resource_usage::copy_source)!=0)return false;
    desc.usage|=resource_usage::copy_source;return true;
}
inline void onInitResource(device*,const resource_desc& desc,const subresource_data*,resource_usage,resource res){
    if((desc.type!=resource_type::texture_2d&&desc.type!=resource_type::surface)||(desc.usage&resource_usage::depth_stencil)==0)return;
    std::unique_lock lock(g_lock);g_resources[res.handle]=desc;
}
inline void onDestroyResource(device*,resource res){std::unique_lock lock(g_lock);g_resources.erase(res.handle);}
inline void onBindTargets(command_list* cmd,uint32_t count,const resource_view* rtvs,resource_view dsv){
    if(t_own)return;
    auto* state=cmd->get_private_data<ListState>();if(!state)return;
    device* dev=cmd->get_device();
    state->current=dsv.handle?dev->get_resource_from_view(dsv).handle:0;
    for(uint32_t i=0;i<count;++i)noteWrite(dev,rtvs[i]);
}
inline void count(command_list* cmd,uint64_t vertices,uint32_t draws){
    if(t_own)return;
    auto* state=cmd->get_private_data<ListState>();if(!state||!state->current)return;
    // Queue state (an immediate command list) is reset by present on another thread.
    std::shared_lock lock(g_lock,std::defer_lock);if(state->queue)lock.lock();
    auto& counts=state->perDepth[state->current];counts.draws+=draws;counts.vertices+=vertices;
}
inline bool onDraw(command_list* cmd,uint32_t vertices,uint32_t instances,uint32_t,uint32_t){count(cmd,uint64_t(vertices)*instances,1);return false;}
inline bool onDrawIndexed(command_list* cmd,uint32_t indices,uint32_t instances,uint32_t,int32_t,uint32_t){count(cmd,uint64_t(indices)*instances,1);return false;}
inline bool onDrawIndirect(command_list* cmd,indirect_command type,resource,uint64_t,uint32_t draws,uint32_t){if(type!=indirect_command::dispatch)count(cmd,0,draws);return false;}
inline void markReversed(command_list* cmd,resource_view dsv,float value){
    if(t_own||!dsv.handle||value==1.0f)return;
    auto* state=cmd->get_private_data<ListState>();if(!state)return;
    const uint64_t target=cmd->get_device()->get_resource_from_view(dsv).handle;
    std::shared_lock lock(g_lock,std::defer_lock);if(state->queue)lock.lock();
    state->perDepth[target].reversed=true;
}
inline bool onClearDepth(command_list* cmd,resource_view dsv,const float* depth,const uint8_t*,uint32_t,const rect*){if(depth)markReversed(cmd,dsv,*depth);return false;}
inline bool onBeginRenderPass(command_list* cmd,uint32_t count,const render_pass_render_target_desc* rts,const render_pass_depth_stencil_desc* ds,render_pass_flags){
    if(t_own)return false;
    if(ds&&ds->depth_load_op==render_pass_load_op::clear)markReversed(cmd,ds->view,ds->clear_depth);
    resource_view views[8]{};const uint32_t n=std::min<uint32_t>(count,8);for(uint32_t i=0;i<n;++i)views[i]=rts[i].view;
    onBindTargets(cmd,n,views,ds?ds->view:resource_view{});
    return false;
}
inline void onReset(command_list* cmd){if(auto* state=cmd->get_private_data<ListState>()){state->perDepth.clear();state->current=0;}}
inline void merge(ListState& into,const ListState& from){for(const auto& [handle,counts]:from.perDepth)into.perDepth[handle].add(counts);}
inline void onExecute(command_queue* queue,command_list* cmd){
    if(cmd==queue->get_immediate_command_list())return;
    auto* into=queue->get_private_data<ListState>();auto* from=cmd->get_private_data<ListState>();if(!into||!from)return;
    std::unique_lock lock(g_lock);merge(*into,*from);
}
inline void onExecuteSecondary(command_list* cmd,command_list* secondary){
    auto* into=cmd->get_private_data<ListState>();auto* from=secondary->get_private_data<ListState>();if(!into||!from)return;
    std::shared_lock lock(g_lock,std::defer_lock);if(into->queue)lock.lock();merge(*into,*from);
}
// Collect the frame's statistics from every graphics queue. Presents of other swap chains that
// drew nothing leave the last frame's statistics in place.
inline void onPresent(command_queue*,swapchain*,const rect*,const rect*,uint32_t,const rect*){
    std::unique_lock lock(g_lock);
    ListState frame(true);
    for(auto* queue:g_queues)if(auto* state=queue->get_private_data<ListState>()){merge(frame,*state);state->perDepth.clear();}
    if(!frame.perDepth.empty())g_frame=std::move(frame.perDepth);
}

// The selected game depth-stencil and a per-frame copy of it the stereo shader can sample.
struct Capture {
    uint64_t source=0; resource_desc desc{}; bool reversed=false; uint32_t draws=0;
    resource backup{}; resource_view view{};
    uint64_t candidate=0; int candidateFrames=0, missingFrames=0; uint64_t frame=0; uint64_t peakScore=0;
    int reversedOverride=-1; // -1 automatic, 0 standard, 1 reversed (ReShade.ini [VISION] DepthReversed)
    uint32_t filterWidth=0, filterHeight=0; // ReShade.ini [VISION] DepthWidth/DepthHeight, 0 = any size
    struct Retired{resource backup;resource_view view;uint64_t after;};
    std::vector<Retired> retired;

    bool isReversed() const {return reversedOverride>=0?reversedOverride==1:reversed;}
    // Resources a command list in flight may still reference are destroyed 60 frames later.
    void retire(){if(backup.handle||view.handle)retired.push_back({backup,view,frame+60});backup={};view={};}
    void destroyRetired(device* dev,bool all){
        for(auto it=retired.begin();it!=retired.end();){
            if(all||frame>=it->after){if(it->view.handle)dev->destroy_resource_view(it->view);if(it->backup.handle)dev->destroy_resource(it->backup);it=retired.erase(it);}
            else ++it;
        }
    }
    void drop(device* dev){
        if(source&&(dev->get_api()==device_api::d3d11||dev->get_api()==device_api::d3d12))reinterpret_cast<IUnknown*>(source)->Release();
        source=0;reversed=false;draws=0;peakScore=0;
    }
    // Everything, after the caller has waited for the GPU.
    void release(device* dev){drop(dev);retire();destroyRetired(dev,true);candidate=0;candidateFrames=missingFrames=0;}

    // Picks this frame's scene depth and records a copy into the backup on cmd (the caller has
    // set t_own). Returns true when the backup holds depth the game rendered this frame.
    bool update(device* dev,command_list* cmd,uint32_t outWidth,uint32_t outHeight){
        ++frame;destroyRetired(dev,false);
        struct Candidate{uint64_t handle;resource_desc desc;Counts counts;};
        std::vector<Candidate> list;
        {
            std::unique_lock lock(g_lock);
            for(const auto& [handle,counts]:g_frame){auto it=g_resources.find(handle);if(it!=g_resources.end())list.push_back({handle,it->second,counts});}
            g_frame.clear(); // a frame without new statistics has no fresh depth
        }
        const float outAspect=float(outWidth)/float(std::max(outHeight,1u));
        // Reversed-Z games clear the scene depth to 0; weighting those double keeps a UI or
        // post-processing depth target with a few hundred draws from outranking the scene.
        auto score=[](const Candidate& c){return uint64_t(c.counts.draws)*(c.counts.reversed?2u:1u);};
        const Candidate* best=nullptr;const Candidate* current=nullptr;
        for(const auto& c:list){
            const auto& t=c.desc.texture;
            if(!c.counts.draws||t.samples>1||!t.height||std::abs(float(t.width)/float(t.height)-outAspect)>0.1f||t.width*4<outWidth||t.width>outWidth*2)continue;
            if(filterWidth&&(t.width!=filterWidth||t.height!=filterHeight))continue;
            if(c.handle==source)current=&c;
            if(!best||score(c)>score(*best)||(score(c)==score(*best)&&c.counts.vertices>best->counts.vertices))best=&c;
        }
        // Another buffer must stay clearly busier for a whole second before the source changes. While
        // the source draws nothing (pause menu, phone, loading) the picture is 2D, and a replacement
        // far less busy than the source ever was must wait ten seconds, so leaving a menu does not
        // land on the menu's depth buffer. (GTA V: scene 2560x1440 with ~1800 draws, a 4K target ~200.)
        auto hold=[&](const Candidate* next){if(next&&candidate==next->handle)++candidateFrames;else{candidate=next?next->handle:0;candidateFrames=next?1:0;}};
        const Candidate* chosen=best;
        if(source){
            if(current){
                missingFrames=0;peakScore=std::max(peakScore,score(*current));
                if(best!=current&&score(*best)>score(*current)+score(*current)/2)hold(best);else hold(nullptr);
                if(candidateFrames<60)chosen=current;
            }else{
                ++missingFrames;
                if(!best)return false;
                hold(best);
                if(candidateFrames<60||(score(*best)*4<peakScore&&missingFrames<600))return false;
            }
        }
        if(!chosen)return false;
        if(chosen->handle!=source){
            const bool sameShape=backup.handle&&desc.texture.width==chosen->desc.texture.width&&desc.texture.height==chosen->desc.texture.height&&format_to_typeless(desc.texture.format)==format_to_typeless(chosen->desc.texture.format);
            drop(dev);if(!sameShape)retire();
            source=chosen->handle;desc=chosen->desc;candidate=0;candidateFrames=missingFrames=0;peakScore=score(*chosen);
            if(dev->get_api()==device_api::d3d11||dev->get_api()==device_api::d3d12)reinterpret_cast<IUnknown*>(source)->AddRef();
        }
        reversed=reversed||chosen->counts.reversed;draws=chosen->counts.draws;
        if(!backup.handle){
            const resource_desc bd(desc.texture.width,desc.texture.height,1,1,format_to_typeless(desc.texture.format),1,memory_heap::default_,resource_usage::shader_resource|resource_usage::copy_dest);
            if(!dev->create_resource(bd,nullptr,resource_usage::shader_resource,&backup)){backup={};return false;}
            if(!dev->create_resource_view(backup,resource_usage::shader_resource,resource_view_desc(format_to_default_typed(desc.texture.format)),&view)){dev->destroy_resource(backup);backup={};view={};return false;}
        }
        const resource src{source};
        const resource_usage old=desc.usage&(resource_usage::depth_stencil|resource_usage::shader_resource);
        cmd->barrier(backup,resource_usage::shader_resource,resource_usage::copy_dest);
        cmd->barrier(src,old,resource_usage::copy_source);
        cmd->copy_resource(src,backup);
        cmd->barrier(src,resource_usage::copy_source,old);
        cmd->barrier(backup,resource_usage::copy_dest,resource_usage::shader_resource);
        return true;
    }
    std::string describe() const {
        if(!source)return "no depth buffer found yet";
        char text[96];sprintf_s(text,"%ux%u %s depth, %u draws",desc.texture.width,desc.texture.height,isReversed()?"reversed":"standard",draws);return text;
    }
};

inline void watchBackBuffers(swapchain* chain){
    const uint32_t n=std::min<uint32_t>(chain->get_back_buffer_count(),8);
    for(uint32_t i=8;i-->0;)g_watched[i].store(i<n?chain->get_back_buffer(i).handle:0); // index 0 last: it enables the check
    g_written.store(0);
}
inline void stopWatching(){for(auto& watched:g_watched)watched.store(0);g_written.store(0);g_composing.store(false);}
inline uint64_t takeWritten(){return g_written.exchange(0);}

inline void registerEvents(){
    reshade::register_event<reshade::addon_event::init_command_list>(onInitList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyList);
    reshade::register_event<reshade::addon_event::init_command_queue>(onInitQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyQueue);
    reshade::register_event<reshade::addon_event::create_resource>(onCreateResource);
    reshade::register_event<reshade::addon_event::init_resource>(onInitResource);
    reshade::register_event<reshade::addon_event::destroy_resource>(onDestroyResource);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindTargets);
    reshade::register_event<reshade::addon_event::begin_render_pass>(onBeginRenderPass);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(onDrawIndirect);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(onClearDepth);
    reshade::register_event<reshade::addon_event::reset_command_list>(onReset);
    reshade::register_event<reshade::addon_event::execute_command_list>(onExecute);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(onExecuteSecondary);
    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_event<reshade::addon_event::copy_resource>(onCopyResource);
    reshade::register_event<reshade::addon_event::copy_texture_region>(onCopyTextureRegion);
    reshade::register_event<reshade::addon_event::resolve_texture_region>(onResolveTextureRegion);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(onClearRenderTarget);
}
inline void unregisterEvents(){
    reshade::unregister_event<reshade::addon_event::init_command_list>(onInitList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(onDestroyList);
    reshade::unregister_event<reshade::addon_event::init_command_queue>(onInitQueue);
    reshade::unregister_event<reshade::addon_event::destroy_command_queue>(onDestroyQueue);
    reshade::unregister_event<reshade::addon_event::create_resource>(onCreateResource);
    reshade::unregister_event<reshade::addon_event::init_resource>(onInitResource);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(onDestroyResource);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(onBindTargets);
    reshade::unregister_event<reshade::addon_event::begin_render_pass>(onBeginRenderPass);
    reshade::unregister_event<reshade::addon_event::draw>(onDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(onDrawIndirect);
    reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(onClearDepth);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(onReset);
    reshade::unregister_event<reshade::addon_event::execute_command_list>(onExecute);
    reshade::unregister_event<reshade::addon_event::execute_secondary_command_list>(onExecuteSecondary);
    reshade::unregister_event<reshade::addon_event::present>(onPresent);
    reshade::unregister_event<reshade::addon_event::copy_resource>(onCopyResource);
    reshade::unregister_event<reshade::addon_event::copy_texture_region>(onCopyTextureRegion);
    reshade::unregister_event<reshade::addon_event::resolve_texture_region>(onResolveTextureRegion);
    reshade::unregister_event<reshade::addon_event::clear_render_target_view>(onClearRenderTarget);
}
}
