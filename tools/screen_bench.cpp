// GPU cost of the whole-screen conversion at 4K on the installed GPU: both eyes of a 3840 x 2160
// picture through a 518 x 294 nearness map, timed with GPU timestamps for several step counts.
// Prints milliseconds per stereo pair. Nothing optical.
#include "screen_stereo.h"
#include <cmath>
#include <iostream>
#include <vector>
using namespace vision;
int main(){try{
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Hardware device");
    {ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;if(SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))){DXGI_ADAPTER_DESC d{};adapter->GetDesc(&d);std::cout<<"GPU: "<<utf8(d.Description)<<"\n";}}
    ScreenStereoRenderer renderer;renderer.init(device.Get());
    const unsigned W=3840,H=2160,DW=518,DH=294;
    std::vector<uint32_t> pixels(size_t(W)*H);for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++)pixels[size_t(y)*W+x]=0xff000000|((x*255/W)<<16)|((y*255/H)<<8)|((x^y)&255);
    // A scene: a far gradient, three near rectangles at different nearness, a nearest small box.
    std::vector<float> nearness(size_t(DW)*DH);for(unsigned y=0;y<DH;y++)for(unsigned x=0;x<DW;x++){float n=.15f+.2f*float(y)/DH;if(x>60&&x<200&&y>40&&y<200)n=.6f;if(x>250&&x<450&&y>80&&y<260)n=.8f;if(x>300&&x<360&&y>120&&y<180)n=1.f;nearness[size_t(y)*DW+x]=n;}
    D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(),W*4,0};ComPtr<ID3D11Texture2D> picture;check(device->CreateTexture2D(&d,&data,&picture),"Picture");ComPtr<ID3D11ShaderResourceView> pictureView;check(device->CreateShaderResourceView(picture.Get(),nullptr,&pictureView),"Picture view");
    D3D11_TEXTURE2D_DESC n=d;n.Width=DW;n.Height=DH;n.Format=DXGI_FORMAT_R32_FLOAT;D3D11_SUBRESOURCE_DATA nd{nearness.data(),DW*4,0};ComPtr<ID3D11Texture2D> nearMap;check(device->CreateTexture2D(&n,&nd,&nearMap),"Nearness");ComPtr<ID3D11ShaderResourceView> nearView;check(device->CreateShaderResourceView(nearMap.Get(),nullptr,&nearView),"Nearness view");
    D3D11_TEXTURE2D_DESC t=d;t.Width=W*2;t.Usage=D3D11_USAGE_DEFAULT;t.BindFlags=D3D11_BIND_RENDER_TARGET;ComPtr<ID3D11Texture2D> target;check(device->CreateTexture2D(&t,nullptr,&target),"Target");ComPtr<ID3D11RenderTargetView> targetView;check(device->CreateRenderTargetView(target.Get(),nullptr,&targetView),"Target view");
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};ComPtr<ID3D11Query> disjoint;check(device->CreateQuery(&qd,&disjoint),"Disjoint query");qd.Query=D3D11_QUERY_TIMESTAMP;ComPtr<ID3D11Query> begin,end;check(device->CreateQuery(&qd,&begin),"Begin");check(device->CreateQuery(&qd,&end),"End");
    auto time=[&](const ScreenSettings& s,bool withDepth){
        context->Begin(disjoint.Get());context->End(begin.Get());renderer.render(context.Get(),pictureView.Get(),withDepth?nearView.Get():nullptr,targetView.Get(),W,H,s);context->End(end.Get());context->End(disjoint.Get());context->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};while(context->GetData(disjoint.Get(),&dj,sizeof(dj),0)==S_FALSE)Sleep(0);UINT64 t0=0,t1=0;while(context->GetData(begin.Get(),&t0,sizeof(t0),0)==S_FALSE)Sleep(0);while(context->GetData(end.Get(),&t1,sizeof(t1),0)==S_FALSE)Sleep(0);
        return dj.Disjoint?-1.:double(t1-t0)*1000/double(dj.Frequency);};
    ScreenSettings s;s.separation=.02f;s.convergence=.85f;s.popOut=.5f;s.depth=true;
    std::cout<<"Copy without depth: "<<time(s,false)<<" ms\n";
    for(unsigned steps:{8u,16u,24u,32u}){s.steps=steps;time(s,true);time(s,true);double sum=0;for(int i=0;i<8;i++)sum+=time(s,true);std::cout<<"Steps "<<steps<<": "<<sum/8<<" ms per pair (both eyes, 3840x2160 each)\n";}
    s.steps=24;s.separation=.05f;std::cout<<"Steps 24 at 5% separation: "<<time(s,true)<<" ms\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
