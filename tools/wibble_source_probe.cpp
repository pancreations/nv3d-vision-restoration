// Capture-only diagnostic: background OpenVR, no scene submission, emitter,
// output window, runtime launch or settings changes. Exercises SourceReader.
#include "wibble_source.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <thread>
using namespace vision;
struct ProbePixels {
    uint64_t hash[2]{};uint64_t nonzero[2]{};
    std::vector<unsigned char> rgb;unsigned width=0,height=0;
};
static float halfFloat(uint16_t bits){
    const int sign=bits&0x8000?-1:1;const unsigned exponent=(bits>>10)&31,mantissa=bits&1023;
    if(!exponent)return sign*std::ldexp(float(mantissa),-24);
    if(exponent==31)return mantissa?0.f:float(sign)*65504.f;
    return sign*std::ldexp(1.f+float(mantissa)/1024.f,int(exponent)-15);
}
static unsigned char encode(float linear){
    linear=std::clamp(linear,0.f,1.f);const float encoded=linear<=.0031308f?linear*12.92f:1.055f*std::pow(linear,1.f/2.4f)-.055f;
    return static_cast<unsigned char>(std::clamp(std::lround(encoded*255),0l,255l));
}
static ProbePixels inspect(ID3D11Device* device,ID3D11DeviceContext* context,const wibble::Picture& picture,bool image){
    D3D11_TEXTURE2D_DESC desc{};picture.texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging),"Probe readback");context->CopyResource(staging.Get(),picture.texture.Get());
    const unsigned bytes=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:4;
    ProbePixels result;result.width=desc.Width*2;result.height=desc.Height;if(image)result.rgb.resize(size_t(result.width)*result.height*3);
    for(UINT eye=0;eye<2;eye++){
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),eye,D3D11_MAP_READ,0,&mapped),"Map probe eye");uint64_t hash=1469598103934665603ull;
        for(UINT y=0;y<desc.Height;y++)for(UINT x=0;x<desc.Width;x++){
            const auto* pixel=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*bytes;
            for(unsigned byte=0;byte<bytes;byte++){hash^=pixel[byte];hash*=1099511628211ull;}
            unsigned char rgb[3]{};
            if(bytes==8){const auto* values=reinterpret_cast<const uint16_t*>(pixel);for(unsigned c=0;c<3;c++)rgb[c]=encode(halfFloat(values[c]));}
            else if(bytes==16){const auto* values=reinterpret_cast<const float*>(pixel);for(unsigned c=0;c<3;c++)rgb[c]=encode(values[c]);}
            else if(desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8X8_UNORM){rgb[0]=pixel[2];rgb[1]=pixel[1];rgb[2]=pixel[0];}
            else{rgb[0]=pixel[0];rgb[1]=pixel[1];rgb[2]=pixel[2];}
            result.nonzero[eye]+=rgb[0]||rgb[1]||rgb[2];
            if(image){const size_t index=(size_t(y)*result.width+eye*desc.Width+x)*3;for(unsigned c=0;c<3;c++)result.rgb[index+c]=rgb[c];}
        }
        context->Unmap(staging.Get(),eye);result.hash[eye]=hash;
    }
    return result;
}
int wmain(int argc,wchar_t** argv){
    std::ofstream report;std::ostream* output=&std::cout;
    try{
        std::filesystem::path imagePath;double duration=3;unsigned adapterIndex=0;
        for(int i=1;i<argc;i++){
            const std::wstring arg=argv[i];
            if(arg==L"--report"&&i+1<argc){report.open(argv[++i]);if(!report)throw std::runtime_error("Cannot open probe report");output=&report;}
            else if(arg==L"--output"&&i+1<argc)imagePath=argv[++i];
            else if(arg==L"--seconds"&&i+1<argc)duration=std::clamp(std::stod(argv[++i]),1.,60.);
            else if(arg==L"--adapter"&&i+1<argc)adapterIndex=unsigned(std::stoul(argv[++i]));
            else throw std::runtime_error("Usage: vision_wibble_source_probe [--seconds N] [--adapter index] [--report path] [--output sbs.ppm]");
        }
        ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Probe factory");ComPtr<IDXGIAdapter> adapter;check(factory->EnumAdapters(adapterIndex,&adapter),"Probe adapter");
        DXGI_ADAPTER_DESC desc{};check(adapter->GetDesc(&desc),"Probe adapter description");*output<<"GPU="<<utf8(desc.Description)<<" LUID="<<desc.AdapterLuid.HighPart<<':'<<desc.AdapterLuid.LowPart<<std::endl;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Probe device");
        wibble::SourceReader source;source.open(device.Get(),context.Get());std::stop_source stop;wibble::Picture picture;std::string previousStatus;
        uint64_t copies=0,changed=0,prior[2]{};double nextReadback=0;ProbePixels last;const double until=qpc()+duration;
        while(qpc()<until){
            if(source.next(stop.get_token(),picture)){
                ++copies;
                if(qpc()>=nextReadback){last=inspect(device.Get(),context.Get(),picture,!imagePath.empty());changed+=prior[0]&&(last.hash[0]!=prior[0]||last.hash[1]!=prior[1]);prior[0]=last.hash[0];prior[1]=last.hash[1];nextReadback=qpc()+.1;
                    *output<<"pair="<<picture.frameId<<" size="<<picture.width<<'x'<<picture.height<<" hash="<<last.hash[0]<<','<<last.hash[1]<<" nonblackPixels="<<last.nonzero[0]<<','<<last.nonzero[1]<<" distinctEyes="<<(last.hash[0]!=last.hash[1])<<std::endl;}
            }
            const auto status=source.status();if(status!=previousStatus){*output<<"status="<<status<<std::endl;previousStatus=status;}
            Sleep(1);
        }
        if(!copies)throw std::runtime_error("No Wibble eye images received: "+source.status());
        if(!imagePath.empty()){
            std::ofstream image(imagePath,std::ios::binary);image<<"P6\n"<<last.width<<' '<<last.height<<"\n255\n";image.write(reinterpret_cast<const char*>(last.rgb.data()),std::streamsize(last.rgb.size()));
            if(!image)throw std::runtime_error("Cannot save probe SBS image");*output<<"image="<<utf8(imagePath.wstring())<<std::endl;
        }
        *output<<"PASS: "<<copies<<" owned eye snapshots, "<<changed<<" changed readbacks. Explicit eye labels; compositor API does not guarantee atomic stereo. No scene/emitter/output changes."<<std::endl;return 0;
    }catch(const std::exception& error){*output<<"FAIL: "<<error.what()<<std::endl;return 1;}
}
