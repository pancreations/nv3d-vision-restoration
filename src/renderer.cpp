#include "renderer.h"
#include "stereo_blit_shader.h"
#include "stereo_shader_bytecode.h"
#include "window_worker.h"
#include <d3dcompiler.h>
#include <d3dkmthk.h>
#include <avrt.h>
#include <wincodec.h>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <numeric>

namespace vision {
Surface::~Surface(){target.Reset();swap.Reset();if(waitable)CloseHandle(waitable);}
void Surface::create(HWND window,const LUID* luid,bool useHDR){
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter> adapter;
    if(luid)check(factory->EnumAdapterByLuid(*luid,IID_PPV_ARGS(&adapter)),"Select display GPU");
    check(D3D11CreateDevice(adapter.Get(),adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create D3D11 device");
    RECT r{};GetClientRect(window,&r);width=std::max(1L,r.right);height=std::max(1L,r.bottom);hdr=useHDR;
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=4;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain),"Create flip swap chain");check(chain.As(&swap),"Waitable swap chain");// Three frames queued ahead. With a latency of 1 every frame was rendered at the last moment, so any
    // stall of a few milliseconds while the game held focus missed a refresh; in black frame insertion
    // that put black where a picture belonged (the black flashing, 2026-09-14). The queue absorbs stalls
    // of up to two refreshes; the display kernel still flips one image per refresh.
    check(swap->SetMaximumFrameLatency(3),"Set frame latency");waitable=swap->GetFrameLatencyWaitableObject();if(!waitable)throw std::runtime_error("No frame latency wait handle.");
    factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain3> color;check(swap.As(&color),"Swap chain color space");auto space=hdr?DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;UINT support=0;check(color->CheckColorSpaceSupport(space,&support),"Check HDR color space");if(!(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))throw std::runtime_error("Selected output does not support requested color space.");check(color->SetColorSpace1(space),"Set presentation color space");
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Swap buffer");check(device->CreateRenderTargetView(buffer.Get(),nullptr,&target),"Swap render target");
}
void Surface::resize(unsigned w,unsigned h){if(!w||!h)return;context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();check(swap->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize output");width=w;height=h;ComPtr<ID3D11Texture2D> b;check(swap->GetBuffer(0,IID_PPV_ARGS(&b)),"Resized buffer");check(device->CreateRenderTargetView(b.Get(),nullptr,&target),"Resized target");}
void Surface::bind(){auto* rt=target.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);}
void Surface::setHdr(bool useHDR){
    if(useHDR==hdr)return;
    ComPtr<IDXGISwapChain3> color;check(swap.As(&color),"Swap chain color space");
    const auto space=useHDR?DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();
    check(swap->ResizeBuffers(0,width,height,useHDR?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Change HDR output format");
    // DXGI reports color-space support for the current buffer format.
    UINT support=0;check(color->CheckColorSpaceSupport(space,&support),"Check output color space");
    if(!(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))throw std::runtime_error("Selected output does not support requested color space.");
    check(color->SetColorSpace1(space),"Change output color space");
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)),"HDR output buffer");
    check(device->CreateRenderTargetView(buffer.Get(),nullptr,&target),"HDR output target");hdr=useHDR;
}



struct DrawState {
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> params;ComPtr<ID3D11SamplerState> sampler;
    std::shared_ptr<StereoFrame> active;ComPtr<ID3D11Texture2D> opened;ComPtr<ID3D11ShaderResourceView> view;ComPtr<IDXGIKeyedMutex> key;
    // The source publishes from a pool of three shared textures. Opening the shared resource and
    // creating its view once per texture keeps that work off the per-pair path of the present
    // thread, which has less than one refresh period between the wait and the present.
    struct Opened { std::weak_ptr<StereoFrame> owner;const StereoFrame* frame=nullptr;ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;ComPtr<IDXGIKeyedMutex> key; };
    std::vector<Opened> pool;
    ~DrawState(){release();}
    void release(){view.Reset();opened.Reset();if(key){key->ReleaseSync(0);key.Reset();}active.reset();}
    void init(ID3D11Device* device){
        check(device->CreateVertexShader(compiled::vs,sizeof(compiled::vs),nullptr,&vs),"Vertex shader");
        check(device->CreatePixelShader(compiled::ps,sizeof(compiled::ps),nullptr,&ps),"Pixel shader");
        D3D11_BUFFER_DESC b{};b.ByteWidth=144;b.Usage=D3D11_USAGE_DYNAMIC;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(device->CreateBuffer(&b,nullptr,&params),"Shader parameters");D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&s,&sampler),"Image sampler");
    }
    void accept(ID3D11Device* device,std::shared_ptr<StereoFrame> f){if(!f || (active && f->pairId==active->pairId && f==active))return;
        Opened* entry=nullptr;for(auto& o:pool)if(o.frame==f.get()&&o.owner.lock()==f){entry=&o;break;}
        if(!entry){
            // A pool texture that the source replaced (new size or format) has died with its frame;
            // its entry goes, so a reused address or handle can never select the old texture.
            std::erase_if(pool,[](const Opened& o){return o.owner.expired();});if(pool.size()>=8)pool.erase(pool.begin());
            Opened o;o.owner=f;o.frame=f.get();check(device->OpenSharedResource(f->sharedHandle,IID_PPV_ARGS(&o.texture)),"Open stereo pair on output adapter");check(o.texture.As(&o.key),"Shared pair mutex");check(device->CreateShaderResourceView(o.texture.Get(),nullptr,&o.view),"Stereo pair view");
            pool.push_back(std::move(o));entry=&pool.back();
        }
        if(entry->key->AcquireSync(0,0)!=S_OK)return;
        release();active=std::move(f);opened=entry->texture;view=entry->view;key=entry->key;
    }
    void draw(ID3D11DeviceContext* context,unsigned w,unsigned h,const Settings& s,Eye eye,bool preview,int pattern,double time,bool overlay=false,bool guardSlot=false,uint64_t refreshIndex=0){
        float p[36]={float(w),float(h),float(time),0,float(int(eye)),float(pattern),preview?1.f:0.f,s.hdr?1.f:0.f,s.depth*4,s.peakNits,s.convergence,s.swapEyes?1.f:0.f,0,0,1,1,s.bandHeight,s.bandCenter,overlay?1.f:0.f,float(s.phaseUs),s.imageGain,std::clamp(s.blackFloor,0.f,.3f),0,0};
        for(size_t i=0;i<8;i++)p[24+i]=s.cancelCrosstalk?std::clamp(s.leakProfile[i],0.f,.95f):0.f;
        p[3]=float(refreshIndex%256);p[22]=guardSlot?1.f:0.f;p[23]=s.guardLevel;
        p[32]=1;
        if(pattern==3 && active){p[12]=float(int(active->packing));p[13]=float(int(active->encoding));p[14]=float(active->width);p[15]=float(active->height);p[32]=active->sdrWhiteLevel;if(active->alignmentApplied)p[10]=0;}
        else if(pattern==3)p[4]=float(int(Eye::Black));
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map shader constants");memcpy(mapped.pData,p,sizeof(p));context->Unmap(params.Get(),0);
        auto* cb=params.Get();auto* srv=view.Get();auto* sm=sampler.Get();context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sm);context->Draw(3,0);srv=nullptr;context->PSSetShaderResources(0,1,&srv);
    }
};
// A game rendering on the same GPU queues command buffers of several milliseconds; the graphics
// kernel time-slices between processes, so the presenter's one-triangle draw can wait behind
// them and miss its refresh. A higher scheduling class for this process puts the presenter's
// work ahead of normal processes. HIGH may need SeIncreaseBasePriorityPrivilege; the next
// class down is tried when the kernel refuses. Reported in the status for the session log.
static std::string raiseGpuSchedulingPriority(){
    struct Level { D3DKMT_SCHEDULINGPRIORITYCLASS level;const char* name; };
    for(Level l:{Level{D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH,"high"},Level{D3DKMT_SCHEDULINGPRIORITYCLASS_ABOVE_NORMAL,"above normal"}})
        if(D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),l.level)==0)return l.name;
    return "normal (kernel refused a higher class)";
}
void Presenter::start(HWND window,const Display& d,const Settings& s,bool preview,int pattern){stop();validate(s);if(s.hdr && !d.hdrEnabled)throw std::runtime_error("Enable HDR for this display in Windows before starting HDR output.");{std::lock_guard l(mutex_);settings_=s;pattern_=pattern;status_={};status_.running=true;status_.preview=preview;paused_=false;++revision_;}emitter_.configure(s);worker_=std::jthread([this,window,d,preview](std::stop_token stop){run(stop,window,d,preview);});}
void Presenter::stop(){stopWindowWorker(worker_);emitter_.suspend();std::lock_guard l(mutex_);status_.running=false;status_.locked=false;}
void Presenter::configure(const Settings& s,int pattern){validate(s);emitter_.configure(s);std::lock_guard l(mutex_);
    // Phase, duration, eye swap, pattern and scene depth are read every frame and must apply live.
    // Only changes that alter the presentation cadence or surface need a full resync.
    bool resync=s.sequence!=settings_.sequence||s.refresh!=settings_.refresh||s.hdr!=settings_.hdr||s.width!=settings_.width||s.height!=settings_.height;
    settings_=s;pattern_=pattern;if(resync){status_.timingPassed=false;++revision_;}}
void Presenter::pause(bool p){std::lock_guard l(mutex_);if(paused_==p)return;paused_=p;++revision_;if(p)emitter_.suspend();}
// Synchronization is never restarted automatically by occlusion, focus loss or slips. This is the
// only way the viewer re-locks it, so it can be reached from a control the game's focus does not steal.
void Presenter::resync(){std::lock_guard l(mutex_);resyncRequested_=true;}
void Presenter::showPhase(bool on){std::lock_guard l(mutex_);overlay_=on;}
RenderStatus Presenter::status()const{std::lock_guard l(mutex_);return status_;}
void Presenter::run(std::stop_token stop,HWND window,Display display,bool preview){
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Games",&task);
    // The output window normally sits unfocused behind the game (the pad follows focus). Windows 11
    // throttles background processes (EcoQoS, ignored timer resolution), which made the presenter and
    // the USB thread miss eye deadlines only while the game had focus. Opt the whole process out.
    if(!preview){SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS);
        PROCESS_POWER_THROTTLING_STATE power{};power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;power.StateMask=0;
        SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power));}
    static const std::string gpuPriority=raiseGpuSchedulingPriority();
    try{
        // Every stage before the first present names itself in the status, so a session log
        // showing zero presents also shows where the output got stuck.
        auto stage=[&](const char* what){std::lock_guard l(mutex_);status_.message=what;};
        stage("Creating the output surface");
        Settings s;{std::lock_guard l(mutex_);s=settings_;}Surface surface;surface.create(window,&display.adapterLuid,s.hdr);stage("Creating the output shaders");DrawState draw;draw.init(surface.device.Get());stage("Waiting for the first frame slot");
        {ComPtr<IDXGIDevice> dxgi;if(SUCCEEDED(surface.device.As(&dxgi)))dxgi->SetGPUThreadPriority(7);}
        struct PresentRecord {UINT id;uint64_t expected;Slot slot;};std::deque<PresentRecord> records;
        TimingTracker clock;uint64_t slotIndex=0,revision=0,lastUsbLate=0;UINT lastObserved=0,lastRefresh=0,lastSubmitted=0;unsigned blank=16;unsigned slipWindow=0;double slipWindowStart=0;
        // Stable present-to-refresh offset. DWM composition (the normal state while the game has focus)
        // reports each present's refresh with a one-refresh wobble from frame to frame; re-anchoring on
        // every report flipped the eye sequence by one refresh every other frame, so black-insertion
        // frames landed on image slots and the picture blacked out and jumped. A new offset is adopted
        // only after it holds for several consecutive presents (a real composition-mode change).
        int64_t presentOffset=0,candidateOffset=0;unsigned candidateCount=0;
        // PresentRefreshCount already identifies the refresh the image reached.
        // Composition mode is diagnostic information, not an extra display delay.
        ComPtr<IDXGISwapChainMedia> media;surface.swap.As(&media);bool composed=false;
        RenderStatus local;local.running=true;local.preview=preview;local.gpuPriority=gpuPriority;local.message=preview?"PREVIEW - side-by-side, no optical validation":"Acquiring presentation timing";
        double begin=qpc(),last=begin,pairTime=0,lastPublish=0,lockedSince=begin;
        std::deque<double> recentIntervals;double intervalSum=0,intervalSumSq=0;
        while(!stop.stop_requested() && IsWindow(window)){
            DWORD wait=WaitForSingleObject(surface.waitable,50);if(wait==WAIT_TIMEOUT){if(!local.presents)stage("Waiting for a frame slot; the swap chain is not turning over");continue;}if(wait!=WAIT_OBJECT_0)throw std::runtime_error("Presentation wait failed.");
            bool paused,overlay,manualResync;int pattern;uint64_t rev;{std::lock_guard l(mutex_);s=settings_;paused=paused_;pattern=pattern_;rev=revision_;overlay=overlay_;manualResync=resyncRequested_;resyncRequested_=false;}
            if(rev!=revision){revision=rev;begin=qpc();lockedSince=begin;local.elapsed=0;local.timingPassed=false;blank=16;slotIndex=0;clock.reset();records.clear();lastObserved=lastRefresh=lastSubmitted=0;emitter_.suspend();}
            // Change the actual buffer precision and color space with the HDR
            // setting, keeping the output window and image source in place.
            surface.setHdr(s.hdr);
            RECT size{};GetClientRect(window,&size);if(size.right<=0||size.bottom<=0){if(!local.presents)stage("The output window has no size");emitter_.suspend();Sleep(20);continue;}
            if(unsigned(size.right)!=surface.width || unsigned(size.bottom)!=surface.height){surface.resize(size.right,size.bottom);blank=16;records.clear();clock.reset();emitter_.suspend();}
            // Anchor the eye sequence to the display refresh counter once statistics are available.
            // A prediction is provisional: feedback below suspends and reacquires
            // synchronization when the image actually lands on a different refresh.
            uint64_t slotSeq=slotIndex;
            if(!preview&&lastObserved){UINT previous=0;if(SUCCEEDED(surface.swap->GetLastPresentCount(&previous)))slotSeq=refreshForPresent(previous+1,lastObserved,lastRefresh);}
            auto slot=sequenceSlot(s.sequence,slotSeq,false);
            if(slot.pairBoundary){draw.accept(surface.device.Get(),source_.latest());pairTime=qpc()-begin;}
            auto emitterStatus=emitter_.status();auto usbState=emitterStatus.state;
            auto resync=[&]{emitter_.suspend();blank=16;slotIndex=0;clock.reset();records.clear();lastObserved=lastRefresh=0;local.resyncs++;local.lastResyncSec=qpc()-begin;local.timingPassed=false;};
            if(manualResync)resync(); // the viewer asked for it; nothing else here restarts synchronization
            // LCD aperture reacquires after a slip; its scheduler cannot free-run.
            // The existing general path rides through a slipped frame, short lead or late USB command: the eye
            // sequence is anchored to the refresh counter and the emitter free-runs one period without
            // a command (its own eye bit still toggles at the boundary), so only that refresh is wrong
            // and the next present-to-refresh mapping corrects the following frame. Slips never
            // resynchronize: a resync blanks 16 frames and re-locks the emitter, and with a game
            // loading the GPU (2026-09-14: a 4K game captured as the source slipped 14 to 23
            // refreshes a second) the old four-slips-per-second rule chained resyncs three to six
            // times a second, which the viewer saw as the glasses going black for seconds. Slips are
            // counted per second and reported instead.
            auto slip=[&](unsigned count){local.misses+=count;slipWindow+=count;if(s.lcd.enabled&&emitterStatus.scheduled)resync();};
            if(!preview&&emitterStatus.late>lastUsbLate)slip(unsigned(emitterStatus.late-lastUsbLate));
            lastUsbLate=emitterStatus.late;
            bool emitterAvailable=usbState==EmitterState::Ready||usbState==EmitterState::Running||usbState==EmitterState::Simulated;
            bool mute=paused || (!preview && (blank>0 || clock.samples<8 || !emitterAvailable));Eye eye=mute?Eye::Black:slot.eye;
            surface.bind();draw.draw(surface.context.Get(),surface.width,surface.height,s,eye,preview && !paused,pattern,pairTime,overlay&&!mute,!mute&&slot.eye==Eye::Black,slotIndex);
            // Predictive trigger for every backend: the eye command is timed from the predicted
            // vblank of the refresh this present will land on, not from retrospective statistics.
            // Retrospective triggering shifted by a frame whenever DWM switched between composition
            // and direct flip, which showed up as sudden crosstalk seconds after a clean start.
            if(!preview&&!mute&&slot.trigger&&lastObserved&&clock.samples>=8&&blank==0) {
                // Predict the next present's refresh before Present can block.
                // Actual refresh statistics below still detect a missed prediction.
                UINT previous=0;check(surface.swap->GetLastPresentCount(&previous),"Predict present identifier");
                const uint64_t nextRefresh=refreshForPresent(previous+1,lastObserved,lastRefresh);
                // Submit on the predicted vblank. Both supported emitters apply phase on-device:
                // NVIDIA through its X delay timer and RP2040 through its scheduled open time.
                const double target=clock.predict(nextRefresh);
                {double leadMs=(target-qpc())*1000;local.leadBins[leadMs<0?0:leadMs<2?1:leadMs<4?2:leadMs<6?3:leadMs<8?4:5]++;}
                if(target-qpc()<.0005) {
                    // Too late to command this refresh: the emitter free-runs one period.
                    slip(1);
                    if(s.lcd.enabled&&emitterStatus.scheduled){mute=true;eye=Eye::Black;draw.draw(surface.context.Get(),surface.width,surface.height,s,eye,false,pattern,pairTime,false,false,slotIndex);}
                } else {
                    Eye commandEye=slot.eye;if(s.swapEyes)commandEye=commandEye==Eye::Left?Eye::Right:Eye::Left;
                    emitter_.submit(commandEye,target,clock.period*1e6);
                }
            }
            UINT id=0;HRESULT h=surface.swap->Present(1,0);
            // Occlusion is a normal, expected state, not a fault: playing a side-by-side game means the
            // game window has focus and covers the output window, and a gamepad only reaches the focused
            // window. Suspending the emitter and blanking 16 frames here re-ran on every occluded present,
            // which the viewer saw as the glasses flashing and the sync resetting the moment they clicked
            // into the game (2026-09-14). The presentation clock and the eye lock are kept across occlusion;
            // only the manual re-lock (UI button / Ctrl+Alt+R) ever restarts synchronization.
            local.occluded=h==DXGI_STATUS_OCCLUDED;
            if(!local.occluded)check(h,"Present stereo output");
            check(surface.swap->GetLastPresentCount(&id),"Present identifier");
            uint64_t expected=lastObserved?refreshForPresent(id,lastObserved,lastRefresh):0;
            records.push_back({id,expected,mute?Slot{Eye::Black,false,false}:slot});while(records.size()>64)records.pop_front();lastSubmitted=id;(void)lastSubmitted;
            DXGI_FRAME_STATISTICS stats{};HRESULT sh=surface.swap->GetFrameStatistics(&stats);
            if(SUCCEEDED(sh)){
                bool modeChanged=false;
                if(media){DXGI_FRAME_STATISTICS_MEDIA fm{};if(SUCCEEDED(media->GetFrameStatisticsMedia(&fm))){bool c=fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSED||fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE;if(c!=composed){composed=c;local.modeChanges++;modeChanged=true;}local.composed=composed;}}
                LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);double sync=double(stats.SyncQPCTime.QuadPart)/double(freq.QuadPart);
                clock.observe(stats.SyncRefreshCount,sync);
                bool mismatch=false;
                if(stats.PresentCount!=lastObserved){
                    auto record=std::find_if(records.begin(),records.end(),[&](auto& r){return r.id==stats.PresentCount;});
                    mismatch=record!=records.end() && record->expected && record->expected!=stats.PresentRefreshCount;
                    const int64_t observedOffset=int64_t(stats.PresentRefreshCount)-int64_t(stats.PresentCount);
                    if(!lastObserved){presentOffset=observedOffset;candidateCount=0;}
                    else if(observedOffset==presentOffset)candidateCount=0;
                    // Two agreeing reports: a real slip moves the offset and keeps it (follow it at once, or every
                    // queued frame shows the wrong slot - 8 turned each slip into ~13 wrong frames), while the
                    // composed wobble alternates every other present and never agrees twice in a row.
                    else if(observedOffset==candidateOffset){if(++candidateCount>=2){presentOffset=candidateOffset;candidateCount=0;}}
                    else{candidateOffset=observedOffset;candidateCount=1;}
                    lastObserved=stats.PresentCount;lastRefresh=uint64_t(int64_t(stats.PresentCount)+presentOffset);
                }
                // A slipped prediction shows one refresh with the wrong eye; the refresh-anchored
                // sequence and the fresh present-to-refresh mapping correct the next frame.
                // A composition-mode change (direct flip <-> DWM composed) moves the present-to-refresh
                // latency by one refresh: the presents already queued land one refresh off, which the
                // mismatch check above counts as slips, and the mapping has caught up by the next
                // frame. Restarting on every mode change made desktop activity that flips composition
                // (another window presenting, an overlay) into repeated black flashes.
                if(!preview&&blank==0&&mismatch)slip(1);(void)modeChanged;
            }else if(sh==DXGI_ERROR_FRAME_STATISTICS_DISJOINT)resync();
            else if(!preview){local.message="Presentation statistics unavailable; stereo triggers disabled";emitter_.suspend();blank=16;}
            if(blank>0){--blank;slotIndex=0;}else ++slotIndex;
            double now=qpc();if(!slipWindowStart)slipWindowStart=now;if(now-slipWindowStart>=1){local.slipsLastSecond=slipWindow;slipWindow=0;slipWindowStart=now;}local.lastIntervalMs=(now-last)*1000;local.maxIntervalMs=std::max(local.maxIntervalMs,local.lastIntervalMs);last=now;local.presents++;local.elapsed=now-begin;local.measuredHz=clock.period?1/clock.period:0;local.sourcePair=draw.active?draw.active->pairId:0;local.locked=!preview && !paused && emitterAvailable && blank==0 && clock.samples>=8;
            {double ms=local.lastIntervalMs;recentIntervals.push_back(ms);intervalSum+=ms;intervalSumSq+=ms*ms;if(recentIntervals.size()>256){double old=recentIntervals.front();recentIntervals.pop_front();intervalSum-=old;intervalSumSq-=old*old;}
             double n=double(recentIntervals.size());double variance=n>1?std::max(0.,intervalSumSq/n-(intervalSum/n)*(intervalSum/n)):0;local.presentJitterUs=std::sqrt(variance)*1000;}
            local.vblankJitterRmsUs=clock.jitterRmsUs;local.vblankJitterMaxUs=clock.jitterMaxUs;local.clockSamples=clock.samples;
            if(!local.locked){lockedSince=now;local.timingPassed=false;} if(!preview)local.elapsed=now-lockedSince;local.timingPassed=local.locked && local.elapsed>=600 && local.resyncs==0;
            if(local.intervals.size()<4096)local.intervals.push_back(local.lastIntervalMs);
            if(preview)local.message="PREVIEW - both eyes side-by-side; no emitter commands";
            else if(paused)local.message="Paused - output black, emitter suspended";
            else if(local.locked)local.message=std::string("Presentation clock acquired (")+(composed?"DWM composed":"direct flip")+") - optical sync still requires your confirmation"+(local.slipsLastSecond?" - "+std::to_string(local.slipsLastSecond)+" slipped refreshes in the last second: the GPU is busy (each slip shows one refresh with the wrong eye; no blanking)":"");
            if(now-lastPublish>.25){std::lock_guard l(mutex_);status_=local;lastPublish=now;}
        }
        emitter_.suspend();{std::lock_guard l(mutex_);local.running=false;local.locked=false;status_=local;}
    }catch(const std::exception& e){emitter_.suspend();std::lock_guard l(mutex_);status_.running=false;status_.locked=false;status_.message=e.what();}
    if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
}
void Presenter::exportReport(const std::filesystem::path& path,const Settings& s,const std::string& source)const{
    auto r=status();auto usb=emitter_.status();std::filesystem::create_directories(path.parent_path());std::ofstream out(path);if(!out)throw std::runtime_error("Cannot write timing report.");
    auto samples=r.intervals;std::sort(samples.begin(),samples.end());auto percentile=[&](double q){return samples.empty()?0:samples[size_t(q*(samples.size()-1))];};
    out<<std::setprecision(10)<<"Vision Restoration timing report\nProfile: "<<s.name<<"\nDisplay: "<<s.displayId<<"\nConnection: "<<s.connection<<"\nMode: "<<s.width<<'x'<<s.height<<" @ "<<s.refresh<<" Hz\nHDR: "<<s.hdr<<"\nPreview: "<<r.preview<<"\nSequence: "<<int(s.sequence)<<"\nPhase us: "<<s.phaseUs<<"\nLeft/right duration us: "<<s.leftUs<<'/'<<s.rightUs<<"\nSource: "<<source<<"\nElapsed seconds: "<<r.elapsed<<"\nPresents: "<<r.presents<<"\nDetected misses: "<<r.misses<<"\nResynchronizations: "<<r.resyncs<<"\nMeasured refresh Hz: "<<r.measuredHz<<"\nCPU present interval p50/p95/p99 ms: "<<percentile(.5)<<'/'<<percentile(.95)<<'/'<<percentile(.99)<<"\nUSB commands/errors/late: "<<usb.commands<<'/'<<usb.errors<<'/'<<usb.late<<"\nLast/max USB transfer us: "<<usb.lastTransferUs<<'/'<<usb.maxTransferUs<<"\nVblank jitter rms/max us: "<<r.vblankJitterRmsUs<<'/'<<r.vblankJitterMaxUs<<"\nPresent interval jitter us: "<<r.presentJitterUs<<"\nEye command timing error last/rms/max us: "<<usb.sendErrorLastUs<<'/'<<usb.sendErrorRmsUs<<'/'<<usb.sendErrorMaxUs<<"\nTiming block writes: "<<usb.timingWrites<<"\nGlasses operation confirmed: "<<s.glassesConfirmed<<"\nEye order confirmed: "<<s.eyeConfirmed<<"\nUser assessment: "<<s.assessment<<"\nMonitor settings: "<<s.monitorNotes<<"\nValidated: "<<s.validated<<"\nThese are host timings, not optical measurements.\n";
    out<<"Emitter identity: "<<usb.identity<<"\nFirmware: "<<usb.firmwareVersion<<"\nPredictive device scheduling: "<<usb.scheduled<<"\nClock uncertainty us: "<<usb.clockUncertaintyUs<<"\nDevice open/close commands: "<<usb.deviceOpens<<'/'<<usb.deviceCloses<<"\nInterval distribution uses the first "<<samples.size()<<" collected samples.\n";
    if(s.lcd.enabled){const auto& t=s.lcd;auto e=lcdExposure(t,apertureWindowHz(r.measuredHz>0?r.measuredHz:s.refresh,s.sequence),s.signalScanUs);
        out<<"LCD temporal aperture: enabled\nSettle us: "<<t.settleUs<<"\nDuration us: "<<t.durationUs<<"\nGlobal phase us: "<<t.phaseUs<<"\nLeft/right adjustment us: "<<t.leftAdjustUs<<'/'<<t.rightAdjustUs<<"\nGuard us: "<<t.guardUs<<"\nScan compensation: "<<t.compensateScanout<<"\nScan/reference: "<<(t.scanoutUs>0?t.scanoutUs:s.signalScanUs)<<'/'<<t.referencePosition<<"\nEye swap: "<<s.swapEyes<<"\nCalibration region: "<<t.target<<"\nExposure valid: "<<e.valid<<"\nL open/close us: "<<e.openUs[0]<<'/'<<e.closeUs[0]<<"\nR open/close us: "<<e.openUs[1]<<'/'<<e.closeUs[1]<<"\nApplied device period/duration us: "<<usb.aperturePeriodUs<<'/'<<usb.apertureDurationUs<<"\n";
    }
    if(!out)throw std::runtime_error("Report write failed.");
}
namespace {
LRESULT CALLBACK probeProc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
struct LineFit{double period=0,epoch=0,rms=0,maxAbs=0;size_t n=0;};
LineFit fitLine(const std::vector<std::pair<double,double>>& s){LineFit f;f.n=s.size();if(s.size()<2)return f;double mx=0,my=0;for(auto& p:s){mx+=p.first;my+=p.second;}mx/=double(s.size());my/=double(s.size());double sxx=0,sxy=0;for(auto& p:s){sxx+=(p.first-mx)*(p.first-mx);sxy+=(p.first-mx)*(p.second-my);}if(sxx<=0)return f;f.period=sxy/sxx;f.epoch=my-f.period*mx;for(auto& p:s){double d=p.second-(f.epoch+f.period*p.first);f.rms+=d*d;f.maxAbs=std::max(f.maxAbs,std::abs(d));}f.rms=std::sqrt(f.rms/double(s.size()));return f;}
}
// The NVIDIA driver reports InVerticalBlank with ScanLine counting from zero inside the
// blanking interval, so "wake time - line x line time" recovers the vblank start. DXGI's
// SyncQPCTime for the same refresh is a fixed offset later; the scan of row 0 starts when
// the blanking interval ends. Measured 2026-09-13 on this PC: DXGI timestamps fit a line to
// about 6 us rms, the scan-line estimate to about 25 us, so DXGI drives the presenter and
// this measurement only calibrates the origin and reports jitter.
VblankMeasurement measureVblank(const Display& display,double seconds){
    VblankMeasurement m;
    if(display.totalLines==0||display.refresh<=0){m.message="No signal timing for this output.";return m;}
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open{};wcscpy_s(open.DeviceName,display.gdiName.c_str());
    if(D3DKMTOpenAdapterFromGdiDisplayName(&open)!=0){m.message="The graphics kernel did not open this output for scan-line queries.";return m;}
    struct Wake{double t;UINT line;BOOLEAN inVb;};std::vector<Wake> wakes;wakes.reserve(size_t(seconds*400));
    std::vector<std::pair<double,double>> dxgi;std::atomic<bool> stop{false};
    std::jthread waiter([&]{
        while(!stop){D3DKMT_WAITFORVERTICALBLANKEVENT w{open.hAdapter,0,open.VidPnSourceId};if(D3DKMTWaitForVerticalBlankEvent(&w)!=0){Sleep(1);continue;}double t=qpc();D3DKMT_GETSCANLINE g{open.hAdapter,open.VidPnSourceId};if(D3DKMTGetScanLine(&g)==0)wakes.push_back({t,g.ScanLine,g.InVerticalBlank});}
    });
    try{
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=probeProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionRestorationVblankProbe";RegisterClassExW(&wc);
        HWND window=CreateWindowExW(0,wc.lpszClassName,L"Vblank probe",WS_POPUP|WS_VISIBLE,display.rect.left+100,display.rect.top+100,320,180,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)throw std::runtime_error("Cannot create the probe window.");
        Surface surface;surface.create(window,&display.adapterLuid,false);
        double start=qpc();UINT lastSync=0;
        while(qpc()-start<seconds){MSG msg;while(PeekMessageW(&msg,window,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            if(WaitForSingleObject(surface.waitable,100)!=WAIT_OBJECT_0)continue;
            surface.bind();float clear[]{0,0,0,1};surface.context->ClearRenderTargetView(surface.target.Get(),clear);surface.swap->Present(1,0);
            DXGI_FRAME_STATISTICS st{};if(SUCCEEDED(surface.swap->GetFrameStatistics(&st))&&st.SyncRefreshCount!=lastSync){lastSync=st.SyncRefreshCount;LARGE_INTEGER f;QueryPerformanceFrequency(&f);dxgi.emplace_back(double(st.SyncRefreshCount),double(st.SyncQPCTime.QuadPart)/double(f.QuadPart));}}
        stop=true;waiter.join();DestroyWindow(window);
    }catch(const std::exception& e){stop=true;waiter.join();D3DKMT_CLOSEADAPTER close{open.hAdapter};D3DKMTCloseAdapter(&close);m.message=e.what();return m;}
    D3DKMT_CLOSEADAPTER close{open.hAdapter};D3DKMTCloseAdapter(&close);
    const double period=1.0/display.refresh,lineTime=period/display.totalLines;
    std::vector<std::pair<double,double>> starts;std::vector<double> lateLines;
    for(auto& w:wakes){if(!w.inVb)continue;starts.emplace_back(0,w.t-w.line*lineTime);lateLines.push_back(double(w.line));}
    if(!starts.empty()){double e0=starts[0].second;for(auto& p:starts)p.first=std::round((p.second-e0)/period);}
    auto sf=fitLine(starts),df=fitLine(dxgi);
    m.signalHz=display.refresh;m.dxgiSamples=unsigned(df.n);m.scanlineSamples=unsigned(sf.n);
    if(df.n<8||df.period<=0){m.message="Too few presentation statistics; is the output attached and not in a power-saving state?";return m;}
    m.measuredHz=1/df.period;m.dxgiJitterRmsUs=df.rms*1e6;m.dxgiJitterMaxUs=df.maxAbs*1e6;m.scanlineJitterRmsUs=sf.rms*1e6;
    std::sort(lateLines.begin(),lateLines.end());auto pct=[&](double q){return lateLines.empty()?0.0:lateLines[std::min(lateLines.size()-1,size_t(q*double(lateLines.size())))];};
    m.wakeLatencyP50Us=pct(.5)*lineTime*1e6;m.wakeLatencyMaxUs=pct(1)*lineTime*1e6;
    m.vblankLengthUs=double(display.totalLines-display.activeLines)*lineTime*1e6;
    std::vector<double> offsets;for(auto& d:dxgi){double best=1e9;for(auto& s:starts){double off=d.second-s.second;if(std::abs(off)<std::abs(best))best=off;}if(std::abs(best)<period/2)offsets.push_back(best*1e6);}
    if(offsets.size()>=8){std::sort(offsets.begin(),offsets.end());m.syncAfterVblankStartUs=offsets[offsets.size()/2];m.scanStartAfterSyncUs=m.vblankLengthUs-m.syncAfterVblankStartUs;m.ok=true;
        std::ostringstream text;text<<std::fixed<<std::setprecision(1)<<"Vblank measured: "<<std::setprecision(3)<<m.measuredHz<<" Hz"<<std::setprecision(1)<<", blanking "<<m.vblankLengthUs<<" us; presentation timestamp "<<m.syncAfterVblankStartUs<<" us after the blanking starts, so the scan of row 0 begins "<<m.scanStartAfterSyncUs<<" us after the timestamp. Timestamp jitter rms "<<m.dxgiJitterRmsUs<<" us, max "<<m.dxgiJitterMaxUs<<" us ("<<m.dxgiSamples<<" refreshes).";m.message=text.str();}
    else{std::ostringstream text;text<<std::fixed<<std::setprecision(1)<<"Presentation timestamps fit "<<std::setprecision(3)<<m.measuredHz<<" Hz"<<std::setprecision(1)<<" with jitter rms "<<m.dxgiJitterRmsUs<<" us, but the scan-line counter gave no usable vblank samples on this GPU; the scan start offset stays unchanged.";m.message=text.str();m.ok=true;}
    return m;
}
void saveSurfacePng(Surface& surface,const std::filesystem::path& path){
    ComPtr<ID3D11Texture2D> buffer;check(surface.swap->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Snapshot buffer");
    saveTexturePng(surface.device.Get(),surface.context.Get(),buffer.Get(),path);
}
void saveSharedFramePng(const StereoFrame& frame,const LUID& luid,const std::filesystem::path& path){
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Snapshot factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"Snapshot adapter");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Snapshot device");
    ComPtr<ID3D11Texture2D> shared;check(device->OpenSharedResource(frame.sharedHandle,IID_PPV_ARGS(&shared)),"Open the published pair");ComPtr<IDXGIKeyedMutex> key;check(shared.As(&key),"Published pair mutex");
    check(key->AcquireSync(0,2000),"Acquire the published pair");
    D3D11_TEXTURE2D_DESC d{};shared->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_DEFAULT;ComPtr<ID3D11Texture2D> copy;check(device->CreateTexture2D(&d,nullptr,&copy),"Snapshot copy");
    D3D11_BOX box{0,0,0,frame.width,frame.height,1};context->CopySubresourceRegion(copy.Get(),0,0,0,0,shared.Get(),0,&box);key->ReleaseSync(0);
    saveTexturePng(device.Get(),context.Get(),copy.Get(),path);
}
void saveTexturePng(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,const std::filesystem::path& path){
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    const bool bgra=d.Format==DXGI_FORMAT_B8G8R8A8_UNORM,rgba=d.Format==DXGI_FORMAT_R8G8B8A8_UNORM,half=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(!bgra&&!rgba&&!half)throw std::runtime_error("Snapshot supports 8-bit RGBA/BGRA and FP16 textures only.");
    d.BindFlags=d.MiscFlags=0;d.MipLevels=1;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&d,nullptr,&staging),"Snapshot staging");context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture,0,nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Snapshot read");
    std::vector<uint8_t> pixels(size_t(d.Width)*d.Height*4);
    auto toHalf=[](uint16_t h){unsigned e=(h>>10)&31,m=h&1023;float v=e==0?m/1024.f*powf(2.f,-14.f):(1+m/1024.f)*powf(2.f,float(int(e)-15));return h&0x8000?-v:v;};
    auto encode=[](float c){c=std::clamp(c,0.f,1.f);return uint8_t(std::lround((c<=.0031308f?12.92f*c:1.055f*powf(c,1/2.4f)-.055f)*255));};
    for(UINT y=0;y<d.Height;y++){auto* row=static_cast<uint8_t*>(map.pData)+y*map.RowPitch;auto* out=pixels.data()+size_t(y)*d.Width*4;
        if(half){auto* v=reinterpret_cast<uint16_t*>(row);for(UINT x=0;x<d.Width;x++){out[x*4+0]=encode(toHalf(v[x*4+2]));out[x*4+1]=encode(toHalf(v[x*4+1]));out[x*4+2]=encode(toHalf(v[x*4+0]));out[x*4+3]=255;}}
        else{std::copy_n(row,d.Width*4,out);if(rgba)for(UINT x=0;x<d.Width;x++)std::swap(out[x*4],out[x*4+2]);}}
    context->Unmap(staging.Get(),0);
    ComPtr<IWICImagingFactory> wic;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"Snapshot WIC");
    std::filesystem::create_directories(path.parent_path());ComPtr<IWICStream> stream;check(wic->CreateStream(&stream),"Snapshot stream");check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Snapshot file");
    ComPtr<IWICBitmapEncoder> encoder;check(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG stream");
    ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame");check(frame->Initialize(nullptr),"PNG initialize");check(frame->SetSize(d.Width,d.Height),"PNG size");auto format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format),"PNG format");if(format!=GUID_WICPixelFormat32bppBGRA)throw std::runtime_error("Unexpected PNG pixel format.");check(frame->WritePixels(d.Height,d.Width*4,UINT(pixels.size()),pixels.data()),"PNG pixels");check(frame->Commit(),"PNG frame commit");check(encoder->Commit(),"PNG commit");
}
bool runGpuSelfTest(const std::filesystem::path& directory){
    std::filesystem::create_directories(directory);ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP test device");DrawState draw;draw.init(device.Get());
    std::ofstream report(directory/L"gpu-test.txt");
    for(bool hdr:{false,true}){
        D3D11_TEXTURE2D_DESC d{};d.Width=640;d.Height=360;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,nullptr,&texture),"Test target");ComPtr<ID3D11RenderTargetView> rt;check(device->CreateRenderTargetView(texture.Get(),nullptr,&rt),"Test RTV");auto* target=rt.Get();context->OMSetRenderTargets(1,&target,nullptr);D3D11_VIEWPORT vp{0,0,640,360,0,1};context->RSSetViewports(1,&vp);
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> read;check(device->CreateTexture2D(&d,nullptr,&read),"Test staging");
        for(int pattern:{0,1,2,4,5,6}){Settings s;s.hdr=hdr;draw.draw(context.Get(),640,360,s,Eye::Left,true,pattern,1);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&map),"Read test pixels");uint64_t sum=0;for(UINT y=0;y<360;y++)for(UINT x=0;x<640u*(hdr?8u:4u);x++)sum+=static_cast<uint8_t*>(map.pData)[y*map.RowPitch+x];
            if(!hdr && pattern==2){ // Portable RGB artifact for visual inspection; no desktop capture.
                std::ofstream image(directory/L"depth-preview.ppm",std::ios::binary);image<<"P6\n640 360\n255\n";for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)image.write(reinterpret_cast<char*>(map.pData)+y*map.RowPitch+x*4,3);
            }
            bool highlight=false;uint64_t rgb=0;
            for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)for(UINT c=0;c<3;c++){
                if(hdr){auto value=reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(map.pData)+y*map.RowPitch)[x*4+c];rgb+=value;highlight|=value>0x3c00&&value<0x7c00;}
                else rgb+=static_cast<uint8_t*>(map.pData)[y*map.RowPitch+x*4+c];
            }
            context->Unmap(read.Get(),0);if(rgb==0)throw std::runtime_error("GPU test produced no RGB pixels.");if(hdr&&pattern==4&&!highlight)throw std::runtime_error("HDR ramp failed to preserve values above 1.0.");report<<(hdr?"HDR":"SDR")<<" pattern "<<pattern<<" checksum "<<sum<<" PASS\n";
        }
        Settings blackSettings;blackSettings.hdr=hdr;draw.draw(context.Get(),640,360,blackSettings,Eye::Black,false,0,0);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE blackMap{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&blackMap),"Read black frame");bool black=true;
        for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)for(UINT c=0;c<(hdr?6u:3u);c++)black&=static_cast<uint8_t*>(blackMap.pData)[y*blackMap.RowPitch+x*(hdr?8:4)+c]==0;
        context->Unmap(read.Get(),0);if(!black)throw std::runtime_error("Black output contains nonzero RGB.");report<<(hdr?"HDR":"SDR")<<" black frame PASS\n";
        for(int pattern:{5,6})for(Eye eye:{Eye::Left,Eye::Right}){
            Settings s;s.hdr=hdr;s.convergence=.05f;draw.draw(context.Get(),640,360,s,eye,false,pattern,0);context->CopyResource(read.Get(),texture.Get());
            D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read isolation test");bool any=false;
            for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)for(UINT c=0;c<(hdr?6u:3u);c++)any|=static_cast<uint8_t*>(m.pData)[y*m.RowPitch+x*(hdr?8:4)+c]!=0;
            context->Unmap(read.Get(),0);if(any!=(int(eye)==pattern-5))throw std::runtime_error("Eye isolation test leaked into the black eye.");
        }
        report<<(hdr?"HDR":"SDR")<<" left/right isolation PASS\n";
        // Check the actual calibration bars in separate output frames, not just
        // a preview checksum. The other eye's bar must equal the empty background.
        for(Eye eye:{Eye::Left,Eye::Right}){
            Settings s;s.hdr=hdr;draw.draw(context.Get(),640,360,s,eye,false,1,0);
            context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};
            check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read separate calibration targets");
            bool correct=true;unsigned stride=hdr?8:4,channels=hdr?6:3;
            for(unsigned y:{90u,187u,284u}){
                auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;
                auto* background=row+64*stride;auto* unwanted=row+(eye==Eye::Left?435:204)*stride;
                auto* wanted=row+(eye==Eye::Left?204:435)*stride;bool visible=false;
                for(unsigned c=0;c<channels;c++){correct&=unwanted[c]==background[c];visible|=wanted[c]!=background[c];}
                correct&=visible;
            }
            context->Unmap(read.Get(),0);if(!correct)throw std::runtime_error("Calibration frame contains the opposite eye's target.");
        }
        report<<(hdr?"HDR":"SDR")<<" separate calibration targets: opposite-eye pixels match background PASS\n";
        // The scene words are geometry seen through the stereo camera. Read both real
        // eye frames: every word must be present in each eye, and its horizontal offset
        // between the eyes (after removing the convergence shift) must match its depth:
        // NEAR crossed, SCREEN zero, FAR uncrossed, and all zero at zero separation.
        auto half=[](uint16_t h){unsigned e=(h>>10)&31,m=h&1023;float v=e==0?m/1024.f*powf(2.f,-14.f):(1+m/1024.f)*powf(2.f,float(int(e)-15));return h&0x8000?-v:v;};
        for(float convergence:{-.05f,0.f,.05f})for(float depth:{0.f,.12f}){
            double centroid[2][3]{};unsigned ink[2][3]{};
            for(Eye eye:{Eye::Left,Eye::Right}){
                Settings s;s.hdr=hdr;s.depth=depth;s.convergence=convergence;draw.draw(context.Get(),640,360,s,eye,false,2,1);
                context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read scene words");
                for(unsigned y=20;y<90;y++){auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;
                    auto value=[&](unsigned x,unsigned c){return hdr?half(reinterpret_cast<uint16_t*>(row)[x*4+c]):float(row[x*4+c]);};
                    for(unsigned x=0;x<640;x++){bool lit=false;for(unsigned c=0;c<3;c++)lit|=value(x,c)!=value(5,c);if(!lit)continue;
                        float r=value(x,0),b=value(x,2);int word=r>b*1.2f?0:b>r*1.2f?2:1;
                        // Convergence shifts the whole image; measure in unshifted content coordinates.
                        centroid[int(eye)][word]+=x+(eye==Eye::Left?1:-1)*convergence*640;ink[int(eye)][word]++;}}
                context->Unmap(read.Get(),0);
            }
            for(int word=0;word<3;word++){
                if(ink[0][word]<100||ink[1][word]<100)throw std::runtime_error("Scene word missing from an eye frame.");
                double shift=centroid[0][word]/ink[0][word]-centroid[1][word]/ink[1][word];
                bool ok=depth==0||word==1?std::abs(shift)<1:word==0?shift>4:shift<-4;
                if(!ok)throw std::runtime_error("Scene word disparity does not match its depth.");
            }
        }
        report<<(hdr?"HDR":"SDR")<<" scene words in both eye frames: NEAR crossed, SCREEN zero, FAR uncrossed disparity across convergence range PASS\n";
        // Stereo area: in the real eye frames every pixel outside the band is exactly black and
        // the whole pattern appears inside it, scaled about the band center.
        for(Eye eye:{Eye::Left,Eye::Right}){
            Settings s;s.hdr=hdr;s.bandHeight=.5f;s.bandCenter=.5f;draw.draw(context.Get(),640,360,s,eye,false,1,0);
            context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read stereo area");
            bool outsideBlack=true,insideLit=false;unsigned stride=hdr?8:4;
            for(unsigned y=0;y<360;y++){auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;bool bandRow=y>=90&&y<270;
                for(unsigned x=0;x<640;x++){bool inside=bandRow&&x>=160&&x<480;for(unsigned c=0;c<(hdr?6u:3u);c++){bool lit=row[x*stride+c]!=0;if(inside)insideLit|=lit;else outsideBlack&=!lit;}}}
            context->Unmap(read.Get(),0);if(!outsideBlack||!insideLit)throw std::runtime_error("Stereo area did not confine the image to the band.");
        }
        report<<(hdr?"HDR":"SDR")<<" stereo area: black outside the band, image inside PASS\n";
        // Calibration readout: both eye frames carry the same digits at screen depth, the value changes the pixels,
        // and a black frame never carries it.
        {std::vector<uint8_t> frames[3];unsigned stride=hdr?8:4;
            // The scene at zero separation is identical in both eyes, so only the readout may differ between frames.
            for(int i=0;i<3;i++){Settings s;s.hdr=hdr;s.depth=0;s.phaseUs=i==2?4321:1234;draw.draw(context.Get(),640,360,s,i==2?Eye::Left:Eye(i),false,2,0,true);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read readout");
                frames[i].resize(size_t(640)*360*stride);for(UINT y=0;y<360;y++)std::copy_n(static_cast<uint8_t*>(m.pData)+y*m.RowPitch,640*stride,frames[i].data()+size_t(y)*640*stride);context->Unmap(read.Get(),0);}
            Settings plain;plain.hdr=hdr;plain.depth=0;draw.draw(context.Get(),640,360,plain,Eye::Left,false,2,0,false);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read plain frame");
            std::vector<uint8_t> none(size_t(640)*360*stride);for(UINT y=0;y<360;y++)std::copy_n(static_cast<uint8_t*>(m.pData)+y*m.RowPitch,640*stride,none.data()+size_t(y)*640*stride);context->Unmap(read.Get(),0);
            bool readoutRows=true;for(UINT y=0;y<360;y++)for(UINT x=0;x<640*stride;x++){bool differs=frames[0][size_t(y)*640*stride+x]!=none[size_t(y)*640*stride+x];if(differs&&(y<318||y>358))readoutRows=false;}
            if(frames[0]!=frames[1]||frames[0]==frames[2]||frames[0]==none||!readoutRows)throw std::runtime_error("Phase readout is not identical in both eyes, ignores the value, or draws outside its row.");
            Settings b;b.hdr=hdr;draw.draw(context.Get(),640,360,b,Eye::Black,false,1,0,true);context->CopyResource(read.Get(),texture.Get());check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read black readout");bool dark=true;unsigned litCount=0,firstX=0,firstY=0;for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)for(UINT ch=0;ch<(hdr?6u:3u);ch++)if(static_cast<uint8_t*>(m.pData)[y*m.RowPitch+x*stride+ch]!=0){if(dark){firstX=x;firstY=y;}dark=false;++litCount;}context->Unmap(read.Get(),0);if(!dark)throw std::runtime_error("Phase readout lit a black frame: "+std::to_string(litCount)+" bytes, first at "+std::to_string(firstX)+","+std::to_string(firstY));
            report<<(hdr?"HDR":"SDR")<<" phase readout: identical in both eyes, value-dependent, absent from black frames PASS\n";}
        {   // Image brightness: a gain lifts the lit frame and can never lift a black frame.
            auto total=[&](const Settings& s,Eye eye){draw.draw(context.Get(),640,360,s,eye,false,1,0);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read gain frame");
                double sum=0;for(UINT y=0;y<360;y++)for(UINT x=0;x<640;x++)for(UINT ch=0;ch<3;ch++){auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;sum+=hdr?double(reinterpret_cast<uint16_t*>(row)[x*4+ch]):double(row[x*4+ch]);}context->Unmap(read.Get(),0);return sum;};
            Settings plain;plain.hdr=hdr;Settings bright;bright.hdr=hdr;bright.imageGain=3;
            if(!(total(bright,Eye::Left)>total(plain,Eye::Left)*1.05))throw std::runtime_error("Image brightness gain did not brighten the eye frame.");
            Settings blackGain;blackGain.hdr=hdr;blackGain.imageGain=8;if(total(blackGain,Eye::Black)!=0)throw std::runtime_error("Image brightness gain lit a black frame.");
            report<<(hdr?"HDR":"SDR")<<" image brightness gain: brightens the eye frame, never a black frame PASS\n";
            // Crosstalk cancellation: the eye frame loses light where the other eye's image is lit,
            // the profile is inert while cancellation is off, and a black frame stays black.
            Settings cancel;cancel.hdr=hdr;cancel.cancelCrosstalk=true;cancel.leakProfile.fill(.2f);
            if(!(total(cancel,Eye::Left)<total(plain,Eye::Left)))throw std::runtime_error("Crosstalk cancellation did not subtract the other eye's leak.");
            Settings inert=cancel;inert.cancelCrosstalk=false;if(total(inert,Eye::Left)!=total(plain,Eye::Left))throw std::runtime_error("Leak profile changed the image while cancellation was off.");
            if(total(cancel,Eye::Black)!=0)throw std::runtime_error("Crosstalk cancellation lit a black frame.");
            report<<(hdr?"HDR":"SDR")<<" crosstalk cancellation: subtracts the other eye's leak, inert when off, never lights a black frame PASS\n";
            // LCD black floor: every pixel of an eye frame is lifted above zero, a black frame stays at zero.
            Settings floor;floor.hdr=hdr;floor.blackFloor=.1f;
            {draw.draw(context.Get(),640,360,floor,Eye::Left,false,1,0);context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read floor frame");bool allLifted=true;
             for(UINT y=0;y<360&&allLifted;y++)for(UINT x=0;x<640&&allLifted;x++){auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;for(unsigned ch=0;ch<3;ch++){double v=hdr?double(reinterpret_cast<uint16_t*>(row)[x*4+ch]):double(row[x*4+ch]);if(v==0)allLifted=false;}}
             context->Unmap(read.Get(),0);if(!allLifted)throw std::runtime_error("LCD black floor left a zero pixel in an eye frame.");}
            if(total(floor,Eye::Black)!=0)throw std::runtime_error("LCD black floor lit a black frame.");
            report<<(hdr?"HDR":"SDR")<<" LCD black floor: lifts every eye-frame pixel, keeps black frames at zero PASS\n";}
        {
            Settings s;s.hdr=hdr;s.guardLevel=.5f;
            auto pixels=[&](Eye eye,bool guard,int pattern,uint64_t index){
                draw.draw(context.Get(),640,360,s,eye,false,pattern,0,false,guard,index);
                context->CopyResource(read.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE m{};
                check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read panel experiment");
                std::vector<float> out(640*360);
                for(unsigned y=0;y<360;y++)for(unsigned x=0;x<640;x++){
                    auto* row=static_cast<uint8_t*>(m.pData)+y*m.RowPitch;
                    out[y*640+x]=hdr?half(reinterpret_cast<uint16_t*>(row)[x*4]):row[x*4]/255.f;
                }
                context->Unmap(read.Get(),0);return out;
            };
            auto reset=pixels(Eye::Black,true,7,0);float expected=hdr?.21404114f*2.5f:.5f;
            for(float v:reset)if(std::abs(v-expected)>.003f)throw std::runtime_error("Neutral reset is not uniform or has wrong transfer curve.");
            for(int pattern:{7,8,9,10,11})for(float v:pixels(Eye::Black,false,pattern,1))if(v!=0)throw std::runtime_error("Panel diagnostic lit a pause/acquisition blank.");
            auto left=pixels(Eye::Left,false,7,0),right=pixels(Eye::Right,false,7,0);
            for(unsigned row=0;row<9;row++)for(unsigned col=0;col<6;col++){
                unsigned y=row*40+20,x=unsigned((col+.5)*640/6);
                bool brighter=left[y*640+x]>right[y*640+x];
                if(brighter!=(col%2==0))throw std::runtime_error("Nine-row optical targets fail an eye/gray transition pair.");
            }
            for(int pattern:{9,10,11})for(Eye eye:{Eye::Left,Eye::Right}){
                auto region=pixels(eye,false,pattern,0);const auto& base=eye==Eye::Left?left:right;
                for(unsigned row=0;row<9;++row)for(unsigned col=0;col<6;++col){
                    unsigned y=row*40+20,x=unsigned((col+.5)*640/6);
                    if(region[y*640+x]!=base[y*640+x])throw std::runtime_error("Region selection changed or cropped a calibration transition.");
                }
            }
            for(uint64_t index:{0ull,1ull,127ull,128ull,255ull,256ull})for(Eye eye:{Eye::Left,Eye::Right,Eye::Black}){
                auto code=pixels(eye,eye==Eye::Black,8,index);
                for(unsigned row=0;row<9;row++){
                    unsigned decoded=0;for(unsigned bit=0;bit<8;bit++)decoded=(decoded<<1)|(code[(row*40+20)*640+96+64*bit]>.5f?1u:0u);
                    if(decoded!=index%256)throw std::runtime_error("Refresh-code diagnostic lost index/parity on a row or reset slot.");
                }
            }
            report<<(hdr?"HDR":"SDR")<<" neutral reset, mute isolation, all nine target rows, refresh-code decode including wrap and guard slots PASS (rendered pixels only)\n";
        }
    }
    // Exercise the actual hook shader and presenter with contrasting packed eyes.
    // The edge pixels detect bilinear sampling across the SBS/TB seam.
    ComPtr<ID3DBlob> hookCode,errors;check(D3DCompile(stereoBlitShader,strlen(stereoBlitShader),"hook-test",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&hookCode,&errors),"Compile hook pixel shader");
    ComPtr<ID3D11PixelShader> hookPS;check(device->CreatePixelShader(hookCode->GetBufferPointer(),hookCode->GetBufferSize(),nullptr,&hookPS),"Hook test shader");
    D3D11_TEXTURE2D_DESC td{};td.Width=160;td.Height=80;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output,staging;check(device->CreateTexture2D(&td,nullptr,&output),"Packed test output");ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv),"Packed test target");
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&td,nullptr,&staging),"Packed test staging");
    auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,160,80,0,1};context->RSSetViewports(1,&vp);
    for(Packing packing:{Packing::SideBySide,Packing::TopBottom}){
        std::vector<uint32_t> pixels(16*8);for(int y=0;y<8;y++)for(int x=0;x<16;x++)pixels[y*16+x]=(packing==Packing::SideBySide?x<8:y<4)?0xff0000ff:0xff00ff00;
        td.Width=16;td.Height=8;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_IMMUTABLE;td.CPUAccessFlags=0;D3D11_SUBRESOURCE_DATA data{pixels.data(),16*4,0};ComPtr<ID3D11Texture2D> input;check(device->CreateTexture2D(&td,&data,&input),"Packed input");
        draw.release();check(device->CreateShaderResourceView(input.Get(),nullptr,&draw.view),"Packed input view");draw.active=std::make_shared<StereoFrame>();draw.active->packing=packing;draw.active->encoding=Encoding::SRGB;draw.active->width=16;draw.active->height=8;
        for(bool hook:{false,true})for(int eye:{0,1})for(float convergence:{-.05f,0.f,.05f}){
            Settings s;s.convergence=convergence;draw.draw(context.Get(),160,80,s,Eye(eye),false,3,0);
            if(hook){
                float p[16]{packing==Packing::SideBySide?eye*.5f:0,packing==Packing::TopBottom?eye*.5f:0,packing==Packing::SideBySide?.5f:1,packing==Packing::TopBottom?.5f:1,(eye==0?1.f:-1.f)*convergence,.5f/16,.5f/8,0,1,.5f,0,0};
                D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(draw.params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m),"Hook constants");memcpy(m.pData,p,sizeof(p));context->Unmap(draw.params.Get(),0);
                context->PSSetShader(hookPS.Get(),nullptr,0);auto* srv=draw.view.Get();context->PSSetShaderResources(0,1,&srv);context->Draw(3,0);srv=nullptr;context->PSSetShaderResources(0,1,&srv);
            }
            context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read packed pixels");bool correct=true;
            for(int y=0;y<80;y++)for(int x=0;x<160;x++){
                float u=(x+.5f)/160+(eye==0?1.f:-1.f)*convergence;bool black=u<0||u>1;auto* pixel=static_cast<uint8_t*>(m.pData)+y*m.RowPitch+x*4;
                correct&=pixel[eye]>= (black?0:254)&&(!black||pixel[eye]==0)&&pixel[1-eye]==0&&pixel[2]==0;
            }
            context->Unmap(staging.Get(),0);if(!correct)throw std::runtime_error(hook?"Hook convergence/eye seam failed.":"Presenter convergence/eye seam failed.");
        }
    }
    draw.release();report<<"Presenter and hook: SBS/TB eye boundaries, signed convergence and black margins PASS\n";
    // Capture must preserve scRGB values in HDR. SDR uses the captured display's
    // white level, not a curve that lifts shadows or darkens every white.
    for(bool hdr:{false,true})for(float white:{1.f,2.5f,4.f}){
        const float values[]{white*.05f,white*.4f,white,1,white*.05f,white*.4f,white,1};
        D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=1;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{values,sizeof(values),0};ComPtr<ID3D11Texture2D> input;check(device->CreateTexture2D(&desc,&data,&input),"Captured color input");
        draw.release();check(device->CreateShaderResourceView(input.Get(),nullptr,&draw.view),"Captured color view");draw.active=std::make_shared<StereoFrame>();draw.active->encoding=Encoding::LinearScRGB;draw.active->width=2;draw.active->height=1;draw.active->sdrWhiteLevel=white;draw.active->alignmentApplied=true;
        desc.Width=32;desc.Height=16;desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.Format=hdr?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D11Texture2D> target,read;check(device->CreateTexture2D(&desc,nullptr,&target),"Captured color target");ComPtr<ID3D11RenderTargetView> targetView;check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"Captured color RTV");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&desc,nullptr,&read),"Captured color readback");
        auto* view=targetView.Get();context->OMSetRenderTargets(1,&view,nullptr);D3D11_VIEWPORT viewport{0,0,32,16,0,1};context->RSSetViewports(1,&viewport);
        for(Eye eye:{Eye::Left,Eye::Right})for(float alignment:{-.05f,.05f}){
            Settings settings;settings.hdr=hdr;settings.convergence=alignment;draw.draw(context.Get(),32,16,settings,eye,false,3,0);
            context->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"Read captured colors");bool correct=true;
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x)for(unsigned channel=0;channel<3;++channel){
                const auto* row=static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch;const float linear=values[channel]/white;
                const float expected=hdr?values[channel]:linear<=.0031308f?12.92f*linear:1.055f*std::pow(linear,1/2.4f)-.055f;
                const float actual=hdr?reinterpret_cast<const float*>(row)[x*4+channel]:row[x*4+channel]/255.f;
                correct&=std::abs(actual-expected)<.006f;
            }context->Unmap(read.Get(),0);if(!correct)throw std::runtime_error("Capture color/AI alignment regression: shadows, gray, white or an eye edge changed.");
        }
    }
    draw.release();report<<"Capture: HDR values preserved, SDR gray/white at three desktop white levels, AI alignment applied once in both eyes PASS\n";
    return true;
}
}
