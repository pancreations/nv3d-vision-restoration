// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_protocol.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vision::rp2040 {
namespace {
bool validOpcode(Opcode op) { const auto n=static_cast<uint8_t>(op); return (n>=1 && n<=8)||n==128; }
}
void put32(std::span<uint8_t> b,uint32_t v) { if(b.size()<4)return; for(unsigned i=0;i<4;++i)b[i]=uint8_t(v>>(8*i)); }
void put64(std::span<uint8_t> b,uint64_t v) { if(b.size()<8)return; for(unsigned i=0;i<8;++i)b[i]=uint8_t(v>>(8*i)); }
uint32_t get32(std::span<const uint8_t> b) { if(b.size()<4)return 0; uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(b[i])<<(8*i);return v; }
uint64_t get64(std::span<const uint8_t> b) { if(b.size()<8)return 0; uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(b[i])<<(8*i);return v; }
uint32_t checksum(std::span<const uint8_t> b) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<b.size();++i) {
        crc ^= (i>=24 && i<28)?0:b[i];
        for(unsigned j=0;j<8;++j) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return crc^0xffffffffu;
}
std::optional<Packet> encode(const Message& m) {
    if(m.length>payloadCapacity || !validOpcode(m.opcode))return {};
    Packet p{}; auto b=std::span<uint8_t>(p);
    p[0]='V';p[1]='R';p[2]='P';p[3]='1';p[4]=protocolVersion;p[5]=uint8_t(m.opcode);
    p[6]=uint8_t(m.length);p[7]=uint8_t(m.length>>8);
    put64(b.subspan(8),m.session);put32(b.subspan(16),m.request);
    std::copy_n(m.payload.begin(),m.length,p.begin()+32);
    put32(b.subspan(24),checksum(p));return p;
}
Result decode(std::span<const uint8_t> b,Message& out) {
    // Never modify caller output on failure.
    if(b.size()!=packetSize)return Result::BadLength;
    if(b[0]!='V'||b[1]!='R'||b[2]!='P'||b[3]!='1')return Result::BadPacket;
    if(b[4]!=protocolVersion)return Result::BadVersion;
    auto op=Opcode(b[5]);if(!validOpcode(op))return Result::BadOpcode;
    uint16_t len=uint16_t(b[6])|(uint16_t(b[7])<<8);
    if(len>payloadCapacity)return Result::BadLength;
    if(get32(b.subspan(24))!=checksum(b))return Result::BadChecksum;
    for(size_t i=20;i<32;++i)if((i<24||i>=28)&&b[i])return Result::BadPacket;
    for(size_t i=32+len;i<packetSize;++i)if(b[i])return Result::BadPacket;
    Message m;m.opcode=op;m.session=get64(b.subspan(8));m.request=get32(b.subspan(16));m.length=len;
    std::copy_n(b.begin()+32,len,m.payload.begin());out=m;return Result::Ok;
}
Result Scheduler::begin(uint64_t session,uint64_t now) {
    // Session epochs strictly increase during a device boot. A later hardware
    // handshake must expose a boot ID so host epochs cannot survive a reboot.
    if(session==0 || session<=lastSession_)return reject(Result::BadSession);
    if(session_!=0)return reject(Result::NotReady); // caller must stop output first
    session_=lastSession_=session;lastNow_=lastActivity_=now;
    configured_=false;hadFrame_=false;lastSequence_=lastClose_=lastOpen_=0;
    head_=count_=0;return Result::Ok;
}
Result Scheduler::configure(uint64_t session,Config c,uint64_t now) {
    if(!session_||session!=session_)return reject(Result::BadSession);
    if(now<lastNow_)return reject(Result::ClockReversed);
    if(count_||hadFrame_)return reject(Result::NotReady);
    // This is the interval BETWEEN eye openings, including held/black refreshes.
    // 30-120 openings/s; exact targets carry the fractional refresh accumulation.
    const auto minPeriod=c.frameAnchored?minAperturePeriodUs:minPeriodUs;
    const auto maxPeriod=c.frameAnchored?maxAperturePeriodUs:maxPeriodUs;
    if(c.periodUs<minPeriod||c.periodUs>maxPeriod||c.leftUs<250||c.rightUs<250||
       c.leftUs>c.periodUs-2*guardUs||c.rightUs>c.periodUs-2*guardUs)return reject(Result::BadConfig);
    if(c.frameAnchored&&(c.frameGuardUs<guardUs||c.frameGuardUs>c.periodUs/2||
       c.leftOpenUs<c.frameGuardUs||c.rightOpenUs<c.frameGuardUs||
       uint64_t(c.leftOpenUs)+c.leftUs>c.periodUs-c.frameGuardUs||
       uint64_t(c.rightOpenUs)+c.rightUs>c.periodUs-c.frameGuardUs))return reject(Result::BadConfig);
    config_=c;configured_=true;lastNow_=lastActivity_=now;return Result::Ok;
}
Result Scheduler::enqueue(Frame f,uint64_t now) {
    if(!session_||f.session!=session_)return reject(Result::BadSession);
    if(!configured_)return reject(Result::NotReady);
    if(now<lastNow_)return reject(Result::ClockReversed);
    if(now-lastActivity_>watchdogUs)return reject(Result::Timeout);
    if(f.eye!=Eye::Left&&f.eye!=Eye::Right)return reject(Result::WrongEye);
    if(f.sequence==0||f.sequence<=lastSequence_)return reject(Result::StaleSequence);
    if(count_==capacity)return reject(Result::QueueFull);
    const auto frameStart=f.openUs;
    if(config_.frameAnchored) {
        const auto delay=f.eye==Eye::Left?config_.leftOpenUs:config_.rightOpenUs;
        if(f.openUs>std::numeric_limits<uint64_t>::max()-delay)return reject(Result::TooFar);
        f.openUs+=delay;
    }
    if(f.openUs<now || f.openUs-now<minLeadUs)return reject(Result::TooSoon);
    if(f.openUs-now>horizonUs)return reject(Result::TooFar);
    uint64_t duration=f.eye==Eye::Left?config_.leftUs:config_.rightUs;
    if(f.openUs>std::numeric_limits<uint64_t>::max()-duration-guardUs)return reject(Result::TooFar);
    if(hadFrame_) {
        if(config_.frameAnchored&&(frameStart<config_.frameGuardUs||lastClose_>frameStart-config_.frameGuardUs))return reject(Result::Overlap);
        if(f.openUs<lastClose_+guardUs)return reject(Result::Overlap);
        if(f.eye==lastEye_)return reject(Result::WrongEye);
        if(frameStart<lastOpen_)return reject(Result::BadConfig);
        const auto delta=frameStart-lastOpen_;
        // Do not free-run across missing display frames. Reacquire a session.
        if(delta+100<config_.periodUs||delta>config_.periodUs+100)return reject(Result::BadConfig);
    }
    auto& e=queue_[(head_+count_)%capacity];e={f,f.openUs+duration,false};++count_;
    lastSequence_=f.sequence;lastClose_=e.closeUs;lastOpen_=frameStart;lastEye_=f.eye;
    hadFrame_=true;lastNow_=lastActivity_=now;return Result::Ok;
}
Step Scheduler::stop(Result reason) {
    session_=0;configured_=false;head_=count_=0;hadFrame_=false;
    Step s;s.forceIdle=true;s.reason=reason;return s;
}
Step Scheduler::advance(uint64_t now) {
    if(!session_)return {};
    if(now<lastNow_)return stop(Result::ClockReversed);
    if(now-lastActivity_>watchdogUs)return stop(Result::Timeout);
    lastNow_=now;Step step;
    while(count_) {
        auto& e=queue_[head_];const auto due=e.opened?e.closeUs:e.frame.openUs;
        if(now<due)break;
        // Do not rapidly play back missed transitions to catch up.
        if(now-due>maxLateUs)return stop(Result::Late);
        step.actions[step.count++]={e.opened?ActionKind::Close:ActionKind::Open,e.frame.eye,e.frame.sequence,due};
        if(e.opened) {head_=(head_+1)%capacity;--count_;}
        else e.opened=true;
    }
    return step;
}
void ClockMap::reset() {samples_=0;low_=high_=anchor_=lastReceive_=0;}
bool ClockMap::observe(double h0,double d1,double d2,double h3) {
    if(!std::isfinite(h0)||!std::isfinite(d1)||!std::isfinite(d2)||!std::isfinite(h3)||
       h0<0||d1<0||h3<h0||d2<d1||h3-h0>20000||d2-d1>h3-h0+20) {reset();return false;}
    if(samples_ && h0<lastReceive_) {reset();return false;}
    const double mid=(h0+h3)/2;
    // Account for bounded drift during the exchange and timer quantization.
    const double slack=(h3-h0)*driftPpm/1e6+2;
    double lo=d2-h3-slack, hi=d1-h0+slack;
    if(lo>hi) {reset();return false;}
    if(samples_) {
        const double drift=(mid-anchor_)*driftPpm/1e6;
        const double a=std::max(lo,low_-drift), b=std::min(hi,high_+drift);
        if(a>b || h3-lastReceive_>maxAgeUs) samples_=0;
        else {lo=a;hi=b;}
    }
    low_=lo;high_=hi;anchor_=mid;lastReceive_=h3;
    if(samples_<1000000)++samples_;
    return true;
}
std::optional<ClockEstimate> ClockMap::estimate(double target,double now)const {
    if(samples_<4||!std::isfinite(target)||!std::isfinite(now)||now<lastReceive_||
       now-lastReceive_>maxAgeUs||target<now||target-now>Scheduler::horizonUs)return {};
    const double uncertainty=(high_-low_)/2+std::abs(target-anchor_)*driftPpm/1e6;
    const double device=target+(low_+high_)/2;
    if(uncertainty>maxUncertaintyUs||device<0||!std::isfinite(device))return {};
    return ClockEstimate{device,uncertainty};
}
Step DeviceEndpoint::advance(uint64_t now) {
    auto step=scheduler_.advance(now);
    if(step.forceIdle)lastFault_=step.reason;
    for(size_t i=0;i<step.count;++i)if(step.actions[i].kind==ActionKind::Open)++opens_;else ++closes_;
    return step;
}
Exchange DeviceEndpoint::receive(std::span<const uint8_t> bytes,uint64_t now,uint64_t replyTime) {
    Exchange exchange;Message m;
    if(decode(bytes,m)!=Result::Ok||m.opcode==Opcode::Reply||replyTime<now)return exchange;
    Result result=Result::Ok;
    uint16_t expected=0;
    if(m.opcode==Opcode::Configure)expected=12;
    if(m.opcode==Opcode::ConfigureAperture)expected=24;
    if(m.opcode==Opcode::Schedule)expected=17;
    if(m.length!=expected)result=Result::BadLength;
    else {
        const auto p=std::span<const uint8_t>(m.payload);
        switch(m.opcode) {
        case Opcode::Hello: result=scheduler_.begin(m.session,now);break;
        case Opcode::Clock: case Opcode::Status: case Opcode::Diagnostics: break; // read-only; no watchdog extension
        case Opcode::Configure:
            result=scheduler_.configure(m.session,{get32(p),get32(p.subspan(4)),get32(p.subspan(8))},now);break;
        case Opcode::ConfigureAperture:
            result=scheduler_.configure(m.session,{get32(p),get32(p.subspan(4)),get32(p.subspan(8)),true,get32(p.subspan(12)),get32(p.subspan(16)),get32(p.subspan(20))},now);break;
        case Opcode::Schedule:
            result=scheduler_.enqueue({m.session,get64(p),get64(p.subspan(8)),Eye(p[16])},now);break;
        case Opcode::Stop:
            if(!m.session||m.session!=scheduler_.session())result=Result::BadSession;
            else {scheduler_.stop();exchange.forceIdle=true;}
            break;
        default: result=Result::BadOpcode;break;
        }
    }
    Message response;response.opcode=Opcode::Reply;response.session=m.session;response.request=m.request;response.length=32;
    auto p=std::span<uint8_t>(response.payload);
    p[0]=uint8_t(result);p[1]=uint8_t(m.opcode);
    p[2]=uint8_t((simulated_?0x80:0x40)|extendedCadenceFlag|apertureFlag|fastCadenceFlag|(scheduler_.ready()?1:0)); // capabilities do not claim optical calibration
    p[3]=uint8_t(Scheduler::capacity);put32(p.subspan(4),uint32_t(scheduler_.queued()));
    put64(p.subspan(8),now);put64(p.subspan(16),replyTime);put64(p.subspan(24),bootId_);
    if(m.opcode==Opcode::Status) {
        put64(p.subspan(8),scheduler_.lastSession());put64(p.subspan(16),scheduler_.session());
    }
    if(m.opcode==Opcode::Diagnostics) {
        put32(p.subspan(4),uint32_t(scheduler_.rejected()));
        put64(p.subspan(8),opens_);put64(p.subspan(16),closes_);put64(p.subspan(24),uint64_t(lastFault_));
    }
    exchange.reply=encode(response);return exchange;
}
}
