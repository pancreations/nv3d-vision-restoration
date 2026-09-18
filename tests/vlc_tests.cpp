// SPDX-License-Identifier: GPL-3.0-or-later
#include "sources.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>
using namespace vision;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using Bytes=std::vector<uint8_t>;
void word(Bytes& b,uint32_t value){for(int i=0;i<4;++i)b.push_back(uint8_t(value>>(i*8)));}
void tag(Bytes& b,const char* text){b.insert(b.end(),text,text+4);}
void chunk(Bytes& b,const char* name,const Bytes& payload){tag(b,name);word(b,uint32_t(payload.size()));b.insert(b.end(),payload.begin(),payload.end());if(payload.size()%2)b.push_back(0);}
void list(Bytes& b,const char* name,const Bytes& payload){Bytes content;tag(content,name);content.insert(content.end(),payload.begin(),payload.end());chunk(b,"LIST",content);}
void movie(const std::filesystem::path& file,Packing packing,unsigned width=128){
    constexpr unsigned height=64,frames=120,rate=24;const unsigned stride=(width*3+3)&~3u,frameBytes=stride*height;
    Bytes header;for(uint32_t v:{1000000/rate,frameBytes*rate,0u,0x10u,frames,0u,1u,frameBytes,width,height,0u,0u,0u,0u})word(header,v);
    Bytes hdrl;chunk(hdrl,"avih",header);
    Bytes stream;tag(stream,"vids");tag(stream,"DIB ");
    for(uint32_t v:{0u,0u,0u,1u,rate,0u,frames,frameBytes,0xffffffffu,0u})word(stream,v);
    // rcFrame consists of four 16-bit values.
    word(stream,0);word(stream,width|(height<<16));
    Bytes strl;chunk(strl,"strh",stream);
    Bytes format;word(format,40);word(format,width);word(format,height);word(format,1u|(24u<<16));
    for(uint32_t v:{0u,frameBytes,0u,0u,0u,0u})word(format,v);
    chunk(strl,"strf",format);list(hdrl,"strl",strl);
    Bytes movi,index;
    for(unsigned n=0;n<frames;++n){
        Bytes pixels(frameBytes);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const bool left=packing==Packing::SideBySide?x<width/2:(height-1-y)<height/2;
            pixels[y*stride+x*3+(left?2:1)]=255;
        }
        tag(index,"00db");word(index,0x10);word(index,uint32_t(movi.size())+4);word(index,frameBytes);
        chunk(movi,"00db",pixels);
    }
    Bytes avi;tag(avi,"AVI ");list(avi,"hdrl",hdrl);list(avi,"movi",movi);chunk(avi,"idx1",index);
    Bytes result;chunk(result,"RIFF",avi);std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(result.data()),std::streamsize(result.size()));
    require(bool(out),"Could not create generated AVI fixture");
}
template<class Predicate>void until(Predicate predicate,const char* message,double seconds=6){const double end=qpc()+seconds;while(!predicate()){if(qpc()>end)throw std::runtime_error(message);std::this_thread::sleep_for(std::chrono::milliseconds(10));}}
void pixels(const std::shared_ptr<StereoFrame>& frame,LUID adapter,Packing packing){
    require(frame&&frame->packing==packing&&frame->encoding==Encoding::SRGB,"VLC frame metadata");
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Test factory");
    ComPtr<IDXGIAdapter> gpu;check(factory->EnumAdapterByLuid(adapter,IID_PPV_ARGS(&gpu)),"Test adapter");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(gpu.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Test device");
    ComPtr<ID3D11Texture2D> shared;check(device->OpenSharedResource(frame->sharedHandle,IID_PPV_ARGS(&shared)),"Open VLC frame");
    ComPtr<IDXGIKeyedMutex> key;check(shared.As(&key),"VLC keyed mutex");require(key->AcquireSync(0,1000)==S_OK,"Acquire VLC frame");
    D3D11_TEXTURE2D_DESC desc{};shared->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;check(device->CreateTexture2D(&desc,nullptr,&read),"Readback texture");context->CopyResource(read.Get(),shared.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"VLC readback");
    auto sample=[&](unsigned x,unsigned y){return static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4;};
    const auto* left=sample(desc.Width/4,desc.Height/4);
    const auto* right=sample(packing==Packing::SideBySide?desc.Width*3/4:desc.Width/4,packing==Packing::TopBottom?desc.Height*3/4:desc.Height/4);
    const bool correct=left[2]>240&&left[1]<15&&right[1]>240&&right[2]<15;
    context->Unmap(read.Get(),0);check(key->ReleaseSync(0),"Release VLC frame");require(correct,"Decoded VLC eye pixels are incorrect");
}
}
int wmain(int argc,wchar_t** argv){
    try{
        auto displays=enumerateDisplays();require(!displays.empty(),"No test display");
        const auto dir=std::filesystem::temp_directory_path()/(L"vision-vlc-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(dir);
        struct Cleanup {std::filesystem::path dir;~Cleanup(){std::error_code ec;std::filesystem::remove_all(dir,ec);}} cleanup{dir};
        const auto file=dir/L"stereo-é.avi";movie(file,Packing::SideBySide);
        SourceConfig config;config.kind=SourceKind::Vlc;config.file=file;config.mediaMute=true;
        StereoSource source;source.start(config,displays.front().adapterLuid);
        until([&]{return source.latest()||!source.status().running;},"VLC startup timeout");
        if(!source.latest()){
            const auto error=source.status().message;
            if(error.starts_with("Install 64-bit VLC")){std::cout<<"SKIP: "<<error<<'\n';return 77;}
            throw std::runtime_error(error);
        }
        auto retained=source.latest();pixels(retained,displays.front().adapterLuid,Packing::SideBySide);
        until([&]{return source.status().mediaTimeMs>300&&source.status().mediaSeekable;},"VLC time/seekability missing");
        source.pauseMedia(true);until([&]{return source.status().mediaPaused;},"VLC did not pause");
        const auto time=source.status().mediaTimeMs;std::this_thread::sleep_for(std::chrono::milliseconds(400));
        require(std::abs(source.status().mediaTimeMs-time)<100,"Paused movie clock advanced");
        source.pauseMedia(false);until([&]{return !source.status().mediaPaused&&source.status().mediaTimeMs>time+150;},"VLC did not resume");
        source.seekMedia(3000);until([&]{return source.status().mediaTimeMs>=2900;},"VLC seek failed");
        source.seekMedia(4600);until([&]{return !source.status().running;},"VLC end-of-file not reported");
        require(source.status().message.find("ended")!=std::string::npos&&source.latest(),"End-of-file did not retain last pair");
        source.stop();pixels(retained,displays.front().adapterLuid,Packing::SideBySide);retained.reset();
        movie(file,Packing::TopBottom);config.packing=Packing::TopBottom;source.start(config,displays.front().adapterLuid);
        until([&]{return bool(source.latest())||!source.status().running;},"Top/bottom startup timeout");pixels(source.latest(),displays.front().adapterLuid,Packing::TopBottom);
        const double stopping=qpc();source.stop();require(qpc()-stopping<3,"VLC shutdown stalled");
        config.file=dir/L"missing.mkv";source.start(config,displays.front().adapterLuid);
        until([&]{return !source.status().running;},"Missing movie was not rejected");require(!source.latest(),"Missing movie leaked old pair");source.stop();
        movie(file,Packing::SideBySide,127);config.file=file;config.packing=Packing::SideBySide;source.start(config,displays.front().adapterLuid);
        until([&]{return !source.status().running;},"Odd packed dimensions not rejected");require(!source.latest(),"Odd movie dimensions published a broken pair");source.stop();
        // Optional independently encoded compressed fixtures, in SBS/TAB order.
        for(int i=1;i<argc&&i<=2;++i){
            config.file=argv[i];config.packing=i==1?Packing::SideBySide:Packing::TopBottom;
            source.start(config,displays.front().adapterLuid);
            until([&]{return bool(source.latest())||!source.status().running;},"Compressed VLC movie startup timeout");
            pixels(source.latest(),displays.front().adapterLuid,config.packing);
            source.stop();std::cout<<"PASS: independently encoded compressed movie "<<i<<'\n';
        }
        std::cout<<"PASS: real VLC decoding, SBS/TAB GPU eye pixels, Unicode path, pause/resume, seeking, end-of-file pair retention, restart/stop, missing/odd movies; no emitter writes\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
