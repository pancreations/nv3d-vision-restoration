// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rp2040_protocol.h"
namespace vision::rp2040 {
struct Wave { std::array<uint32_t,4> words{};size_t count=0;uint32_t durationUs=0; };
// Timings are IR protocol facts, not calibrated lens transparency times.
// Corrected close-eye mapping: b3nn/3DVisionAVR fix-3dvision-irprotocol.
Wave makeWave(ActionKind kind,Eye eye);
}
