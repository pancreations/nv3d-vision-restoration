// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_host.h"
#include "rp2040_wave.h"
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
    DeviceEndpoint device{1234,false};bool corrupt=false,disconnected=false,simulated=false;
    std::vector<Message> requests;
    Packet exchange(const Packet& p,std::stop_token stop)override {
        if(stop.stop_requested()||disconnected)throw std::runtime_error("link disconnected");
        Message m;check(decode(p,m)==Result::Ok,"host request parse");requests.push_back(m);
        time+=20;auto e=device.receive(p,uint64_t(time+5000),uint64_t(time+5000));time+=30;
        check(e.reply.has_value(),"model returns reply");auto packet=*e.reply;
        if(simulated){Message r;decode(packet,r);r.payload[2]|=0x80;packet=*encode(r);}
        if(corrupt)packet[16]^=1;
        return packet;
    }
};
void host() {
    ModelLink link;Client c(link,[&]{return link.time;});c.inspect();
    check(c.status().bootId==1234&&c.status().session==0,"read-only identity");
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
int main(){try{host();waves();std::cout<<checks<<" checks passed; hardware remains untested\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
