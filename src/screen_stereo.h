// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "platform.h"
#include "core.h"
namespace vision {
// Draws the whole-screen conversion: both eyes of a picture through a nearness map, side by side,
// and the picture's downscale for the depth network.
class ScreenStereoRenderer {
    ComPtr<ID3D11VertexShader> vs_;ComPtr<ID3D11PixelShader> ps_,down_,search_,apply_;ComPtr<ID3D11Buffer> params_;ComPtr<ID3D11SamplerState> sampler_,nearest_;
    // Both eyes' source offsets at the depth search resolution, side by side.
    ComPtr<ID3D11Texture2D> source_;ComPtr<ID3D11RenderTargetView> sourceTarget_;ComPtr<ID3D11ShaderResourceView> sourceView_;unsigned sourceWidth_=0,sourceHeight_=0;
    void constants(ID3D11DeviceContext* context,const float* values);
public:
    void init(ID3D11Device* device);
    // Renders the left eye into the left half and the right eye into the right half of `target`
    // (2 x eyeWidth by height). Without a nearness map both eyes get the picture unchanged.
    // settings.depthScale below 1 runs the depth search on a smaller grid; the eyes stay eyeWidth x height.
    void render(ID3D11DeviceContext* context,ID3D11ShaderResourceView* picture,ID3D11ShaderResourceView* nearness,ID3D11RenderTargetView* target,unsigned eyeWidth,unsigned height,const ScreenSettings& settings);
    // Shrinks `picture` (which has a mip chain) into `target` of the given size, encoding scRGB to sRGB when hdr.
    void downscale(ID3D11DeviceContext* context,ID3D11ShaderResourceView* picture,ID3D11RenderTargetView* target,unsigned width,unsigned height,float mipLevel,bool hdr,float sdrWhiteLevel=1);
};
}
