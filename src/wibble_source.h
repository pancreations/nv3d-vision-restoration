#pragma once
#include "core.h"
#include "platform.h"
#include <memory>
#include <stop_token>

namespace vision::wibble {
struct MirrorLayout {
    unsigned width=0,height=0,subresource=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    Encoding encoding=Encoding::SRGB;
};
// Validate the explicit eye's selected mip/array slice and map its copied bytes
// to a concrete texture format. The presenter performs any SRGB decoding.
MirrorLayout describeMirror(const D3D11_TEXTURE2D_DESC& texture,const D3D11_SHADER_RESOURCE_VIEW_DESC& view);
bool compatibleMirrors(const MirrorLayout& left,const MirrorLayout& right);
// Compositor timing is a pacing hint, not the identity of either mirror image.
// Keep polling at 120 Hz if timing is missing or stops advancing. A completed
// copy must not be rejected because the compositor advanced while it ran.
class MirrorCadence {
    bool haveCopy_=false,timedCopy_=false;
    double copiedAt_=0;
    uint32_t frame_=0;
public:
    bool begin(double now,bool timed,uint32_t frame);
};
struct Picture {
    ComPtr<ID3D11Texture2D> texture;
    unsigned width=0,height=0;Encoding encoding=Encoding::SRGB;
    double acquiredAt=0;uint64_t frameId=0;
};
class SourceReader {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    SourceReader();~SourceReader();SourceReader(const SourceReader&)=delete;SourceReader& operator=(const SourceReader&)=delete;
    void open(ID3D11Device* device,ID3D11DeviceContext* context);
    // Nonblocking when waiting for SteamVR or a new compositor frame. A true
    // result owns a completed 2-slice snapshot, L=0/R=1, valid until next().
    // The caller must copy/publish it before asking for another picture.
    bool next(std::stop_token stop,Picture& picture);
    std::string status()const;
    bool requiresElevation()const;
};
}
