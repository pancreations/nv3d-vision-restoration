// Whole-screen conversion shader on WARP: a near bar must land crossed (right in the left eye, left
// in the right eye), a run without depth must copy the picture into both eyes, and the depth view
// must show the bar brighter than its surroundings. Rendered pixels only; nothing optical.
#include "screen_stereo.h"
#include <iostream>
#include <vector>
#include <cmath>
using namespace vision;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"WARP device");
    ScreenStereoRenderer renderer;renderer.init(device.Get());
    const unsigned W=96,H=48,DW=24,DH=12;
    std::vector<uint32_t> pixels(W*H,0xff202020);for(unsigned y=0;y<H;y++)for(unsigned x=40;x<56;x++)pixels[y*W+x]=0xffffffff; // a white bar, BGRA
    // The bar is the nearest thing. The nearness region is wider than the bar (texels 9-14, pixels 36-60)
    // because the map is sampled bilinearly: its one-texel ramp must lie outside the bar's own edges.
    std::vector<float> nearness(DW*DH,0.f);for(unsigned y=0;y<DH;y++)for(unsigned x=9;x<15;x++)nearness[y*DW+x]=1.f;
    D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),W*4,0};ComPtr<ID3D11Texture2D> picture;check(device->CreateTexture2D(&d,&data,&picture),"Picture");ComPtr<ID3D11ShaderResourceView> pictureView;check(device->CreateShaderResourceView(picture.Get(),nullptr,&pictureView),"Picture view");
    D3D11_TEXTURE2D_DESC n=d;n.Width=DW;n.Height=DH;n.Format=DXGI_FORMAT_R32_FLOAT;D3D11_SUBRESOURCE_DATA nd{nearness.data(),DW*4,0};ComPtr<ID3D11Texture2D> nearMap;check(device->CreateTexture2D(&n,&nd,&nearMap),"Nearness");ComPtr<ID3D11ShaderResourceView> nearView;check(device->CreateShaderResourceView(nearMap.Get(),nullptr,&nearView),"Nearness view");
    D3D11_TEXTURE2D_DESC t=d;t.Width=W*2;t.Usage=D3D11_USAGE_DEFAULT;t.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> target;check(device->CreateTexture2D(&t,nullptr,&target),"Target");ComPtr<ID3D11RenderTargetView> targetView;check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"Target view");
    t.BindFlags=0;t.Usage=D3D11_USAGE_STAGING;t.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&t,nullptr,&staging),"Staging");
    auto render=[&](const ScreenSettings& s,bool withDepth){
        renderer.render(context.Get(),pictureView.Get(),withDepth?nearView.Get():nullptr,targetView.Get(),W,H,s);context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read");std::vector<uint32_t> out(W*2*H);
        for(unsigned y=0;y<H;y++)memcpy(out.data()+size_t(y)*W*2,static_cast<uint8_t*>(m.pData)+y*m.RowPitch,W*2*4);context->Unmap(staging.Get(),0);return out;
    };
    auto centroid=[&](const std::vector<uint32_t>& out,unsigned half){double sum=0;unsigned count=0;for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)if((out[size_t(y)*W*2+half*W+x]&0xff)>200){sum+=x;count++;}return count?sum/count:-1.;};
    ScreenSettings s;s.separation=.1f;s.convergence=.5f;s.popOut=.5f;s.steps=24;s.depth=true;s.showDepth=false;
    auto stereo=render(s,true);const double left=centroid(stereo,0),right=centroid(stereo,1);
    require(left>0&&right>0,"The bar is missing from an eye");
    require(left-right>3,"A near bar must be crossed: further right in the left eye than in the right eye");
    // No depth map, or depth switched off: both eyes are the picture.
    for(int mode=0;mode<2;mode++){ScreenSettings flat=s;flat.depth=mode==0;auto plain=render(flat,mode!=0);
        for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){const uint32_t source=pixels[y*W+x];require(plain[size_t(y)*W*2+x]==source&&plain[size_t(y)*W*2+W+x]==source,"Without depth both eyes must equal the picture");}}
    // The depth view: the bar is brighter than the background in both eyes.
    ScreenSettings view=s;view.showDepth=true;auto shown=render(view,true);
    for(unsigned half=0;half<2;half++)require((shown[size_t(24)*W*2+half*W+48]&0xff)>(shown[size_t(24)*W*2+half*W+10]&0xff)+64,"The depth view must show the near bar brighter");
    // A far bar (nearness 0 where the background is 1) lands uncrossed.
    for(auto& v:nearness)v=1-v;
    ComPtr<ID3D11Texture2D> farMap;check(device->CreateTexture2D(&n,&nd,&farMap),"Far nearness");ComPtr<ID3D11ShaderResourceView> farView;check(device->CreateShaderResourceView(farMap.Get(),nullptr,&farView),"Far view");
    renderer.render(context.Get(),pictureView.Get(),farView.Get(),targetView.Get(),W,H,s);context->CopyResource(staging.Get(),target.Get());
    {D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m),"Read far");std::vector<uint32_t> out(W*2*H);for(unsigned y=0;y<H;y++)memcpy(out.data()+size_t(y)*W*2,static_cast<uint8_t*>(m.pData)+y*m.RowPitch,W*2*4);context->Unmap(staging.Get(),0);
     const double l=centroid(out,0),r=centroid(out,1);require(l>0&&r>0&&r-l>3,"A far bar must be uncrossed: further left in the left eye than in the right eye");}
    // The neural network sees the same SDR colors for each Windows white level.
    for(float white:{1.f,2.5f,4.f}){
        const float color[]{white*.02f,white*.4f,white,1};D3D11_TEXTURE2D_DESC in{};in.Width=in.Height=in.MipLevels=in.ArraySize=in.SampleDesc.Count=1;in.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;in.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{color,sizeof(color),0};ComPtr<ID3D11Texture2D> hdr;check(device->CreateTexture2D(&in,&data,&hdr),"HDR network picture");ComPtr<ID3D11ShaderResourceView> hdrView;check(device->CreateShaderResourceView(hdr.Get(),nullptr,&hdrView),"HDR network view");
        auto out=in;out.Format=DXGI_FORMAT_R8G8B8A8_UNORM;out.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> converted,read;check(device->CreateTexture2D(&out,nullptr,&converted),"Network color target");ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(converted.Get(),nullptr,&view),"Network color RTV");
        out.BindFlags=0;out.Usage=D3D11_USAGE_STAGING;out.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&out,nullptr,&read),"Network color readback");
        renderer.downscale(context.Get(),hdrView.Get(),view.Get(),1,1,0,true,white);context->CopyResource(read.Get(),converted.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(read.Get(),0,D3D11_MAP_READ,0,&m),"Read network colors");
        bool correct=true;for(int channel=0;channel<3;++channel){const float linear=color[channel]/white;const float expected=linear<=.0031308f?linear*12.92f:1.055f*std::pow(linear,1/2.4f)-.055f;correct&=std::abs(static_cast<uint8_t*>(m.pData)[channel]/255.f-expected)<.006f;}
        context->Unmap(read.Get(),0);require(correct,"HDR capture changed the depth network's input colors");
    }
    std::cout<<"PASS: near/far eye disparity, unchanged image without depth, depth view, and HDR network colors at three desktop white levels\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
