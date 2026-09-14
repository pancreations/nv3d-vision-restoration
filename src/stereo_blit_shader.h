#pragma once

namespace vision {
inline constexpr char stereoBlitShader[]=R"HLSL(
cbuffer P:register(b0){float4 area;float4 adjustment;float4 band;};
Texture2D image:register(t0);SamplerState smp:register(s0);
struct V{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float4 ps(V i):SV_TARGET{
    float2 uv=i.uv;
    // Stereo area (band.x = height fraction, band.y = center from the top; 0 = whole output):
    // scale the eye image about the band center and keep everything outside black.
    float h=band.x>0?band.x:1;if(h<.999){float2 local=(uv-float2(.5,band.y))/h+.5;if(any(local<0)||any(local>1))return float4(0,0,0,1);uv=local;}
    uv.x+=adjustment.x;
    if(uv.x<0||uv.x>1)return float4(0,0,0,1);
    // Clamp inside this eye's half texel centers, not the whole packed texture.
    float2 p=clamp(area.xy+uv*area.zw,area.xy+adjustment.yz,area.xy+area.zw-adjustment.yz);
    return image.SampleLevel(smp,p,0);
}
)HLSL";
}
