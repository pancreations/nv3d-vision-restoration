// SPDX-License-Identifier: GPL-3.0-or-later
// Resource-description fixtures. These tests do not initialize OpenVR, open a
// display, modify WibbleWobble, or access an emitter.
#include "wibble_source.h"
#include <iostream>
#include <stdexcept>
using namespace vision;
using namespace vision::wibble;
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}require(rejected,message);}
D3D11_TEXTURE2D_DESC texture(DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_TYPELESS){
    D3D11_TEXTURE2D_DESC result{};result.Width=3700;result.Height=2080;
    result.MipLevels=1;result.ArraySize=1;result.SampleDesc.Count=1;result.Format=format;return result;
}
D3D11_SHADER_RESOURCE_VIEW_DESC view(DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB){
    D3D11_SHADER_RESOURCE_VIEW_DESC result{};result.Format=format;result.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    result.Texture2D.MipLevels=1;return result;
}
}
int main(){try{
    {
        MirrorCadence cadence;
        require(cadence.begin(0,true,10),"First timed mirror copy starts immediately");
        require(!cadence.begin(.001,true,10),"Repeated timing does not busy-copy the same mirrors");
        require(cadence.begin(1./240,true,11),"Fresh 240 Hz compositor frames are not capped to 120 Hz");
        require(cadence.begin(2./240,true,12),"Consecutive fresh compositor frames remain eligible");
        require(!cadence.begin(.009,true,12),"Unchanged frame is paced after a fresh copy");
        require(cadence.begin(.018,true,12),"Frozen frame timing cannot starve mirror capture");
        require(cadence.begin(6.,true,12),"Long timing stalls do not force disruptive runtime reconnects");
        require(cadence.begin(6.001,true,13),"Advancing timing resumes immediately after a timing stall");
    }
    {
        MirrorCadence cadence;
        require(cadence.begin(0,false,0),"Mirror capture starts even without compositor timing");
        require(!cadence.begin(.001,false,0),"Missing timing still bounds the polling cadence");
        require(cadence.begin(.009,false,0),"Missing timing continues mirror capture");
        require(cadence.begin(.010,true,100),"Newly available timing wakes capture immediately");
        require(!cadence.begin(.011,false,0),"Transient timing failure does not busy-copy");
        require(cadence.begin(.019,false,0),"Loss of timing never permanently freezes the last pair");
        require(cadence.begin(.020,true,UINT32_MAX),"Large frame indices retain pacing semantics");
        require(cadence.begin(.021,true,0),"Compositor frame-index wrap is treated as a new frame");
    }
    {
        MirrorCadence cadence;
        // Each copy takes longer than a 240 Hz compositor interval. There is
        // never a stable start/end frame index, but copies must keep starting.
        unsigned copies=0;double acquired=0;
        for(unsigned attempt=0;attempt<100;attempt++){
            const auto before=uint32_t(acquired*240);
            if(cadence.begin(acquired,true,before))++copies;
            acquired+=.010;
        }
        require(copies==100,"Slow completed copies cannot be starved by advancing compositor timing");
    }
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}){
        auto mapped=describeMirror(texture(),view(format));
        require(mapped.width==3700&&mapped.height==2080,"Full eye dimensions are preserved without SBS cropping");
        require(mapped.format==DXGI_FORMAT_R8G8B8A8_UNORM&&mapped.encoding==Encoding::SRGB,"Copied RGBA bytes are decoded as SRGB exactly once by the presenter");
    }
    for(auto format:{DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}){
        auto mapped=describeMirror(texture(DXGI_FORMAT_B8G8R8A8_TYPELESS),view(format));
        require(mapped.format==DXGI_FORMAT_B8G8R8A8_UNORM&&mapped.encoding==Encoding::SRGB,"BGRA channel order and encoded values remain intact");
    }
    for(auto format:{DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB}){
        auto mapped=describeMirror(texture(DXGI_FORMAT_B8G8R8X8_TYPELESS),view(format));
        require(mapped.format==DXGI_FORMAT_B8G8R8X8_UNORM&&mapped.encoding==Encoding::SRGB,"BGRX does not acquire an invented alpha interpretation");
    }
    {
        auto half=describeMirror(texture(DXGI_FORMAT_R16G16B16A16_TYPELESS),view(DXGI_FORMAT_R16G16B16A16_FLOAT));
        require(half.format==DXGI_FORMAT_R16G16B16A16_FLOAT&&half.encoding==Encoding::LinearScRGB,"Float eye textures retain linear HDR precision");
        auto full=describeMirror(texture(DXGI_FORMAT_R32G32B32A32_TYPELESS),view(DXGI_FORMAT_R32G32B32A32_FLOAT));
        require(full.format==DXGI_FORMAT_R32G32B32A32_FLOAT&&full.encoding==Encoding::LinearScRGB,"Full float eye values are not reinterpreted as integers");
        require(!compatibleMirrors(half,full),"Different eye storage precision is not silently copied into one array");
    }
    {
        auto allocation=texture();allocation.ArraySize=4;allocation.MipLevels=4;
        auto left=view();left.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        left.Texture2DArray={1,1,2,1};auto right=left;right.Texture2DArray.FirstArraySlice=3;
        const auto l=describeMirror(allocation,left),r=describeMirror(allocation,right);
        require(l.width==1850&&l.height==1040,"The selected mip determines eye dimensions");
        require(l.subresource==9&&r.subresource==13,"Explicit eye view slices determine the source subresource");
        require(compatibleMirrors(l,r),"Distinct eye slices with equal dimensions and format are compatible");
        right.Texture2DArray.MostDetailedMip=2;
        require(!compatibleMirrors(l,describeMirror(allocation,right)),"Unequal eye sizes are rejected");
        right=left;right.Texture2DArray.FirstArraySlice=4;
        rejects([&]{describeMirror(allocation,right);},"An out-of-range eye slice cannot be copied");
        right=left;right.Texture2DArray.ArraySize=2;
        rejects([&]{describeMirror(allocation,right);},"One eye must identify exactly one slice");
        right=left;right.Texture2DArray.MostDetailedMip=4;
        rejects([&]{describeMirror(allocation,right);},"An out-of-range selected mip cannot be copied");
        right=left;right.Texture2DArray.MipLevels=4;
        rejects([&]{describeMirror(allocation,right);},"A view cannot extend past the remaining mip levels");
        right.Texture2DArray.MipLevels=UINT(-1);
        require(describeMirror(allocation,right).subresource==l.subresource,"All remaining mips sentinel still selects the explicit first mip");
        rejects([&]{describeMirror(allocation,view());},"A non-array view cannot select an array allocation");
    }
    {
        const auto l=describeMirror(texture(),view());
        const auto bgra=describeMirror(texture(DXGI_FORMAT_B8G8R8A8_TYPELESS),view(DXGI_FORMAT_B8G8R8A8_UNORM));
        require(!compatibleMirrors(l,bgra),"Different RGB/BGR channel layouts are not combined into one pair");
        auto altered=l;altered.encoding=Encoding::LinearScRGB;
        require(!compatibleMirrors(l,altered),"One shared pair cannot apply different eye transfer functions");
        rejects([]{describeMirror(texture(),view(DXGI_FORMAT_B8G8R8A8_UNORM));},"Resource and view format families must agree");
        rejects([]{describeMirror(texture(),view(DXGI_FORMAT_R8G8B8A8_TYPELESS));},"A typeless shader view gives no interpretation for its bytes");
        rejects([]{describeMirror(texture(DXGI_FORMAT_R8G8B8A8_UINT),view(DXGI_FORMAT_R8G8B8A8_UINT));},"Integer shader data is not a display color texture");
        rejects([]{describeMirror(texture(DXGI_FORMAT_D24_UNORM_S8_UINT),view(DXGI_FORMAT_R24_UNORM_X8_TYPELESS));},"Depth data cannot masquerade as an eye color image");
        rejects([]{describeMirror(texture(DXGI_FORMAT_BC1_UNORM),view(DXGI_FORMAT_BC1_UNORM));},"Compressed textures require a conversion, not a raw eye copy");
        rejects([]{describeMirror(texture(DXGI_FORMAT_R10G10B10A2_TYPELESS),view(DXGI_FORMAT_R10G10B10A2_UNORM));},"Ten-bit pixels without transfer-function metadata are not assigned a guessed color space");
    }
    {
        auto allocation=texture();auto selection=view();
        allocation.Width=0;rejects([&]{describeMirror(allocation,selection);},"Zero-width texture rejected");
        allocation=texture();allocation.Height=0;rejects([&]{describeMirror(allocation,selection);},"Zero-height texture rejected");
        allocation=texture();allocation.SampleDesc.Count=4;rejects([&]{describeMirror(allocation,selection);},"Multisample textures require resolution before copying");
        allocation=texture();allocation.MipLevels=0;rejects([&]{describeMirror(allocation,selection);},"Unresolved allocation mip count rejected");
        allocation=texture();allocation.ArraySize=0;rejects([&]{describeMirror(allocation,selection);},"Empty texture array rejected");
        allocation=texture();selection.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;
        rejects([&]{describeMirror(allocation,selection);},"Cubemap is not an explicitly selected eye image");
        selection=view();selection.Texture2D.MipLevels=0;
        rejects([&]{describeMirror(allocation,selection);},"Empty view rejected");
        selection=view();selection.Texture2D.MostDetailedMip=32;allocation.MipLevels=33;
        rejects([&]{describeMirror(allocation,selection);},"Invalid oversized mip shifts are rejected");
    }
    std::cout<<"Wibble source layout and capture cadence: "<<checks<<" checks passed; no runtime or USB access.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
