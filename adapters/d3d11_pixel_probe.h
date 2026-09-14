// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace vision::hook {
// Small asynchronous samples of the input, left and right images. No screenshots,
// no per-frame readback, and no wait for the GPU. Used to diagnose black output.
class PixelProbe {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tiles_;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    UINT captured_=0;
    static float half(uint16_t value){
        const int exp=(value>>10)&31, fraction=value&1023;
        const float magnitude=exp==0?std::ldexp(float(fraction),-24):exp==31?0.f:std::ldexp(float(1024+fraction),exp-25);
        return value&0x8000?-magnitude:magnitude;
    }
public:
    bool pending() const {return captured_!=0;}
    bool initialize(ID3D11Device* device,DXGI_FORMAT format){
        format_=format;
        if(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_B8G8R8A8_UNORM&&
           format!=DXGI_FORMAT_R10G10B10A2_UNORM&&format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=48;desc.ArraySize=3;desc.MipLevels=1;
        desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&tiles_));
    }
    void capture(ID3D11DeviceContext* context,ID3D11Resource* source,UINT width,UINT height,UINT eye){
        if(!tiles_||width<16||height<16||eye>=3)return;
        for(UINT y=0;y<3;++y)for(UINT x=0;x<3;++x){
            const UINT sx=std::min(width-16,(x+1)*width/4),sy=std::min(height-16,(y+1)*height/4);
            D3D11_BOX box{sx,sy,0,sx+16,sy+16,1};
            context->CopySubresourceRegion(tiles_.Get(),eye,x*16,y*16,0,source,0,&box);
        }
        captured_|=1u<<eye;
    }
    bool poll(ID3D11DeviceContext* context,std::string& result){
        if(!tiles_||!captured_)return false;
        std::string message="[Vision] Pixel probe";
        for(UINT eye=0;eye<3;++eye){
            if(!(captured_&(1u<<eye)))continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr=context->Map(tiles_.Get(),eye,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return false;
            if(FAILED(hr)){captured_=0;result="[Vision] Pixel probe readback unavailable";return true;}
            double sum=0;float maximum=0;UINT lit=0;
            for(UINT y=0;y<48;++y)for(UINT x=0;x<48;++x){
                const auto* pixel=static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*(format_==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4);
                float rgb[3]{};
                if(format_==DXGI_FORMAT_R16G16B16A16_FLOAT){uint16_t values[4];memcpy(values,pixel,8);for(int c=0;c<3;++c)rgb[c]=half(values[c]);}
                else if(format_==DXGI_FORMAT_R10G10B10A2_UNORM){uint32_t value;memcpy(&value,pixel,4);for(int c=0;c<3;++c)rgb[c]=float((value>>(c*10))&1023)/1023.f;}
                else for(int c=0;c<3;++c)rgb[c]=pixel[c]/255.f;
                float level=std::max({std::abs(rgb[0]),std::abs(rgb[1]),std::abs(rgb[2])});sum+=level;maximum=std::max(maximum,level);if(level>0.00001f)++lit;
            }
            context->Unmap(tiles_.Get(),eye);
            char stats[144];sprintf_s(stats," %s(avg=%.6f max=%.6f nonblack=%u/2304)",eye==0?"source":eye==1?"left":"right",sum/2304,maximum,lit);message+=stats;
        }
        captured_=0;result=message;return true;
    }
};
}
