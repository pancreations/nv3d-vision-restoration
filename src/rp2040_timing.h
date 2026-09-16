// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.h"
#include "rp2040_protocol.h"
#include <cmath>
namespace vision {
inline bool rp2040TimingSupported(double refresh,Sequence sequence,bool extended,bool fast=false) {
    if(!sequenceValid(sequence)||!std::isfinite(refresh))return false;
    if(!extended)return sequence==Sequence::Alternating&&refresh>=119.5&&refresh<=120.5;
    if(refresh<59.5||refresh>240.5||sequenceHold(sequence)+sequenceBlack(sequence)>4)return false;
    const double period=1000000.0/sequenceEmitterHz(refresh,sequence);
    return period>=(fast?rp2040::Scheduler::minPeriodUs:8300)&&period<=rp2040::Scheduler::maxPeriodUs;
}
struct Rp2040Phase { double delayUs; bool flipEye; };
inline Rp2040Phase rp2040Phase(double phase,double refresh,Sequence sequence) {
    const double period=periodUs(sequenceEmitterHz(refresh,sequence));
    const double full=wrapPhase(phase,2*period);
    // The second half of the cycle is equivalent to exchanging eyes. Keep
    // targets within one opening interval while preserving optical eye parity.
    return {full>=period?full-period:full,full>=period};
}
}
