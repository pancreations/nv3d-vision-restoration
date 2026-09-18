#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <stop_token>

namespace vision::direct {
// Provider SDK. Call on the producer's immediate-context thread after rendering
// an eye. S_FALSE means one eye was retained; S_OK means the pair was published.
// WAS_STILL_DRAWING means retry the SAME submission: nothing was consumed.
// Every pair needs a monotonically increasing ID and explicit eye identity.
// This API transports rendered stereo; it does not generate a second camera.
// Read-only readiness check for the game selection UI; does not claim a reader.
bool available(uint32_t channel);

enum class Eye : uint32_t { Left,Right };
enum class Encoding : uint32_t { SRGB,LinearScRGB,PQ2020 };
class Producer {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Producer();~Producer();
    Producer(const Producer&)=delete;Producer& operator=(const Producer&)=delete;
    HRESULT open(ID3D11Device* device,uint32_t channel=GetCurrentProcessId());
    HRESULT submit(ID3D11Texture2D* texture,UINT subresource,Eye eye,uint64_t pairId,
        Encoding encoding=Encoding::SRGB,std::stop_token stop={},const D3D11_BOX* region=nullptr,bool deferPublication=false);
    bool connected()const;
    // An adapter may stage copies before Present, then expose the pair only
    // after that Present succeeds. S_FALSE means the partner eye is pending.
    HRESULT commit();
    // Explicitly abandon an incomplete pair on a renderer reset. Never changes
    // a pair already handed to the consumer.
    HRESULT reset(bool discardUnreadIfDisconnected=false);
};

// A Reader holds producer ownership until release(). Only one consumer per
// channel is allowed. A stopped source leaves its last complete pair visible.
class Reader {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Reader();~Reader();
    Reader(const Reader&)=delete;Reader& operator=(const Reader&)=delete;
    HRESULT open(ID3D11Device* device,uint32_t channel);
    HRESULT acquire(); // S_FALSE: no pair yet; broken pipe: producer closed
    void release();
    ID3D11Texture2D* texture()const;
    uint64_t pairId()const;
    Encoding encoding()const;
};
}
