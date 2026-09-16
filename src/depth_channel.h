// SPDX-License-Identifier: GPL-3.0-or-later
// Shared-memory channel between the app (host) and the depth helper process (VisionDepth.exe).
// The host writes a small BGRA picture of the captured screen; the helper answers with the
// network's relative depth of every pixel of that picture, larger meaning nearer.
#pragma once
#include <cstdint>
namespace vision::depth {
inline constexpr uint32_t magic=0x44335056;   // "VP3D"
inline constexpr uint32_t version=1;
inline constexpr uint32_t patch=14;           // the network takes multiples of its patch size
inline constexpr uint32_t maxSide=1036;       // 74 patches; both buffers are sized for a square of this side
inline constexpr uint32_t headerBytes=4096;
inline constexpr uint64_t pictureBytes=uint64_t(maxSide)*maxSide*4;   // BGRA8
inline constexpr uint64_t depthBytes=uint64_t(maxSide)*maxSide*4;     // float32
inline constexpr uint64_t channelBytes=headerBytes+pictureBytes+depthBytes;
enum State : uint32_t { Starting=0, Ready=1, Failed=2 };
struct Header {
    uint32_t magic=0,version=0;
    uint32_t hostPid=0,helperPid=0;
    uint32_t sourceWidth=0,sourceHeight=0;   // host: size of the captured picture (its aspect ratio)
    uint32_t maxNetSide=0;                   // host: longest side the network input may have
    uint32_t netWidth=0,netHeight=0;         // helper: input size it settled on
    uint32_t state=Starting;
    uint32_t pictureSeq=0;                   // host: incremented after a complete picture write
    uint32_t pictureTaken=0;                 // helper: pictureSeq it has finished reading
    uint32_t depthSeq=0;                     // helper: incremented after a complete depth write
    uint32_t depthBusy=0;                    // helper: 1 while the depth buffer is being written
    double pictureTimestamp=0;               // host: capture time of the picture (qpc seconds)
    double depthTimestamp=0;                 // helper: pictureTimestamp of the picture the depth belongs to
    double inferenceMs=0,loadMs=0;           // helper: last network run, model load
    char message[256]{};                     // helper: model and device, or the error
};
static_assert(sizeof(Header)<=headerBytes);
inline constexpr const wchar_t* mappingPrefix=L"Local\\VisionDepth.Channel.";
inline constexpr const wchar_t* pictureEventPrefix=L"Local\\VisionDepth.Picture.";
inline constexpr const wchar_t* depthEventPrefix=L"Local\\VisionDepth.Depth.";
}
