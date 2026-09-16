// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only, one-shot diagnostic of the selected process's Geo11 source.
// Not a presentation path: the legacy producer has no cross-process frame fence.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT h,const char* message){if(FAILED(h)){char s[256];sprintf_s(s,"%s: 0x%08lx",message,h);throw std::runtime_error(s);}}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=3)throw std::runtime_error("Usage: vision_pair_inspect <game-pid> <output.bmp>");
        const DWORD pid=std::stoul(argv[1]);if(!pid)throw std::runtime_error("Specify the actual game PID");
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);if(!process)throw std::runtime_error("Cannot open selected game process");
        if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT){CloseHandle(process);throw std::runtime_error("Selected game process has exited");}
        CloseHandle(process);
        auto name=L"Local\\KatangaMappedFile.Vision."+std::to_wstring(pid);
        HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());if(!mapping)throw std::runtime_error("Selected game has not published a Geo11 eye resource");
        auto* view=static_cast<const volatile uint32_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,4));if(!view){CloseHandle(mapping);throw std::runtime_error("Cannot map eye-resource handle");}
        const auto handle=*view;UnmapViewOfFile(const_cast<uint32_t*>(view));CloseHandle(mapping);if(!handle)throw std::runtime_error("Eye-resource handle is empty");
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Diagnostic device");
        ComPtr<ID3D11Texture2D> eyes;check(device->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(handle)),IID_PPV_ARGS(&eyes)),"Open source eyes");
        D3D11_TEXTURE2D_DESC desc{};eyes->GetDesc(&desc);std::printf("Game PID=%lu source=%ux%u format=%u; each eye=%ux%u\n",pid,desc.Width,desc.Height,desc.Format,desc.Width/2,desc.Height);
        const bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgb10=desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM;
        if((!rgba&&!bgra&&!rgb10)||desc.ArraySize!=1||desc.SampleDesc.Count!=1||(desc.Width%2))throw std::runtime_error("Diagnostic BMP supports two RGBA/BGRA8 or RGB10 eyes");
        auto sd=desc;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=sd.MiscFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&sd,nullptr,&staging),"Readback allocation");context->CopyResource(staging.Get(),eyes.Get());
        D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"Read source snapshot");
        std::vector<uint8_t> pixels(size_t(desc.Width)*desc.Height*4);uint64_t differing=0,leftContent=0,rightContent=0;
        const UINT half=desc.Width/2;
        for(UINT y=0;y<desc.Height;++y){auto* row=static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch;auto* destination=pixels.data()+size_t(y)*desc.Width*4;
            for(UINT x=0;x<desc.Width;++x){auto* s=row+x*4;auto* d=destination+x*4;
                if(rgb10){uint32_t p;memcpy(&p,s,4);d[2]=uint8_t((p&1023)*255/1023);d[1]=uint8_t(((p>>10)&1023)*255/1023);d[0]=uint8_t(((p>>20)&1023)*255/1023);}
                else{d[0]=s[rgba?2:0];d[1]=s[1];d[2]=s[rgba?0:2];}d[3]=255;
            }
            for(UINT x=0;x<half;++x){auto* l=destination+x*4;auto* r=destination+(x+half)*4;bool differs=false;for(int c=0;c<3;++c)differs|=std::abs(int(l[c])-int(r[c]))>3;differing+=differs;leftContent+=std::max({l[0],l[1],l[2]})>10;rightContent+=std::max({r[0],r[1],r[2]})>10;}
        }context->Unmap(staging.Get(),0);
        BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(pixels.size());
        BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(desc.Width);info.biHeight=-LONG(desc.Height);info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=DWORD(pixels.size());
        std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);out.write(reinterpret_cast<char*>(&file),sizeof(file));out.write(reinterpret_cast<char*>(&info),sizeof(info));out.write(reinterpret_cast<char*>(pixels.data()),std::streamsize(pixels.size()));out.close();if(!out)throw std::runtime_error("Cannot write diagnostic BMP");
        std::printf("Source differences=%llu/%llu; nonblack left=%llu right=%llu\n",differing,uint64_t(half)*desc.Height,leftContent,rightContent);
        std::puts("Diagnostic source snapshot only; not proof of synchronized presentation or optical stereo.");return 0;
    }catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}
