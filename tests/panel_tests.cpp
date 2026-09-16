#include "core.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace vision;
static void require(bool b,const char* m){if(!b)throw std::runtime_error(m);}
template<class F>static void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}require(threw,"Expected rejection");}
int main(){try{
    for(double hz:{120.,144.,240.})for(auto drive:{PanelDrive::Direct,PanelDrive::BlackGuard,PanelDrive::NeutralGuard,PanelDrive::Preload}){
        Settings s;s.refresh=hz;s.panelResponseUs=7311;s.panelRiseUs=2719;s.cancelCrosstalk=true;s.blackFloor=.1f;s.imageGain=4;
        applyPanelDrive(s,drive);validate(s);
        require(!s.cancelCrosstalk&&s.blackFloor==0&&s.imageGain==1&&s.bandHeight==1,"Experiments must not depend on cancellation or crop");
        require(s.panelResponseUs==7311&&s.panelRiseUs==2719,"A drive experiment must not invent panel measurements");
        require((s.guardLevel>0)==(drive==PanelDrive::NeutralGuard),"Only neutral reset changes guard light");
        unsigned triggers=0;for(unsigned i=0;i<cycleLength(s.sequence);++i){auto slot=sequenceSlot(s.sequence,i,false);if(slot.trigger){require(slot.eye!=Eye::Black,"No emitter command for reset slots");++triggers;}}
        require(triggers==2,"Exactly one trigger per eye per stereo pair");
        if(drive==PanelDrive::Preload){
            require(sequenceSlot(s.sequence,0,false).eye==Eye::Left&&!sequenceSlot(s.sequence,0,false).trigger,"Preload first refresh settles");
            require(sequenceSlot(s.sequence,1,false).eye==Eye::Left&&sequenceSlot(s.sequence,1,false).trigger,"Second refresh views the same eye");
            require(sequenceSlot(s.sequence,0,false).pairBoundary&&!sequenceSlot(s.sequence,1,false).pairBoundary,"Preload must hold one source pair");
        }
        require(std::abs(hz/cycleLength(s.sequence)-(drive==PanelDrive::Direct?hz/2:hz/4))<1e-9,"Honest frames per eye");
        auto file=std::filesystem::temp_directory_path()/"vision-panel-experiment-test.ini";saveProfile(file,s);auto loaded=loadProfile(file);std::filesystem::remove(file);
        require(loaded.guardLevel==s.guardLevel&&loaded.sequence==s.sequence&&loaded.phaseUs==s.phaseUs,"Experiment profile roundtrip");
    }
    Settings s;auto key=opticalCalibrationKey(s);s.phaseUs=1000;require(opticalCalibrationKey(s)==key,"Phase is the sweep variable");
    s.leftUs+=1;require(opticalCalibrationKey(s)!=key,"A new shutter invalidates optical marks");s=Settings{};s.guardLevel=.5f;require(opticalCalibrationKey(s)!=key,"Reset invalidates optical marks");
    s=Settings{};s.displayId="other";require(opticalCalibrationKey(s)!=key,"Display invalidates optical marks");
    s=Settings{};s.hdr=true;require(opticalCalibrationKey(s)!=key,"HDR invalidates optical marks");
    s=Settings{};s.guardLevel=std::numeric_limits<float>::quiet_NaN();rejects([&]{validate(s);});s.guardLevel=1.1f;rejects([&]{validate(s);});s.guardLevel=-.1f;rejects([&]{validate(s);});
    std::array<PhaseWindow,6> ranges{{{100,600},{200,700},{300,800},{250,600},{350,700},{400,550}}};
    auto common=intersectPhaseWindows(1000,ranges);require(common.size()==1&&common[0].beginUs==400&&common[0].endUs==550,"Both lenses and all thirds must overlap");
    ranges[5]={800,900};require(intersectPhaseWindows(1000,ranges).empty(),"One bad region rules out the phase");
    std::array<PhaseWindow,2> wrap{{{900,200},{950,100}}};common=intersectPhaseWindows(1000,wrap);require(common.size()==1&&common[0].beginUs==950&&common[0].endUs==100,"Zero crossing must not discard a valid common range");
    wrap={{{100,900},{800,200}}};common=intersectPhaseWindows(1000,wrap);require(common.size()==2,"Disconnected common ranges remain separate");
    wrap={{{100,200},{200,300}}};require(intersectPhaseWindows(1000,wrap).empty(),"Touching endpoints leave no usable phase margin");
    wrap={{{100,100},{0,200}}};require(intersectPhaseWindows(1000,wrap).empty(),"Equal marks are not a full-cycle pass");
    require(intersectPhaseWindows(1000,{}).empty(),"No measurements is not a pass");
    wrap[0]={0,1000};rejects([&]{intersectPhaseWindows(1000,wrap);});rejects([&]{intersectPhaseWindows(0,{});});
    // Independent membership oracle on a discrete circle, including wrapped/disjoint cases.
    for(int a=0;a<10;a++)for(int b=0;b<10;b++)for(int c=0;c<10;c++)for(int d=0;d<10;d++){
        std::array<PhaseWindow,2> w{{{double(a),double(b)},{double(c),double(d)}}};auto out=intersectPhaseWindows(10,w);
        auto contains=[](PhaseWindow z,double x){return z.beginUs<z.endUs?(x>z.beginUs&&x<z.endUs):z.beginUs>z.endUs&&(x>z.beginUs||x<z.endUs);};
        for(int i=0;i<10;i++){double x=i+.5;bool expected=contains(w[0],x)&&contains(w[1],x),actual=false;for(auto z:out)actual|=contains(z,x);require(actual==expected,"Circular intersection membership mismatch");}
    }
    std::cout<<"PASS: panel experiments, profile compatibility, fixed-shutter optical range intersections and invalidation. No optical hardware claim.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
