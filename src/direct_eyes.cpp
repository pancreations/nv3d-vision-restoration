#include "direct_eyes.h"
#include "gpu_completion.h"
#include <algorithm>
#include <mutex>
#include <string>

namespace vision::direct {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t magic=0x45595256,version=1;
enum State:LONG { Initializing,Idle,Filling,Ready,Reading,Closed };
struct alignas(8) Shared {
    uint32_t magic=0,version=0,size=0,reserved=0;
    volatile LONG state=Initializing,reader=0;
    uint64_t handle=0,pairId=0;
    uint32_t encoding=0,reserved2=0;
};
static_assert(sizeof(Shared)==48);
std::wstring name(uint32_t id){return L"Local\\VisionDirectEyes.v1."+std::to_wstring(id);}
struct Mapping {
    HANDLE handle=nullptr;Shared* view=nullptr;
    ~Mapping(){if(view)UnmapViewOfFile(view);if(handle)CloseHandle(handle);}
};
bool supported(DXGI_FORMAT f){return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM||f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R10G10B10A2_UNORM;}
LONG state(Shared* s){return InterlockedCompareExchange(&s->state,0,0);}
}
bool available(uint32_t channel){
    if(!channel)return false;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name(channel).c_str());if(!mapping)return false;
    const auto* view=static_cast<const Shared*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(Shared)));
    const bool ready=view&&view->magic==magic&&view->version==version&&view->size==sizeof(Shared)&&view->state!=Initializing&&view->state!=Closed;
    if(view)UnmapViewOfFile(view);CloseHandle(mapping);return ready;
}
struct Producer::Impl {
    Mapping map;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;ComPtr<IDXGIKeyedMutex> key;ComPtr<ID3D11Query> done;
    D3D11_TEXTURE2D_DESC desc{};uint64_t pendingId=0,lastId=0;unsigned eyes=0;
    Encoding encoding{};std::mutex mutex;
    ~Impl(){if(map.view)InterlockedExchange(&map.view->state,Closed);}
};
Producer::Producer()=default;Producer::~Producer()=default;
HRESULT Producer::open(ID3D11Device* device,uint32_t channel){
    if(!device||!channel||impl_)return E_INVALIDARG;
    auto p=std::make_unique<Impl>();p->device=device;device->GetImmediateContext(&p->context);
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};HRESULT hr=device->CreateQuery(&q,&p->done);if(FAILED(hr))return hr;
    p->map.handle=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Shared),name(channel).c_str());
    if(!p->map.handle)return HRESULT_FROM_WIN32(GetLastError());
    if(GetLastError()==ERROR_ALREADY_EXISTS)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    p->map.view=static_cast<Shared*>(MapViewOfFile(p->map.handle,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));
    if(!p->map.view)return HRESULT_FROM_WIN32(GetLastError());
    auto* s=p->map.view;s->magic=magic;s->version=version;s->size=sizeof(Shared);
    InterlockedExchange(&s->state,Idle);impl_=std::move(p);return S_OK;
}
bool Producer::connected()const{return impl_&&InterlockedCompareExchange(&impl_->map.view->reader,0,0)!=0&&state(impl_->map.view)!=Closed;}
HRESULT Producer::submit(ID3D11Texture2D* input,UINT subresource,Eye eye,uint64_t id,Encoding encoding,std::stop_token stop,const D3D11_BOX* region,bool deferPublication){
    if(!impl_||!input||uint32_t(eye)>1||uint32_t(encoding)>2||!id)return E_INVALIDARG;
    auto& p=*impl_;std::lock_guard lock(p.mutex);auto* s=p.map.view;
    if(stop.stop_requested())return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const auto current=state(s);
    if(current==Ready||current==Reading)return DXGI_ERROR_WAS_STILL_DRAWING;
    if(current!=Idle&&current!=Filling)return E_UNEXPECTED;
    if(id<=p.lastId||(p.eyes&&(p.pendingId!=id||p.encoding!=encoding||(p.eyes&(1u<<uint32_t(eye))))))return E_INVALIDARG;
    ComPtr<ID3D11Device> owner;input->GetDevice(&owner);if(owner.Get()!=p.device.Get())return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC in{};input->GetDesc(&in);
    if(!in.MipLevels||subresource>=in.MipLevels*in.ArraySize||in.SampleDesc.Count!=1||!supported(in.Format))return E_INVALIDARG;
    const UINT mip=subresource%in.MipLevels;
    in.Width=std::max(1u,in.Width>>mip);in.Height=std::max(1u,in.Height>>mip);
    if(region){if(region->left>=region->right||region->top>=region->bottom||region->right>in.Width||region->bottom>in.Height||region->front!=0||region->back!=1)return E_INVALIDARG;in.Width=region->right-region->left;in.Height=region->bottom-region->top;}
    const bool changed=!p.texture||in.Width!=p.desc.Width||in.Height!=p.desc.Height||in.Format!=p.desc.Format;
    if(changed&&p.eyes)return E_INVALIDARG;
    HRESULT hr;
    if(changed){
        D3D11_TEXTURE2D_DESC d{};d.Width=in.Width;d.Height=in.Height;d.Format=in.Format;d.MipLevels=1;d.ArraySize=2;d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> next;ComPtr<IDXGIKeyedMutex> key;ComPtr<IDXGIResource> resource;HANDLE handle;
        if(FAILED(hr=p.device->CreateTexture2D(&d,nullptr,&next))||FAILED(hr=next.As(&key))||FAILED(hr=next.As(&resource))||FAILED(hr=resource->GetSharedHandle(&handle)))return hr;
        p.texture=std::move(next);p.key=std::move(key);p.desc=d;s->handle=uint64_t(uintptr_t(handle));
    }
    hr=p.key->AcquireSync(0,0);if(hr!=S_OK)return hr==HRESULT(WAIT_TIMEOUT)?DXGI_ERROR_WAS_STILL_DRAWING:FAILED(hr)?hr:E_FAIL;
    p.context->CopySubresourceRegion(p.texture.Get(),uint32_t(eye),0,0,0,input,subresource,region);
    p.context->End(p.done.Get());p.context->Flush();
    hr=waitForGpuCompletion([&](BOOL* complete){return p.context->GetData(p.done.Get(),complete,sizeof(*complete),0);},stop);
    const HRESULT released=p.key->ReleaseSync(0);
    // An uncertain copy must never be reused/published. Reopen the producer
    // after a GPU error or cancellation, preserving the consumer's last pair.
    if(FAILED(hr)||FAILED(released)){InterlockedExchange(&s->state,Closed);return FAILED(hr)?hr:released;}
    p.pendingId=id;p.encoding=encoding;p.eyes|=1u<<uint32_t(eye);
    if(p.eyes!=3){InterlockedExchange(&s->state,Filling);return S_FALSE;}
    if(deferPublication){InterlockedExchange(&s->state,Filling);return S_OK;}
    s->pairId=id;s->encoding=uint32_t(encoding);p.lastId=id;p.eyes=0;
    InterlockedExchange(&s->state,Ready);return S_OK;
}
HRESULT Producer::commit(){if(!impl_)return E_UNEXPECTED;auto& p=*impl_;std::lock_guard lock(p.mutex);auto* s=p.map.view;
    if(state(s)!=Filling)return E_UNEXPECTED;if(p.eyes!=3)return S_FALSE;
    s->pairId=p.pendingId;s->encoding=uint32_t(p.encoding);p.lastId=p.pendingId;p.eyes=0;InterlockedExchange(&s->state,Ready);return S_OK;
}
HRESULT Producer::reset(bool discardUnreadIfDisconnected){if(!impl_)return E_UNEXPECTED;auto& p=*impl_;std::lock_guard lock(p.mutex);
    auto current=state(p.map.view);
    if(current==Ready&&discardUnreadIfDisconnected&&!connected())current=InterlockedCompareExchange(&p.map.view->state,Idle,Ready)==Ready?Idle:state(p.map.view);
    if(current==Ready||current==Reading)return DXGI_ERROR_WAS_STILL_DRAWING;
    if(current==Closed)return E_UNEXPECTED;p.eyes=0;InterlockedExchange(&p.map.view->state,Idle);return S_OK;
}
struct Reader::Impl {
    Mapping map;ComPtr<ID3D11Device> device;ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> key;uint64_t handle=0;bool acquired=false,registered=false;
    ~Impl(){if(acquired){key->ReleaseSync(0);InterlockedCompareExchange(&map.view->state,Ready,Reading);}if(registered)InterlockedExchange(&map.view->reader,0);}
};
Reader::Reader()=default;Reader::~Reader()=default;
HRESULT Reader::open(ID3D11Device* device,uint32_t channel){
    if(!device||!channel||impl_)return E_INVALIDARG;
    auto p=std::make_unique<Impl>();p->device=device;
    p->map.handle=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name(channel).c_str());if(!p->map.handle)return HRESULT_FROM_WIN32(GetLastError());
    p->map.view=static_cast<Shared*>(MapViewOfFile(p->map.handle,FILE_MAP_ALL_ACCESS,0,0,sizeof(Shared)));if(!p->map.view)return HRESULT_FROM_WIN32(GetLastError());
    auto* s=p->map.view;if(state(s)==Initializing||s->magic!=magic||s->version!=version||s->size!=sizeof(Shared))return E_INVALIDARG;
    if(InterlockedCompareExchange(&s->reader,1,0)!=0)return HRESULT_FROM_WIN32(ERROR_BUSY);
    p->registered=true;impl_=std::move(p);return S_OK;
}
HRESULT Reader::acquire(){
    if(!impl_||impl_->acquired)return E_UNEXPECTED;auto& p=*impl_;auto* s=p.map.view;
    const auto previous=InterlockedCompareExchange(&s->state,Reading,Ready);
    if(previous!=Ready)return previous==Closed?HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE):S_FALSE;
    HRESULT hr=S_OK;
    if(s->encoding>2||!s->handle||!s->pairId)hr=E_INVALIDARG;
    else if(s->handle!=p.handle){
        ComPtr<ID3D11Texture2D> next;ComPtr<IDXGIKeyedMutex> key;
        hr=p.device->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(s->handle)),IID_PPV_ARGS(&next));
        if(SUCCEEDED(hr)){D3D11_TEXTURE2D_DESC d{};next->GetDesc(&d);if(d.ArraySize!=2||d.MipLevels!=1||d.SampleDesc.Count!=1||!supported(d.Format))hr=E_INVALIDARG;}
        if(SUCCEEDED(hr))hr=next.As(&key);
        if(SUCCEEDED(hr)){p.texture=std::move(next);p.key=std::move(key);p.handle=s->handle;}
    }
    if(SUCCEEDED(hr))hr=p.key->AcquireSync(0,0);
    if(hr!=S_OK){InterlockedCompareExchange(&s->state,Ready,Reading);return FAILED(hr)?hr:DXGI_ERROR_WAS_STILL_DRAWING;}
    p.acquired=true;return S_OK;
}
void Reader::release(){if(!impl_||!impl_->acquired)return;auto& p=*impl_;p.key->ReleaseSync(0);p.acquired=false;InterlockedCompareExchange(&p.map.view->state,Idle,Reading);}
ID3D11Texture2D* Reader::texture()const{return impl_&&impl_->acquired?impl_->texture.Get():nullptr;}
uint64_t Reader::pairId()const{return impl_&&impl_->acquired?impl_->map.view->pairId:0;}
Encoding Reader::encoding()const{return impl_&&impl_->acquired?Encoding(impl_->map.view->encoding):Encoding::SRGB;}
}
