// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_wave.h"
namespace vision::rp2040 {
Wave makeWave(ActionKind kind,Eye eye) {
    Wave w;
    if((eye!=Eye::Left&&eye!=Eye::Right)||(kind!=ActionKind::Open&&kind!=ActionKind::Close))return w;
    std::array<uint32_t,3> durations{};size_t n=3;
    if(kind==ActionKind::Open) {
        if(eye==Eye::Left){durations={43,0,0};n=1;}
        else durations={23,46,31};
    } else durations=eye==Eye::Left?std::array<uint32_t,3>{23,21,24}:std::array<uint32_t,3>{23,78,40};
    // Four-instruction PIO loop has four cycles of edge-to-edge overhead.
    // At 1 MHz each low 31-bit field is requested microseconds minus four.
    for(size_t i=0;i<n;++i) {
        w.words[w.count++]=(i%2?0u:0x80000000u)|(durations[i]-4);
        w.durationUs+=durations[i];
    }
    w.words[w.count++]=0; // final LOW, then block on an empty FIFO
    w.durationUs+=4;return w;
}
}
