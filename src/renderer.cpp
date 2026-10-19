#include "renderer.h"
#include "stream_view.h"
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
void Surface::create(HWND window,const LUID* luid,bool useHDR,unsigned latency){
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter> adapter;
    if(luid)check(factory->EnumAdapterByLuid(*luid,IID_PPV_ARGS(&adapter)),"Select display GPU");
    check(D3D11CreateDevice(adapter.Get(),adapter?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create D3D11 device");
    hdr=useHDR;
    // Stereo scales buffering with refresh so higher rates retain time to
    // survive GPU/Present stalls, regardless of how the emitter schedules IR.
    makeSwapChain(factory.Get(),window,latency);
}
void Surface::attach(ID3D11Device* existing,ID3D11DeviceContext* existingContext,HWND window,unsigned latency){
    device=existing;context=existingContext;hdr=false;
    ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi),"Stream view DXGI device");
    ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(&adapter),"Stream view adapter");
    ComPtr<IDXGIFactory2> factory;check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Stream view DXGI factory");
    makeSwapChain(factory.Get(),window,latency);
}
void Surface::makeSwapChain(IDXGIFactory2* factory,HWND window,unsigned latency){
    if(latency<1||latency>maxPresentationQueueDepth)throw std::runtime_error("Presentation queue depth must be between 1 and 8.");
    RECT r{};GetClientRect(window,&r);width=std::max(1L,r.right);height=std::max(1L,r.bottom);
    // Two writable buffers beyond the present queue (and the established four-buffer minimum).
    // The frame-latency wait can release one frame more than the queue depth: 44 of 44 recorded
    // miss events (2026-10-12) began with a zero-wait present, and with a single spare buffer that
    // extra frame left none to draw into, so the display held a frame and the whole six-frame
    // queue landed one refresh late (black frames on the eye refreshes: a visible flash).
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=hdr?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=std::max(4u,latency+2);desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> chain;check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&chain),"Create flip swap chain");check(chain.As(&swap),"Waitable swap chain");
    check(swap->SetMaximumFrameLatency(latency),"Set frame latency");waitable=swap->GetFrameLatencyWaitableObject();if(!waitable)throw std::runtime_error("No frame latency wait handle.");
    factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    applyColor(hdr);
}
// HDR presents in the display's own signal format, 10-bit PQ/Rec.2020 (HDR10). The FP16 scRGB
// chain it replaced had to be converted to that signal by DWM or the display pipeline, and in
// HDR every switch between composition and direct flip cost 20-40 missed refreshes (each a
// flash with black frame insertion) where the same switches in SDR cost none. There is no
// other HDR format: an output that refuses HDR10 is an error. Buffers must be unbound.
void Surface::applyColor(bool useHDR){
    ComPtr<IDXGISwapChain3> color;check(swap.As(&color),"Swap chain color space");
    auto use=[&](DXGI_FORMAT format,DXGI_COLOR_SPACE_TYPE space){
        DXGI_SWAP_CHAIN_DESC1 current{};check(swap->GetDesc1(&current),"Output format");
        if(current.Format!=format)check(swap->ResizeBuffers(0,width,height,format,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Change output format");
        // DXGI reports color-space support for the current buffer format.
        UINT support=0;return SUCCEEDED(color->CheckColorSpaceSupport(space,&support))&&(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)&&SUCCEEDED(color->SetColorSpace1(space));
    };
    if(useHDR&&!use(DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))throw std::runtime_error("Selected output does not accept HDR10 (10-bit PQ, Rec.2020).");
    if(!useHDR&&!use(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709))throw std::runtime_error("Selected output does not support requested color space.");
    pq=useHDR;
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Swap buffer");check(device->CreateRenderTargetView(buffer.Get(),nullptr,&target),"Swap render target");
}
void Surface::resize(unsigned w,unsigned h){if(!w||!h)return;context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();check(swap->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT),"Resize output");width=w;height=h;ComPtr<ID3D11Texture2D> b;check(swap->GetBuffer(0,IID_PPV_ARGS(&b)),"Resized buffer");check(device->CreateRenderTargetView(b.Get(),nullptr,&target),"Resized target");}
void Surface::bind(){auto* rt=target.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);}
void Surface::setHdr(bool useHDR){
    if(useHDR==hdr)return;
    context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();
    applyColor(useHDR);hdr=useHDR;
}

struct DrawState {
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> params;ComPtr<ID3D11SamplerState> sampler;
    std::shared_ptr<StereoFrame> active;ComPtr<ID3D11Texture2D> opened;ComPtr<ID3D11ShaderResourceView> view;ComPtr<IDXGIKeyedMutex> key;
    // The source publishes from a pool of three shared textures. Opening the shared resource and
    // creating its view once per texture keeps that work off the per-pair path of the present
    // thread, which has less than one refresh period between the wait and the present.
    struct Opened { std::weak_ptr<StereoFrame> owner;const StereoFrame* frame=nullptr;ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;ComPtr<IDXGIKeyedMutex> key; };
    std::vector<Opened> pool;
    // The bound target is HDR10: the shader encodes its linear scRGB result to PQ/Rec.2020.
    bool pqOutput=false;
    // The control menu drawn into eye frames (see Presenter::showMenu): its image, its rectangle in
    // output UV and the part of the image it fills.
    ComPtr<ID3D11ShaderResourceView> menuView;float menuRect[4]{},menuTexture[2]{1,1};
    ~DrawState(){release();}
    void release(){view.Reset();opened.Reset();if(key){key->ReleaseSync(0);key.Reset();}active.reset();}
    void init(ID3D11Device* device){
        check(device->CreateVertexShader(compiled::vs,sizeof(compiled::vs),nullptr,&vs),"Vertex shader");
        check(device->CreatePixelShader(compiled::ps,sizeof(compiled::ps),nullptr,&ps),"Pixel shader");
        D3D11_BUFFER_DESC b{};b.ByteWidth=176;b.Usage=D3D11_USAGE_DYNAMIC;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;check(device->CreateBuffer(&b,nullptr,&params),"Shader parameters");D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;check(device->CreateSamplerState(&s,&sampler),"Image sampler");
    }
    bool accept(ID3D11Device* device,std::shared_ptr<StereoFrame> f){if(!f)return false;if(active && f->pairId==active->pairId && f==active)return true;
        Opened* entry=nullptr;for(auto& o:pool)if(o.frame==f.get()&&o.owner.lock()==f){entry=&o;break;}
        if(!entry){
            // A pool texture that the source replaced (new size or format) has died with its frame;
            // its entry goes, so a reused address or handle can never select the old texture.
            std::erase_if(pool,[](const Opened& o){return o.owner.expired();});if(pool.size()>=8)pool.erase(pool.begin());
            Opened o;o.owner=f;o.frame=f.get();check(device->OpenSharedResource(f->sharedHandle,IID_PPV_ARGS(&o.texture)),"Open stereo pair on output adapter");check(o.texture.As(&o.key),"Shared pair mutex");check(device->CreateShaderResourceView(o.texture.Get(),nullptr,&o.view),"Stereo pair view");
            pool.push_back(std::move(o));entry=&pool.back();
        }
        if(entry->key->AcquireSync(0,0)!=S_OK)return false;
        release();active=std::move(f);opened=entry->texture;view=entry->view;key=entry->key;
        return true;
    }
    void draw(ID3D11DeviceContext* context,unsigned w,unsigned h,const Settings& s,Eye eye,bool preview,int pattern,double time,bool overlay=false,bool guardSlot=false,uint64_t refreshIndex=0){
        float p[44]={float(w),float(h),float(time),0,float(int(eye)),float(pattern),preview?1.f:0.f,s.hdr?1.f:0.f,s.depth*4,s.peakNits,s.convergence,s.swapEyes?1.f:0.f,0,0,1,1,s.bandHeight,s.bandCenter,overlay?1.f:0.f,float(s.phaseUs),s.imageGain,std::clamp(s.blackFloor,-.3f,.3f),0,0};
        for(size_t i=0;i<8;i++)p[24+i]=s.cancelCrosstalk?std::clamp(s.leakProfile[i],0.f,.95f):0.f;
        p[3]=float(refreshIndex%256);p[22]=guardSlot?1.f:0.f;p[23]=s.guardLevel;
        p[32]=1;p[34]=pqOutput&&s.hdr?1.f:0.f;
        // Only eye slots carry the menu; decided before a missing source turns the slot black.
        p[35]=menuView&&eye!=Eye::Black?1.f:0.f;for(int i=0;i<4;++i)p[36+i]=menuRect[i];p[40]=menuTexture[0];p[41]=menuTexture[1];
        if(pattern==3 && active){p[12]=float(int(active->packing));p[13]=float(int(active->encoding));p[14]=float(active->width);p[15]=float(active->height);p[32]=active->sdrWhiteLevel;p[33]=active->eyeAspect;if(active->alignmentApplied)p[10]=0;}
        else if(pattern==3)p[4]=float(int(Eye::Black));
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(params.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map shader constants");memcpy(mapped.pData,p,sizeof(p));context->Unmap(params.Get(),0);
        auto* cb=params.Get();ID3D11ShaderResourceView* views[2]{};views[active&&active->packing==Packing::SeparateEyes?1:0]=view.Get();auto* sm=sampler.Get();context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,2,views);{auto* m=menuView.Get();context->PSSetShaderResources(3,1,&m);}context->PSSetSamplers(0,1,&sm);context->Draw(3,0);views[0]=views[1]=nullptr;context->PSSetShaderResources(0,2,views);{ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(3,1,&none);}
    }
};
// Another process saturating the GPU (a Cycles viewport, a 4K game) keeps its work on the GPU for
// tens of milliseconds; the graphics kernel time-slices between processes, so the presenter's
// one-triangle draw waited behind it, outran the six-refresh flip queue and the display repeated a
// frame: a black slot landed where an eye belonged. Measured with experiments/gpu-contention under a
// Cycles OptiX render on the RTX 5070 Ti: HIGH class waited up to 57 ms (54-124 waits over 25 ms in
// 10 s), REALTIME at most 1.4 ms with none over one refresh. REALTIME is granted without elevation
// on this system. It is required: a lower class is the failure itself, so a refusal stops the output.
static std::string raiseGpuSchedulingPriority(){
    const NTSTATUS status=D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(),D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
    if(status!=0){char text[160];std::snprintf(text,sizeof(text),"Windows refused realtime GPU scheduling for the output (status 0x%08lX); other GPU work would delay the eye frames.",(unsigned long)status);throw std::runtime_error(text);}
    return "realtime";
}
void Presenter::start(HWND window,const Display& d,const Settings& s,bool preview,int pattern){start(window,d,s,preview,pattern,Diagnostics{});}
void Presenter::start(HWND window,const Display& d,const Settings& s,bool preview,int pattern,const Diagnostics& diagnostics){stop();validate(s);if(s.hdr && !d.hdrEnabled)throw std::runtime_error("Enable HDR for this display in Windows before starting HDR output.");{std::lock_guard l(mutex_);settings_=s;pattern_=pattern;status_={};status_.running=true;status_.preview=preview;paused_=false;++revision_;}emitter_.configure(s);worker_=std::jthread([this,window,d,preview,diagnostics](std::stop_token stop){run(stop,window,d,preview,diagnostics);});}
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
void Presenter::showMenu(HANDLE shared,uint64_t version,const RECT& rect,unsigned textureWidth,unsigned textureHeight){std::lock_guard l(mutex_);menu_={shared,version,rect,textureWidth,textureHeight};}
void Presenter::hideMenu(){std::lock_guard l(mutex_);menu_={};}
void Presenter::streamTo(HWND window,bool rightEye){
    std::unique_lock l(mutex_);streamWindow_=window;streamRightEye_=rightEye;status_.streamMessage.clear();
    if(window)return;
    status_.streaming=false;
    // Wait for the presenter to drop the swap chain before the caller destroys the
    // window. Presenting into a destroyed HWND is undefined; the wait bounds itself
    // so a stalled presenter can never hang the user interface.
    streamIdle_.wait_for(l,std::chrono::milliseconds(500),[this]{return !streamOpen_;});
}
std::vector<std::string> Presenter::takeMissEvents(){std::lock_guard l(mutex_);std::vector<std::string> out(missEvents_.begin(),missEvents_.end());missEvents_.clear();return out;}
RenderStatus Presenter::status()const{std::lock_guard l(mutex_);return status_;}
void Presenter::run(std::stop_token stop,HWND window,Display display,bool preview,Diagnostics diagnostics){
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Games",&task);
    if(mmcss)AvSetMmThreadPriority(mmcss,AVRT_PRIORITY_HIGH);
    // The output window normally sits unfocused behind the game (the pad follows focus). Windows 11
    // throttles background processes (EcoQoS, ignored timer resolution), which made the presenter and
    // the USB thread miss eye deadlines only while the game had focus. Opt the whole process out.
    if(!preview){SetPriorityClass(GetCurrentProcess(),HIGH_PRIORITY_CLASS);
        PROCESS_POWER_THROTTLING_STATE power{};power.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        power.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;power.StateMask=0;
        SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power,sizeof(power));}
    try{
        const std::string gpuPriority=raiseGpuSchedulingPriority();
        // Every stage before the first present names itself in the status, so a session log
        // showing zero presents also shows where the output got stuck.
        auto stage=[&](const char* what){std::lock_guard l(mutex_);status_.message=what;};
        stage("Creating the output surface");
        const unsigned queueDepth=diagnostics.queueDepth?diagnostics.queueDepth:preview?3:presentationQueueDepth(display.refresh,emitter_.status().scheduled);
        Settings s;{std::lock_guard l(mutex_);s=settings_;}Surface surface;surface.create(window,&display.adapterLuid,s.hdr,queueDepth);stage("Creating the output shaders");DrawState draw;draw.init(surface.device.Get());stage("Waiting for the first frame slot");
        {ComPtr<IDXGIDevice> dxgi;if(SUCCEEDED(surface.device.As(&dxgi)))dxgi->SetGPUThreadPriority(7);}
        struct PresentRecord {UINT id;uint64_t expected;Slot slot;bool muted;};std::deque<PresentRecord> records;
        TimingTracker clock;uint64_t slotIndex=0,revision=0,lastUsbLate=0;UINT lastObserved=0,lastRefresh=0;unsigned blank=16;unsigned slipWindow=0;double slipWindowStart=0;
        // Both DWM and direct flip report the actual displayed refresh. Recover
        // from a slip immediately, without retaining a stale eye/black parity.
        PresentRefreshAnchor anchor;
        PresentRefreshReservation reservation;
        // PresentRefreshCount already identifies the refresh the image reached.
        // Composition mode is diagnostic information, not an extra display delay.
        ComPtr<IDXGISwapChainMedia> media;surface.swap.As(&media);bool composed=false;int presentationMode=-1;
        RenderStatus local;local.running=true;local.preview=preview;local.gpuPriority=gpuPriority;local.queueDepth=queueDepth;local.message=preview?"PREVIEW - side-by-side, no optical validation":"Acquiring presentation timing";
        double begin=qpc(),last=begin,pairTime=0,lastPublish=0,lockedSince=begin;
        std::deque<double> recentIntervals;double intervalSum=0,intervalSumSq=0;
        // 2D stream view (stream_view.h): a second swap chain on this same device and
        // context, so it draws from the stereo pair this thread already holds open.
        std::unique_ptr<Surface> stream;HWND streamOpened=nullptr;double lastStream=0;
        struct TraceFrame {double time,wait,source,draw,present,stats,stream;UINT submitted,expected,observed,refresh,sync;HRESULT result;bool composed;double gpuMs=-1,slackMs=-1;int mode=-1;};
        std::vector<TraceFrame> trace;if(!diagnostics.trace.empty())trace.reserve(30000);
        // Miss evidence: the last 48 frames are always kept. A miss snapshots them plus the
        // 16 frames after it, so each event shows which stage held the output (wait, source,
        // draw, present, stream) and how the queued frames landed on the display.
        std::deque<TraceFrame> recentFrames;std::vector<TraceFrame> eventFrames;std::string eventHeader;unsigned eventFramesLeft=0;uint64_t eventMisses=0,eventBlack=0,lastMissCount=0;
        auto traceLine=[](std::ostringstream& o,const TraceFrame& f){o<<std::fixed<<std::setprecision(2)<<f.time*1000<<' '<<f.wait*1000<<' '<<f.source*1000<<' '<<f.draw*1000<<' '<<f.present*1000<<' '<<f.stats*1000<<' '<<f.stream*1000<<' '<<f.submitted<<' '<<f.expected<<' '<<f.observed<<' '<<f.refresh<<' '<<f.sync<<' '<<(f.mode==DXGI_FRAME_PRESENTATION_MODE_OVERLAY?"overlay":f.mode==DXGI_FRAME_PRESENTATION_MODE_NONE?"direct":f.mode==DXGI_FRAME_PRESENTATION_MODE_COMPOSED?"composed":f.mode==DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE?"failed":"?")<<" | "<<f.gpuMs<<' '<<f.slackMs<<'\n';};
        // GPU completion of each output frame: an event query ended after the frame's draw, polled
        // (without flushing) once per loop. gpu = ms from Present until the frame was seen finished,
        // slack = ms from then until the frame's refresh. Both are bounded by the ~4 ms poll: a frame
        // that misses its refresh with slack still positive was finished in time, and the hold was
        // downstream of the GPU; a negative slack means the frame itself was not rendered in time.
        struct GpuMark {UINT present=0;ComPtr<ID3D11Query> done;double submitted=0,displayAt=0;};
        // The control menu image from the UI thread. It is copied (when a new version is published and
        // the UI is not writing it) into a texture of this device, so a busy UI never costs a frame.
        MenuState menuState;struct {HANDLE handle=nullptr;uint64_t version=0;ComPtr<ID3D11Texture2D> shared,copy;ComPtr<IDXGIKeyedMutex> key;ComPtr<ID3D11ShaderResourceView> view;} menuImage;
        std::deque<GpuMark> gpuMarks;std::vector<ComPtr<ID3D11Query>> spareQueries;
        auto pollGpu=[&]{
            while(!gpuMarks.empty()&&gpuMarks.front().present){
                auto& m=gpuMarks.front();if(surface.context->GetData(m.done.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)break;
                const double t=qpc();
                auto mark=[&](TraceFrame& f){if(f.submitted==m.present&&f.gpuMs<0){f.gpuMs=(t-m.submitted)*1000;f.slackMs=m.displayAt>0?(m.displayAt-t)*1000:-1;}};
                for(auto& f:recentFrames)mark(f);for(auto& f:eventFrames)mark(f);
                spareQueries.push_back(std::move(m.done));gpuMarks.pop_front();
            }
        };
        while(!stop.stop_requested() && IsWindow(window)){
            const double waitBegin=qpc();
            DWORD wait=WaitForSingleObject(surface.waitable,50);if(wait==WAIT_TIMEOUT){if(!local.presents)stage("Waiting for a frame slot; the swap chain is not turning over");continue;}if(wait!=WAIT_OBJECT_0)throw std::runtime_error("Presentation wait failed.");
            const double waitEnd=qpc();
            pollGpu();
            bool paused,overlay,manualResync,streamRight;int pattern;uint64_t rev;HWND streamWindow;{std::lock_guard l(mutex_);s=settings_;paused=paused_;pattern=pattern_;rev=revision_;overlay=overlay_;manualResync=resyncRequested_;resyncRequested_=false;streamWindow=streamWindow_;streamRight=streamRightEye_;menuState=menu_;}
            if(rev!=revision){revision=rev;begin=qpc();lockedSince=begin;local.elapsed=0;local.timingPassed=false;blank=16;slotIndex=0;clock.reset();anchor.reset();reservation.reset();records.clear();lastObserved=lastRefresh=0;emitter_.suspend();}
            // Change the actual buffer precision and color space with the HDR
            // setting, keeping the output window and image source in place.
            surface.setHdr(s.hdr);
            RECT size{};GetClientRect(window,&size);if(size.right<=0||size.bottom<=0){if(!local.presents)stage("The output window has no size");emitter_.suspend();Sleep(20);continue;}
            if(unsigned(size.right)!=surface.width || unsigned(size.bottom)!=surface.height){surface.resize(size.right,size.bottom);blank=16;records.clear();clock.reset();anchor.reset();reservation.reset();lastObserved=lastRefresh=0;emitter_.suspend();}
            // Anchor the eye sequence to the display refresh counter once statistics are available.
            // Feedback below corrects future submissions when a prediction slips.
            uint64_t slotSeq=slotIndex;
            if(!preview&&anchor.valid){UINT previous=0;if(SUCCEEDED(surface.swap->GetLastPresentCount(&previous))){
                const auto ready=clock.futureRefresh(refreshForPresent(previous+1,lastObserved,lastRefresh),qpc());
                slotSeq=reservation.next(ready);
                // Display feedback can lag a burst of submissions after a stall.
                // Each accepted frame still needs its own upcoming refresh;
                // repeatedly choosing the same one repeats its black/eye pixels.
                if(slotSeq!=ready)++local.queueRefillCorrections;
            }}
            auto slot=sequenceSlot(s.sequence,slotSeq,false);
            if(menuState.handle&&!preview){
                if(menuState.handle!=menuImage.handle){
                    menuImage={};check(surface.device->OpenSharedResource(menuState.handle,IID_PPV_ARGS(&menuImage.shared)),"Open the control menu image");check(menuImage.shared.As(&menuImage.key),"Control menu image mutex");
                    D3D11_TEXTURE2D_DESC d{};menuImage.shared->GetDesc(&d);d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;
                    check(surface.device->CreateTexture2D(&d,nullptr,&menuImage.copy),"Control menu copy");check(surface.device->CreateShaderResourceView(menuImage.copy.Get(),nullptr,&menuImage.view),"Control menu view");
                    menuImage.handle=menuState.handle;
                }
                if(menuState.version!=menuImage.version&&menuImage.key->AcquireSync(0,0)==S_OK){surface.context->CopyResource(menuImage.copy.Get(),menuImage.shared.Get());check(menuImage.key->ReleaseSync(0),"Release the control menu image");menuImage.version=menuState.version;}
                D3D11_TEXTURE2D_DESC d{};menuImage.copy->GetDesc(&d);const auto& r=menuState.rect;
                draw.menuView=menuImage.version?menuImage.view:nullptr;
                draw.menuRect[0]=float(r.left)/float(surface.width);draw.menuRect[1]=float(r.top)/float(surface.height);draw.menuRect[2]=float(r.right)/float(surface.width);draw.menuRect[3]=float(r.bottom)/float(surface.height);
                draw.menuTexture[0]=float(r.right-r.left)/float(d.Width);draw.menuTexture[1]=float(r.bottom-r.top)/float(d.Height);
            }else{draw.menuView=nullptr;if(!menuState.handle)menuImage={};}
            auto emitterStatus=emitter_.status();auto usbState=emitterStatus.state;
            auto resync=[&]{emitter_.suspend();blank=16;slotIndex=0;clock.reset();anchor.reset();reservation.reset();records.clear();lastObserved=lastRefresh=0;local.resyncs++;local.lastResyncSec=qpc()-begin;local.timingPassed=false;};
            if(manualResync)resync();
            // Timing misses update diagnostics and the refresh anchor. Only the
            // user selects pause, stop or 2D; never demote running stereo here.
            auto slip=[&](unsigned count){
                local.misses+=count;slipWindow+=count;
                lockedSince=qpc();local.timingPassed=false;
                // The device schedules explicit future boundaries. A missed slot
                // must not turn into sixteen black frames and a shutter shutdown.
            };
            if(!preview&&emitterStatus.late>lastUsbLate)slip(unsigned(emitterStatus.late-lastUsbLate));
            lastUsbLate=emitterStatus.late;
            bool emitterAvailable=usbState==EmitterState::Ready||usbState==EmitterState::Running||usbState==EmitterState::Simulated;
            const bool steady=diagnostics.steadyTimingProbe;
            bool mute=paused || (!preview&&!steady&&(blank>0||clock.samples<8||!emitterAvailable));
            Eye eye=mute?Eye::Black:steady?streamViewEye(s,false):slot.eye;
            // Do not consume ordered source pairs during pause or acquisition
            // blanking: no eye image is being submitted in those slots.
            const double sourceBegin=qpc();
            if(slot.pairBoundary&&!mute){auto next=source_.forPresentation();if(draw.accept(surface.device.Get(),next))source_.presented(next);pairTime=qpc()-begin;}
            const double sourceEnd=qpc();
            // Opening a new shared texture/view can stall the driver after the
            // initial prediction. Re-select BOTH pixels and shutter deadline
            // before drawing; never submit the old black/eye slot after a stall.
            auto retarget=[&](double now){
                if(preview||!anchor.valid||mute)return false;
                const uint64_t readyRefresh=clock.futureRefresh(reservation.next(slotSeq),now);
                if(readyRefresh!=slotSeq){
                    slip(unsigned(std::min<uint64_t>(readyRefresh-slotSeq,UINT_MAX)));
                    slotSeq=readyRefresh;slot=sequenceSlot(s.sequence,slotSeq,false);
                    eye=steady?streamViewEye(s,false):slot.eye;
                    return true;
                }
                return false;
            };
            retarget(sourceEnd);
            surface.bind();draw.pqOutput=surface.pq;draw.draw(surface.context.Get(),surface.width,surface.height,steady?steadyViewSettings(s):s,eye,preview&&!paused,pattern,pairTime,overlay&&!mute&&!steady,!mute&&!steady&&slot.eye==Eye::Black,slotIndex);
            // Mapping constants/submitting the draw can also stall the driver.
            // Replace expired pixels before Present, keeping the acquired pair.
            if(retarget(qpc()))draw.draw(surface.context.Get(),surface.width,surface.height,steady?steadyViewSettings(s):s,eye,preview&&!paused,pattern,pairTime,overlay&&!mute&&!steady,!mute&&!steady&&slot.eye==Eye::Black,slotIndex);
            const double drawEnd=qpc();
            // Predictive trigger for every backend: the eye command is timed from the predicted
            // vblank of the refresh this present will land on, not from retrospective statistics.
            // Retrospective triggering shifted by a frame whenever DWM switched between composition
            // and direct flip, which showed up as sudden crosstalk seconds after a clean start.
            const bool simulatedProbe=diagnostics.steadyTimingProbe&&diagnostics.simulateProbeTriggers&&usbState==EmitterState::Simulated;
            if(!preview&&(!steady||simulatedProbe)&&!mute&&slot.trigger&&anchor.valid&&clock.samples>=8&&blank==0) {
                // Predict the next present's refresh before Present can block.
                // Actual refresh statistics below still detect a missed prediction.
                const uint64_t nextRefresh=slotSeq;
                // Submit on the predicted vblank. Both supported emitters apply phase on-device:
                // NVIDIA through its X delay timer and RP2040 through its scheduled open time.
                const double target=clock.predict(nextRefresh);
                {double leadMs=(target-qpc())*1000;local.leadBins[leadMs<0?0:leadMs<2?1:leadMs<4?2:leadMs<6?3:leadMs<8?4:5]++;}
                const double lead=target-qpc();
                if(lead<.0005||lead>clock.period*(maxPresentationQueueDepth+4)) {
                    // A stale/past target or an unconfirmed counter restart is
                    // not a usable deadline. Leave the firmware's cadence alone.
                    slip(1);
                } else {
                    Eye commandEye=slot.eye;if(s.swapEyes)commandEye=commandEye==Eye::Left?Eye::Right:Eye::Left;
                    emitter_.submit(commandEye,target,clock.period*1e6,local.clockCorrections);
                }
            }
            {ComPtr<ID3D11Query> q;if(!spareQueries.empty()){q=std::move(spareQueries.back());spareQueries.pop_back();}else{D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};check(surface.device->CreateQuery(&qd,&q),"Output frame completion query");}
             surface.context->End(q.Get());gpuMarks.push_back({0,std::move(q),0,0});}
            const double presentBegin=qpc();
            UINT id=0;HRESULT h=surface.swap->Present(1,0);
            const double presentEnd=qpc();
            // Occlusion is a normal, expected state, not a fault: playing a side-by-side game means the
            // game window has focus and covers the output window, and a gamepad only reaches the focused
            // window. Suspending the emitter and blanking 16 frames here re-ran on every occluded present,
            // which the viewer saw as the glasses flashing and the sync resetting the moment they clicked
            // into the game (2026-09-14). The presentation clock and the eye lock are kept across occlusion;
            // only the manual re-lock (UI button / Ctrl+Alt+R) ever restarts synchronization.
            local.occluded=h==DXGI_STATUS_OCCLUDED;
            if(!local.occluded)check(h,"Present stereo output");
            check(surface.swap->GetLastPresentCount(&id),"Present identifier");
            uint64_t expected=anchor.valid?slotSeq:0;
            if(!local.occluded&&(records.empty()||records.back().id!=id)){
                if(!preview&&anchor.valid)reservation.commit(id,expected);
                records.push_back({id,expected,mute?Slot{Eye::Black,false,false}:slot,mute});while(records.size()>64)records.pop_front();
            }
            DXGI_FRAME_STATISTICS stats{};HRESULT sh=surface.swap->GetFrameStatistics(&stats);
            if(SUCCEEDED(sh)){
                bool modeChanged=false,directFlipReport=false;
                if(media){DXGI_FRAME_STATISTICS_MEDIA fm{};if(SUCCEEDED(media->GetFrameStatisticsMedia(&fm))){presentationMode=int(fm.CompositionMode);bool c=fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSED||fm.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE;directFlipReport=!c;if(c!=composed){composed=c;local.modeChanges++;modeChanged=true;}local.composed=composed;}}
                LARGE_INTEGER freq;QueryPerformanceFrequency(&freq);double sync=double(stats.SyncQPCTime.QuadPart)/double(freq.QuadPart);
                const bool hadClock=clock.samples>=8;
                const auto outliers=clock.rejectedSamples,corrections=clock.corrections,previousClockRefresh=clock.lastRefresh;
                clock.observeContinuous(stats.SyncRefreshCount,sync);
                if(clock.lastRefresh<previousClockRefresh){
                    // Only a confirmed counter replacement moves the fitted
                    // clock backwards. Reservations in its old counter domain
                    // must not hold the new sequence far into the future.
                    reservation.reset();anchor.reset();records.clear();lastObserved=lastRefresh=0;
                }
                local.clockOutliers+=clock.rejectedSamples-outliers;
                local.clockCorrections+=clock.corrections-corrections;
                if(hadClock&&clock.samples<8)++local.clockReacquires;
                bool mismatch=false;
                // Keep the present anchor and timestamp clock in the same counter
                // domain while a changed/discontinuous report is being checked.
                if((!anchor.valid||stats.PresentCount!=lastObserved)&&clock.lastRefresh==stats.SyncRefreshCount){
                    auto record=std::find_if(records.begin(),records.end(),[&](auto& r){return r.id==stats.PresentCount;});
                    mismatch=record!=records.end()&&!record->muted&&record->expected&&record->expected!=stats.PresentRefreshCount;
                    if(record!=records.end()&&!record->muted&&record->slot.eye==Eye::Black&&sequenceSlot(s.sequence,stats.PresentRefreshCount,false).eye!=Eye::Black)++local.blackOnImage;
                    anchor.observe(stats.PresentCount,stats.PresentRefreshCount,!directFlipReport);
                    reservation.observe(stats.PresentCount);
                    lastObserved=anchor.present;lastRefresh=UINT(anchor.refresh);
                }
                // Repair future submissions without a full blanking/relock cycle.
                // Already queued frames retain their old eye/black slot, so a slip
                // can affect several refreshes; it is not necessarily a single wrong eye.
                if(!preview&&blank==0&&mismatch)slip(1);(void)modeChanged;
            }else if(sh==DXGI_ERROR_FRAME_STATISTICS_DISJOINT){
                // DXGI can restart its statistics across desktop presentation
                // changes. It is not a request to disable/reinitialize the IR
                // emitter or insert sixteen black frames. Keep the last clock
                // and anchor until subsequent samples establish their successor.
                ++local.statsDisjoints;
            }
            else if(!preview){local.message="Presentation statistics unavailable; reacquiring stereo timing";}
            if(blank>0){--blank;slotIndex=0;}else ++slotIndex;
            double now=qpc();if(!slipWindowStart)slipWindowStart=now;if(now-slipWindowStart>=1){local.slipsLastSecond=slipWindow;slipWindow=0;slipWindowStart=now;}local.lastIntervalMs=(now-last)*1000;local.maxIntervalMs=std::max(local.maxIntervalMs,local.lastIntervalMs);last=now;local.presents++;local.elapsed=now-begin;local.measuredHz=clock.period?1/clock.period:0;local.sourcePair=draw.active?draw.active->pairId:0;local.locked=!preview && !paused && emitterAvailable && blank==0 && clock.samples>=8;
            {double ms=local.lastIntervalMs;recentIntervals.push_back(ms);intervalSum+=ms;intervalSumSq+=ms*ms;if(recentIntervals.size()>256){double old=recentIntervals.front();recentIntervals.pop_front();intervalSum-=old;intervalSumSq-=old*old;}
             double n=double(recentIntervals.size());double variance=n>1?std::max(0.,intervalSumSq/n-(intervalSum/n)*(intervalSum/n)):0;local.presentJitterUs=std::sqrt(variance)*1000;}
            local.vblankJitterRmsUs=clock.jitterRmsUs;local.vblankJitterMaxUs=clock.jitterMaxUs;local.clockSamples=clock.samples;
            if(!local.locked){lockedSince=now;local.timingPassed=false;} if(!preview)local.elapsed=now-lockedSince;local.timingPassed=local.locked && local.elapsed>=600 && local.resyncs==0;
            if(local.intervals.size()<4096)local.intervals.push_back(local.lastIntervalMs);
            if(preview)local.message="PREVIEW - both eyes side-by-side; no emitter commands";
            else if(paused)local.message="Paused - output black, emitter suspended";
            else if(local.locked)local.message=std::string("Presentation clock acquired (")+(composed?"DWM composed":"direct flip")+") - optical sync still requires your confirmation"+(local.slipsLastSecond?" - "+std::to_string(local.slipsLastSecond)+" timing misses in the last second; late black frames can cause visible flashes":"");
            // The 2D stream view is drawn last, from the pair this thread already holds
            // open above, and only when its swap chain reports a free buffer.
            // Present must ALSO request DO_NOT_WAIT: a free frame-latency slot
            // does not guarantee that the GPU/compositor can accept a present.
            // Skip a busy stream frame instead of delaying the next stereo slot.
            // Any failure drops the stream view and leaves the stereo output
            // untouched - a broken screen share must not cost the viewer their 3D.
            // A message left by a failure stays until a new window is asked for: it is the
            // only place the viewer learns why their screen share went away.
            if(streamWindow!=streamOpened){stream.reset();streamOpened=nullptr;local.streaming=false;local.streamFrames=0;if(streamWindow)local.streamMessage.clear();{std::lock_guard l(mutex_);streamOpen_=false;}streamIdle_.notify_all();}
            if(streamWindow&&!IsWindow(streamWindow)){std::lock_guard l(mutex_);if(streamWindow_==streamWindow)streamWindow_=nullptr;streamWindow=nullptr;}
            if(streamWindow)try{
                if(!stream){stream=std::make_unique<Surface>();stream->attach(surface.device.Get(),surface.context.Get(),streamWindow,2);streamOpened=streamWindow;local.streaming=true;local.streamMessage="2D stream view running";{std::lock_guard l(mutex_);streamOpen_=true;}}
                RECT view{};GetClientRect(streamWindow,&view);
                if(view.right>0&&view.bottom>0&&now-lastStream>=1/streamViewRate&&WaitForSingleObject(stream->waitable,0)==WAIT_OBJECT_0){
                    if(unsigned(view.right)!=stream->width||unsigned(view.bottom)!=stream->height)stream->resize(view.right,view.bottom);
                    stream->bind();
                    draw.draw(surface.context.Get(),stream->width,stream->height,streamViewSettings(s),streamViewEye(s,streamRight),false,pattern,pairTime);
                    const HRESULT presented=stream->swap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
                    if(presented!=DXGI_ERROR_WAS_STILL_DRAWING){
                        check(presented,"Present the stream view");
                        if(presented==S_OK)++local.streamFrames;
                    }
                    lastStream=now;
                }
            }catch(const std::exception& e){
                stream.reset();streamOpened=nullptr;local.streaming=false;local.streamMessage=std::string("2D stream view stopped: ")+e.what();
                {std::lock_guard l(mutex_);streamOpen_=false;if(streamWindow_==streamWindow)streamWindow_=nullptr;}
                streamIdle_.notify_all();lastPublish=0; // report the failure now, not in a quarter of a second
            }
            if(diagnostics.steadyTimingProbe){local.locked=false;local.message="Steady timing diagnostic: one eye, no emitter commands";}
            TraceFrame frame{now-begin,waitEnd-waitBegin,sourceEnd-sourceBegin,drawEnd-sourceEnd,presentEnd-presentBegin,now-presentEnd,qpc()-now,id,UINT(expected),stats.PresentCount,stats.PresentRefreshCount,stats.SyncRefreshCount,sh,composed};frame.mode=presentationMode;
            if(!diagnostics.trace.empty()&&trace.size()<30000)trace.push_back(frame);
            {auto& m=gpuMarks.back();if(!m.present){m.present=id;m.submitted=presentBegin;m.displayAt=anchor.valid&&clock.samples>=8&&expected?clock.predict(expected):0;}}
            if(eventFramesLeft){eventFrames.push_back(frame);
                if(--eventFramesLeft==0){std::ostringstream o;o<<std::fixed<<std::setprecision(1)<<"misses +"<<local.misses-eventMisses<<", black on image +"<<local.blackOnImage-eventBlack<<", capture pair "<<local.sourcePair<<", queue "<<queueDepth<<"\n"<<eventHeader;
                    for(size_t i=0;i<eventFrames.size();++i){if(i==eventFrames.size()-16)o<<"--- miss detected on the frame above; following frames:\n";traceLine(o,eventFrames[i]);}
                    eventFrames.clear();std::lock_guard l(mutex_);if(missEvents_.size()<64)missEvents_.push_back(o.str());}}
            else if(!preview&&local.misses>lastMissCount){
                std::ostringstream o;o<<std::fixed<<std::setprecision(3)<<"=== miss at "<<now-begin<<" s into output ("<<(composed?"composed":"direct flip")<<")\nms: time wait source draw present stats stream | submitted expected observed refresh sync mode | gpu slack\n";
                eventHeader=o.str();eventFrames.assign(recentFrames.begin(),recentFrames.end());eventFramesLeft=16;eventMisses=lastMissCount;eventBlack=local.blackOnImage;
            }
            lastMissCount=local.misses;
            recentFrames.push_back(frame);if(recentFrames.size()>48)recentFrames.pop_front();
            if(now-lastPublish>.25){std::lock_guard l(mutex_);status_=local;lastPublish=now;}
        }
        if(!diagnostics.trace.empty()){
            std::ofstream report(diagnostics.trace);report<<"time,wait,source,draw,present,stats,stream,submitted,expected,observed,refresh,sync,result,composed\n"<<std::setprecision(10);
            for(const auto& f:trace)report<<f.time<<','<<f.wait<<','<<f.source<<','<<f.draw<<','<<f.present<<','<<f.stats<<','<<f.stream<<','<<f.submitted<<','<<f.expected<<','<<f.observed<<','<<f.refresh<<','<<f.sync<<','<<f.result<<','<<f.composed<<'\n';
        }
        stream.reset();emitter_.suspend();{std::lock_guard l(mutex_);local.running=false;local.locked=false;local.streaming=false;streamOpen_=false;status_=local;}streamIdle_.notify_all();
    }catch(const std::exception& e){emitter_.suspend();{std::lock_guard l(mutex_);status_.running=false;status_.locked=false;status_.streaming=false;streamOpen_=false;status_.message=e.what();}streamIdle_.notify_all();}
    if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
}
void Presenter::exportReport(const std::filesystem::path& path,const Settings& s,const std::string& source)const{
    auto r=status();auto usb=emitter_.status();std::filesystem::create_directories(path.parent_path());std::ofstream out(path);if(!out)throw std::runtime_error("Cannot write timing report.");
    auto samples=r.intervals;std::sort(samples.begin(),samples.end());auto percentile=[&](double q){return samples.empty()?0:samples[size_t(q*(samples.size()-1))];};
    out<<std::setprecision(10)<<"Vision Restoration timing report\nProfile: "<<s.name<<"\nDisplay: "<<s.displayId<<"\nConnection: "<<s.connection<<"\nMode: "<<s.width<<'x'<<s.height<<" @ "<<s.refresh<<" Hz\nHDR: "<<s.hdr<<"\nPreview: "<<r.preview<<"\nSequence: "<<int(s.sequence)<<"\nPhase us: "<<s.phaseUs<<"\nLeft/right duration us: "<<s.leftUs<<'/'<<s.rightUs<<"\nSource: "<<source<<"\nElapsed seconds: "<<r.elapsed<<"\nPresents: "<<r.presents<<"\nDetected misses: "<<r.misses<<"\nResynchronizations: "<<r.resyncs<<"\nMeasured refresh Hz: "<<r.measuredHz<<"\nCPU present interval p50/p95/p99 ms: "<<percentile(.5)<<'/'<<percentile(.95)<<'/'<<percentile(.99)<<"\nUSB commands/errors/late: "<<usb.commands<<'/'<<usb.errors<<'/'<<usb.late<<"\nLast/max USB transfer us: "<<usb.lastTransferUs<<'/'<<usb.maxTransferUs<<"\nVblank jitter rms/max us: "<<r.vblankJitterRmsUs<<'/'<<r.vblankJitterMaxUs<<"\nPresent interval jitter us: "<<r.presentJitterUs<<"\nEye command timing error last/rms/max us: "<<usb.sendErrorLastUs<<'/'<<usb.sendErrorRmsUs<<'/'<<usb.sendErrorMaxUs<<"\nTiming block writes: "<<usb.timingWrites<<"\nGlasses operation confirmed: "<<s.glassesConfirmed<<"\nEye order confirmed: "<<s.eyeConfirmed<<"\nUser assessment: "<<s.assessment<<"\nMonitor settings: "<<s.monitorNotes<<"\nValidated: "<<s.validated<<"\nThese are host timings, not optical measurements.\n";
    out<<"Presenter status: "<<r.message<<"\n";
    out<<"Queued presentation frames: "<<r.queueDepth<<"\nQueue refill corrections: "<<r.queueRefillCorrections<<"\n";
    out<<"Clock rejected samples/confirmed corrections: "<<r.clockOutliers<<'/'<<r.clockCorrections<<"\nStatistics discontinuities: "<<r.statsDisjoints<<"\nRepeated eye commands suppressed: "<<usb.repeatedCommands<<"\n";
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
            report<<(hdr?"HDR":"SDR")<<" LCD black floor: lifts every eye-frame pixel, keeps black frames at zero PASS\n";
            // Negative black level: the black point moves up, so the eye frame carries less light.
            Settings crush;crush.hdr=hdr;crush.blackFloor=-.2f;
            if(!(total(crush,Eye::Left)<total(plain,Eye::Left)))throw std::runtime_error("Negative black level did not darken the eye frame.");
            if(total(crush,Eye::Black)!=0)throw std::runtime_error("Negative black level lit a black frame.");
            report<<(hdr?"HDR":"SDR")<<" darker black level: darkens the eye frame, keeps black frames at zero PASS\n";}
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
    for(Packing packing:{Packing::SideBySide,Packing::TopBottom,Packing::SeparateEyes}){
        const bool separate=packing==Packing::SeparateEyes;
        std::vector<uint32_t> pixels(16*8*(separate?2:1));for(size_t n=0;n<pixels.size();++n){const int x=int(n%16),y=int(n/16);pixels[n]=(separate?y<8:packing==Packing::SideBySide?x<8:y<4)?0xff0000ff:0xff00ff00;}
        td.Width=16;td.Height=8;td.ArraySize=separate?2:1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_IMMUTABLE;td.CPUAccessFlags=0;D3D11_SUBRESOURCE_DATA data[2]{{pixels.data(),16*4,0},{separate?pixels.data()+16*8:nullptr,16*4,0}};ComPtr<ID3D11Texture2D> input;check(device->CreateTexture2D(&td,data,&input),"Stereo test input");
        draw.release();check(device->CreateShaderResourceView(input.Get(),nullptr,&draw.view),"Packed input view");draw.active=std::make_shared<StereoFrame>();draw.active->packing=packing;draw.active->encoding=Encoding::SRGB;draw.active->width=16;draw.active->height=8;
        for(bool hook:{false,true})for(int eye:{0,1})for(float convergence:{-.05f,0.f,.05f}){
            if(hook&&separate)continue; // legacy hook shader only consumes packed textures
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
        if(packing==Packing::SideBySide){
            // At a 2:1 output, square eyes need side bars and 4:1 eyes need
            // top/bottom bars. Check every pixel, both eyes, SDR/HDR and the
            // separate inspection panes so an aspect fix cannot mix eyes.
            for(float aspect:{1.f,2.f,4.f})for(bool preview:{false,true})for(bool hdr:{false,true})for(int eye:{0,1}){
                draw.active->eyeAspect=aspect;Settings s;s.hdr=hdr;
                draw.draw(context.Get(),160,80,s,Eye(eye),preview,3,0);
                context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};
                check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read SBS aspect pixels");bool correct=true;
                for(int y=0;y<80;++y)for(int x=0;x<160;++x){
                    if(preview&&y>=72)continue; // inspection label overlays the bottom rows
                    const int paneWidth=preview?80:160,localX=x%paneWidth,selected=preview?x/80:eye;
                    const float outputAspect=float(paneWidth)/80;
                    const float width=aspect<outputAspect?80*aspect:float(paneWidth);
                    const float height=aspect>outputAspect?paneWidth/aspect:80.f;
                    const bool black=std::abs(localX+.5f-paneWidth*.5f)>width*.5f||std::abs(y+.5f-40)>height*.5f;
                    const auto* pixel=static_cast<uint8_t*>(m.pData)+y*m.RowPitch+x*4;
                    correct&=(black?pixel[selected]==0:pixel[selected]>=254)&&pixel[1-selected]==0&&pixel[2]==0;
                }
                context->Unmap(staging.Get(),0);if(!correct)throw std::runtime_error("Half/Full SBS aspect, bars or inspection eye split failed.");
            }
            draw.active->eyeAspect=0;
            report<<"Half/Full SBS: eye aspect, pillarbox/letterbox, SDR/HDR and inspection panes PASS\n";
        }
        // Reuse the same DrawState/context exactly as the live stereo+stream
        // loop does. Every stream pixel must stay on the selected eye, even
        // immediately after a black slot, with all panel corrections enabled.
        for(bool swap:{false,true})for(bool right:{false,true}){
            Settings panel;panel.sequence=Sequence::BlackInsertion;panel.swapEyes=swap;
            panel.bandHeight=.4f;panel.convergence=.05f;panel.imageGain=4;
            panel.cancelCrosstalk=true;panel.leakProfile.fill(.4f);
            const int selected=int(streamViewEye(panel,right));
            for(unsigned index=0;index<12;++index){
                draw.draw(context.Get(),160,80,panel,sequenceSlot(panel.sequence,index,false).eye,false,3,0);
                draw.draw(context.Get(),160,80,streamViewSettings(panel),streamViewEye(panel,right),false,3,0);
                context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};
                check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read stable stream pixels");bool correct=true;
                for(unsigned y=0;y<80;++y)for(unsigned x=0;x<160;++x){
                    const auto* pixel=static_cast<uint8_t*>(m.pData)+y*m.RowPitch+x*4;
                    correct&=pixel[selected]>=254&&pixel[1-selected]==0&&pixel[2]==0;
                }
                context->Unmap(staging.Get(),0);
                if(!correct)throw std::runtime_error("2D stream inherited a black slot, opposite eye, or panel correction.");
            }
        }
        // The explicitly selected diagnostic shows one eye at every refresh.
        // This mode is never selected automatically during stereo playback.
        for(bool swap:{false,true}){
            Settings panel;panel.hdr=false;panel.sequence=Sequence::BlackInsertion;
            panel.swapEyes=swap;panel.bandHeight=.4f;panel.imageGain=4;panel.convergence=.05f;
            for(unsigned index=0;index<24;++index){
                const auto selected=streamViewEye(panel,false);
                draw.draw(context.Get(),160,80,steadyViewSettings(panel),selected,false,3,0);
                context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE m{};
                check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read explicit diagnostic pixels");bool correct=true;
                for(unsigned y=0;y<80;++y)for(unsigned x=0;x<160;++x){
                    const auto* pixel=static_cast<uint8_t*>(m.pData)+y*m.RowPitch+x*4;
                    correct&=pixel[int(selected)]>=254&&pixel[1-int(selected)]==0&&pixel[2]==0;
                }
                context->Unmap(staging.Get(),0);
                if(!correct)throw std::runtime_error("Explicit steady diagnostic contained black/opposite-eye frames.");
            }
        }
    }
    draw.release();report<<"Presenter: separate texture-array eyes, SBS/TB eye boundaries, signed convergence and black margins PASS; hook: SBS/TB PASS\n";
    report<<"2D stream: all pixels retain one eye through repeated L/B/R/B cycles, both eye selections and eye swap, SBS/TAB/arrays PASS\n";
    report<<"Explicit timing diagnostic: fixed eye at every refresh; SBS/TAB/arrays PASS\n";
    // Uniform eye colors cannot detect a vertical wrap. Use a monotonic red
    // ramp, distinct top/bottom blue markers and a different green eye code.
    // The aspect matches the live 3840x2126 client fitted to a 3840x2160 display;
    // smaller textures keep this offscreen geometry check inexpensive.
    {
        constexpr unsigned inputWidth=16,inputHeight=128,outWidth=384,outHeight=216;
        constexpr float eyeAspect=3840.f/2126.f;
        std::vector<float> pixels(size_t(inputWidth)*inputHeight*4);
        for(unsigned y=0;y<inputHeight;++y)for(unsigned x=0;x<inputWidth;++x){
            auto* pixel=pixels.data()+(size_t(y)*inputWidth+x)*4;
            pixel[0]=(float(y)+.5f)/inputHeight;
            pixel[1]=x<inputWidth/2?.25f:.75f;
            pixel[2]=y<inputHeight/8?.875f:y>=inputHeight*7/8?.125f:.5f;pixel[3]=1;
        }
        D3D11_TEXTURE2D_DESC desc{};desc.Width=inputWidth;desc.Height=inputHeight;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),inputWidth*4*sizeof(float),0};ComPtr<ID3D11Texture2D> input;
        check(device->CreateTexture2D(&desc,&data,&input),"Vertical capture marker input");
        draw.release();check(device->CreateShaderResourceView(input.Get(),nullptr,&draw.view),"Vertical capture marker view");
        draw.active=std::make_shared<StereoFrame>();draw.active->packing=Packing::SideBySide;draw.active->encoding=Encoding::LinearScRGB;
        draw.active->width=inputWidth;draw.active->height=inputHeight;draw.active->eyeAspect=eyeAspect;
        desc.Width=outWidth;desc.Height=outHeight;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target,read;check(device->CreateTexture2D(&desc,nullptr,&target),"Vertical capture marker target");
        ComPtr<ID3D11RenderTargetView> targetView;check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"Vertical capture marker RTV");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc,nullptr,&read),"Vertical capture marker readback");
        auto* view=targetView.Get();context->OMSetRenderTargets(1,&view,nullptr);D3D11_VIEWPORT viewport{0,0,float(outWidth),float(outHeight),0,1};context->RSSetViewports(1,&viewport);
        for(bool preview:{false,true})for(Eye eye:{Eye::Left,Eye::Right}){
            Settings settings;settings.hdr=true;draw.draw(context.Get(),outWidth,outHeight,settings,eye,preview,3,0);
            context->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"Read vertical capture markers");
            bool correct=true;const unsigned paneWidth=preview?outWidth/2:outWidth;
            const float fittedHeight=paneWidth/eyeAspect,top=(outHeight-fittedHeight)*.5f;
            for(unsigned pane=0;pane<(preview?2u:1u);++pane){
                float previous=-1;
                for(unsigned y=0;y<outHeight;++y){
                    const auto* row=reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch);
                    const auto* pixel=row+(pane*paneWidth+paneWidth/2)*4;
                    const float sourceY=(float(y)+.5f-top)/fittedHeight;
                    if(sourceY<0||sourceY>1){correct&=pixel[0]==0&&pixel[1]==0&&pixel[2]==0;continue;}
                    const float expected=std::clamp(sourceY,.5f/inputHeight,1-.5f/inputHeight);
                    correct&=std::abs(pixel[0]-expected)<.003f&&pixel[0]>=previous;
                    correct&=std::abs(pixel[1]-((preview?pane:unsigned(eye))==0?.25f:.75f))<.003f;
                    if(sourceY<.06f)correct&=pixel[2]>.85f;
                    if(sourceY>.94f)correct&=pixel[2]<.15f;
                    previous=pixel[0];
                }
            }
            context->Unmap(read.Get(),0);
            if(!correct)throw std::runtime_error("Captured SBS vertical rows wrapped, reversed, crossed eyes or escaped the fitted image.");
        }
        draw.release();report<<"Capture geometry: monotonic vertical rows, distinct top/bottom markers and both eyes; 3840x2126 aspect to 16:9, HDR full-eye and inspection panes PASS\n";
    }
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
    // HDR10 output: captured scRGB reaches the 10-bit target as PQ/Rec.2020 code values, black
    // frames stay exactly zero, and the SDR stream view on the same draw state is not encoded.
    {
        const float values[]{.05f,.4f,1,1,2.5f,12.5f,0,1}; // left half of the SBS pair: 4, 32, 80 nits; right: 200, 1000, 0
        D3D11_TEXTURE2D_DESC desc{};desc.Width=2;desc.Height=1;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{values,sizeof(values),0};ComPtr<ID3D11Texture2D> input;check(device->CreateTexture2D(&desc,&data,&input),"HDR10 input");
        draw.release();check(device->CreateShaderResourceView(input.Get(),nullptr,&draw.view),"HDR10 input view");draw.active=std::make_shared<StereoFrame>();draw.active->encoding=Encoding::LinearScRGB;draw.active->width=2;draw.active->height=1;draw.active->alignmentApplied=true;
        desc.Width=32;desc.Height=16;desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
        ComPtr<ID3D11Texture2D> target,read;check(device->CreateTexture2D(&desc,nullptr,&target),"HDR10 target");ComPtr<ID3D11RenderTargetView> targetView;check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"HDR10 RTV");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&desc,nullptr,&read),"HDR10 readback");
        auto* view=targetView.Get();context->OMSetRenderTargets(1,&view,nullptr);D3D11_VIEWPORT viewport{0,0,32,16,0,1};context->RSSetViewports(1,&viewport);
        auto pqCode=[](double scrgb){const double y=std::clamp(scrgb*80/10000,0.,1.);if(y<1e-7)return 0.;const double p=std::pow(y,.1593017578125);return std::pow((.8359375+18.8515625*p)/(1+18.6875*p),78.84375)*1023;};
        auto pixels=[&](const Settings& settings,Eye eye){draw.draw(context.Get(),32,16,settings,eye,false,3,0);context->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read HDR10");
            std::vector<uint32_t> out(32*16);for(unsigned y=0;y<16;++y)memcpy(&out[y*32],static_cast<const uint8_t*>(m.pData)+y*m.RowPitch,32*4);context->Unmap(read.Get(),0);return out;};
        draw.pqOutput=true;Settings settings;settings.hdr=true;
        for(Eye eye:{Eye::Left,Eye::Right}){
            const auto out=pixels(settings,eye);const float* rgb=values+(eye==Eye::Left?0:4);
            const double r=.6274040*rgb[0]+.3292820*rgb[1]+.0433136*rgb[2],g=.0690970*rgb[0]+.9195400*rgb[1]+.0113612*rgb[2],b=.0163916*rgb[0]+.0880132*rgb[1]+.8955950*rgb[2];
            for(uint32_t v:out){const double actual[3]{double(v&1023),double((v>>10)&1023),double((v>>20)&1023)},expected[3]{pqCode(r),pqCode(g),pqCode(b)};
                for(int c=0;c<3;++c)if(std::abs(actual[c]-expected[c])>1.5)throw std::runtime_error("HDR10 output: wrong PQ/Rec.2020 code value.");}
        }
        for(uint32_t v:pixels(settings,Eye::Black))if(v&0x3fffffff)throw std::runtime_error("HDR10 output: black frame is not code value zero.");
        // The curve must match the ST 2084 reference point: 100 nits encodes to 0.5081.
        if(std::abs(pqCode(1.25)/1023-.5081)>.001)throw std::runtime_error("HDR10 reference curve is wrong.");
        Settings sdr=streamViewSettings(settings);const auto stream=pixels(sdr,Eye::Left);
        // The SDR stream path stays sRGB even with pqOutput set: gray 0.4 of white -> sRGB 170.
        const uint32_t s0=stream[0];if(std::abs(int((s0>>10)&1023)-int(std::lround((1.055*std::pow(.4/1.,1/2.4)-.055)*1023)))>2)throw std::runtime_error("HDR10 encoding leaked into the SDR stream view.");
        draw.pqOutput=false;
    }
    draw.release();report<<"HDR10 output: PQ/Rec.2020 code values for captured scRGB, black frames at zero, SDR stream view unencoded PASS\n";
    // The control menu inside the stereo picture: exact menu pixels inside its rectangle in both eye
    // frames (SDR), the picture elsewhere, nothing in a black frame, and nothing once it is removed.
    {
        std::vector<uint32_t> menuPixels(16*8,0xff3060c0u); // RGBA8: r=0xc0,g=0x60,b=0x30
        D3D11_TEXTURE2D_DESC md{};md.Width=16;md.Height=8;md.MipLevels=md.ArraySize=md.SampleDesc.Count=1;md.Format=DXGI_FORMAT_R8G8B8A8_UNORM;md.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA mdata{menuPixels.data(),16*4,0};ComPtr<ID3D11Texture2D> menuTexture;check(device->CreateTexture2D(&md,&mdata,&menuTexture),"Menu test image");
        check(device->CreateShaderResourceView(menuTexture.Get(),nullptr,&draw.menuView),"Menu test view");
        // An 8x4 menu at (8,4) on a 32x16 output, filling half of a 16x8 image in each direction.
        draw.menuRect[0]=8/32.f;draw.menuRect[1]=4/16.f;draw.menuRect[2]=16/32.f;draw.menuRect[3]=8/16.f;draw.menuTexture[0]=.5f;draw.menuTexture[1]=.5f;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=32;desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target,read;check(device->CreateTexture2D(&desc,nullptr,&target),"Menu test target");ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"Menu test RTV");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&desc,nullptr,&read),"Menu test readback");
        auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);D3D11_VIEWPORT viewport{0,0,32,16,0,1};context->RSSetViewports(1,&viewport);
        auto pixels=[&](Eye eye){Settings settings;draw.draw(context.Get(),32,16,settings,eye,false,1,0);context->CopyResource(read.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read menu test");
            std::vector<uint32_t> out(32*16);for(unsigned y=0;y<16;++y)memcpy(&out[y*32],static_cast<const uint8_t*>(m.pData)+y*m.RowPitch,32*4);context->Unmap(read.Get(),0);return out;};
        for(Eye eye:{Eye::Left,Eye::Right}){const auto out=pixels(eye);
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x){const bool inside=x>=8&&x<16&&y>=4&&y<8;
                if(inside&&(out[y*32+x]&0xffffff)!=0x3060c0u)throw std::runtime_error("Menu pixels are not the menu image in an eye frame.");
                if(!inside&&(out[y*32+x]&0xffffff)==0x3060c0u)throw std::runtime_error("Menu drawn outside its rectangle.");}}
        for(uint32_t v:pixels(Eye::Black))if(v&0xffffff)throw std::runtime_error("Menu lit a black frame.");
        draw.menuView.Reset();for(uint32_t v:pixels(Eye::Left))if((v&0xffffff)==0x3060c0u)throw std::runtime_error("Menu still drawn after it was removed.");
    }
    draw.release();report<<"Control menu inside the stereo picture: exact pixels in both eye frames, only in its rectangle, never in black frames PASS\n";
    return true;
}
}
