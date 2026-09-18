#include "direct_eyes.h"
#include "gpu_completion.h"
#include "sources.h"
#include "producer_wait.h"
#include "capture_layout.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace vision;
static void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--reader"){
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Child device");
        direct::Reader reader;check(reader.open(device.Get(),uint32_t(std::stoul(argv[2]))),"Child reader");
        HRESULT hr=S_FALSE;const double until=qpc()+5;while((hr=reader.acquire())==S_FALSE&&qpc()<until)Sleep(1);require(hr==S_OK,"Child acquisition");
        D3D11_TEXTURE2D_DESC d{};reader.texture()->GetDesc(&d);require(d.Width==31&&d.Height==17&&d.ArraySize==2,"Child eye dimensions");
        d.MiscFlags=d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&d,nullptr,&staging),"Child readback");context->CopyResource(staging.Get(),reader.texture());
        for(UINT eye=0;eye<2;eye++){D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(staging.Get(),eye,D3D11_MAP_READ,0,&m),"Child pixels");const uint32_t color=*static_cast<const uint32_t*>(m.pData);context->Unmap(staging.Get(),eye);require(color==(eye?0xff00ff00:0xff0000ff),"Cross-process eye pixels corrupted");}
        reader.release();return 0;
    }
    struct Event {HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(value)CloseHandle(value);}} permit;
    require(permit.value!=nullptr,"Producer event");
    require(waitForProducerPermit(permit.value,true,[]{return false;})==DXGI_ERROR_WAS_STILL_DRAWING,"Nonblocking Present acknowledged an unavailable slot");
    require(waitForProducerPermit(permit.value,false,[]{return false;},25)==DXGI_ERROR_WAIT_TIMEOUT,"Present timeout reported success");
    require(waitForProducerPermit(permit.value,false,[]{return true;})==DXGI_ERROR_DEVICE_REMOVED,"Closed output did not cancel producer wait");
    std::jthread delayedPermit([&]{Sleep(150);SetEvent(permit.value);});const auto waitStart=GetTickCount64();
    check(waitForProducerPermit(permit.value,false,[]{return false;}),"Delayed producer permit");
    require(GetTickCount64()-waitStart>=100,"Producer skipped a pair after the old 100 ms timeout");delayedPermit.join();
    int polls=0;
    capture::EyeRegion regions[2];require(capture::regions(capture::Mode::Sequential,31,17,1,regions)&&regions[0].right==31,"Sequential eyes were halved");
    require(capture::regions(capture::Mode::Array,31,17,2,regions)&&regions[1].subresource==1,"Stereo array layer mapping");
    require(!capture::regions(capture::Mode::SideBySide,31,17,1,regions),"Odd packed source accepted");
    capture::SequentialClock sequence;require(sequence.eye()==0&&sequence.eye(true)==1&&sequence.pair()==1,"Initial eye phase");sequence.presented();require(sequence.eye()==1&&sequence.pair()==1&&!sequence.boundary(),"Second eye identity");sequence.presented();require(sequence.eye()==0&&sequence.pair()==2&&sequence.boundary(),"Next pair identity");
    check(waitForGpuCompletion([&](BOOL* done){*done=++polls>=3;return polls==1?S_FALSE:S_OK;},{}),"Completion polling");
    require(polls==3,"S_OK with FALSE was mistaken for completed GPU work");
    require(waitForGpuCompletion([](BOOL*){return S_FALSE;},{},std::chrono::milliseconds(0))==DXGI_ERROR_WAIT_TIMEOUT,"Timeout published unfinished work");
    require(waitForGpuCompletion([](BOOL*){return DXGI_ERROR_DEVICE_REMOVED;},{})==DXGI_ERROR_DEVICE_REMOVED,"Device failure was lost");
    std::stop_source cancel;cancel.request_stop();
    require(waitForGpuCompletion([](BOOL*){throw std::runtime_error("Polled cancelled work");return S_OK;},cancel.get_token())==HRESULT_FROM_WIN32(ERROR_CANCELLED),"Cancellation was lost");
    ComPtr<ID3D11Device> device,consumer;ComPtr<ID3D11DeviceContext> context,readContext;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Producer device");
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;check(device.As(&dx),"DXGI");check(dx->GetAdapter(&adapter),"Adapter");DXGI_ADAPTER_DESC ad{};check(adapter->GetDesc(&ad),"Adapter description");
    check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&consumer,nullptr,&readContext),"Consumer device");
    auto texture=[&](UINT w,UINT h,uint32_t color){
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        std::vector<uint32_t> pixels(size_t(w)*h,color);D3D11_SUBRESOURCE_DATA data{pixels.data(),w*4,0};ComPtr<ID3D11Texture2D> t;
        check(device->CreateTexture2D(&d,&data,&t),"Eye texture");return t;
    };
    auto left=texture(31,17,0xff0000ff),right=texture(31,17,0xff00ff00);
    {
        const uint32_t channel=GetCurrentProcessId()|0x40000000;direct::Producer cross;check(cross.open(device.Get(),channel),"Cross-process producer");
        require(cross.submit(left.Get(),0,direct::Eye::Left,1)==S_FALSE,"Cross-process left");require(cross.submit(right.Get(),0,direct::Eye::Right,1)==S_OK,"Cross-process right");
        wchar_t exe[32768]{};require(GetModuleFileNameW(nullptr,exe,32768)!=0,"Test executable");
        std::wstring command=L"\""+std::wstring(exe)+L"\" --reader "+std::to_wstring(channel);
        STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
        require(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process)!=FALSE,"Start child consumer");
        const auto waited=WaitForSingleObject(process.hProcess,10000);DWORD code=1;
        if(waited==WAIT_TIMEOUT){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,1000);}else GetExitCodeProcess(process.hProcess,&code);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);require(waited==WAIT_OBJECT_0&&code==0,"Cross-process shared eye transport failed");
        require(cross.submit(left.Get(),0,direct::Eye::Left,2)==S_FALSE,"Child did not acknowledge pair");
    }
    auto pixels=[&](ID3D11Texture2D* t,UINT width,uint32_t l,uint32_t r){
        D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);require(d.Width==width&&d.Height==17&&d.ArraySize==2,"Separate eye dimensions changed");
        d.MiscFlags=d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;
        check(consumer->CreateTexture2D(&d,nullptr,&staging),"Readback texture");readContext->CopyResource(staging.Get(),t);
        for(UINT eye=0;eye<2;++eye){D3D11_MAPPED_SUBRESOURCE m{};check(readContext->Map(staging.Get(),eye,D3D11_MAP_READ,0,&m),"Eye readback");bool valid=true;
            for(UINT y=0;y<d.Height;y++)for(UINT x=0;x<d.Width;x++)valid&=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData)+y*m.RowPitch)[x]==(eye?r:l);
            readContext->Unmap(staging.Get(),eye);require(valid,"Wrong eye, truncated width, or overwritten pair");}
    };
    const auto channel=GetCurrentProcessId();require(!direct::available(channel),"Absent provider reported ready");direct::Producer producer;check(producer.open(device.Get(),channel),"Open producer");require(direct::available(channel),"Live provider not reported ready");
    {direct::Producer duplicate;require(FAILED(duplicate.open(device.Get(),channel)),"Duplicate producer replaced channel");}
    {
        direct::Reader reader;check(reader.open(consumer.Get(),channel),"Open reader");
        {direct::Reader duplicate;require(FAILED(duplicate.open(consumer.Get(),channel)),"Two readers claimed the same channel");}
        require(producer.submit(right.Get(),0,direct::Eye::Right,1)==S_FALSE,"First right eye not retained");
        require(reader.acquire()==S_FALSE,"An incomplete pair became visible");
        require(producer.submit(right.Get(),0,direct::Eye::Right,1)==E_INVALIDARG,"Duplicate eye was accepted");
        require(producer.submit(left.Get(),0,direct::Eye::Left,2)==E_INVALIDARG,"Different pair IDs were combined");
        require(producer.submit(left.Get(),0,direct::Eye::Left,1)==S_OK,"Complete pair not published");
        require(producer.submit(left.Get(),0,direct::Eye::Left,2)==DXGI_ERROR_WAS_STILL_DRAWING,"Unread pair overwritten");
        check(reader.acquire(),"Acquire complete pair");require(reader.pairId()==1,"Pair ID changed");pixels(reader.texture(),31,0xff0000ff,0xff00ff00);
        require(producer.submit(left.Get(),0,direct::Eye::Left,2)==DXGI_ERROR_WAS_STILL_DRAWING,"Held pair overwritten");reader.release();
        require(producer.submit(left.Get(),0,direct::Eye::Left,1)==E_INVALIDARG,"Old pair ID accepted");
        require(producer.submit(left.Get(),0,direct::Eye::Left,2)==S_FALSE,"Partial pair failed");check(producer.reset(),"Reset incomplete pair");
        auto wideLeft=texture(47,17,0xffff0000),wideRight=texture(47,17,0xff00ffff);
        require(producer.submit(wideLeft.Get(),0,direct::Eye::Left,3)==S_FALSE,"Resize first eye");require(producer.submit(wideRight.Get(),0,direct::Eye::Right,3)==S_OK,"Resize second eye");
        check(reader.acquire(),"Acquire resized pair");pixels(reader.texture(),47,0xffff0000,0xff00ffff);reader.release();
        require(producer.submit(left.Get(),0,direct::Eye::Left,4,direct::Encoding::SRGB,{},nullptr,true)==S_FALSE,"Deferred first eye");
        require(producer.submit(right.Get(),0,direct::Eye::Right,4,direct::Encoding::SRGB,{},nullptr,true)==S_OK,"Deferred second eye");
        require(reader.acquire()==S_FALSE,"Pair became visible before Present succeeded");check(producer.reset(),"Failed Present discards staged pair");
        require(reader.acquire()==S_FALSE,"Cancelled Present published a pair");
    }
    StereoSource source;SourceConfig config;config.kind=SourceKind::DirectEyes;config.directChannel=channel;source.start(config,ad.AdapterLuid);
    auto submit=[&](uint64_t id){
        for(auto eye:{direct::Eye::Left,direct::Eye::Right}){
            HRESULT hr;const double until=qpc()+3;do{hr=producer.submit(eye==direct::Eye::Left?left.Get():right.Get(),0,eye,id);if(hr==DXGI_ERROR_WAS_STILL_DRAWING)std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(hr==DXGI_ERROR_WAS_STILL_DRAWING&&qpc()<until);
            check(hr,"Submit source eyes");}
    };
    submit(4);submit(5);submit(6);
    const double until=qpc()+3;while(source.status().frames<3&&qpc()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(source.status().frames==3,"Direct source did not receive three complete pairs");
    auto newest=source.latest();auto first=source.forPresentation();require(first&&first!=newest&&first->pairId==1,"Source drained to latest instead of preserving pair order");
    require(first->packing==Packing::SeparateEyes,"Direct eyes became packed");
    ComPtr<ID3D11Texture2D> opened;check(consumer->OpenSharedResource(first->sharedHandle,IID_PPV_ARGS(&opened)),"Open source snapshot");ComPtr<IDXGIKeyedMutex> key;check(opened.As(&key),"Snapshot mutex");require(key->AcquireSync(0,100)==S_OK,"Acquire source snapshot");pixels(opened.Get(),31,0xff0000ff,0xff00ff00);check(key->ReleaseSync(0),"Release snapshot");
    source.presented(first);auto second=source.forPresentation();require(second->pairId==2,"Second pair missing");source.presented(second);require(source.forPresentation()->pairId==3,"Third pair missing");
    require(source.status().dropped==0,"Direct path dropped a pair");
    // With the pool retained, the consumer blocks until cancellation instead
    // of acknowledging and losing another complete pair.
    submit(7);const double before=qpc();source.stop();require(qpc()-before<1,"Blocked direct source did not stop promptly");
    std::cout<<"PASS: GPU completion failures, separate eye pixels, explicit identity, FIFO, backpressure, resizing and cancellation\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
