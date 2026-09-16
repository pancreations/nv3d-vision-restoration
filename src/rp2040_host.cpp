// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_host.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace vision::rp2040 {
bool validConfig(Config config) {
    Scheduler scheduler;
    return scheduler.begin(1,1)==Result::Ok&&scheduler.configure(1,config,1)==Result::Ok;
}
const char* resultName(Result r) {
    switch(r) {
    case Result::Ok:return "OK";case Result::BadSession:return "stale session";
    case Result::BadConfig:return "unsupported timing or missed cadence";case Result::TooSoon:return "event arrived too late";
    case Result::TooFar:return "event beyond scheduling horizon";case Result::NotReady:return "device not ready";
    case Result::StaleSequence:return "duplicate event";case Result::WrongEye:return "eye sequence mismatch";
    case Result::Overlap:return "shutter intervals overlap";case Result::Timeout:return "host timeout";
    case Result::Late:return "late device executor";default:return "protocol error";
    }
}
Client::Client(Link& link,std::function<double()> now,bool allowSimulation):link_(link),now_(std::move(now)),allowSimulation_(allowSimulation){}
Message Client::transact(Opcode op,std::span<const uint8_t> payload,std::stop_token stopToken) {
    if(stopToken.stop_requested())throw std::runtime_error("RP2040 operation cancelled");
    if(payload.size()>payloadCapacity||request_==UINT32_MAX)throw std::runtime_error("RP2040 request limit; reconnect");
    Message m;m.opcode=op;m.session=status_.session;m.request=++request_;m.length=uint16_t(payload.size());
    std::copy(payload.begin(),payload.end(),m.payload.begin());const auto encoded=encode(m);
    const double before=now_();const auto packet=link_.exchange(*encoded,stopToken);const double after=now_();
    status_.roundTripUs=after-before;Message reply;
    if(decode(packet,reply)!=Result::Ok||reply.opcode!=Opcode::Reply||reply.length!=32||
       reply.request!=m.request||reply.session!=m.session||reply.payload[1]!=uint8_t(op))throw std::runtime_error("RP2040 reply mismatch; stop and reconnect");
    const bool simulated=(reply.payload[2]&0x80)!=0;
    if((simulated&&!allowSimulation_)||(!simulated&&(reply.payload[2]&0x40)==0))throw std::runtime_error("RP2040 firmware capability mismatch");
    status_.simulated=simulated;
    status_.extendedCadence=(reply.payload[2]&extendedCadenceFlag)!=0;
    status_.aperture=(reply.payload[2]&apertureFlag)!=0;
    status_.fastCadence=(reply.payload[2]&fastCadenceFlag)!=0;
    if(op!=Opcode::Diagnostics) {
        const auto boot=get64(std::span(reply.payload).subspan(24));
        if(!boot)throw std::runtime_error("RP2040 boot identity missing");
        if(status_.bootId&&boot!=status_.bootId){clock_.reset();status_.clockReady=false;status_.session=0;throw std::runtime_error("RP2040 rebooted; reconnect before stereo");}
        status_.bootId=boot;
    }
    const auto result=Result(reply.payload[0]);
    if(op==Opcode::Schedule&&(result==Result::TooSoon||result==Result::BadConfig||result==Result::WrongEye||result==Result::Overlap||result==Result::NotReady||result==Result::BadSession))throw ScheduleMiss(std::string("RP2040 Schedule: ")+resultName(result));
    // A deadline/watchdog fault has already stopped the device. Stop remains
    // idempotent only if Status confirms that no session is active.
    if(op==Opcode::Stop&&result==Result::BadSession){inspect(stopToken);if(!status_.session)return reply;}
    if(result!=Result::Ok)throw std::runtime_error(std::string("RP2040 opcode ")+std::to_string(unsigned(op))+": "+resultName(result));
    if(op==Opcode::Clock) {
        const auto p=std::span<const uint8_t>(reply.payload);
        clock_.observe(before,double(get64(p.subspan(8))),double(get64(p.subspan(16))),after);
        auto e=clock_.estimate(after+4000,after);status_.clockReady=e.has_value();
        status_.uncertaintyUs=e?e->uncertaintyUs:std::numeric_limits<double>::infinity();
    }
    return reply;
}
void Client::inspect(std::stop_token token) {
    auto reply=transact(Opcode::Status,{},token);auto p=std::span(reply.payload);
    status_.lastEpoch=get64(p.subspan(8));status_.session=get64(p.subspan(16));
}
bool Client::sampleClock(std::stop_token token){transact(Opcode::Clock,{},token);return status_.clockReady;}
void Client::start(Config c,std::stop_token token) {
    if(!validConfig(c))throw InvalidTiming("RP2040 timing does not fit the device frame guards; current session retained");
    inspect(token);
    if(!c.frameAnchored&&c.periodUs<8300&&!status_.fastCadence)throw std::runtime_error("RP2040 firmware 0.4.0 required for more than 120 eye openings/s");
    if(c.frameAnchored&&!status_.aperture)throw std::runtime_error("RP2040 firmware 0.4.0 required for LCD aperture timing");
    if(!c.frameAnchored&&!status_.extendedCadence&&(c.periodUs<8300||c.periodUs>8367))
        throw std::runtime_error("RP2040 firmware update required for LCD preload and slower cadence (flash VisionEmitter.uf2)");
    if(status_.lastEpoch==UINT64_MAX)throw std::runtime_error("RP2040 epoch exhausted; reboot emitter");
    stop(token);
    status_.session=++status_.lastEpoch;
    transact(Opcode::Hello,{},token);
    std::array<uint8_t,24> p{};put32(p,c.periodUs);put32(std::span(p).subspan(4),c.leftUs);put32(std::span(p).subspan(8),c.rightUs);
    put32(std::span(p).subspan(12),c.leftOpenUs);put32(std::span(p).subspan(16),c.rightOpenUs);put32(std::span(p).subspan(20),c.frameGuardUs);
    try{transact(c.frameAnchored?Opcode::ConfigureAperture:Opcode::Configure,std::span(p).first(c.frameAnchored?24:12),token);}catch(...){try{stop();}catch(...){}throw;}
    sequence_=0;
}
void Client::schedule(Eye eye,double target,std::stop_token token) {
    if(!status_.session)throw std::runtime_error("RP2040 session not started");
    const double now=now_();auto e=clock_.estimate(target,now);
    if(!e){status_.clockReady=false;throw ScheduleMiss("RP2040 clock needs fresh samples");}
    status_.uncertaintyUs=e->uncertaintyUs;
    const double budget=std::max(1000.0,status_.roundTripUs*2);
    if(target-now<Scheduler::minLeadUs+e->uncertaintyUs+budget)throw ScheduleMiss("RP2040 prediction has insufficient USB lead time");
    if(sequence_==UINT64_MAX)throw std::runtime_error("RP2040 sequence exhausted");
    std::array<uint8_t,17> p{};put64(p,++sequence_);put64(std::span(p).subspan(8),uint64_t(std::llround(e->deviceUs)));p[16]=uint8_t(eye);
    transact(Opcode::Schedule,p,token);++status_.events;
}
void Client::stop(std::stop_token token) {
    if(!status_.session)return;
    transact(Opcode::Stop,{},token);status_.session=0;sequence_=0;
}
Message Client::diagnostics(std::stop_token token){return transact(Opcode::Diagnostics,{},token);}
}
