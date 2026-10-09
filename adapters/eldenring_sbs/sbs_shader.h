// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace vision::elden {
inline constexpr char shader[]=R"HLSL(
cbuffer Settings:register(b0) { float convergence; float reverseEyes; float padding0; float padding1; };
Texture2D leftImage:register(t0); Texture2D rightImage:register(t1);
SamplerState linearClamp:register(s0);
struct Vertex { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
Vertex vs(uint id:SV_VertexID) {
    Vertex o;o.uv=float2((id<<1)&2,id&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;
}
float4 ps(Vertex p):SV_TARGET {
    bool right=p.uv.x>=.5;float2 uv=float2(frac(p.uv.x*2),p.uv.y);
    if(reverseEyes>.5)right=!right;
    // A uniform image translation sets the screen plane for the parallel
    // cameras. It does not synthesize geometry or sample a depth buffer.
    uv.x+=right?-convergence:convergence;
    if(uv.x<0||uv.x>1)return float4(0,0,0,1);
    float4 color=right?rightImage.SampleLevel(linearClamp,uv,0):leftImage.SampleLevel(linearClamp,uv,0);
    return float4(color.rgb,1);
}
)HLSL";
}
