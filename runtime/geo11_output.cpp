// SPDX-License-Identifier: GPL-3.0-or-later
// Geo11's documented proxy_d3d11 chain loads this output adapter beneath its
// renderer. The game keeps its HWND, message loop, input and established fix.
// A logical backbuffer isolates game rendering from the physical swapchain.
// Only the output thread presents; it uses a separate device and keeps one
// completed stereo pair for an entire eye/black cycle, even if the game stalls.
#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <avrt.h>
#include "sync_protocol.h"
#include "stereo_blit_shader.h"
#ifdef VISION_STEREO_TESTING
#include "output_test_api.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
namespace sync=vision::sync;
#ifdef VISION_STEREO_TESTING
std::mutex testMutex;VisionOutputTestStats testStats;
std::atomic<UINT> testProducerDelay{0};
std::atomic<UINT> testOutputDelay{0};
#endif
using CreateDevice=decltype(&D3D11CreateDevice);
using FactoryCreate=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*,IUnknown*,DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**);
using FactoryCreate1=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
constexpr UINT outputBuffers=4,outputLatency=3;
CreateDevice realCreate=nullptr;FactoryCreate realChain=nullptr;FactoryCreate1 realChain1=nullptr;
// Private metadata forwards through a provider's COM wrapper without depending
// on its class layout, version-specific offsets, or shader implementation.
constexpr GUID nativeDeviceTag{0x923466ef,0x6857,0x45b9,{0x92,0x24,0x9c,0xa1,0x2c,0x71,0xec,0xb6}};
decltype(&CreateFileMappingW) realMapping=nullptr;decltype(&OpenFileMappingW) realOpenMapping=nullptr;
std::wstring pairMapping(){return L"Local\\KatangaMappedFile.Vision."+std::to_wstring(GetCurrentProcessId());}
bool katangaName(LPCWSTR name){return name&&(_wcsicmp(name,L"Local\\KatangaMappedFile")==0||_wcsicmp(name,L"KatangaMappedFile")==0);}
HANDLE WINAPI createMapping(HANDLE file,LPSECURITY_ATTRIBUTES security,DWORD protection,DWORD high,DWORD low,LPCWSTR name){
    if(katangaName(name)){auto isolated=pairMapping();return realMapping(file,security,protection,high,low,isolated.c_str());}return realMapping(file,security,protection,high,low,name);
}
HANDLE WINAPI openMapping(DWORD access,BOOL inherit,LPCWSTR name){
    if(katangaName(name)){auto isolated=pairMapping();return realOpenMapping(access,inherit,isolated.c_str());}return realOpenMapping(access,inherit,name);
}
std::once_flag initOnce;std::mutex hooksMutex;bool ready=false;thread_local bool internal=false;
int64_t qpc(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart;}
int64_t frequency(){static auto f=[](){LARGE_INTEGER t;QueryPerformanceFrequency(&t);return t.QuadPart;}();return f;}
void log(const char* message,HRESULT hr=S_OK){
    wchar_t module[MAX_PATH];GetModuleFileNameW(nullptr,module,MAX_PATH);
    auto file=std::filesystem::path(module).parent_path()/L"VisionStereo11.log";
    FILE* f=nullptr;_wfopen_s(&f,file.c_str(),L"ab");if(f){std::fprintf(f,"%llu %s (0x%08lx)\n",GetTickCount64(),message,hr);std::fclose(f);}
}
struct Internal { bool previous=internal;Internal(){internal=true;}~Internal(){internal=previous;} };
struct Host {
    HANDLE mapping=nullptr,event=nullptr;sync::Shared* shared=nullptr;
    ~Host(){close();}
    void close(){if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(event)CloseHandle(event);mapping=event=nullptr;shared=nullptr;}
    bool connect(){
        if(!shared){
#ifdef VISION_STEREO_TESTING
            auto name=L"Local\\VisionRestoration.GameSync.Test."+std::to_wstring(GetCurrentProcessId());
            mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
#else
            mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,sync::mappingName);
#endif
            if(!mapping)return false;
            shared=static_cast<sync::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(sync::Shared)));
            if(!shared||shared->magic!=sync::magic||shared->version!=sync::version){close();return false;}
#ifndef VISION_STEREO_TESTING
            event=OpenEventW(EVENT_MODIFY_STATE,FALSE,sync::frameEventName);
#endif
        }
        const auto age=qpc()-shared->hostHeartbeatQpc;
        if(age<0||age>frequency()*3){close();return false;}
        return true;
    }
};
enum class SlotState{Free,Writing,Ready,Reading};
struct PairSlot {
    ComPtr<ID3D11Texture2D> producer;ComPtr<IDXGIKeyedMutex> producerLock;HANDLE handle=nullptr;
    ComPtr<ID3D11Texture2D> consumer;ComPtr<IDXGIKeyedMutex> consumerLock;
    ComPtr<ID3D11Fence> producerFence,consumerFence;uint64_t fenceValue=0;
    SlotState state=SlotState::Free;uint64_t serial=0;
};
struct PairPool { D3D11_TEXTURE2D_DESC desc{};std::array<PairSlot,3> slots; };
bool sameFormat(const D3D11_TEXTURE2D_DESC& a,const D3D11_TEXTURE2D_DESC& b){return a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format;}
bool supported(DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||f==DXGI_FORMAT_R10G10B10A2_UNORM||f==DXGI_FORMAT_R16G16B16A16_FLOAT;}
DXGI_FORMAT outputFormat(DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_R8G8B8A8_UNORM:f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?DXGI_FORMAT_B8G8R8A8_UNORM:f;}
HRESULT compile(const char* entry,const char* profile,ID3DBlob** blob){
    ComPtr<ID3DBlob> errors;auto hr=D3DCompile(vision::stereoBlitShader,strlen(vision::stereoBlitShader),"VisionStereo11",nullptr,nullptr,entry,profile,0,0,blob,&errors);
    if(FAILED(hr))log(errors?static_cast<char*>(errors->GetBufferPointer()):"Shader compilation failed",hr);return hr;
}
class StereoChain final:public IDXGISwapChain4 {
    std::atomic<ULONG> refs_{1};ComPtr<ID3D11Device> gameDevice_;ComPtr<ID3D11DeviceContext> gameContext_;
    ComPtr<IDXGIFactory> factory_;DXGI_SWAP_CHAIN_DESC gameDesc_{};ComPtr<ID3D11Texture2D> gameBuffer_;
    std::mutex producerMutex_,stateMutex_;std::shared_ptr<PairPool> pool_;uint64_t serial_=0;
    std::shared_ptr<PairPool> pendingPairPool_;PairSlot* pendingPair_=nullptr;
    HANDLE katangaMap_=nullptr;const volatile uint32_t* katangaView_=nullptr;uint32_t katangaHandle_=0;ComPtr<ID3D11Texture2D> katangaTexture_;
    ComPtr<ID3D11Device> outputDevice_;ComPtr<ID3D11DeviceContext> outputContext_;ComPtr<IDXGISwapChain3> physical_;
    ComPtr<ID3D11DeviceContext4> gameFenceContext_,outputFenceContext_;
    ComPtr<ID3D11Fence> gameCompletion_;ComPtr<ID3D11Query> gameCompletionQuery_;
    HANDLE gameCompletionEvent_=nullptr,gameCompletionTimer_=nullptr;
    uint64_t gameCompletionValue_=0;bool gameGpuPending_=false;
    std::atomic<uint64_t> gpuCompleted_{0},gpuWaitTicks_{0};
    ComPtr<ID3D11VertexShader> vs_;ComPtr<ID3D11PixelShader> ps_;ComPtr<ID3D11Buffer> constants_;ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> snapshot_;ComPtr<ID3D11ShaderResourceView> snapshotView_;D3D11_TEXTURE2D_DESC snapshotDesc_{};
    ComPtr<ID3D11RenderTargetView> outputTarget_;
    HANDLE latency_=nullptr,producerPermit_=nullptr;std::jthread worker_;std::atomic<bool> resized_{false};std::atomic<UINT> logicalPresents_{0};
    std::atomic<DXGI_COLOR_SPACE_TYPE> colorSpace_{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
    std::atomic<uint64_t> received_{0};std::atomic<bool> lost_{false};
    HRESULT createLogicalBuffer(){
        auto& d=gameDesc_;if(!d.BufferDesc.Width||!d.BufferDesc.Height){RECT r{};GetClientRect(d.OutputWindow,&r);if(!d.BufferDesc.Width)d.BufferDesc.Width=std::max<LONG>(1,r.right);if(!d.BufferDesc.Height)d.BufferDesc.Height=std::max<LONG>(1,r.bottom);}
        D3D11_TEXTURE2D_DESC td{};td.Width=d.BufferDesc.Width;td.Height=d.BufferDesc.Height;td.MipLevels=td.ArraySize=1;
        td.Format=d.BufferDesc.Format;td.SampleDesc=d.SampleDesc;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> next;auto hr=gameDevice_->CreateTexture2D(&td,nullptr,&next);if(SUCCEEDED(hr))gameBuffer_=std::move(next);return hr;
    }
    HRESULT createOutput(){
        Internal scope;ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> adapter;
        auto hr=gameDevice_.As(&gd);if(FAILED(hr))return hr;if(FAILED(hr=gd->GetAdapter(&adapter)))return hr;
        if(FAILED(hr=realCreate(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&outputDevice_,nullptr,&outputContext_)))return hr;
        // Prioritize only our small display workload, not the game's rendering
        // or the entire process. Keep the normal relative DXGI priority range.
        ComPtr<IDXGIDevice> outputDxgi;
        if(SUCCEEDED(outputDevice_.As(&outputDxgi)))log("Output GPU context priority +7",outputDxgi->SetGPUThreadPriority(7));
        gameContext_.As(&gameFenceContext_);outputContext_.As(&outputFenceContext_);
        ComPtr<ID3D11Device5> game5;
        bool forceQuery=false;
#ifdef VISION_STEREO_TESTING
        wchar_t queryTest[2];forceQuery=GetEnvironmentVariableW(L"VISION_STEREO_TEST_QUERY",queryTest,2)>0;
#endif
        if(!forceQuery&&gameFenceContext_&&SUCCEEDED(gameDevice_.As(&game5))&&SUCCEEDED(game5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&gameCompletion_)))){
            gameCompletionEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!gameCompletionEvent_)return HRESULT_FROM_WIN32(GetLastError());
            log("Game GPU backpressure uses a completion fence");
        }else{
            D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};if(FAILED(hr=gameDevice_->CreateQuery(&query,&gameCompletionQuery_)))return hr;
            gameCompletionTimer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            log("Game GPU backpressure uses a completion query");
        }
        ComPtr<IDXGIFactory2> factory;if(FAILED(hr=adapter->GetParent(IID_PPV_ARGS(&factory))))return hr;
        DXGI_SWAP_CHAIN_DESC1 d{};d.Width=gameDesc_.BufferDesc.Width;d.Height=gameDesc_.BufferDesc.Height;d.Format=outputFormat(gameDesc_.BufferDesc.Format);
        d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=outputBuffers;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;d.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        // Geo11 hooks the factory after the proxy returns its game device.
        // Our private output device must never re-enter that outer hook: it
        // deliberately has no Geo11 HackerDevice wrapper. Use the native
        // trampoline captured while creating the game device instead.
        if(!realChain1)return E_UNEXPECTED;
        ComPtr<IDXGISwapChain1> chain;if(FAILED(hr=realChain1(factory.Get(),outputDevice_.Get(),gameDesc_.OutputWindow,&d,nullptr,nullptr,&chain)))return hr;
        if(FAILED(hr=chain.As(&physical_)))return hr;
        // Match the working app presenter: keep three refreshes queued so a
        // short CPU/GPU scheduling delay does not extend a black slot on screen.
        if(FAILED(hr=physical_->SetMaximumFrameLatency(outputLatency)))return hr;
        latency_=physical_->GetFrameLatencyWaitableObject();if(!latency_)return E_FAIL;
        factory->MakeWindowAssociation(gameDesc_.OutputWindow,DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3DBlob> vs,ps;if(FAILED(hr=compile("vs","vs_5_0",&vs))||FAILED(hr=compile("ps","ps_5_0",&ps)))return hr;
        if(FAILED(hr=outputDevice_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vs_)))return hr;
        if(FAILED(hr=outputDevice_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&ps_)))return hr;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=48;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(hr=outputDevice_->CreateBuffer(&cb,nullptr,&constants_)))return hr;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        return outputDevice_->CreateSamplerState(&sd,&sampler_);
    }
    HRESULT finishGameGpu(bool noWait){
        if(!gameGpuPending_)return S_OK;
        const auto begin=qpc(),deadline=begin+frequency()*5;
        for(;;){
            bool done=false;
            if(gameCompletion_){const auto value=gameCompletion_->GetCompletedValue();if(value==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;done=value>=gameCompletionValue_;}
            // Let the query path flush polling work. DONOTFLUSH can leave the
            // driver reporting an old result for hundreds of milliseconds,
            // even after the initial End/Flush submission completed.
            else{BOOL completed=FALSE;const auto hr=gameContext_->GetData(gameCompletionQuery_.Get(),&completed,sizeof(completed),0);if(FAILED(hr))return hr;done=hr==S_OK&&completed;}
            if(done){
                gameGpuPending_=false;++gpuCompleted_;gpuWaitTicks_+=uint64_t(qpc()-begin);
                // A keyed-mutex handoff can enqueue a GPU dependency even
                // when AcquireSync returns immediately on the CPU. Publish
                // ONLY after GPU completion, so output never waits on a newly
                // submitted game frame and can keep its previous snapshot.
                if(pendingPair_){
                    {std::lock_guard lock(stateMutex_);pendingPair_->serial=++serial_;pendingPair_->state=SlotState::Ready;}
                    pendingPair_=nullptr;pendingPairPool_.reset();
                    if(++received_==1)log("Acquired first GPU-completed full-resolution Geo11 pair");
                }
                return S_OK;
            }
            if(noWait)return DXGI_ERROR_WAS_STILL_DRAWING;
            if(lost_)return DXGI_ERROR_DEVICE_REMOVED;
            if(qpc()>deadline){auto hr=gameDevice_->GetDeviceRemovedReason();return FAILED(hr)?hr:DXGI_ERROR_DEVICE_HUNG;}
            if(gameCompletion_)WaitForSingleObject(gameCompletionEvent_,100);
            else if(gameCompletionTimer_){LARGE_INTEGER due;due.QuadPart=-10000;SetWaitableTimer(gameCompletionTimer_,&due,0,nullptr,nullptr,FALSE);WaitForSingleObject(gameCompletionTimer_,100);}
            else Sleep(1);
        }
    }
    HRESULT markGameGpu(){
        if(gameCompletion_){auto hr=gameFenceContext_->Signal(gameCompletion_.Get(),++gameCompletionValue_);if(FAILED(hr))return hr;
            if(FAILED(hr=gameCompletion_->SetEventOnCompletion(gameCompletionValue_,gameCompletionEvent_)))return hr;}
        else gameContext_->End(gameCompletionQuery_.Get());
        gameContext_->Flush();gameGpuPending_=true;return S_OK;
    }
    HRESULT capturePair(){
#ifdef VISION_STEREO_TESTING
        if(auto delay=testProducerDelay.exchange(0)){
            {std::lock_guard lock(testMutex);++testStats.producerStalls;}
            Sleep(delay);
        }
#endif
        // Called after Geo11 finishes its eye packing, on the producer's own
        // immediate context. This ordering is essential: Katanga has no frame fence.
        if(!katangaView_){
            katangaMap_=OpenFileMappingW(FILE_MAP_READ,FALSE,pairMapping().c_str());if(!katangaMap_)return S_FALSE;
            katangaView_=static_cast<const volatile uint32_t*>(MapViewOfFile(katangaMap_,FILE_MAP_READ,0,0,4));
            if(!katangaView_){CloseHandle(katangaMap_);katangaMap_=nullptr;return E_FAIL;}
        }
        const uint32_t handle=*katangaView_;if(!handle)return S_FALSE;
        if(handle!=katangaHandle_){
            ComPtr<ID3D11Texture2D> texture;auto hr=gameDevice_->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(handle)),IID_PPV_ARGS(&texture));
            if(FAILED(hr)){static bool reported=false;if(!reported){log("Cannot open provider pair on the game device",hr);reported=true;}return hr;}katangaTexture_=std::move(texture);katangaHandle_=handle;
            D3D11_TEXTURE2D_DESC description{};katangaTexture_->GetDesc(&description);char message[256];sprintf_s(message,"Provider texture: %ux%u format=%u array=%u mips=%u samples=%u usage=%u bind=%u misc=%u",description.Width,description.Height,description.Format,description.ArraySize,description.MipLevels,description.SampleDesc.Count,description.Usage,description.BindFlags,description.MiscFlags);log(message);
        }
        D3D11_TEXTURE2D_DESC td{};katangaTexture_->GetDesc(&td);
        // The fix may use its own resolution/upscaling settings. The provider
        // defines two equal-width eyes, independently of the display dimensions.
        if(td.Width<2||(td.Width%2)||!td.Height||td.ArraySize!=1||td.SampleDesc.Count!=1||!supported(td.Format))return E_INVALIDARG;
        std::shared_ptr<PairPool> pool;
        {std::lock_guard lock(stateMutex_);pool=pool_;}
        if(!pool||!sameFormat(pool->desc,td)){
            pool=std::make_shared<PairPool>();pool->desc=td;
            td.Usage=D3D11_USAGE_DEFAULT;td.MipLevels=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.CPUAccessFlags=0;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            for(auto& slot:pool->slots){
                // Allocate private transport resources on our native display
                // device; a stereo renderer may modify game-device allocations.
                bool forceFence=false;
#ifdef VISION_STEREO_TESTING
                wchar_t testFence[2];forceFence=GetEnvironmentVariableW(L"VISION_STEREO_TEST_FENCES",testFence,2)>0;
#endif
                auto hr=forceFence?E_NOTIMPL:outputDevice_->CreateTexture2D(&td,nullptr,&slot.consumer);
                if(FAILED(hr)){
                    // Some older game compatibility paths reject keyed mutex
                    // resources. Use standard shared D3D11 GPU fences instead;
                    // never consume an unsynchronized legacy shared texture.
                    ComPtr<ID3D11Device5> producerDevice,consumerDevice;
                    if(!gameFenceContext_||!outputFenceContext_||FAILED(gameDevice_.As(&producerDevice))||FAILED(outputDevice_.As(&consumerDevice)))return hr;
                    auto plain=td;plain.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
                    if(FAILED(hr=outputDevice_->CreateTexture2D(&plain,nullptr,&slot.consumer)))return hr;
                    if(FAILED(hr=consumerDevice->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&slot.consumerFence))))return hr;
                    HANDLE sharedFence=nullptr;if(FAILED(hr=slot.consumerFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&sharedFence)))return hr;
                    hr=producerDevice->OpenSharedFence(sharedFence,IID_PPV_ARGS(&slot.producerFence));CloseHandle(sharedFence);if(FAILED(hr))return hr;
                }else if(FAILED(hr=slot.consumer.As(&slot.consumerLock)))return hr;
                ComPtr<IDXGIResource> resource;if(FAILED(hr=slot.consumer.As(&resource))||FAILED(hr=resource->GetSharedHandle(&slot.handle)))return hr;
                if(FAILED(hr=gameDevice_->OpenSharedResource(slot.handle,IID_PPV_ARGS(&slot.producer)))){log("Cannot import shared pair into game device",hr);return hr;}
                if(!slot.producerFence&&FAILED(hr=slot.producer.As(&slot.producerLock))){log("Cannot acquire imported pair mutex",hr);return hr;}
            }
            log(pool->slots[0].producerFence?"Pair transport synchronized with shared D3D11 fences":"Pair transport synchronized with DXGI keyed mutexes");
            std::lock_guard lock(stateMutex_);pool_=pool;
        }
        PairSlot* selected=nullptr;uint64_t key=0;
        {std::lock_guard lock(stateMutex_);
            auto reusable=[](const PairSlot& slot){return !slot.producerFence||slot.producerFence->GetCompletedValue()>=slot.fenceValue;};
            for(auto& slot:pool->slots)if(slot.state==SlotState::Free&&reusable(slot)){selected=&slot;break;}
            if(!selected)for(auto& slot:pool->slots)if(slot.state==SlotState::Ready&&reusable(slot)&&(!selected||slot.serial<selected->serial))selected=&slot;
            if(!selected)return S_FALSE;key=selected->state==SlotState::Ready?1:0;selected->state=SlotState::Writing;
        }
        const auto hr=selected->producerFence?(selected->producerFence->GetCompletedValue()>=selected->fenceValue?S_OK:HRESULT(WAIT_TIMEOUT)):selected->producerLock->AcquireSync(key,0);
        if(hr!=S_OK){std::lock_guard lock(stateMutex_);selected->state=key?SlotState::Ready:SlotState::Free;return hr;}
        if(selected->producerFence)gameFenceContext_->Wait(selected->producerFence.Get(),selected->fenceValue);
        gameContext_->CopyResource(selected->producer.Get(),katangaTexture_.Get());
        HRESULT released;
        if(selected->producerFence){if(!(selected->fenceValue&1))++selected->fenceValue;else selected->fenceValue+=2;released=gameFenceContext_->Signal(selected->producerFence.Get(),selected->fenceValue);gameContext_->Flush();}
        else released=selected->producerLock->ReleaseSync(1);
        if(SUCCEEDED(released)){pendingPairPool_=pool;pendingPair_=selected;}
        else{std::lock_guard lock(stateMutex_);selected->state=SlotState::Free;}
        return released;
    }
    bool acceptPair(){
        std::shared_ptr<PairPool> pool;PairSlot* slot=nullptr;
        {std::lock_guard lock(stateMutex_);pool=pool_;if(!pool)return false;
            for(auto& s:pool->slots)if(s.state==SlotState::Ready&&(!slot||s.serial>slot->serial))slot=&s;
            if(!slot)return false;slot->state=SlotState::Reading;
        }
        HRESULT hr=S_OK;
        if(!slot->consumer){hr=outputDevice_->OpenSharedResource(slot->handle,IID_PPV_ARGS(&slot->consumer));if(SUCCEEDED(hr))hr=slot->consumer.As(&slot->consumerLock);}
        bool copied=false;
        const auto acquired=slot->consumerFence?(slot->consumerFence->GetCompletedValue()>=slot->fenceValue?S_OK:HRESULT(WAIT_TIMEOUT)):slot->consumerLock->AcquireSync(1,0);
        if(SUCCEEDED(hr)&&acquired==S_OK){
            if(slot->consumerFence)outputFenceContext_->Wait(slot->consumerFence.Get(),slot->fenceValue);
            if(!snapshot_||!sameFormat(snapshotDesc_,pool->desc)){
                auto td=pool->desc;td.Usage=D3D11_USAGE_DEFAULT;td.MiscFlags=0;td.CPUAccessFlags=0;td.MipLevels=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                snapshotView_.Reset();snapshot_.Reset();hr=outputDevice_->CreateTexture2D(&td,nullptr,&snapshot_);
                if(SUCCEEDED(hr))hr=outputDevice_->CreateShaderResourceView(snapshot_.Get(),nullptr,&snapshotView_);
                if(SUCCEEDED(hr))snapshotDesc_=td;
            }
            if(SUCCEEDED(hr)){outputContext_->CopyResource(snapshot_.Get(),slot->consumer.Get());copied=true;}
            if(slot->consumerFence){++slot->fenceValue;outputFenceContext_->Signal(slot->consumerFence.Get(),slot->fenceValue);outputContext_->Flush();}else slot->consumerLock->ReleaseSync(0);
            std::lock_guard lock(stateMutex_);slot->state=SlotState::Free;
        }else{std::lock_guard lock(stateMutex_);slot->state=SlotState::Ready;}
        return copied;
    }
    HRESULT draw(int eye){
        if(!outputTarget_){ComPtr<ID3D11Texture2D> back;auto hr=physical_->GetBuffer(0,IID_PPV_ARGS(&back));if(FAILED(hr))return hr;
            if(FAILED(hr=outputDevice_->CreateRenderTargetView(back.Get(),nullptr,&outputTarget_)))return hr;}
        const float black[4]={0,0,0,1};outputContext_->ClearRenderTargetView(outputTarget_.Get(),black);
        if(eye!=2&&snapshotView_){
            DXGI_SWAP_CHAIN_DESC1 d{};physical_->GetDesc1(&d);D3D11_VIEWPORT vp{0,0,float(d.Width),float(d.Height),0,1};outputContext_->RSSetViewports(1,&vp);
            auto* rt=outputTarget_.Get();outputContext_->OMSetRenderTargets(1,&rt,nullptr);
            float values[12]={float(eye)*.5f,0,.5f,1,0,.5f/float(snapshotDesc_.Width),.5f/float(snapshotDesc_.Height),0,1,.5f,0,0};
            outputContext_->UpdateSubresource(constants_.Get(),0,nullptr,values,0,0);
            auto* cb=constants_.Get();outputContext_->PSSetConstantBuffers(0,1,&cb);auto* srv=snapshotView_.Get();outputContext_->PSSetShaderResources(0,1,&srv);
            auto* sampler=sampler_.Get();outputContext_->PSSetSamplers(0,1,&sampler);
            outputContext_->VSSetShader(vs_.Get(),nullptr,0);outputContext_->PSSetShader(ps_.Get(),nullptr,0);
            outputContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);outputContext_->Draw(3,0);
        }
        outputContext_->ClearState();
#ifdef VISION_STEREO_TESTING
        // Read the physical backbuffer, after our draw and before Present.
        // Deliberately unavailable in the production output loop.
        ComPtr<ID3D11Texture2D> back;auto hr=physical_->GetBuffer(0,IID_PPV_ARGS(&back));if(FAILED(hr))return hr;
        D3D11_TEXTURE2D_DESC td{};back->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.MiscFlags=0;
        ComPtr<ID3D11Texture2D> readback;
        if(SUCCEEDED(outputDevice_->CreateTexture2D(&td,nullptr,&readback))){
            outputContext_->CopyResource(readback.Get(),back.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            if(SUCCEEDED(outputContext_->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped))){
                auto* p=static_cast<uint8_t*>(mapped.pData)+(td.Height/2)*mapped.RowPitch+(td.Width/2)*4;
                uint32_t pixel=0;memcpy(&pixel,p,4);outputContext_->Unmap(readback.Get(),0);
                std::lock_guard lock(testMutex);++testStats.samples[eye];testStats.pixel[eye]=pixel;testStats.width=td.Width;testStats.height=td.Height;
            }
        }
#endif
        return S_OK;
    }
    void output(std::stop_token stop){
        DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Games",&task);
        if(mmcss)AvSetMmThreadPriority(mmcss,AVRT_PRIORITY_HIGH);
        else SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
        Internal scope;Host host;uint32_t ringHead=0;std::array<sync::FrameRecord,sync::ringSize> ring{};uint64_t presents=0,errors=0;
        unsigned slot=0;int sequence=0;bool previouslyEnabled=false;DXGI_COLOR_SPACE_TYPE space=DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        uint64_t reportAt=GetTickCount64(),reportedPresents=0,reportedPairs=0,reportedCompleted=0,missedRefreshes=0,corrections=0,producerBusy=0;UINT previousRefresh=0,previousCount=0;
        DXGI_SWAP_CHAIN_DESC gameDesc;{std::lock_guard lock(producerMutex_);gameDesc=gameDesc_;}
        int64_t lastPresent=0,maxPresentGap=0;
        while(!stop.stop_requested()){
#ifdef VISION_STEREO_TESTING
            if(auto delay=testOutputDelay.exchange(0)){
                {std::lock_guard lock(testMutex);++testStats.outputStalls;testStats.alignedRun=0;}
                Sleep(delay);
            }
#endif
            const auto wait=WaitForSingleObject(latency_,20);if(stop.stop_requested())break;
            if(wait==WAIT_TIMEOUT)continue;if(wait!=WAIT_OBJECT_0){lost_=true;log("Output latency wait failed",HRESULT_FROM_WIN32(GetLastError()));break;}
            // Capture/resize may wait inside the game or driver. Output must
            // keep displaying its completed pair instead of sharing that wait.
            bool resize=false;{std::unique_lock lock(producerMutex_,std::try_to_lock);
                if(lock.owns_lock()){gameDesc=gameDesc_;resize=resized_.exchange(false);}else ++producerBusy;}
            if(resize){
                outputContext_->ClearState();outputTarget_.Reset();snapshotView_.Reset();snapshot_.Reset();slot=0;
                auto hr=physical_->ResizeBuffers(outputBuffers,gameDesc.BufferDesc.Width,gameDesc.BufferDesc.Height,outputFormat(gameDesc.BufferDesc.Format),DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
                if(FAILED(hr)){log("Physical ResizeBuffers failed",hr);lost_=true;break;}
            }
            if(space!=colorSpace_.load()){space=colorSpace_.load();physical_->SetColorSpace1(space);}
            const bool connected=host.connect();auto* sh=host.shared;
            const bool owns=connected&&(!sh->gamePid||sh->gamePid==GetCurrentProcessId()||qpc()-sh->gameHeartbeatQpc>frequency()*3);
            DWORD foregroundPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
            bool focused=foregroundPid==GetCurrentProcessId();
#ifdef VISION_STEREO_TESTING
            // The private-channel pixel harness never activates its test HWND.
            focused=true;
#endif
            const bool enabled=owns&&sh->enabled&&sync::sequenceKnown(sh->sequence)&&focused;
            if(enabled!=previouslyEnabled||(enabled&&sequence!=sh->sequence)){sequence=enabled?sh->sequence:0;slot=0;previouslyEnabled=enabled;}
            const unsigned cycle=sync::slotsPerFrame(sequence);
            // Select the eye BEFORE submission, from the most recent scanout
            // clock and the presents still queued. After a stall, an empty
            // queue starts at the next physical refresh, not the successor of
            // a frame displayed several refreshes ago. Never change eye phase.
            DXGI_FRAME_STATISTICS before{};UINT submitted=0;
            if(enabled&&SUCCEEDED(physical_->GetFrameStatistics(&before))&&SUCCEEDED(physical_->GetLastPresentCount(&submitted))&&before.PresentCount){
                const unsigned scheduled=unsigned(sync::nextDisplayRefresh(submitted,before.PresentCount,before.PresentRefreshCount,before.SyncRefreshCount)%cycle);
                if(slot!=scheduled)++corrections;slot=scheduled;
            }
            if(slot==0||!enabled)acceptPair();
            auto hr=draw(enabled?sync::slotEye(sequence,slot):0);if(FAILED(hr)){log("Output draw failed",hr);lost_=true;break;}
            hr=physical_->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)continue;
            const bool visible=hr==S_OK&&IsWindowVisible(gameDesc.OutputWindow)&&!IsIconic(gameDesc.OutputWindow);
            if(FAILED(hr)){++errors;log("Physical Present failed",hr);lost_=true;break;}
            UINT id=0;physical_->GetLastPresentCount(&id);DXGI_FRAME_STATISTICS stats{};const bool valid=SUCCEEDED(physical_->GetFrameStatistics(&stats));
#ifdef VISION_STEREO_TESTING
            if(presents%240==0){char detail[256];sprintf_s(detail,"Phase stats: last=%u valid=%d shown=%u refresh=%u clock=%u selected=%u",id,int(valid),stats.PresentCount,stats.PresentRefreshCount,stats.SyncRefreshCount,slot);log(detail);}
#endif
            if(visible){ring[ringHead%sync::ringSize]={id,int32_t(slot)};++ringHead;++presents;}
            const auto presentedAt=qpc();if(lastPresent&&visible)maxPresentGap=std::max(maxPresentGap,presentedAt-lastPresent);lastPresent=presentedAt;
            // A game Present is a producer boundary, not permission to run
            // without a limit. Admit one new pair per complete display cycle.
            // Otherwise removing the game's vsync lets it saturate the GPU
            // and starve the independent output thread of refresh deadlines.
            if((visible&&slot==0)||!visible)SetEvent(producerPermit_);
#ifdef VISION_STEREO_TESTING
            {std::lock_guard lock(testMutex);testStats.pairs=received_;testStats.presents=presents;testStats.presentResult=hr;testStats.windowVisible=IsWindowVisible(gameDesc.OutputWindow);testStats.windowIconic=IsIconic(gameDesc.OutputWindow);}
#endif
            // Statistics describe what reached scanout. Keep them diagnostic;
            // corrections happen before the NEXT draw, against the same fixed
            // refresh rule the host uses for the glasses.
            if(valid&&stats.PresentCount!=previousCount){
                if(previousCount&&stats.PresentCount>previousCount&&stats.PresentRefreshCount>previousRefresh){
                    auto elapsed=stats.PresentRefreshCount-previousRefresh,displayed=stats.PresentCount-previousCount;
                    if(elapsed>displayed)missedRefreshes+=elapsed-displayed;
                }previousCount=stats.PresentCount;previousRefresh=stats.PresentRefreshCount;
            }
#ifdef VISION_STEREO_TESTING
            if(enabled&&valid&&stats.PresentCount){
                int shown=-1;for(uint32_t k=0;k<std::min(ringHead,sync::ringSize);++k){auto r=ring[(ringHead-1-k)%sync::ringSize];if(r.presentId==stats.PresentCount){shown=r.eye;break;}}
                std::lock_guard lock(testMutex);
                if(shown>=0&&testStats.observedPresent!=stats.PresentCount){testStats.observedPresent=stats.PresentCount;
                    if(unsigned(shown)==stats.PresentRefreshCount%cycle)++testStats.alignedRun;else{
                        ++testStats.misaligned;testStats.alignedRun=0;
                        if(testStats.misaligned<=8){char detail[256];sprintf_s(detail,"Phase mismatch: shown=%u refresh=%u clock=%u slot=%d; before shown=%u refresh=%u clock=%u submitted=%u selected=%u",stats.PresentCount,stats.PresentRefreshCount,stats.SyncRefreshCount,shown,before.PresentCount,before.PresentRefreshCount,before.SyncRefreshCount,submitted,slot);log(detail);}
                    }}
            }
#endif
            if(owns){
                InterlockedIncrement(&sh->seq);MemoryBarrier();sh->gamePid=GetCurrentProcessId();sh->gameHeartbeatQpc=qpc();sh->hwnd=reinterpret_cast<uintptr_t>(gameDesc.OutputWindow);
                sh->api=sync::ApiD3D11;sh->width=gameDesc.BufferDesc.Width;sh->height=gameDesc.BufferDesc.Height;sh->format=gameDesc.BufferDesc.Format;sh->backBuffers=outputBuffers;
                sh->active=enabled&&visible&&snapshot_?1:0;sh->frames=received_;sh->nativePresents=presents;sh->presentErrors=errors;
                sh->statsValid=valid?1:0;sh->presentCount=stats.PresentCount;sh->presentRefreshCount=stats.PresentRefreshCount;sh->syncRefreshCount=stats.SyncRefreshCount;sh->syncQpc=stats.SyncQPCTime.QuadPart;
                sh->hookFlags=sync::HookSequences|sync::HookPatterns|sync::HookPhaseLocked|sync::HookRefreshSlots;sh->ringHead=ringHead;std::copy(ring.begin(),ring.end(),sh->ring);
                sh->composed=1;ComPtr<IDXGISwapChainMedia> media;if(SUCCEEDED(physical_.As(&media))){DXGI_FRAME_STATISTICS_MEDIA ms{};if(SUCCEEDED(media->GetFrameStatisticsMedia(&ms)))sh->composed=ms.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSED||ms.CompositionMode==DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE;}
                wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);auto name=std::filesystem::path(path).filename().string();strncpy_s(sh->gameName,name.c_str(),_TRUNCATE);
                strncpy_s(sh->message,!snapshot_?"Geo11: waiting for complete eyes":!visible?"Geo11 presentation occluded or minimized":!focused?"Geo11 mono: game does not have foreground input":enabled?"Geo11 full-resolution eyes; in-game sequential output":"Geo11 left-eye output; host stereo inactive",_TRUNCATE);
                MemoryBarrier();InterlockedIncrement(&sh->seq);if(host.event)SetEvent(host.event);
            }
            if(visible)slot=(slot+1)%cycle;else{slot=0;Sleep(20);}
            auto now=GetTickCount64();if(now-reportAt>=2000){
                char message[512];const double seconds=double(now-reportAt)/1000;const auto completed=gpuCompleted_.load();
                sprintf_s(message,"Output: %.1f presents/s, %.1f pairs/s, GPU-completed=%.1f/s, GPU-wait=%.1fms, missed-refresh estimate=%llu, corrections=%llu, max-gap=%.2fms, producer-busy=%llu, stereo=%d, focused=%d, stats=%d",double(presents-reportedPresents)/seconds,double(received_-reportedPairs)/seconds,double(completed-reportedCompleted)/seconds,1000.*double(gpuWaitTicks_.exchange(0))/frequency(),missedRefreshes,corrections,1000.*double(maxPresentGap)/frequency(),producerBusy,int(enabled),int(focused),int(valid));log(message);
                reportAt=now;reportedPresents=presents;reportedPairs=received_;reportedCompleted=completed;missedRefreshes=corrections=producerBusy=0;maxPresentGap=0;
            }
        }
        if(host.connect()&&host.shared->gamePid==GetCurrentProcessId()){InterlockedIncrement(&host.shared->seq);host.shared->active=0;MemoryBarrier();InterlockedIncrement(&host.shared->seq);if(host.event)SetEvent(host.event);}
        if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
    }
public:
    StereoChain(ID3D11Device* device,IDXGIFactory* factory,const DXGI_SWAP_CHAIN_DESC& d):gameDevice_(device),factory_(factory),gameDesc_(d){device->GetImmediateContext(&gameContext_);}
    HRESULT initialize(){producerPermit_=CreateEventW(nullptr,FALSE,TRUE,nullptr);if(!producerPermit_)return HRESULT_FROM_WIN32(GetLastError());auto hr=createLogicalBuffer();if(SUCCEEDED(hr))hr=createOutput();if(SUCCEEDED(hr)){worker_=std::jthread([this](std::stop_token stop){output(stop);});log("Created independent in-game Geo11 output");}return hr;}
    ~StereoChain(){log("Releasing logical swapchain");if(worker_.joinable()){worker_.request_stop();worker_.join();}log("Output worker stopped");if(katangaView_)UnmapViewOfFile(const_cast<uint32_t*>(katangaView_));if(katangaMap_)CloseHandle(katangaMap_);if(latency_)CloseHandle(latency_);if(producerPermit_)CloseHandle(producerPermit_);if(gameCompletionEvent_)CloseHandle(gameCompletionEvent_);if(gameCompletionTimer_)CloseHandle(gameCompletionTimer_);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** p)override{if(!p)return E_POINTER;*p=nullptr;if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIDeviceSubObject)||iid==__uuidof(IDXGISwapChain)||iid==__uuidof(IDXGISwapChain1)||iid==__uuidof(IDXGISwapChain2)||iid==__uuidof(IDXGISwapChain3)||iid==__uuidof(IDXGISwapChain4)){*p=static_cast<IDXGISwapChain4*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID k,UINT n,const void* d)override{return physical_->SetPrivateData(k,n,d);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID k,const IUnknown* d)override{return physical_->SetPrivateDataInterface(k,d);}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID k,UINT* n,void* d)override{return physical_->GetPrivateData(k,n,d);}
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** p)override{return factory_->QueryInterface(iid,p);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** p)override{return gameDevice_->QueryInterface(iid,p);}
    HRESULT STDMETHODCALLTYPE Present(UINT,UINT flags)override{
        if(flags&DXGI_PRESENT_TEST)return IsIconic(gameDesc_.OutputWindow)?DXGI_STATUS_OCCLUDED:S_OK;
        if(lost_)return DXGI_ERROR_DEVICE_REMOVED;
        std::lock_guard lock(producerMutex_);
        const bool noWait=(flags&DXGI_PRESENT_DO_NOT_WAIT)!=0;
        auto hr=finishGameGpu(noWait);if(FAILED(hr))return hr;
        auto wait=WaitForSingleObject(producerPermit_,flags&DXGI_PRESENT_DO_NOT_WAIT?0:100);
        if(wait!=WAIT_OBJECT_0)return flags&DXGI_PRESENT_DO_NOT_WAIT?DXGI_ERROR_WAS_STILL_DRAWING:S_OK;
        hr=capturePair();
        static std::atomic<int> warnings{0};if(FAILED(hr)&&warnings++<8)log("Geo11 pair acquisition failed",hr);
        // Logical Present removed DXGI's implicit GPU backpressure. A CPU timer
        // alone cannot replace it: GPU-heavy frames can otherwise accumulate
        // even when capture drops a pair. Bound all game work, not just copies.
        if(FAILED(hr=markGameGpu()))return hr;
        if(!noWait&&FAILED(hr=finishGameGpu(false))){lost_=true;log("Game GPU completion failed",hr);return hr;}
        ++logicalPresents_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i,REFIID iid,void** p)override{if(i)return DXGI_ERROR_INVALID_CALL;std::lock_guard lock(producerMutex_);return gameBuffer_->QueryInterface(iid,p);}
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen,IDXGIOutput*)override{std::lock_guard lock(producerMutex_);gameDesc_.Windowed=!fullscreen;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* b,IDXGIOutput** o)override{if(b)*b=!gameDesc_.Windowed;if(o)return physical_->GetContainingOutput(o);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d)override{if(!d)return E_POINTER;std::lock_guard lock(producerMutex_);*d=gameDesc_;return S_OK;}
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT w,UINT h,DXGI_FORMAT f,UINT flags)override{
        std::lock_guard lock(producerMutex_);auto old=gameDesc_;gameDesc_.BufferDesc.Width=w;gameDesc_.BufferDesc.Height=h;if(count)gameDesc_.BufferCount=count;if(f!=DXGI_FORMAT_UNKNOWN)gameDesc_.BufferDesc.Format=f;gameDesc_.Flags=flags;
        auto hr=createLogicalBuffer();if(FAILED(hr)){gameDesc_=old;return hr;}katangaTexture_.Reset();katangaHandle_=0;{std::lock_guard state(stateMutex_);pool_.reset();}resized_=true;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* d)override{if(!d)return E_POINTER;std::lock_guard lock(producerMutex_);gameDesc_.BufferDesc.RefreshRate=d->RefreshRate;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o)override{return physical_->GetContainingOutput(o);}
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s)override{if(!s)return E_POINTER;*s={};return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;}
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n)override{if(!n)return E_POINTER;*n=logicalPresents_;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d)override{if(!d)return E_POINTER;std::lock_guard lock(producerMutex_);*d={};d->Width=gameDesc_.BufferDesc.Width;d->Height=gameDesc_.BufferDesc.Height;d->Format=gameDesc_.BufferDesc.Format;d->SampleDesc=gameDesc_.SampleDesc;d->BufferUsage=gameDesc_.BufferUsage;d->BufferCount=gameDesc_.BufferCount;d->SwapEffect=gameDesc_.SwapEffect;d->Flags=gameDesc_.Flags;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d)override{if(!d)return E_POINTER;*d={};d->Windowed=gameDesc_.Windowed;d->RefreshRate=gameDesc_.BufferDesc.RefreshRate;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* w)override{if(!w)return E_POINTER;*w=gameDesc_.OutputWindow;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID,void** p)override{if(p)*p=nullptr;return E_NOINTERFACE;}
    HRESULT STDMETHODCALLTYPE Present1(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* p)override{if(p&&(p->DirtyRectsCount||p->pScrollRect))return DXGI_ERROR_INVALID_CALL;return Present(interval,flags);}
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported()override{return FALSE;}
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** p)override{if(!p)return E_POINTER;*p=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c)override{return physical_->SetBackgroundColor(c);}
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c)override{return physical_->GetBackgroundColor(c);}
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r)override{return r==DXGI_MODE_ROTATION_IDENTITY?S_OK:DXGI_ERROR_UNSUPPORTED;}
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r)override{if(!r)return E_POINTER;*r=DXGI_MODE_ROTATION_IDENTITY;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT,UINT)override{return DXGI_ERROR_UNSUPPORTED;}
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w,UINT* h)override{if(!w||!h)return E_POINTER;*w=gameDesc_.BufferDesc.Width;*h=gameDesc_.BufferDesc.Height;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n)override{if(!n)return E_POINTER;*n=1;return S_OK;}
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject()override{return nullptr;}
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F*)override{return DXGI_ERROR_UNSUPPORTED;}
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m)override{if(!m)return E_POINTER;*m={1,0,0,1,0,0};return S_OK;}
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex()override{return 0;}
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE s,UINT* f)override{return physical_->CheckColorSpaceSupport(s,f);}
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE s)override{colorSpace_=s;return S_OK;}
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT*,IUnknown*const*)override{return ResizeBuffers(n,w,h,f,flags);}
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE,UINT,void*)override{return DXGI_ERROR_UNSUPPORTED;}
};
bool allowed(IUnknown* input,const DXGI_SWAP_CHAIN_DESC& d,ComPtr<ID3D11Device>& device){
    if(internal||!d.OutputWindow||d.SampleDesc.Count!=1||!supported(d.BufferDesc.Format))return false;
    if(FAILED(input->QueryInterface(IID_PPV_ARGS(&device))))return false;
    ID3D11Device* native=nullptr;UINT bytes=sizeof(native);
    if(SUCCEEDED(device->GetPrivateData(nativeDeviceTag,&bytes,&native))&&bytes==sizeof(native)&&native)device=native;
    // The module is installed only through an explicit Geo11 output profile.
    // Host absent means mono presentation, not a second display or window.
    wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);auto ini=std::filesystem::path(path).parent_path()/L"d3dxdm.ini";
    wchar_t mode[64];GetPrivateProfileStringW(L"Device",L"direct_mode",L"",mode,64,ini.c_str());return _wcsicmp(mode,L"katanga_vr")==0;
}
HRESULT STDMETHODCALLTYPE createChain(IDXGIFactory* f,IUnknown* d,DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out){
    if(!desc||!out)return realChain(f,d,desc,out);ComPtr<ID3D11Device> device;if(!allowed(d,*desc,device))return realChain(f,d,desc,out);
    auto* chain=new StereoChain(device.Get(),f,*desc);auto hr=chain->initialize();if(FAILED(hr)){log("Stereo output creation failed",hr);chain->Release();*out=nullptr;return hr;}*out=chain;return S_OK;
}
HRESULT STDMETHODCALLTYPE createChain1(IDXGIFactory2* f,IUnknown* d,HWND w,const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGIOutput* restrict,IDXGISwapChain1** out){
    if(!desc||!out||desc->Stereo||restrict)return realChain1(f,d,w,desc,full,restrict,out);
    DXGI_SWAP_CHAIN_DESC old{};old.BufferDesc.Width=desc->Width;old.BufferDesc.Height=desc->Height;old.BufferDesc.Format=desc->Format;old.SampleDesc=desc->SampleDesc;old.BufferUsage=desc->BufferUsage;old.BufferCount=desc->BufferCount;old.OutputWindow=w;old.Windowed=full?full->Windowed:TRUE;old.SwapEffect=desc->SwapEffect;old.Flags=desc->Flags;if(full)old.BufferDesc.RefreshRate=full->RefreshRate;
    ComPtr<ID3D11Device> device;if(!allowed(d,old,device))return realChain1(f,d,w,desc,full,restrict,out);
    auto* chain=new StereoChain(device.Get(),f,old);auto hr=chain->initialize();if(FAILED(hr)){log("Stereo output creation failed",hr);chain->Release();*out=nullptr;return hr;}*out=chain;return S_OK;
}
void attachFactory(ID3D11Device* device){
    if(internal||!device)return;std::lock_guard lock(hooksMutex);if(realChain&&realChain1)return;
    ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&gd)))||FAILED(gd->GetAdapter(&adapter))||FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))return;
    auto** vt=*reinterpret_cast<void***>(factory.Get());
    if(!realChain){if(MH_CreateHook(vt[10],reinterpret_cast<void*>(createChain),reinterpret_cast<void**>(&realChain))==MH_OK)MH_EnableHook(vt[10]);else log("Cannot hook factory CreateSwapChain");}
    if(!realChain1){if(MH_CreateHook(vt[15],reinterpret_cast<void*>(createChain1),reinterpret_cast<void**>(&realChain1))==MH_OK)MH_EnableHook(vt[15]);else log("Cannot hook factory CreateSwapChainForHwnd");}
}
void initialize(){
    std::call_once(initOnce,[]{
        wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);auto path=std::filesystem::path(system)/L"d3d11.dll";
        HMODULE native=GetModuleHandleW(path.c_str());if(!native)native=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!native){log("System D3D11 unavailable",HRESULT_FROM_WIN32(GetLastError()));return;}
        wchar_t actual[MAX_PATH];GetModuleFileNameW(native,actual,MAX_PATH);if(_wcsicmp(path.c_str(),actual)!=0){log("Refusing redirected system D3D11 module");return;}
        realCreate=reinterpret_cast<CreateDevice>(GetProcAddress(native,"D3D11CreateDevice"));
        auto status=MH_Initialize();ready=realCreate&&(status==MH_OK||status==MH_ERROR_ALREADY_INITIALIZED);
        if(ready){
            // Geo11 exposes a session-wide handle, with no producer PID. Keep
            // that existing interface private to this game when we own output.
            auto kernel=GetModuleHandleW(L"kernel32.dll");auto create=GetProcAddress(kernel,"CreateFileMappingW"),open=GetProcAddress(kernel,"OpenFileMappingW");
            ready=MH_CreateHook(reinterpret_cast<void*>(create),reinterpret_cast<void*>(createMapping),reinterpret_cast<void**>(&realMapping))==MH_OK&&MH_CreateHook(reinterpret_cast<void*>(open),reinterpret_cast<void*>(openMapping),reinterpret_cast<void**>(&realOpenMapping))==MH_OK;
            if(ready)ready=MH_EnableHook(reinterpret_cast<void*>(create))==MH_OK&&MH_EnableHook(reinterpret_cast<void*>(open))==MH_OK;
        }
        log(ready?"VisionStereo11 initialized":"VisionStereo11 initialization failed");
    });
}
}
#ifdef VISION_STEREO_TESTING
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:VisionOutputReadTestStats=_VisionOutputReadTestStats@4")
#pragma comment(linker, "/EXPORT:VisionOutputDelayTestProducer=_VisionOutputDelayTestProducer@4")
#pragma comment(linker, "/EXPORT:VisionOutputDelayTestDisplay=_VisionOutputDelayTestDisplay@4")
#endif
extern "C" __declspec(dllexport) void WINAPI VisionOutputReadTestStats(VisionOutputTestStats* stats){if(stats){std::lock_guard lock(testMutex);*stats=testStats;}}
extern "C" __declspec(dllexport) void WINAPI VisionOutputDelayTestProducer(UINT milliseconds){testProducerDelay=std::min(milliseconds,1000u);}
extern "C" __declspec(dllexport) void WINAPI VisionOutputDelayTestDisplay(UINT milliseconds){testOutputDelay=std::min(milliseconds,1000u);}
#endif
extern "C" HRESULT WINAPI VisionStereoBootstrap(){
    initialize();if(!ready)return E_FAIL;
    ComPtr<ID3D11Device> device;
    auto hr=realCreate(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr);
    if(SUCCEEDED(hr))attachFactory(device.Get());
    return SUCCEEDED(hr)&&realChain&&realChain1?S_OK:E_FAIL;
}
extern "C" HRESULT WINAPI VisionCreateDevice(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE sw,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context){
    initialize();if(!ready)return E_FAIL;
    // The game context remains on its original thread. Resource references can
    // also retire on our output thread, so retain the default thread-safe device.
    flags&=~D3D11_CREATE_DEVICE_SINGLETHREADED;
    auto hr=realCreate(adapter,type,sw,flags,levels,count,sdk,device,level,context);
    if(SUCCEEDED(hr)&&device&&*device){(*device)->SetPrivateData(nativeDeviceTag,sizeof(*device),device);attachFactory(*device);}return hr;
}
extern "C" HRESULT WINAPI VisionCreateDeviceAndSwapChain(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE sw,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** chain,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context){
    ComPtr<ID3D11Device> created;ComPtr<ID3D11DeviceContext> ctx;auto hr=VisionCreateDevice(adapter,type,sw,flags,levels,count,sdk,&created,level,&ctx);if(FAILED(hr))return hr;
    if(chain){*chain=nullptr;if(!desc)return E_INVALIDARG;ComPtr<IDXGIDevice> gd;ComPtr<IDXGIAdapter> ad;ComPtr<IDXGIFactory> f;
        if(FAILED(hr=created.As(&gd))||FAILED(hr=gd->GetAdapter(&ad))||FAILED(hr=ad->GetParent(IID_PPV_ARGS(&f))))return hr;auto copy=*desc;if(FAILED(hr=f->CreateSwapChain(created.Get(),&copy,chain)))return hr;
    }
    if(device)*device=created.Detach();if(context)*context=ctx.Detach();return S_OK;
}
