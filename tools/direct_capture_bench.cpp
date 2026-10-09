// Offscreen D3D11 stereo producer -> real StereoSource -> GPU-completed snapshot.
// This tests direct transport, not desktop capture, an injected game, or optical output.
#include "direct_eyes.h"
#include "sources.h"
#include "gpu_completion.h"
#include <DirectXPackedVector.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <vector>
using namespace vision;
static double percentile(std::vector<double> values,double p){std::sort(values.begin(),values.end());return values.empty()?0:values[size_t(p*(values.size()-1))];}
int main(int argc,char** argv){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try{
        if(argc>3||(argc>2&&std::string(argv[2])!="load"))throw std::runtime_error("Usage: vision_direct_capture_bench <report.csv> [load]");
        const bool load=argc>2;
        std::ofstream file(argc>1?argv[1]:"direct-capture-benchmark.csv");if(!file)throw std::runtime_error("Cannot write benchmark report");
        file<<"path,width,height,load,round,frames,producer_to_copy_p50_ms,producer_to_copy_p95_ms,producer_to_copy_max_ms,frames_over_1ms,dropped,pixels_verified,producer_completion_p50_ms,producer_completion_p95_ms,source_completion_p50_ms,source_completion_p95_ms\n";
        ComPtr<ID3D11Device> device,consumer;ComPtr<ID3D11DeviceContext> context,readContext;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Producer device");
        ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> adapter;check(device.As(&dx),"Producer DXGI");check(dx->GetAdapter(&adapter),"Producer adapter");DXGI_ADAPTER_DESC ad{};check(adapter->GetDesc(&ad),"Adapter description");
        check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&consumer,nullptr,&readContext),"Snapshot consumer");
        std::wcout<<ad.Description<<L" | offscreen direct transport; no desktop capture"<<std::endl;
        for(unsigned round=1;round<=2;++round)for(auto size:{std::pair{1280u,720u},std::pair{3840u,2160u}}){
            const auto [width,height]=size;
            std::vector<double> waits;for(unsigned i=0;i<32;++i){const double before=qpc();shortWait(nullptr,50);waits.push_back((qpc()-before)*1000);}
            std::cout<<"50 us timer diagnostic median/max "<<percentile(waits,.5)<<" / "<<percentile(waits,1)<<" ms"<<std::endl;
            D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            std::array<ComPtr<ID3D11Texture2D>,2> eyes;std::array<ComPtr<ID3D11RenderTargetView>,2> targets;
            for(unsigned eye=0;eye<2;++eye){check(device->CreateTexture2D(&desc,nullptr,&eyes[eye]),"Producer eye");check(device->CreateRenderTargetView(eyes[eye].Get(),nullptr,&targets[eye]),"Producer eye target");}
            ComPtr<ID3D11Texture2D> stress;ComPtr<ID3D11RenderTargetView> stressTarget;
            if(load){desc.Width=3840;desc.Height=2160;check(device->CreateTexture2D(&desc,nullptr,&stress),"Stress texture");check(device->CreateRenderTargetView(stress.Get(),nullptr,&stressTarget),"Stress target");}
            desc.Width=1;desc.Height=4;desc.ArraySize=2;desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;check(consumer->CreateTexture2D(&desc,nullptr,&staging),"Corner staging");
            direct::Producer producer;const auto channel=GetCurrentProcessId();check(producer.open(device.Get(),channel),"Open direct producer");
            StereoSource source;SourceConfig config;config.kind=SourceKind::DirectEyes;config.directChannel=channel;source.start(config,ad.AdapterLuid);
            struct Opened{ComPtr<ID3D11Texture2D> texture;ComPtr<IDXGIKeyedMutex> key;};std::map<HANDLE,Opened> opened;
            std::vector<double> ages,producerTimes,sourceTimes;uint64_t id=0,over=0,verified=0;
            const double start=qpc(),sampleStart=start+.5,finish=sampleStart+2;double next=start;
            while(qpc()<finish){
                while(qpc()<next)shortWait(nullptr,std::max(1L,long(std::min((next-qpc())*1e6,500.))));
                ++id;const float marker=float(id%64)/64.f;const float colors[2][4]{{marker,.25f,.5f,1},{1-marker,.5f,.25f,1}};
                if(load)for(unsigned i=0;i<8;++i)context->ClearRenderTargetView(stressTarget.Get(),colors[i%2]);
                const double submitted=qpc();
                for(unsigned eye=0;eye<2;++eye){
                    context->ClearRenderTargetView(targets[eye].Get(),colors[eye]);
                    HRESULT hr;const double deadline=qpc()+5;
                    do{hr=producer.submit(eyes[eye].Get(),0,direct::Eye(eye),id,direct::Encoding::LinearScRGB);if(hr==DXGI_ERROR_WAS_STILL_DRAWING)shortWait(nullptr,50);}while(hr==DXGI_ERROR_WAS_STILL_DRAWING&&qpc()<deadline);
                    check(hr,"Submit direct eye");
                }
                const double producerDone=qpc();
                std::shared_ptr<StereoFrame> frame;const double deadline=qpc()+5;
                do{frame=source.forPresentation();if(frame&&frame->pairId==id)break;if(!source.status().running)throw std::runtime_error(source.status().message);shortWait(nullptr,50);}while(qpc()<deadline);
                if(!frame||frame->pairId!=id)throw std::runtime_error("Direct pair delivery timed out");
                if(frame->width!=width||frame->height!=height||frame->packing!=Packing::SeparateEyes||frame->encoding!=Encoding::LinearScRGB||frame->publishedAt<submitted)throw std::runtime_error("Direct snapshot identity, dimensions, encoding or clock invalid");
                const double age=(frame->publishedAt-submitted)*1000;
                if(submitted>=sampleStart){ages.push_back(age);producerTimes.push_back((producerDone-submitted)*1000);sourceTimes.push_back((frame->publishedAt-producerDone)*1000);if(age>=1)++over;}
                // Check both eyes and all four corners periodically; readback occurs after the timed endpoint.
                if(id%32==0){
                    auto& snapshot=opened[frame->sharedHandle];if(!snapshot.texture){check(consumer->OpenSharedResource(frame->sharedHandle,IID_PPV_ARGS(&snapshot.texture)),"Open source snapshot");check(snapshot.texture.As(&snapshot.key),"Snapshot mutex");}
                    const HRESULT locked=snapshot.key->AcquireSync(0,1000);if(locked!=S_OK)throw std::runtime_error("Snapshot mutex timeout or failure");
                    for(unsigned eye=0;eye<2;++eye)for(unsigned corner=0;corner<4;++corner){const unsigned x=corner&1?width-1:0,y=corner&2?height-1:0;D3D11_BOX box{x,y,0,x+1,y+1,1};readContext->CopySubresourceRegion(staging.Get(),eye,0,corner,0,snapshot.texture.Get(),eye,&box);}
                    bool valid=true;
                    for(unsigned eye=0;eye<2;++eye){D3D11_MAPPED_SUBRESOURCE mapped{};const HRESULT hr=readContext->Map(staging.Get(),eye,D3D11_MAP_READ,0,&mapped);if(FAILED(hr)){snapshot.key->ReleaseSync(0);check(hr,"Read direct marker");}
                        for(unsigned corner=0;corner<4;++corner)for(unsigned component=0;component<4;++component)valid&=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData)+corner*mapped.RowPitch)[component]==DirectX::PackedVector::XMConvertFloatToHalf(colors[eye][component]);
                        readContext->Unmap(staging.Get(),eye);
                    }
                    check(snapshot.key->ReleaseSync(0),"Release snapshot");if(!valid)throw std::runtime_error("Direct HDR eye pixels changed or mixed between frames");++verified;
                }
                source.presented(frame);frame.reset();
                next+=1./240.;if(next<qpc())next=qpc();
            }
            const auto status=source.status();source.stop();if(ages.size()<10||status.dropped)throw std::runtime_error("Insufficient frames or dropped direct pair");
            file<<std::fixed<<std::setprecision(3)<<"direct-stereo-source,"<<width<<','<<height<<','<<load<<','<<round<<','<<ages.size()<<','<<percentile(ages,.5)<<','<<percentile(ages,.95)<<','<<percentile(ages,1)<<','<<over<<','<<status.dropped<<','<<verified<<','<<percentile(producerTimes,.5)<<','<<percentile(producerTimes,.95)<<','<<percentile(sourceTimes,.5)<<','<<percentile(sourceTimes,.95)<<'\n';file.flush();
            std::cout<<width<<'x'<<height<<" round "<<round<<": producer-to-copy "<<percentile(ages,.5)<<" / "<<percentile(ages,.95)<<" / "<<percentile(ages,1)<<" ms (median/p95/max), "<<over<<" frames >=1 ms | producer/source medians "<<percentile(producerTimes,.5)<<" / "<<percentile(sourceTimes,.5)<<std::endl;
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
