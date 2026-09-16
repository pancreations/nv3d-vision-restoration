// SPDX-License-Identifier: GPL-3.0-or-later
// Shared-memory contract between Vision Restoration (host, owns the emitter) and the
// in-game presenter (owns the physical swap chain). Plain data only: this header
// is compiled into both binaries. The hook never talks USB; the host never touches the game.
#pragma once
#include <cstdint>
namespace vision::sync {
constexpr wchar_t mappingName[]=L"Local\\VisionRestoration.GameSync.v1";
constexpr wchar_t frameEventName[]=L"Local\\VisionRestoration.GameSync.v1.Frame";
constexpr uint32_t magic=0x4433564E; // "NV3D"
constexpr uint32_t version=1;
constexpr uint32_t ringSize=256;
enum Api : uint32_t { ApiUnknown=0, ApiD3D11=1, ApiD3D12=2 };
enum HostState : int32_t { HostUnknown=0, HostReady=1, HostPausedByOutput=2, HostEmitterNotReady=3 };
// eye: the slot of the sequence this present carries. Left/Right: 0 = left half, 1 = right
// half. A hook that sets HookSequences in hookFlags follows the host's four-slot sequences
// and records slot indices 0..3 (see slotEye); the host derives the eye and the trigger.
struct FrameRecord { uint32_t presentId; int32_t eye; };
// HookSequences: the hook presents the host's four-slot sequences.
// HookPhaseLocked: every slot is shown on the refresh its number implies ((refresh - slot) mod cycle
// stays constant), so once that phase is known the host may drive the glasses from the refresh clock.
// HookPatterns: the hook also presents encoded hold/black sequences (codes 16..47, up to 16 slots).
// HookRefreshSlots: slot = physical refresh counter modulo sequence length.
// The host must not learn a different phase from late frames during recovery.
// This adds a capability without changing the v1 shared-memory layout.
enum HookFlags : uint32_t { HookSequences=1, HookPhaseLocked=2, HookPatterns=4, HookRefreshSlots=8 };
inline constexpr uint64_t nextDisplayRefresh(uint32_t submitted,uint32_t shown,uint32_t shownRefresh,uint32_t clockRefresh){
    const uint32_t queued=submitted-shown;
    const uint64_t current=clockRefresh>shownRefresh?clockRefresh:shownRefresh;
    return current+uint64_t(queued)+1;
}
// Sequence codes: 0 Left/Right, 1 Left/Black/Right/Black, 2 Left/Left/Right/Right, 16 + (hold-1)*8 + black
// for any other hold/black pair. Mirrors vision::makeSequence without depending on core.
inline constexpr unsigned sequenceHold(int32_t s){return s==2?2u:s>=16?unsigned((s-16)/8)+1:1u;}
inline constexpr unsigned sequenceBlack(int32_t s){return s==1?1u:s>=16?unsigned((s-16)%8):0u;}
inline constexpr bool sequenceKnown(int32_t s){return (s>=0&&s<=2)||(s>=16&&s<48&&sequenceHold(s)+sequenceBlack(s)<=8);}
inline constexpr unsigned slotsPerFrame(int32_t sequence){return 2*(sequenceHold(sequence)+sequenceBlack(sequence));}
// 0 = left, 1 = right, 2 = black. Mirrors vision::sequenceSlot without depending on core.
inline constexpr int slotEye(int32_t sequence,unsigned slot){const unsigned run=sequenceHold(sequence)+sequenceBlack(sequence);const unsigned j=slot%run;return j<sequenceHold(sequence)?int((slot/run)%2):2;}
struct Shared {
    uint32_t magic, version;
    // Host block. Written by Vision Restoration, read by the hook.
    uint32_t hostSeq; uint32_t hostPid; int64_t hostHeartbeatQpc;
    int32_t enabled;    // stereo allowed; Geo11 output uses mono when disabled
    int32_t packing;    // 0 side-by-side (left | right), 1 top / bottom (left over right)
    int32_t swapHalves; // 1 = the right half holds the left eye
    int32_t convergenceMicrounits; // eye-width offset * 1e6; formerly reserved, zero in older hosts
    int32_t bandHeightMicrounits; // stereo area height * 1e6 (0 = whole output, older hosts)
    int32_t bandCenterMicrounits; // stereo area center from the top * 1e6
    int32_t hostState;  // HostState: why the host has or has not enabled the hook (0 = older host)
    int32_t sequence;   // sequence code (see slotsPerFrame); presents per game frame = slotsPerFrame; formerly reserved, zero in older hosts
    // Hook block. Written by the game process under a seqlock (seq is odd while writing).
    volatile uint32_t seq;
    uint32_t gamePid; int64_t gameHeartbeatQpc; uint64_t hwnd;
    uint32_t api, width, height, backBuffers, format, active, composed, statsValid;
    uint64_t presentCount, presentRefreshCount, syncRefreshCount; int64_t syncQpc;
    uint64_t frames, nativePresents, presentErrors;
    uint32_t ringHead; uint32_t hookFlags; // HookFlags; formerly reserved, zero in older hooks
    FrameRecord ring[ringSize]; // ring[(ringHead-1) % ringSize] is the newest present
    char gameName[64]; char message[128];
};
}
