#include "core.h"
#include "lcd_rp2040.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace vision;
namespace {unsigned checks=0;void check(bool b,const char* text){++checks;if(!b)throw std::runtime_error(text);}}
int main(){try {
    LcdTiming t;t.enabled=true;
    auto e=lcdExposure(t,120);check(e.valid&&std::abs(e.periodUs-8333.333333)<.001,"120 refreshes are 8.333 ms each");
    check(e.openUs[0]==4000&&e.closeUs[0]==4750,"settling precedes aperture");
    for(double hz:{100.,119.88,120.,144.,165.,180.,200.,239.991,240.}) {
        auto x=t;x.settleUs=1e6/hz*.55;x.durationUs=8000;
        check(fitLcdDuration(x,hz),"all target refresh rates use dynamic exposure bounds");
        auto w=lcdExposure(x,hz);check(w.valid&&w.closeUs[0]<=w.periodUs-x.guardUs,"shutter cannot cross next-eye guard");
        check(w.openUs[0]>=x.guardUs&&w.durationUs>=250,"start guard and minimum duration");
    }
    t.durationUs=8000;check(!lcdExposure(t,120).valid,"overlong requested exposure is rejected");
    check(fitLcdDuration(t,120)&&t.durationUs==3983,"automatic sweep fit preserves guard and cadence margin");
    t.phaseUs=8333;check(!fitLcdDuration(t,120),"phase cannot move exposure into the next eye");
    t=LcdTiming{};t.settleUs=5000;t.phaseUs=-1000;t.leftAdjustUs=-100;t.rightAdjustUs=100;
    e=lcdExposure(t,120);check(e.valid&&e.openUs[0]==3900&&e.openUs[1]==4100,"independent eye phase corrections");
    t.phaseUs=-5000;check(!lcdExposure(t,120).valid,"start guard cannot be crossed");
    t=LcdTiming{};t.compensateScanout=true;t.settleUs=1000;t.durationUs=500;
    check(!lcdExposure(t,120).valid,"unknown scanout is not silently guessed");
    e=lcdExposure(t,120,6000);check(e.valid&&e.openUs[0]==4000,"progressive scan shifts chosen reference row");
    for(int region=0;region<4;++region){t.target=region;check(lcdExposure(t,120,6000).openUs==e.openUs,"calibration target does not retime or crop the exposure");}
    auto sweep=lcdSettleSweep(LcdTiming{},120);check(sweep.size()>20,"useful settling sweep coverage");
    for(size_t i=0;i<sweep.size();++i){check(lcdExposure(sweep[i],120).valid,"every sweep candidate fits");if(i)check(sweep[i].settleUs>sweep[i-1].settleUs&&sweep[i].durationUs<=sweep[i-1].durationUs,"later settling narrows aperture");}
    LcdObservation clean{sweep.back(),120,0,{1,1,1,1,1,1}},bright{sweep.front(),120,0,{1,1,2,1,1,1}};
    check(lcdObservationBetter(clean,bright),"clean narrow aperture wins over brighter ghosting");
    bright.crosstalk.fill(1);check(lcdObservationBetter(bright,clean),"brightness breaks separation ties");
    bright.crosstalk[5]=0;check(!lcdObservationComplete(bright)&&!lcdObservationBetter(bright,clean),"unobserved region cannot win");
    for(double hz:{100.,120.,144.,165.,180.,200.,240.}){
        auto current=LcdTiming{};current.settleUs=1e6/hz*.4;current.enabled=true;check(fitLcdDuration(current,hz),"live edit baseline valid");
        for(auto member:{&LcdTiming::settleUs,&LcdTiming::durationUs,&LcdTiming::phaseUs,&LcdTiming::leftAdjustUs,&LcdTiming::rightAdjustUs,&LcdTiming::guardUs,&LcdTiming::scanoutUs,&LcdTiming::referencePosition})
            for(double value:{-10000.,0.,250.,1000.,8000.,50000.}){
                auto edited=current;edited.*member=value;
                check(constrainLcdAdjustment(edited,current,hz),"slider request constrained without stopping");
                check(lcdExposure(edited,hz).valid,"live constrained edit respects both frame guards");
                auto unrelated=edited;unrelated.*member=current.*member;
                check(unrelated==current,"one slider never moves any other slider");
                const auto config=lcdDeviceConfig(edited,1e6/hz);
                check(rp2040::validConfig(config),"every accepted live edit also passes the firmware checks");
                // Both eye orders must still fit when the following anchor
                // arrives at the earliest cadence the firmware will accept.
                for(auto first:{rp2040::Eye::Left,rp2040::Eye::Right}){
                    rp2040::Scheduler device;device.begin(1,1);device.configure(1,config,1);
                    check(device.enqueue({1,1,10000,first},1)==rp2040::Result::Ok,"first aperture accepted");
                    const auto second=first==rp2040::Eye::Left?rp2040::Eye::Right:rp2040::Eye::Left;
                    check(device.enqueue({1,2,10000+config.periodUs-100,second},2)==rp2040::Result::Ok,"boundary edit preserves next-eye guard despite clock jitter");
                }
            }
        auto edited=current;edited.compensateScanout=true;edited.scanoutUs=50000;
        check(constrainLcdAdjustment(edited,current,hz)&&lcdExposure(edited,hz).valid,"invalid scan toggle retains a working aperture");
    }
    // This exact kind of fractional guard passed the old UI epsilon check,
    // then rounded to 251 us on a board whose left opening was at 250 us.
    t=LcdTiming{};t.settleUs=250;t.guardUs=250.0000005;
    check(!lcdExposure(t,120).valid,"fractional start guard cannot pass UI and fail firmware");
    t=LcdTiming{};t.settleUs=250;auto requested=t;requested.guardUs=2000;
    check(constrainLcdAdjustment(requested,t,120)&&requested==t,"start guard slider cannot move settling or duration");
    t=LcdTiming{};t.settleUs=4540;t.durationUs=1460;t.phaseUs=-4270.9999999999991;t.leftAdjustUs=-20;
    check(rp2040::validConfig(lcdDeviceConfig(t,8333.54)),"user's saved boundary timing reaches firmware unchanged");
    {
        auto applied=LcdTiming{};auto draft=applied;draft.settleUs=8000;
        const auto previous=applied;
        check(!applyLcdRequest(draft,applied,120)&&draft.settleUs==8000&&applied==previous,"out-of-window draft remains editable and leaves applied timing alone");
        draft.phaseUs=-4000;draft.durationUs=3000;
        check(applyLcdRequest(draft,applied,120)&&applied==draft,"second control can make a previously invalid draft valid");
        draft.durationUs=8000;
        check(!applyLcdRequest(draft,applied,240)&&draft.durationUs==8000,"requested duration stays at slider position at a different refresh");
        auto oled=LcdTiming{};oled.settleUs=0;oled.phaseUs=841;oled.durationUs=1265;
        check(rp2040::validConfig(lcdDeviceConfig(oled,1e6/120)),"OLED 120 uses the same aperture controls");
        oled.phaseUs=420.5;oled.durationUs=632.5;
        check(rp2040::validConfig(lcdDeviceConfig(oled,1e6/240)),"OLED 240 uses the same aperture controls");
        oled.phaseUs=2618;oled.durationUs=4501;
        auto bfi=lcdDeviceConfig(oled,1e6/240,0,2,2);
        check(bfi.periodUs==8333&&bfi.frameAnchored&&bfi.leftUs==4501,"240 BFI may expose into its black frame without crossing the next eye");
        auto slow=lcdDeviceConfig(oled,1e6/120,0,2,2);
        check(slow.periodUs==16666&&!slow.frameAnchored,"120 BFI uses the existing long-cadence firmware path");
        auto repeated=lcdDeviceConfig(previous,1e6/120,0,1,2);
        check(!repeated.frameAnchored&&repeated.leftOpenUs+repeated.leftUs<8333,"held frames stop exposure before opposite-eye content");
        Settings shared;shared.refresh=240;shared.sequence=Sequence::BlackInsertion;shared.lcd=oled;shared.lcd.enabled=true;validate(shared);
        check(apertureWindowHz(240,Sequence::BlackInsertion)==120,"BFI window accounts for black refresh at measured rate");
        // Official emitter still receives its original timing-register packets.
        const auto packet=timingPacket(120,841,1265);
        check(packet.size()==28&&nvidiaEffectiveShutterUs(120,841,1265)==1265,"official emitter preserves established OLED 120 phase/duration");
    }
    auto file=std::filesystem::temp_directory_path()/"vision-lcd-aperture-test.ini";
    Settings s;s.lcd.enabled=true;s.lcd.leftAdjustUs=-100;s.lcd.rightAdjustUs=200;s.lcd.target=3;s.signalScanUs=7000;
    saveProfile(file,s);auto read=loadProfile(file);check(read.lcd==s.lcd&&read.signalScanUs==0,"LCD profile round trip; signal timing is refreshed at runtime");
    s.lcd.enabled=false;s.phaseUs=841;s.leftUs=s.rightUs=1265;s.sequence=Sequence::Alternating;saveProfile(file,s);read=loadProfile(file);
    check(!read.lcd.enabled&&read.phaseUs==841&&read.leftUs==1265,"OLED 120 timing remains unchanged and aperture is opt-in");
    s.refresh=239.991;s.sequence=Sequence::BlackInsertion;s.phaseUs=2618;s.leftUs=s.rightUs=4501;saveProfile(file,s);read=loadProfile(file);
    check(read.sequence==Sequence::BlackInsertion&&read.refresh==s.refresh&&read.phaseUs==2618&&read.leftUs==4501&&!read.lcd.enabled,"OLED 240 BFI profile remains unchanged");
    std::filesystem::remove(file);
    std::cout<<checks<<" LCD aperture checks passed; no optical behavior inferred\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
