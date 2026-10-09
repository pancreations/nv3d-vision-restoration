#pragma once
#include "sources.h"

namespace vision {
// DXGI returns desktop pixels, including ordinary covering windows. Window
// targets retain their client size; pixels outside the chosen output are black.
// One acquired frame is owned until release(); all GPU copies must be queued first.
struct CapturedPicture {
    ComPtr<ID3D11Texture2D> texture;
    unsigned left=0,top=0,width=0,height=0;
    double timestamp=0;
    double acquiredAt=0; // host QPC when the capture API delivered this frame
    Encoding encoding=Encoding::SRGB;
};
class GpuCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    GpuCapture(ID3D11Device* device,const SourceConfig& config);
    ~GpuCapture();
    // A bounded wait lets capture workers sleep until a frame arrives. Zero is nonblocking.
    bool next(CapturedPicture& picture,unsigned timeoutMs=0);
    void release();
    std::string backend() const;
};
}
