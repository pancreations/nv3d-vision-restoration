// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "stereo.h"
#include "sbs_shader.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace vision::elden;
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void check(HRESULT result,const char* message){require(SUCCEEDED(result),message);}
ComPtr<ID3DBlob> compile(const char* entry,const char* profile){
    ComPtr<ID3DBlob> code,error;auto result=D3DCompile(shader,strlen(shader),"SBS",nullptr,nullptr,entry,profile,0,0,&code,&error);
    if(FAILED(result)&&error)fprintf(stderr,"%s\n",static_cast<char*>(error->GetBufferPointer()));check(result,"shader compilation");return code;
}
void cameraTests(){
    Camera base;require(valid(base),"identity camera must validate");
    auto left=eyeCamera(base,0,.064f),right=eyeCamera(base,1,.064f);
    require(std::abs(left.basis[3][0]+.032f)<1e-6f&&std::abs(right.basis[3][0]-.032f)<1e-6f,"two distinct positions");
    require(left.basis[2][2]==right.basis[2][2],"parallel optical axes");
    base.basis[0][0]=0;base.basis[0][2]=1;base.basis[2][0]=-1;base.basis[2][2]=0;
    require(valid(base),"rotated camera validates");left=eyeCamera(base,0,.064f);right=eyeCamera(base,1,.064f);
    require(std::abs(right.basis[3][2]-left.basis[3][2]-.064f)<1e-6f,"separation follows local right");
    base.basis[0][0]=std::numeric_limits<float>::quiet_NaN();require(!valid(base),"reject invalid matrix");
    base=Camera{};base.basis[0][0]=8;require(!valid(base),"reject non-camera memory");
    base=Camera{};base.fov=0;require(!valid(base),"reject zero FOV");
    PairClock clock;using A=PairClock::Action;
    require(clock.accept(1,1)==A::Ignore&&!clock.complete,"right cannot start pair");
    require(clock.accept(2,0)==A::StageLeft,"stage left");
    require(clock.accept(2,1)==A::Ignore&&!clock.complete,"same camera sample cannot supply both eyes");
    require(clock.accept(3,1)==A::Complete&&clock.pairs==1,"complete distinct consecutive eyes");
    require(clock.accept(4,0)==A::StageLeft&&clock.complete,"hold previous complete pair while rendering left");
    require(clock.accept(6,1)==A::Ignore&&clock.pairs==1,"reject missing camera updates");
    clock.reset();require(!clock.pending&&!clock.complete,"resize discards incomplete and visible pair");
    require(clock.accept(7,1)==A::Ignore,"post-reset right cannot pair with stale left");
    for(uint64_t n=8;n<1008;n+=2){require(clock.accept(n,0)==A::StageLeft,"sequence left");require(clock.accept(n+1,1)==A::Complete,"sequence right");}
}
void gpuTests(){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP device");
    auto vertex=compile("vs","vs_5_0"),pixel=compile("ps","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs),"VS");
    check(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps),"PS");
    constexpr unsigned w=128,h=32;using Color=std::array<float,4>;
    std::vector<Color> left(w*h),right(w*h);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){left[y*w+x]={float(x+.5f)/w,.25f,0,1};right[y*w+x]={0,.75f,float(x+.5f)/w,1};}
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> images[2];ComPtr<ID3D11ShaderResourceView> views[2];
    for(int eye=0;eye<2;++eye){D3D11_SUBRESOURCE_DATA data{eye?right.data():left.data(),w*sizeof(Color),0};check(device->CreateTexture2D(&desc,&data,&images[eye]),"eye texture");check(device->CreateShaderResourceView(images[eye].Get(),nullptr,&views[eye]),"eye SRV");}
    desc.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> rtv;
    check(device->CreateTexture2D(&desc,nullptr,&target),"output texture");check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"output RTV");
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&desc,nullptr,&readback),"readback");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;check(device->CreateBuffer(&bd,nullptr,&constants),"constants");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;ComPtr<ID3D11SamplerState> sampler;check(device->CreateSamplerState(&sd,&sampler),"sampler");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"raster");
    D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto* out=rtv.Get();context->OMSetRenderTargets(1,&out,nullptr);auto* smp=sampler.Get();context->PSSetSamplers(0,1,&smp);auto* cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);
    ID3D11ShaderResourceView* inputs[]{views[0].Get(),views[1].Get()};context->PSSetShaderResources(0,2,inputs);
    for(int swap=0;swap<2;++swap)for(float convergence:{0.f,.02f,-.02f}){
        const float values[]{convergence,float(swap),0,0};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);context->Draw(3,0);context->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"map output");
        for(unsigned x=0;x<w;++x){const auto* color=reinterpret_cast<const Color*>(static_cast<const unsigned char*>(mapped.pData)+16*mapped.RowPitch)+x;
            const bool rightEye=(x>=w/2)^(swap!=0);float u=(float(x%(w/2))+.5f)/float(w/2)+(rightEye?-convergence:convergence);
            if(u<0||u>1)require((*color)[0]==0&&(*color)[1]==0&&(*color)[2]==0,"convergence border black");
            else {u=std::clamp(u,.5f/w,1.f-.5f/w);require(std::abs((*color)[1]-(rightEye?.75f:.25f))<1e-5f,"correct eye half");require(std::abs((*color)[rightEye?2:0]-u)<1e-4f,"full image squeezed into half, convergence applied");}
            require((*color)[3]==1,"opaque SBS");
        }context->Unmap(readback.Get(),0);
    }
}
}
int main(){try{cameraTests();gpuTests();printf("PASS: %u camera, pair integrity and GPU SBS checks\n",checks);return 0;}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
