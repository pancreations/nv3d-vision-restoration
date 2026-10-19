#include "emitter.h"
#include "platform.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace vision;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Predicate>static void waitFor(Predicate ready,const char* message){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!ready()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(ready(),message);
}
int main(){try{
    Emitter emitter;Settings settings;settings.refresh=240;settings.sequence=Sequence::BlackInsertion;
    emitter.configure(settings);emitter.connect({}, {},true);
    waitFor([&]{return emitter.status().state==EmitterState::Simulated;},"Simulator startup");
    const double p=1./120,first=qpc()+.1;
    require(!emitterCadenceDiscontinuous(Eye::Right,first+p,Eye::Left,first+2*p+.002,p),
            "Fixture must exercise the phase change missed by cadence detection");
    // Old-clock commands already in the FIFO must run before the new clock
    // takes effect. Two new-clock commands need only one recovery.
    emitter.submit(Eye::Left,first,1e6/240,0);
    emitter.submit(Eye::Right,first+p,1e6/240,0);
    emitter.submit(Eye::Left,first+2*p+.002,1e6/240,1);
    emitter.submit(Eye::Right,first+3*p+.002,1e6/240,1);
    // A rejected duplicate carrying the next revision cannot consume it.
    emitter.submit(Eye::Right,first+3*p+.00201,1e6/240,2);
    emitter.submit(Eye::Left,first+4*p+.002,1e6/240,2);
    emitter.submit(Eye::Right,first+5*p+.002,1e6/240,2);
    waitFor([&]{return emitter.status().commands==6;},"Queued commands did not finish");
    auto status=emitter.status();
    require(status.clockRelocks==2&&status.repeatedCommands==1&&status.late==0,
            "Clock changes must recover once each, including after duplicate rejection");
    // Let the FIFO empty, then drop the first command of another clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    emitter.submit(Eye::Left,qpc()-.002,1e6/240,3);
    waitFor([&]{return emitter.status().late==1;},"Stale command was not dropped");
    require(emitter.status().clockRelocks==2,"Dropped command consumed clock recovery");
    emitter.submit(Eye::Right,qpc()+.1,1e6/240,3);
    waitFor([&]{return emitter.status().commands==7;},"Recovery after stale command did not finish");
    require(emitter.status().clockRelocks==3,"Stale command swallowed the next clock recovery");
    // An explicit suspend begins a new session; its initial revision is a
    // baseline, even if the worker has not yet observed suspended_=true.
    emitter.suspend();emitter.submit(Eye::Left,qpc()+.1,1e6/240,0);
    waitFor([&]{return emitter.status().commands==8;},"New generation did not start");
    status=emitter.status();
    require(status.clockRelocks==3&&status.errors==0&&status.timingWrites==0,
            "New generation spuriously recovered or simulation wrote USB configuration");
    emitter.disconnect();
    std::cout<<"PASS: queued clock revisions, sub-half-period phase recovery, duplicate/late rejection, generation reset; simulated emitter only\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
