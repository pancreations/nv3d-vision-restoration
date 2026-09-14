// SPDX-License-Identifier: GPL-3.0-or-later
// Depth-based stereo for games that render a single camera: each eye is resampled from the
// finished frame using the game's own depth buffer. Parallax follows two-camera geometry,
// p = separation * (1 - convergenceDistance / distance). A perspective depth buffer stores a value
// proportional to 1 / distance (reversed-Z, infinite far plane: exactly near / distance), so
// p = separation * (1 - depth / convergenceDepth) is linear in the stored value and needs neither
// near nor far plane. Surfaces nearer than the convergence plane come out of the screen; that side
// is compressed smoothly (tanh) so it never exceeds popOut times the separation.
//
// Each output pixel searches along its row for the source pixel that lands on it, nearest surface
// first, which keeps foreground edges in front of what they hide. Where the game never saw the
// background (a disocclusion), the background beside the edge is continued rather than the edge.
#pragma once
namespace vision {
inline constexpr char depthStereoShader[]=R"HLSL(
// stereo: x eye (-1 left, +1 right), y separation at infinity (fraction of the width, both eyes),
//         z convergence (depth value at zero parallax, reversed), w pop-out limit (fraction of y)
// source: x 1 = reversed depth, y 1 = depth rendered this frame, zw depth texture size
// view:   x 1 = show depth instead of the image, y host eye offset (fraction of width), zw 1/output size
// band:   x stereo area height (0 = whole output), y its center from the top
cbuffer P:register(b0){float4 stereo;float4 source;float4 view;float4 band;};
Texture2D image:register(t0);Texture2D<float> depthMap:register(t1);SamplerState smp:register(s0);
struct V{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
// 1 at the near plane, 0 at infinity.
float nearness(float2 uv){
    int2 size=int2(source.zw);int2 p=clamp(int2(uv*source.zw),int2(0,0),size-1);
    float d=depthMap.Load(int3(p,0));return source.x>.5?d:1-d;
}
// Half of the screen parallax for a surface at this nearness, as a fraction of the width.
float shift(float n){
    float halfSeparation=stereo.y*.5;float r=n/max(stereo.z,1e-6);
    if(r<=1)return halfSeparation*(1-r);
    float pop=max(stereo.w,1e-3);return -halfSeparation*pop*tanh((r-1)/pop);
}
float4 ps(V i):SV_TARGET{
    float2 uv=i.uv;
    float h=band.x>0?band.x:1;if(h<.999){float2 local=(uv-float2(.5,band.y))/h+.5;if(any(local<0)||any(local>1))return float4(0,0,0,1);uv=local;}
    uv.x-=stereo.x*view.y;
    if(uv.x<0||uv.x>1)return float4(0,0,0,1);
    if(view.x>.5){float g=source.y>.5?sqrt(saturate(nearness(uv)/max(stereo.z,1e-6)*.5)):0;return float4(g,g,g,1);} // mid grey = screen depth
    if(source.y<.5||stereo.y<=0)return image.SampleLevel(smp,uv,0);
    // This eye shows the source at uv.x - eye * t, where t is the half-parallax of that source.
    // March t from the nearest allowed surface to infinity; the first crossing is the visible one.
    float s=stereo.x;float lo=-stereo.y*.5*stereo.w,hi=stereo.y*.5;
    float prev=lo;float x=uv.x-s*hi;
    [loop]for(int k=1;k<=32;k++){
        float t=lerp(lo,hi,k/32.);
        if(shift(nearness(float2(uv.x-s*t,uv.y)))-t<=0){
            float a=prev,b=t;
            [loop]for(int r=0;r<6;r++){float m=(a+b)*.5;if(shift(nearness(float2(uv.x-s*m,uv.y)))-m<=0)b=m;else a=m;}
            float ta=shift(nearness(float2(uv.x-s*a,uv.y))),tb=shift(nearness(float2(uv.x-s*b,uv.y)));
            // A depth jump across the tiny bracket is an occlusion edge: continue the background (a)
            // instead of smearing the foreground edge (b) into the hole.
            x=uv.x-s*(abs(ta-tb)>4*(b-a)+2*view.z?a:b);
            break;
        }
        prev=t;
    }
    return image.SampleLevel(smp,float2(x,uv.y),0);
}
)HLSL";
}
