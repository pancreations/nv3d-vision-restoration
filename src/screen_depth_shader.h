// SPDX-License-Identifier: GPL-3.0-or-later
// Whole-screen conversion shaders. `ps` resamples one eye of the captured picture through the
// network's nearness map: the two-camera search of adapters/depth_stereo_shader.h, with the map
// sampled bilinearly because it is only a few hundred pixels across. `psDown` shrinks the picture
// to the network input through the picture's mip chain.
#pragma once
namespace vision {
inline constexpr char screenDepthShader[]=R"HLSL(
// stereo: x eye (-1 left, +1 right), y separation at infinity (fraction of the width, both eyes),
//         z convergence (nearness at zero parallax), w pop-out limit (fraction of y)
// mode:   x 1 = a nearness map is bound (else the picture is copied), y 1 = show the nearness instead
//         of the picture, z search steps, w mip level for the downscale
// view:   xy 1 / eye image size, z 1 = scRGB picture, w captured SDR white level
cbuffer P:register(b0){float4 stereo;float4 mode;float4 view;};
Texture2D image:register(t0);Texture2D<float> nearMap:register(t1);SamplerState smp:register(s0);
struct V{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float3 srgb(float3 c){return lerp(12.92*c,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float nearness(float2 uv){return nearMap.SampleLevel(smp,uv,0);}
// Half of the screen parallax for a surface at this nearness, as a fraction of the width.
float shift(float n){
    float halfSeparation=stereo.y*.5;float r=n/max(stereo.z,1e-6);
    if(r<=1)return halfSeparation*(1-r);
    float pop=max(stereo.w,1e-3);return -halfSeparation*pop*tanh((r-1)/pop);
}
float4 ps(V i):SV_TARGET{
    float2 uv=i.uv;
    if(mode.y>.5){float g=saturate(nearness(uv)/max(stereo.z,1e-6)*.5);return float4(g,g,g,1);} // mid grey = screen depth
    if(mode.x<.5||stereo.y<=0)return image.SampleLevel(smp,uv,0);
    // This eye shows the source at uv.x - eye * t, where t is the half-parallax of that source.
    // March t from the nearest allowed surface to infinity; the first crossing is the visible one.
    float s=stereo.x;float lo=-stereo.y*.5*stereo.w,hi=stereo.y*.5;
    int steps=clamp(int(mode.z),4,64);
    float prev=lo;float x=uv.x-s*hi;
    [loop]for(int k=1;k<=steps;k++){
        float t=lerp(lo,hi,float(k)/float(steps));
        if(shift(nearness(float2(uv.x-s*t,uv.y)))-t<=0){
            float a=prev,b=t;
            [loop]for(int r=0;r<5;r++){float m=(a+b)*.5;if(shift(nearness(float2(uv.x-s*m,uv.y)))-m<=0)b=m;else a=m;}
            float ta=shift(nearness(float2(uv.x-s*a,uv.y))),tb=shift(nearness(float2(uv.x-s*b,uv.y)));
            // A depth jump across the tiny bracket is an occlusion edge: continue the background (a)
            // instead of smearing the foreground edge (b) into the hole.
            x=uv.x-s*(abs(ta-tb)>4*(b-a)+2*view.x?a:b);
            break;
        }
        prev=t;
    }
    return image.SampleLevel(smp,float2(x,uv.y),0);
}
float4 psDown(V i):SV_TARGET{
    float3 c=image.SampleLevel(smp,i.uv,mode.w).rgb;
    if(view.z>.5)c=srgb(saturate(c/max(view.w,.01))); // Match the captured display's actual SDR white.
    return float4(c,1);
}
)HLSL";
}
