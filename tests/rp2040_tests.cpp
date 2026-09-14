// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_protocol.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace vision::rp2040;
namespace {
uint64_t checks=0;
void check(bool ok,const char* label) {++checks;if(!ok)throw std::runtime_error(label);}
void start(Scheduler& s,uint64_t epoch=1,uint64_t now=0) {
    check(s.begin(epoch,now)==Result::Ok,"begin session");
    check(s.configure(epoch,{},now)==Result::Ok,"configure baseline");
}
void packets() {
    const std::array<uint8_t,9> reference{'1','2','3','4','5','6','7','8','9'};
    check(checksum(reference)==0xcbf43926,"standard CRC32 check value");
    Message input;input.opcode=Opcode::Schedule;input.session=0x0102030405060708ull;
    input.request=0x12345678;input.length=32;
    for(unsigned i=0;i<32;++i)input.payload[i]=uint8_t(i);
    auto packet=encode(input);check(packet.has_value(),"encode valid envelope");
    check((*packet)[8]==8&&(*packet)[15]==1&&(*packet)[16]==0x78,"little endian wire layout");
    Message output;check(decode(*packet,output)==Result::Ok,"decode valid envelope");
    check(output.payload==input.payload&&output.session==input.session&&output.request==input.request,"round trip");
    for(size_t len=0;len<64;++len) {
        output.session=42;
        check(decode(std::span(*packet).first(len),output)==Result::BadLength,"reject truncated packet");
        check(output.session==42,"failed parse leaves output unchanged");
    }
    auto extra=std::vector<uint8_t>(65);check(decode(extra,output)==Result::BadLength,"reject oversized packet");
    for(unsigned bit=0;bit<512;++bit) {
        auto damaged=*packet;damaged[bit/8]^=uint8_t(1u<<(bit%8));
        check(decode(damaged,output)!=Result::Ok,"reject every single bit mutation");
    }
    auto malformed=*packet;malformed[6]=33;
    put32(std::span(malformed).subspan(24),checksum(malformed));
    check(decode(malformed,output)==Result::BadLength,"reject oversized payload with valid CRC");
    malformed=*packet;malformed[20]=1;put32(std::span(malformed).subspan(24),checksum(malformed));
    check(decode(malformed,output)==Result::BadPacket,"reject reserved flags");
    input.length=0;packet=encode(input);malformed=*packet;malformed[63]=1;
    put32(std::span(malformed).subspan(24),checksum(malformed));
    check(decode(malformed,output)==Result::BadPacket,"reject nonzero padding");
    input.length=33;check(!encode(input),"encoder bounds");
    input.length=0;input.opcode=Opcode(77);check(!encode(input),"encoder opcode check");
    uint32_t random=0x12345678;
    for(unsigned n=0;n<20000;++n) {
        Packet noise{};for(auto& b:noise){random^=random<<13;random^=random>>17;random^=random<<5;b=uint8_t(random);}
        check(decode(noise,output)!=Result::Ok,"random malformed input rejected");
    }
    std::cout<<"PASS packet framing: truncation, CRC, bounds, padding, 20000 malformed packets\n";
}
void scheduling() {
    Scheduler s;check(s.begin(0,0)==Result::BadSession,"zero session forbidden");start(s);
    check(s.configure(1,{6944,1500,1500},0)==Result::BadConfig,"144 Hz not advertised");
    check(s.configure(1,{4167,1500,1500},0)==Result::BadConfig,"240 Hz not advertised");
    check(s.configure(1,{8333,0,1500},0)==Result::BadConfig,"zero interval forbidden");
    check(s.configure(1,{8333,8333,1500},0)==Result::BadConfig,"overlong interval forbidden");
    check(s.enqueue({2,1,10000,Eye::Left},0)==Result::BadSession,"foreign epoch rejected");
    check(s.enqueue({1,1,499,Eye::Left},0)==Result::TooSoon,"USB event without lead rejected");
    check(s.enqueue({1,1,50001,Eye::Left},0)==Result::TooFar,"long horizon rejected");
    check(s.enqueue({1,1,10000,Eye(9)},0)==Result::WrongEye,"invalid eye rejected");
    check(s.enqueue({1,1,10000,Eye::Left},0)==Result::Ok,"queue left");
    check(s.enqueue({1,1,18333,Eye::Right},0)==Result::StaleSequence,"duplicate rejected");
    check(s.enqueue({1,2,18333,Eye::Left},0)==Result::WrongEye,"same eye rejected");
    check(s.enqueue({1,2,11000,Eye::Right},0)==Result::Overlap,"overlapping intervals rejected");
    check(s.enqueue({1,2,26667,Eye::Right},0)==Result::BadConfig,"missing refresh requires resync");
    check(s.enqueue({1,2,18333,Eye::Right},0)==Result::Ok,"queue right");
    check(s.configure(1,{},0)==Result::NotReady,"configuration cannot change queued output");
    check(s.begin(2,0)==Result::NotReady,"new session requires stop");
    check(s.advance(9999).count==0,"do not emit early");
    auto a=s.advance(10000);check(a.count==1&&a.actions[0].kind==ActionKind::Open&&a.actions[0].eye==Eye::Left,"left opens at deadline");
    check(s.advance(10000).count==0,"repeated tick cannot double emit");
    a=s.advance(11500);check(a.count==1&&a.actions[0].kind==ActionKind::Close,"left closes");
    a=s.advance(18333);check(a.count==1&&a.actions[0].eye==Eye::Right,"right opens");
    a=s.stop();check(a.forceIdle&&s.queued()==0&&!s.ready(),"stop clears queued work and requires idle");
    check(s.advance(19833).count==0,"stopped events cannot replay");
    check(s.begin(1,20000)==Result::BadSession,"old epoch cannot restart");
    start(s,2,20000);
    check(s.enqueue({1,3,30000,Eye::Left},20000)==Result::BadSession,"in flight old-session packet rejected");
    check(s.enqueue({2,1,30000,Eye::Left},20000)==Result::Ok,"new epoch sequence restarts");
    a=s.advance(30101);check(a.forceIdle&&a.reason==Result::Late&&!s.ready(),"late alarm shuts down rather than catching up");
    start(s,3,40000);a=s.advance(140001);
    check(a.forceIdle&&a.reason==Result::Timeout,"host loss watchdog");
    start(s,4,200000);a=s.advance(199999);
    check(a.forceIdle&&a.reason==Result::ClockReversed,"device clock regression fails closed");
    start(s,5,300000);
    check(s.enqueue({5,1,310000,Eye::Left},300000)==Result::Ok,"pending first frame");
    check(s.enqueue({5,2,318333,Eye::Right},300000)==Result::Ok,"pending second frame");
    a=s.advance(330000);check(a.forceIdle&&a.count==0&&s.queued()==0,"no catch-up burst after stalled executor");
    Scheduler overflow;const auto nearMax=std::numeric_limits<uint64_t>::max()-1000;
    start(overflow,1,nearMax);
    check(overflow.enqueue({1,1,nearMax+500,Eye::Left},nearMax)==Result::TooFar,"timestamp addition cannot overflow");
    // Queue several frames before execution, as when a USB batch arrives.
    Scheduler batch;start(batch);
    for(uint64_t i=0;i<6;++i)check(batch.enqueue({1,i+1,1000+i*8333,i%2?Eye::Right:Eye::Left},0)==Result::Ok,"short future batch accepted");
    for(uint64_t i=0;i<6;++i) {
        const auto t=1000+i*8333;a=batch.advance(t);
        check(a.count==1&&a.actions[0].sequence==i+1,"batch preserves order");
        check(batch.advance(t+1500).count==1,"batch close event");
    }
    check(batch.queued()==0,"batch completely drained");
    std::cout<<"PASS scheduler faults: missed refresh, late alarm, overlap, stale epoch, stop, timeout, overflow\n";
}
void clocks() {
    ClockMap map;check(!map.estimate(1000,0),"no clock before samples");
    // Simulated device is 5 ms ahead, gains 100 ppm; deliberately asymmetric USB.
    auto device=[](double host){return 5000+host*1.0001;};
    for(unsigned i=0;i<1200;++i) {
        double h0=1000+i*5000.0, forward=30+(i%5)*10, back=20+(i%7)*9;
        double receive=h0+forward, send=receive+5, h3=send+back;
        check(map.observe(h0,device(receive),device(send),h3),"accept timestamp exchange");
        if(i>=3) {
            const double target=h3+4000;auto e=map.estimate(target,h3);
            check(e.has_value(),"clock remains usable under 100 ppm drift");
            check(std::abs(e->deviceUs-device(target))<=e->uncertaintyUs,"true device time within conservative bounds");
        }
    }
    check(!map.estimate(9000000,9000000),"stale clock cannot schedule");
    check(!map.observe(10,10,20,9)&&map.samples()==0,"invalid exchange clears confidence");
    check(!map.observe(0,0,15,1),"impossible device processing interval rejected");
    check(!map.observe(std::numeric_limits<double>::quiet_NaN(),0,0,0),"NaN rejected");
    for(unsigned i=0;i<4;++i)check(map.observe(i*10000.0,5000+i*10000.0,5000+i*10000.0,2000+i*10000.0),"high RTT exchange framed correctly");
    check(!map.estimate(35000,32000),"high uncertainty not called synchronized");
    map.reset();for(unsigned i=0;i<4;++i)map.observe(i*1000,5000+i*1000,5000+i*1000,i*1000+20);
    check(map.observe(5000,100000,100005,5020)&&map.samples()==1,"clock discontinuity reacquires confidence");
    check(!map.estimate(6000,5020),"clock jump cannot immediately schedule");
    std::cout<<"PASS clock mapping: asymmetric USB, 100 ppm drift, uncertainty, stale samples, clock jumps\n";
}
void sustained() {
    Scheduler s;start(s);uint64_t opens=0, closes=0;
    // Event-driven simulation: 600 seconds of device time, no wall-clock wait.
    for(uint64_t frame=0;frame<72000;++frame) {
        const uint64_t open=10000+(frame*25000)/3;
        const uint64_t usbDelay=100+(frame*7919)%1101;
        const uint64_t arrival=open-4000+usbDelay;
        check(!s.advance(arrival).forceIdle,"healthy USB jitter does not lose session");
        const Eye eye=frame%2?Eye::Right:Eye::Left;
        check(s.enqueue({1,frame+1,open,eye},arrival)==Result::Ok,"delayed packet still arrives before lead limit");
        const auto a=s.advance(open);
        check(a.count==1&&a.actions[0].kind==ActionKind::Open&&a.actions[0].eye==eye,"intended eye executed");
        opens+=a.count;
        const auto b=s.advance(open+1500);
        check(b.count==1&&b.actions[0].kind==ActionKind::Close&&b.actions[0].eye==eye,"matching eye closed");
        closes+=b.count;
    }
    check(opens==72000&&closes==72000&&s.rejected()==0,"ten minute simulation complete");
    std::cout<<"PASS 600-second logical 120 Hz simulation: 72000 opens, 72000 closes; USB delay 100-1200 us\n";
    std::cout<<"SIMULATION ONLY: no USB device, GPIO, IR waveform, monitor or glasses tested.\n";
}
void endpoint() {
    SimulatedDevice device(0xabc);Message request;request.session=1;request.request=1;
    auto send=[&](Result expected,uint64_t now=0) {
        auto packet=encode(request);check(packet.has_value(),"host request encoded");
        auto exchange=device.receive(*packet,now,now+2);
        Message response;check(exchange.reply&&decode(*exchange.reply,response)==Result::Ok,"device reply framed");
        check(response.opcode==Opcode::Reply&&response.request==request.request&&response.session==request.session,"response correlation");
        check(response.payload[0]==uint8_t(expected)&&response.payload[1]==uint8_t(request.opcode),"command acknowledgement");
        check((response.payload[2]&0x80)!=0&&get64(std::span(response.payload).subspan(24))==0xabc,"simulation and boot identity explicit");
        ++request.request;return exchange;
    };
    send(Result::Ok); // hello
    request.opcode=Opcode::Configure;request.length=32;send(Result::BadLength);
    request.length=12;auto p=std::span(request.payload);put32(p,8333);put32(p.subspan(4),1500);put32(p.subspan(8),1500);
    send(Result::Ok);
    request.opcode=Opcode::Schedule;request.length=17;request.payload.fill(0);
    put64(p,1);put64(p.subspan(8),10000);p[16]=0;send(Result::Ok);
    send(Result::StaleSequence);
    request.session=2;put64(p,2);put64(p.subspan(8),18333);p[16]=1;send(Result::BadSession);
    check(device.advance(10000).count==1,"wire request schedules actual model event");
    request.opcode=Opcode::Stop;request.length=0;send(Result::BadSession,10000);
    check(device.advance(11500).count==1,"stale stop cannot interrupt current epoch");
    request.session=1;check(send(Result::Ok,11500).forceIdle,"wire stop requests physical idle");
    request.opcode=Opcode::Hello;send(Result::BadSession,11500);request.session=2;send(Result::Ok,11500);
    request.opcode=Opcode::Clock;
    auto exchange=send(Result::Ok,12000);Message response;decode(*exchange.reply,response);
    check(get64(std::span(response.payload).subspan(8))==12000&&get64(std::span(response.payload).subspan(16))==12002,"clock response has receive and send timestamps");
    request.opcode=Opcode::Status;send(Result::Ok,110000);
    check(device.advance(111501).reason==Result::Timeout,"status polling cannot keep abandoned output armed");
    request.opcode=Opcode::Hello;request.session=3;send(Result::Ok,120000);
    check(device.disconnect().forceIdle,"transport disconnect forces idle");
    auto bad=*encode(request);bad[30]=1;
    check(!device.receive(bad,120000,120001).reply,"corrupt wire input cannot elicit success");
    std::cout<<"PASS simulated USB endpoint: handshake, config ACK, scheduled events, clock reply, stale STOP, disconnect\n";
}
}
int main() {
    try {packets();scheduling();clocks();endpoint();sustained();std::cout<<checks<<" checks passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
