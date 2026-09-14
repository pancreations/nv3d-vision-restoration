// SPDX-License-Identifier: GPL-3.0-or-later
// Shared-memory frame channel: the Blender add-on (blender/vision_stereo) publishes its 3D viewport as a
// side-by-side stereo pair plus where that viewport sits on screen, and Vision Restoration presents it in a
// click-through window laid exactly over the viewport. The sender creates both mappings; the host only
// opens them. blender/vision_stereo/channel.py mirrors this layout, so change both together.
#pragma once
#include <cstdint>
namespace vision::frames {
constexpr wchar_t headerName[]=L"Local\\VisionRestoration.FrameChannel.v2";
constexpr wchar_t frameEventName[]=L"Local\\VisionRestoration.FrameChannel.v2.Frame";
// Pixel data lives in headerName + ".Data.<senderPid>.<generation>", slotCount * slotBytes long.
constexpr uint32_t magic=0x4233564E; // "NV3B"
constexpr uint32_t version=2;
constexpr uint32_t slotCount=3;
enum Format : uint32_t { FormatRGBA8BottomUp=0 }; // OpenGL readback order
enum HostState : int32_t { HostIdle=0, HostWaitingForEmitter=1, HostPresenting=2 };
// The sender never writes the newest slot. seq is odd while a slot is being written; a reader
// that sees seq change across its copy discards the frame.
struct Slot { uint32_t seq,width,height,packing; uint64_t frameId; };
struct Header {
    uint32_t magic,version;
    // Sender block.
    uint32_t senderPid,generation; int64_t senderHeartbeatQpc;
    uint64_t slotBytes;
    uint32_t format; int32_t latestSlot; // -1 = nothing published yet
    int32_t active;  // the 3D viewport is switched on
    int32_t visible; // Blender is not minimised and no other window covers the viewport
    int32_t viewLeft,viewTop,viewWidth,viewHeight; // the viewport image's screen rectangle, virtual-screen pixels
    Slot slots[slotCount];
    char senderName[64];
    // Host block.
    uint32_t hostPid; int32_t hostState; int64_t hostHeartbeatQpc;
    uint64_t framesReceived,framesTorn;
    int32_t outputRunning,emitterReady;
    char hostMessage[160];
};
static_assert(sizeof(Slot)==24&&sizeof(Header)==400,"frame channel layout is shared with channel.py");
}
