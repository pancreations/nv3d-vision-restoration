// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_host.h"
#include "rp2040_wave.h"
#include "rp2040_usb_packet.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace vision::rp2040;
namespace {
unsigned checks=0;
void check(bool ok,const char* s){++checks;if(!ok)throw std::runtime_error(s);}
template<class F>void rejects(F f,const char* s){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,s);}
class ModelLink : public Link {
public:
    double time=100000;
    DeviceEndpoint device{1234,false};bool corrupt=false,disconnected=false,simulated=false,legacy=false,noAperture=false;
    std::vector<Message> requests;
    Packet exchange(const Packet& p,std::stop_token stop)override {
        if(stop.stop_requested()||disconnected)throw std::runtime_error("link disconnected");
        Message m;check(decode(p,m)==Result::Ok,"host request parse");requests.push_back(m);
        time+=20;auto e=device.receive(p,uint64_t(time+5000),uint64_t(time+5000));time+=30;
        check(e.reply.has_value(),"model returns reply");auto packet=*e.reply;
        if(simulated){Message r;decode(packet,r);r.payload[2]|=0x80;packet=*encode(r);}
        if(legacy){Message r;decode(packet,r);r.payload[2]&=uint8_t(~(extendedCadenceFlag|apertureFlag|fastCadenceFlag));packet=*encode(r);}
        if(noAperture){Message r;decode(packet,r);r.payload[2]&=uint8_t(~(apertureFlag|fastCadenceFlag));packet=*encode(r);}
        if(corrupt)packet[16]^=1;
        return packet;
    }
};
void host() {
    ModelLink link;Client c(link,[&]{return link.time;});c.inspect();
    check(c.status().bootId==1234&&c.status().session==0,"read-only identity");
    check(c.status().extendedCadence,"device advertises LCD cadence capability");
    for(unsigned i=0;i<4;++i){c.sampleClock();link.time+=100;}
    check(c.status().clockReady,"host clock maps device");c.start({});
    const double target=link.time+4000;c.schedule(Eye::Left,target);
    auto request=link.requests.back();auto scheduled=get64(std::span(request.payload).subspan(8));
    check(std::abs(double(scheduled)-(target+5000))<30,"host converts target to device epoch");
    auto step=link.device.advance(scheduled);check(step.count==1&&step.actions[0].eye==Eye::Left,"actual command reaches model scheduler");
    // Align simulated host progression to the already-executed device event.
    link.time=double(scheduled)-5000;
    c.stop();check(link.device.advance(scheduled+1500).count==0,"stop cancels pending close window");
    c.start({});check(c.status().session==2,"new session uses device epoch");
    rejects([&]{c.schedule(Eye::Left,link.time+100);},"host refuses insufficient lead before sending");
    c.stop();
    c.start({16667,1500,1500});c.stop();
    ModelLink old;old.legacy=true;Client legacy(old,[&]{return old.time;});legacy.inspect();
    check(!legacy.status().extendedCadence,"old firmware remains distinguishable");
    rejects([&]{legacy.start({16667,1500,1500});},"old firmware rejects LCD preload before arming");
    for(const auto& r:old.requests)check(r.opcode!=Opcode::Hello&&r.opcode!=Opcode::Configure,"unsupported cadence sent no arm command");
    legacy.start({});legacy.stop();
    ModelLink apertureLink;Client aperture(apertureLink,[&]{return apertureLink.time;});aperture.inspect();
    check(aperture.status().aperture,"frame-aperture capability identified");
    aperture.start({8333,750,750,true,4000,4200,250});
    check(apertureLink.requests.back().opcode==Opcode::ConfigureAperture&&apertureLink.requests.back().length==24,"aperture configuration uses explicit opcode and bounded payload");
    const auto activeSession=aperture.status().session,requestCount=apertureLink.requests.size();
    rejects([&]{aperture.start({8333,750,750,true,250,250,251});},"invalid new guard is rejected before USB writes");
    check(aperture.status().session==activeSession&&apertureLink.requests.size()==requestCount,"invalid timing leaves working session untouched");
    aperture.stop();
    check(aperture.status().fastCadence,"high refresh capability identified");
    aperture.start({4167,750,750});aperture.stop();
    ModelLink oldCadence;oldCadence.noAperture=true;Client noAperture(oldCadence,[&]{return oldCadence.time;});
    rejects([&]{noAperture.start({8333,750,750,true,4000,4200,250});},"0.3 firmware cannot silently accept new phase semantics");
    rejects([&]{noAperture.start({4167,750,750});},"0.3 firmware rejects direct 240 Hz before arming");
    for(const auto& r:oldCadence.requests)check(r.opcode!=Opcode::Hello,"old firmware was not armed for aperture mode");
    ModelLink recovery;Client recovering(recovery,[&]{return recovery.time;});
    for(int n=0;n<4;++n)recovering.sampleClock();
    recovering.start({8333,750,750,true,4000,4000,250});
    auto first=recovery.time+4000;recovering.schedule(Eye::Left,first);
    bool missed=false;try{recovering.schedule(Eye::Right,first+16666);}catch(const ScheduleMiss&){missed=true;}
    check(missed,"missing refresh surfaces as a recoverable scheduling miss");
    recovering.stop();recovering.start({8333,750,750,true,4000,4000,250});
    recovering.schedule(Eye::Left,recovery.time+4000);recovering.stop();
    recovering.start({8333,750,750,true,4000,4000,250});
    const auto targetAfterRestart=recovery.time+4000;
    recovering.schedule(Eye::Left,targetAfterRestart);
    recovery.time=targetAfterRestart+4000+Scheduler::maxLateUs+1;
    check(recovery.device.advance(uint64_t(recovery.time+5000)).forceIdle,"delayed executor stops the model device");
    recovering.stop();check(recovering.status().session==0,"stop accepts already-stopped device after deadline fault");
    for(int n=0;n<4;++n)recovering.sampleClock();
    recovering.start({8333,750,750,true,4000,4000,250});
    recovering.schedule(Eye::Right,recovery.time+4000);recovering.stop();
    ModelLink unknown;unknown.simulated=true;Client physical(unknown,[&]{return unknown.time;});
    rejects([&]{physical.inspect();},"simulation cannot masquerade as real emitter");
    link.corrupt=true;rejects([&]{c.inspect();},"corrupt USB reply rejected");link.corrupt=false;
    link.device=DeviceEndpoint(5678,false);rejects([&]{c.inspect();},"device reboot invalidates clock");
    check(!c.status().clockReady,"reboot clears confidence");
    ModelLink gone;Client other(gone,[&]{return gone.time;});other.inspect();gone.disconnected=true;
    rejects([&]{other.sampleClock();},"USB disconnect surfaces without retry loop");
    std::stop_source stop;stop.request_stop();rejects([&]{other.inspect(stop.get_token());},"cancel honored before transfer");
    std::cout<<"PASS Windows host protocol via injected link: identity, ACK, clock mapping, schedule, stop, reboot, corruption, cancellation\n";
}
void usbReplies() {
    double now=0;unsigned calls=0;
    const auto packet=receiveUsbReply([&](Packet& p,unsigned timeout){
        check(timeout>0&&timeout<=20,"USB read has a bounded timeout");now+=1000;
        if(++calls==1)return size_t(0); // trailing terminator of the previous reply
        p.fill(0x42);return p.size();
    },[&]{return now;});
    check(calls==2&&packet.front()==0x42&&packet.back()==0x42,"ZLP followed by complete reply is accepted");
    now=0;calls=0;
    rejects([&]{receiveUsbReply([&](Packet&,unsigned timeout){
        ++calls;check(timeout==21-calls,"empty packets do not renew USB deadline");now+=1000;return size_t(0);
    },[&]{return now;});},"endless ZLPs time out");
    check(now==20000&&calls==20,"USB reply deadline bounds the whole read loop");
    for(size_t size:{size_t(1),size_t(63)})rejects([&]{receiveUsbReply([&](Packet&,unsigned){return size;},[&]{return now;});},"nonempty short reply rejected");
    std::stop_source cancelled;calls=0;
    rejects([&]{receiveUsbReply([&](Packet&,unsigned){++calls;cancelled.request_stop();return size_t(0);},[&]{return now;},cancelled.get_token());},"cancellation checked between ZLP reads");
    check(calls==1,"cancelled USB reply is not read again");
    std::cout<<"PASS USB reply framing: trailing ZLP, deadline, short packet and cancellation\n";
}
void waves() {
    for(auto eye:{Eye::Left,Eye::Right})for(auto kind:{ActionKind::Open,ActionKind::Close}) {
        const auto wave=makeWave(kind,eye);check(wave.count==2||wave.count==4,"token fits FIFO without DMA");
        // Execute the four PIO instructions cycle-by-cycle (1 MHz), including
        // pull and decrement-branch overhead, and recover physical edge times.
        size_t fifo=0;unsigned pc=0;uint32_t osr=0,x=0;unsigned level=0,cycles=0;
        std::vector<unsigned> edges;std::vector<unsigned> levels;
        while(cycles<1000) {
            if(pc==0){if(fifo==wave.count)break;osr=wave.words[fifo++];pc=1;}
            else if(pc==1){unsigned next=osr>>31;if(next!=level){edges.push_back(cycles);levels.push_back(next);level=next;}osr<<=1;pc=2;}
            else if(pc==2){x=osr>>1;pc=3;}
            else {const bool branch=x!=0;--x;pc=branch?3:0;}
            ++cycles;
        }
        check(level==0&&levels.front()==1&&levels.back()==0,"wave stalls LOW");
        std::vector<unsigned> expected;
        if(kind==ActionKind::Open)expected=eye==Eye::Left?std::vector<unsigned>{43}:std::vector<unsigned>{23,46,31};
        else expected=eye==Eye::Left?std::vector<unsigned>{23,21,24}:std::vector<unsigned>{23,78,40};
        check(edges.size()==expected.size()+1,"expected edge count");
        for(size_t i=0;i<expected.size();++i)check(edges[i+1]-edges[i]==expected[i],"PIO pulse duration exact in instruction simulation");
        check(wave.durationUs<250,"physical token fits minimum logical interval");
    }
    check(makeWave(ActionKind::Open,Eye(8)).count==0,"invalid eye produces no IR");
    std::cout<<"PASS IR token PIO instruction simulation: all four token waveforms, edge timing, final LOW\n";
}
}
int main(){try{host();usbReplies();waves();std::cout<<checks<<" checks passed; these tests simulate hardware\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
