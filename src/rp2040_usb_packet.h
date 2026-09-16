// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rp2040_protocol.h"
#include <cmath>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace vision::rp2040 {
// TinyUSB terminates each full-size reply with a zero-length USB packet.
// That terminator may be returned by the next bulk read. Ignore it, keeping
// one deadline for the whole reply rather than renewing it after every ZLP.
// read fills a Packet, returns its byte count, and throws on transport errors.
template<class Read,class Now>
Packet receiveUsbReply(Read&& read,Now&& now,std::stop_token stop={}) {
    Packet packet{};
    const double deadline=now()+20000;
    for(;;) {
        if(stop.stop_requested())throw std::runtime_error("RP2040 transfer cancelled");
        const double remaining=deadline-now();
        if(remaining<=0)throw std::runtime_error("RP2040 USB IN reply timed out");
        const auto count=read(packet,unsigned(std::ceil(remaining/1000)));
        if(count==packet.size())return packet;
        if(count!=0)throw std::runtime_error("RP2040 USB IN short reply: "+std::to_string(count)+" bytes (expected 64)");
    }
}
}
