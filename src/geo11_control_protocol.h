// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace vision::geo11 {
constexpr uint32_t controlsMagic=0x31435647;
inline std::wstring controlsName(uint32_t pid){return L"Local\\VisionRestoration.Geo11Controls."+std::to_wstring(pid);}
// A separate channel keeps the presentation/emitter v1 ABI unchanged.
struct Controls {
    uint32_t magic,version,pid;
    uint32_t key,modifiers; // configured reload key; Ctrl=1, Shift=2, Alt=4
    volatile int32_t request,acknowledged;
};
}
