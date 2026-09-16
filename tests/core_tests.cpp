#include "core.h"
#include "rp2040_timing.h"
#include "sync_protocol.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace vision;
static int checks=0;
static void require(bool b,const char* msg){++checks;if(!b)throw std::runtime_error(msg);}
template<class F>void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}require(threw,"Expected validation failure");}
int main(){try{
    require(rp2040TimingSupported(120,Sequence::Repeated,true),"RP2040 LCD 120 Hz preload supported");
    require(rp2040TimingSupported(119.88,makeSequence(4,0),true),"Fractional TV with four held refreshes supported");
    require(rp2040TimingSupported(60,Sequence::Repeated,true),"60 Hz LCD can test 15 images per eye/s");
    require(!rp2040TimingSupported(60,makeSequence(3,0),true),"Too slow to maintain supported cadence");
    require(!rp2040TimingSupported(120,Sequence::Repeated,false),"Old firmware cannot offer preload");
    require(rp2040TimingSupported(240,Sequence::Alternating,true,true),"0.4 firmware offers OLED 240 Hz LR");
    require(rp2040TimingSupported(240,Sequence::BlackInsertion,true),"OLED 240 Hz BFI still works with 0.3");
    require(!rp2040TimingSupported(240,Sequence::Alternating,true),"0.3 firmware cannot offer direct 240 Hz");
    for(auto seq:{Sequence::Alternating,Sequence::Repeated,makeSequence(3,1)}) {
        const double p=periodUs(sequenceEmitterHz(120,seq));
        for(double phase:{-100.,0.,100.,p-100,p,p+100,2*p-100}) {
            auto mapped=rp2040Phase(phase,120,seq);
            require(mapped.delayUs>=0&&mapped.delayUs<p,"RP2040 targets stay within one eye interval");
            require(std::abs(mapped.delayUs+(mapped.flipEye?p:0)-wrapPhase(phase,2*p))<1e-6,"Full cycle phase retains eye parity");
        }
        unsigned triggers=0;
        for(unsigned i=0;i<cycleLength(seq);++i){auto slot=sequenceSlot(seq,i,false);if(slot.trigger){++triggers;require(i%(cycleLength(seq)/2)==sequenceHold(seq)-1,"RP2040 opens only after preload refreshes");}}
        require(triggers==2,"Exactly one opening per eye in repeated/black cycle");
    }
    // Retired image processing must not come back through an old saved profile.
    for(int version:{3,4}){
        auto file=std::filesystem::temp_directory_path()/"vision-retired-blending-test.ini";
        {std::ofstream f(file);f<<"version "<<version<<"\nwidth 1920\nheight 1080\nrefresh 120\nphase 2000\nleft 1200\nright 900\ndepth 0.04\nconvergence 0.01\nghost_left 0.5\nghost_right 0.4\ncross 1\nvalidated 1\n";}
        auto clean=loadProfile(file);
        require(clean.phaseUs==2000&&clean.leftUs==1200&&clean.rightUs==900,"Retiring image blending preserves saved timing");
        require(std::abs(clean.convergence-.01f)<1e-6&&!clean.validated,"Old image processing cannot carry optical validation forward");
        saveProfile(file,clean);std::ifstream input(file);std::string saved((std::istreambuf_iterator<char>(input)),{});input.close();
        require(saved.find("ghost_")==std::string::npos&&saved.find("cross ")==std::string::npos&&saved.find("version 11")!=std::string::npos,"Migrated profiles cannot restore cross-eye blending or subtraction");
        std::filesystem::remove(file);
    }
    require(refreshRatesMatch(144,143.988)&&refreshRatesMatch(120,119.88),"Nominal presets accept fractional monitor rates");
    require(!refreshRatesMatch(144,120)&&!refreshRatesMatch(165,144)&&!refreshRatesMatch(240,144),"Different IR and monitor rates trigger mismatch");
    require(!refreshRatesMatch(144,std::nan(""))&&!refreshRatesMatch(144,0),"Missing monitor rate never matches a preset");
    for(unsigned hz:{120u,144u,165u,240u}){
        Settings preset;preset.refresh=120;preset.phaseUs=2400;preset.leftUs=1200;preset.rightUs=1000;preset.depth=.08f;preset.convergence=.02f;preset.validated=true;
        applyTimingPreset(preset,hz);double scale=120.0/hz;
        require(preset.refresh==hz&&std::abs(preset.phaseUs-2400*scale)<1e-8,"IR preset scales phase with the frame period");
        require(std::abs(preset.leftUs-1200*scale)<1e-8&&std::abs(preset.rightUs-1000*scale)<1e-8,"IR preset preserves each eye's duty cycle");
        require(preset.depth==.08f&&preset.convergence==.02f&&!preset.validated,"IR preset preserves image settings and clears optical validation");
        auto p=timingPacket(preset.refresh,preset.phaseUs,preset.leftUs);require(p.size()==28,"Every IR preset generates emitter timers");
        applyTimingPreset(preset,120);require(std::abs(preset.phaseUs-2400)<1e-8&&std::abs(preset.leftUs-1200)<1e-8,"Switching rates back preserves timing ratios");
    }
    {Settings s;s.leftUs=250;s.rightUs=3000;applyTimingPreset(s,240);require(s.leftUs==250&&s.rightUs==1500,"IR preset respects minimum shutter duration");rejects([&]{applyTimingPreset(s,90);});require(s.refresh==240,"Unsupported preset leaves settings intact");applyTimingPreset(s,100);require(s.refresh==100&&std::abs(s.leftUs-600)<1e-9&&std::abs(s.rightUs-std::min(3600.0,calibrationMaxShutterUs(100)))<1e-9,"100 Hz preset scales shutter by the frame period");}
    // Recorded display-refresh anchors: queue depth and composition must not
    // invent an extra refresh between an image and its shutter command.
    for(uint64_t displayedRefresh:{1000ull,1001ull,2005ull})for(uint32_t queued=1;queued<=3;queued++){
        auto refresh=refreshForPresent(40+queued,40,displayedRefresh);
        require(refresh==displayedRefresh+queued,"Present maps directly to its display refresh");
        require(presentForRefresh(refresh,40,displayedRefresh)==40+queued,"Game and presenter agree on the same image");
        require(sequenceSlot(Sequence::Alternating,refresh,false).eye==sequenceSlot(Sequence::Alternating,displayedRefresh+queued,false).eye,"Queue latency preserves shutter eye");
    }
    require(refreshForPresent(0,UINT32_MAX,50)==51,"Present counter wrap preserves next refresh");
    require(presentForRefresh(51,UINT32_MAX,50)==0,"Inverse present mapping handles counter wrap");
    require(refreshForPresent(42,41,1002)==1003,"Late frame feedback reanchors the next prediction");
    Settings identity;identity.emitterId="rp2040:serial1";identity.emitterFirmware="v1";
    require(identity.convergence==0,"Convergence defaults to unchanged alignment");
    for(float value:{-.05f,.023f,.05f}){Settings c;c.convergence=value;auto file=std::filesystem::temp_directory_path()/"vision-convergence-test.ini";saveProfile(file,c);require(loadProfile(file).convergence==value,"Convergence profile round trip");std::filesystem::remove(file);}
    for(float value:{-.051f,.051f,std::nanf(""),INFINITY}){Settings c;c.convergence=value;rejects([&]{validate(c);});}
    for(int version:{1,2}){auto file=std::filesystem::temp_directory_path()/"vision-legacy-convergence-test.ini";{std::ofstream f(file);f<<"version "<<version<<"\nwidth 1920\nheight 1080\nrefresh 120\n";}require(loadProfile(file).convergence==0,"Legacy profiles retain original alignment");std::filesystem::remove(file);}
    auto identityKey=profileKey(identity);identity.emitterId="rp2040:serial2";require(profileKey(identity)!=identityKey,"Emitter identity isolates profiles");
    identity.emitterId="rp2040:serial1";identity.emitterFirmware="v2";require(profileKey(identity)!=identityKey,"Firmware isolates profiles");
    auto identityPath=std::filesystem::temp_directory_path()/"vision-emitter-profile-test.ini";saveProfile(identityPath,identity);auto loadedIdentity=loadProfile(identityPath);require(loadedIdentity.emitterId==identity.emitterId&&loadedIdentity.emitterFirmware==identity.emitterFirmware,"Emitter profile round trip");std::filesystem::remove(identityPath);
    require(std::abs(periodUs(120)-8333.333333)<.001,"120 Hz period");require(std::abs(periodUs(144)-6944.444444)<.001,"144 Hz period");rejects([]{periodUs(0);});
    // Decode the wire timers as the FX2 does, independently of nvidiaSchedule.
    // Negative reloads count UP: subtracting 214/264/364 ticks delays X.
    auto wireTimer=[](const auto& bytes,size_t offset,double mhz){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=uint32_t(bytes[offset+i])<<(8*i);return (1.0-int32_t(v))/mhz;};
    for(double hz:{60.0,72.0,100.0,120.0,144.0,165.0,240.0}){
        double p=periodUs(hz),band=p>9523.75?91.0:p>8695.6?66.0:53.5;
        for(double phase=0;phase<2*p;phase+=25){
            auto config=timingPacket(hz,phase,500);auto command=eyePacket(Eye::Left,hz,phase);
            double opening=wireTimer(command,4,12)-6+wireTimer(config,8,4)+band;
            double distance=wrapSignedPhase(opening-phase,p);
            require(std::abs(distance)<.5,"Firmware countdown puts phase at the requested time");
            bool followingPeriod=std::llround((opening-phase)/p)%2!=0;
            require(command[1]==(followingPeriod?0xfe:0xff),"Phase wrap preserves eye identity across the full stereo cycle");
            double leftOpening=opening+(command[1]==0xfe?p:0);
            require(std::abs(wrapSignedPhase(leftOpening-phase,2*p))<.5,"Extended phase moves the same lens across the entire stereo cycle");
            auto rightCommand=eyePacket(Eye::Right,hz,phase);
            require(rightCommand[1]!=(command[1]),"The other image always commands the other lens");
            auto wider=timingPacket(hz,phase,600);
            require(wireTimer(wider,8,4)==wireTimer(config,8,4)&&wireTimer(wider,12,4)-wireTimer(config,12,4)==100,"Shutter edit moves the close countdown without moving the open countdown");
            require(wireTimer(config,8,4)+band+wireTimer(config,12,4)+1000<=p+.5,"Wire timer sequence fits before next boundary");
        }
        for(double phase:{0.0,1000.0,p-.25}){
            require(eyePacket(Eye::Left,hz,phase)[1]!=eyePacket(Eye::Left,hz,phase+p)[1],"An added eye period is a different physical phase");
            auto original=eyePacket(Eye::Left,hz,phase),repeated=eyePacket(Eye::Left,hz,phase+2*p);
            require(original[1]==repeated[1]&&std::abs(wireTimer(original,4,12)-wireTimer(repeated,4,12))<.1,"A complete stereo cycle repeats within one timer tick");
        }
    }
    for(Sequence sequence:{Sequence::Alternating,Sequence::BlackInsertion,Sequence::Repeated}){
        Settings extended;extended.refresh=144;extended.sequence=sequence;double cycle=phaseCycleUs(extended.refresh,sequence);
        require(std::abs(cycle-(sequence==Sequence::Alternating?2:4)*1000000.0/144)<1e-6,"Phase range spans both eyes for every sequence");
        extended.phaseUs=cycle-10;auto file=std::filesystem::temp_directory_path()/"vision-extended-phase-test.ini";
        saveProfile(file,extended);require(loadProfile(file).phaseUs==extended.phaseUs,"Extended phase survives profile reload");std::filesystem::remove(file);
        extended.phaseUs=cycle+1;rejects([&]{validate(extended);});
    }
    require(wrapPhase(-10,100)==90,"Negative phase wrap");require(wrapPhase(210,100)==10,"Positive phase wrap");
    require(wrapSignedPhase(100,100)==0,"Full refresh is zero phase");require(wrapSignedPhase(90,100)==-10,"Late full-frame phase becomes schedulable negative phase");require(wrapSignedPhase(-60,100)==40,"Signed phase wraps continuously");
    require(wrapSignedPhase(100,100)==0,"Full-refresh phase is zero");require(wrapSignedPhase(90,100)==-10,"Late full-frame phase schedules early");require(wrapSignedPhase(-60,100)==40,"Negative phase wraps into schedulable half-frame");
    for(uint64_t i=0;i<100;i++){auto a=sequenceSlot(Sequence::Alternating,i,false);require(a.eye==(i%2?Eye::Right:Eye::Left),"Eye order");require(a.trigger,"Every alternating slot triggers");require(sequenceSlot(Sequence::Alternating,i,true).eye!=a.eye,"Eye swap");}
    auto b=sequenceSlot(Sequence::BlackInsertion,1,false);require(b.eye==Eye::Black&&!b.trigger,"Black slot has no trigger");require(sequenceSlot(Sequence::Repeated,1,false).trigger,"Repeated slot opens after settle");require(!sequenceSlot(Sequence::Repeated,0,false).trigger,"Settling slot no trigger");
    auto packet=timingPacket(120,3000,2080);require(packet.size()==28,"Timing packet length");require(packet[4]==0xdb&&packet[5]==0x29&&packet[6]==0xff&&packet[7]==0xff,"W timer little endian");require(packet[24]==0x61&&packet[25]==0x79&&packet[26]==0xfe&&packet[27]==0xff,"Period timer");require(packet[12]==0x81&&packet[13]==0xdf&&packet[14]==0xff,"Duration timer");
    auto shifted=timingPacket(120,3100,2080);require(shifted[8]==packet[8]&&shifted[9]==packet[9],"Small phase steps move the emitter boundary, not X");require(shifted[12]==packet[12]&&shifted[13]==packet[13],"Phase never changes shutter duration");rejects([]{timingPacket(120,std::nan(""),1000);});
    auto l=eyePacket(Eye::Left,120,3000),r=eyePacket(Eye::Right,120,3000);require(l[0]==0xaa&&l[1]==0xff&&r[1]==0xfe,"Explicit eye commands");require(eyePacket(Eye::Left,120,3100)[4]!=l[4]||eyePacket(Eye::Left,120,3100)[5]!=l[5],"Phase moves the boundary distance in the eye command");rejects([]{eyePacket(Eye::Black,120,0);});rejects([]{timingPacket(120,0,10000);});
    // Firmware guard: open token + shutter + close token must finish inside one period, or the emitter skips the other eye.
    for(double hz:{100.0,120.0,144.0,165.0,240.0})for(double phase=0;phase<periodUs(hz);phase+=25){auto s=nvidiaSchedule(hz,phase);double p=periodUs(hz);require(std::abs(wrapPhase(s.openAfterCommandUs,p)-phase)<1e-6,"Schedule reproduces the requested window start");require(s.boundaryUs>=nvidiaBoundaryMarginUs-1e-9&&s.boundaryUs<=p-nvidiaBoundaryMarginUs+1e-9,"Boundary stays inside its safe range");require(s.delayUs>=nvidiaDelayCenterUs-nvidiaBoundaryMarginUs-1e-9&&s.delayUs<=nvidiaDelayCenterUs+nvidiaBoundaryMarginUs+1e-9,"X stays inside its safe range");require(s.delayUs+nvidiaBandCorrectionUs(hz)+nvidiaEffectiveShutterUs(hz,phase,nvidiaMaxShutterUs(hz))+nvidiaSequenceGuardUs<=p+1e-6,"Token sequence never overruns the period");
        if(s.delayUs==nvidiaDelayMinUs)require(nvidiaEffectiveShutterUs(hz,phase,nvidiaMaxShutterUs(hz))==nvidiaMaxShutterUs(hz),"At the shortest X the full ceiling is honoured");}
    {unsigned shortest=0,total=0;for(double phase=0;phase<periodUs(120);phase+=25){++total;if(nvidiaSchedule(120,phase).delayUs==nvidiaDelayMinUs)++shortest;}require(shortest*4>total*3,"The schedule keeps X at its minimum over most of the cycle");
     require(std::abs(nvidiaMaxShutterUs(120)-(periodUs(120)-nvidiaDelayMinUs-nvidiaBandCorrectionUs(120)-nvidiaSequenceGuardUs))<1e-9&&nvidiaMaxShutterUs(120)>6900,"120 Hz emitter period allows a 6.9 ms shutter");
     require(nvidiaEffectiveShutterUs(120,2618,4501)==4501&&std::abs(nvidiaSchedule(120,2618).openAfterCommandUs-2618)<1e-9,"The confirmed OLED phase keeps its window start and full shutter");
     rejects([]{nvidiaEffectiveShutterUs(120,0,-1);});}
    {auto l=nvidiaEyeTiming(120,3000,Eye::Left,1500,1200,500),r=nvidiaEyeTiming(120,3000,Eye::Right,1500,1200,500);require(std::abs(r.delayUs-l.delayUs-500)<1e-9&&l.durationUs==1500&&r.durationUs==1200,"Right eye offset and per-eye durations");auto clamped=nvidiaEyeTiming(120,3000,Eye::Right,1500,1500,-5000);require(clamped.delayUs==150,"Offset clamps at the minimum X");auto w=eyeTimingPacket(120,r.delayUs,r.durationUs);require(w.size()==12&&w[0]==1&&w[1]==4&&w[2]==8&&w[3]==0,"Per-eye X/Y write targets block offset 4");rejects([]{eyeTimingPacket(120,7000,3000);});}
    require(std::abs(nvidiaLegacyPhaseUs(120,6528)-2872.05)<1,"Version 3 phase converts to the window start it produced");require(std::abs(nvidiaLegacyPhaseUs(144,3668)-629.3)<1,"Version 3 conversion at 144 Hz");
    Settings s;s.name="G8 \"HDR\" calibration";s.displayId="Samsung-path";s.connection="DP:0";s.hdr=true;s.monitorNotes="Line one\nLine two";s.phaseUs=-20;s.rightUs=1200;
    auto path=std::filesystem::temp_directory_path()/"vision-core-profile-test.ini";saveProfile(path,s);auto copy=loadProfile(path);require(copy.name==s.name&&copy.monitorNotes==s.monitorNotes&&copy.hdr&&copy.phaseUs==-20&&copy.rightUs==1200,"Profile round trip");s.phaseUs=30;saveProfile(path,s);require(loadProfile(path).phaseUs==30,"Replace profile");std::filesystem::remove(path);
    auto key=profileKey(s);s.hdr=false;require(profileKey(s)!=key,"SDR HDR profile isolation");s.refresh=144;require(profileKey(s)!=key,"Refresh isolation");s.leftUs=99999;rejects([&]{validate(s);});s.leftUs=1000;s.refresh=std::nan("");rejects([&]{validate(s);});
    std::vector<uint8_t> fw{0,1,0xe6,0,1,0,3,0,0,2,3,4,0,1,0xe6,0,0};auto blocks=parseFirmware(fw);require(blocks.size()==3&&blocks[1].data.size()==3,"Firmware records");fw.pop_back();rejects([&]{parseFirmware(fw);});rejects([]{extractFirmware(std::vector<uint8_t>{'M','Z'});});
    TimingTracker timing;for(unsigned i=1;i<=20;i++)timing.observe(i,10+i/120.0);require(timing.samples>=8&&std::abs(timing.period-1/120.0)<1e-9,"Refresh estimator");require(std::abs(timing.predict(21)-(10+21/120.0))<1e-9,"Refresh prediction");auto samples=timing.samples;timing.observe(20,10+20/120.0);require(samples==timing.samples,"Duplicate stats ignored");timing.observe(21,10+20/120.0+1/60.0);require(timing.samples<8,"Mode change loses lock");
    {   // Stereo area: settled-window geometry of a panel that rewrites rows top to bottom.
        auto full=settledWindow(120,Sequence::Alternating,8100,1,.5,200);require(full.closeUs-full.openUs<250,"4K 120 Hz Left/Right: the whole-screen window is shorter than any shutter when the scan fills the refresh");
        auto band=settledWindow(120,Sequence::Alternating,8100,.25,.5,200);require(std::abs((band.closeUs-band.openUs)-(1e6/120-200-8100*.25))<1e-6,"Band window = hold - response - scan*height");
        auto moved=settledWindow(120,Sequence::Alternating,8100,.25,.2,200);require(std::abs((moved.closeUs-moved.openUs)-(band.closeUs-band.openUs))<1e-6&&moved.openUs<band.openUs,"Band position moves the window without changing its length");
        auto repeated=settledWindow(240,Sequence::Repeated,4050,1,.5,200);require(std::abs((repeated.closeUs-repeated.openUs)-(1e6/120-200-4050))<1e-6,"240 Hz Left/Left/Right/Right holds each eye for two refreshes");
        auto inserted=settledWindow(240,Sequence::BlackInsertion,4050,1,.5,200);require(inserted.closeUs-inserted.openUs<0,"Black insertion keeps a one-refresh hold");
        require(std::abs(bandHeightForShutter(120,Sequence::Alternating,8100,1800,200,500)-(1e6/120-200-1800-500)/8100)<1e-9,"Fit band to shutter");
        require(bandHeightForShutter(240,Sequence::Repeated,4050,1500,200,500)==1,"Whole screen fits at 240 Hz repeated");require(bandHeightForShutter(120,Sequence::Alternating,8100,7000,200,500)==.1,"Band never below 10%");require(bandHeightForShutter(120,Sequence::Alternating,0,1500,200,500)==1,"Unknown scan keeps the whole screen");
        double phase=suggestedPhaseUs(120,Sequence::Alternating,band,1800);require(std::abs(phase-(band.openUs+((band.closeUs-band.openUs)-1800)/2))<1e-9,"Suggested phase centers the shutter in the window");
        require(suggestedPhaseUs(120,Sequence::Alternating,full,1800)==wrapPhase(full.openUs,1e6/60),"A window shorter than the shutter opens with the window");
        Settings area;area.bandHeight=.4f;area.bandCenter=.3f;area.panelResponseUs=2500;area.panelScanUs=6000;validate(area);auto file=std::filesystem::temp_directory_path()/"vision-band-test.ini";saveProfile(file,area);auto loaded=loadProfile(file);
        require(std::abs(loaded.bandHeight-.4f)<1e-6&&std::abs(loaded.bandCenter-.3f)<1e-6&&loaded.panelResponseUs==2500&&loaded.panelScanUs==6000,"Stereo area survives profile reload");std::filesystem::remove(file);
        require(Settings{}.bandHeight==1&&Settings{}.bandCenter==.5f,"Whole output by default");
        for(float h:{.05f,1.01f,std::nanf("")}){Settings c;c.bandHeight=h;c.bandCenter=.5f;rejects([&]{validate(c);});}
        {Settings c;c.bandHeight=.5f;c.bandCenter=.1f;rejects([&]{validate(c);});}{Settings c;c.panelResponseUs=-1;rejects([&]{validate(c);});}{Settings c;c.panelScanUs=60000;rejects([&]{validate(c);});}
    }
    {   // Numeric illumination model: rows follow the scan, fade over the response, and are lit continuously or by a strobe.
        PanelTiming oled{8100,200};
        double best=bestModelPhaseUs(120,Sequence::Alternating,oled,1,.5,1800);auto full=estimateLeakage(120,Sequence::Alternating,oled,1,.5,best,1800);
        require(full.mean>.02&&(full.top>.01||full.bottom>.01),"Whole-screen Left/Right at 4K 120 Hz leaks at the top or bottom at every phase");
        auto band=settledWindow(120,Sequence::Alternating,8100,.25,.5,200);best=bestModelPhaseUs(120,Sequence::Alternating,oled,.25,.5,1800);auto clean=estimateLeakage(120,Sequence::Alternating,oled,.25,.5,best,1800);
        require(clean.mean<.001&&clean.top<.001&&clean.bottom<.001,"A 25 % band has a clean phase");require(best>=band.openUs-100&&best+1800<=band.closeUs+100,"Numeric best phase lies inside the analytic settled window");
        require(clean.brightness>.99,"A clean window is fully lit for a sample-and-hold panel");
        PanelTiming fast{4050,200};best=bestModelPhaseUs(240,Sequence::BlackInsertion,fast,1,.5,4000);auto bfi=estimateLeakage(240,Sequence::BlackInsertion,fast,1,.5,best,4000);
        require(bfi.mean<.001,"Software black frame insertion at 240 Hz shows no other-eye rows during a 4 ms shutter");require(bfi.brightness>.4&&bfi.brightness<1,"Black frames trade brightness, not separation");
        auto rep=settledWindow(240,Sequence::Repeated,4050,1,.5,200);best=bestModelPhaseUs(240,Sequence::Repeated,fast,1,.5,1500);
        require(estimateLeakage(240,Sequence::Repeated,fast,1,.5,best,1500).mean<.001,"Left/Left/Right/Right at 240 Hz has a whole-screen clean window");require(best>=rep.openUs-100&&best+1500<=rep.closeUs+100,"Repeated best phase lies inside the analytic window");
        require(std::abs(emitterPhaseFromModel(best,240,Sequence::Repeated,0,0)-wrapPhase(best-periodUs(240),phaseCycleUs(240,Sequence::Repeated)))<1e-9,"Repeated frames command the emitter at the second refresh");
        // A strobed monitor scans fast and pulses after the rows settled (LightBoost: scan ~4 ms, pulse in the blanking).
        PanelTiming strobed{4000,200,Illumination::Strobed,7000,800};best=bestModelPhaseUs(120,Sequence::Alternating,strobed,1,.5,1500);auto pulse=estimateLeakage(120,Sequence::Alternating,strobed,1,.5,best,1500);
        require(pulse.mean<.001&&best<=7001&&best+1500>=7799,"Strobed panel: the suggested shutter covers the pulse without leakage");
        require(std::abs(pulse.brightness-800./1500)<.02,"Only the pulse lights a strobed panel");
        PanelTiming wrapped{4000,200,Illumination::Strobed,8000,600};auto crossing=estimateLeakage(120,Sequence::Alternating,wrapped,1,.5,7900,800);require(crossing.brightness>.7&&crossing.mean<.05&&crossing.top>crossing.bottom,"A pulse running past the refresh boundary keeps lighting; only the rows the next scan reached leak");
        PanelTiming slowStrobed{8100,3000,Illumination::Strobed,7000,800};best=bestModelPhaseUs(120,Sequence::Alternating,slowStrobed,1,.5,1500);require(estimateLeakage(120,Sequence::Alternating,slowStrobed,1,.5,best,1500).bottom>.05,"A slow panel is not settled when an early pulse fires: the bottom still leaks");
        PanelTiming early{8100,3000,Illumination::Strobed,5000,800};auto e=estimateLeakage(120,Sequence::Alternating,early,1,.5,5000,800);require(e.bottom>.5&&e.top<.1,"A strobe before the bottom rows settled leaks at the bottom only");
        auto late=estimateLeakage(120,Sequence::Alternating,oled,1,.5,0,1800);require(late.mean>.3&&late.bottom>=.99,"At phase 0 the lower rows still show the other eye");
        require(bandHeightForLeakage(240,Sequence::Repeated,fast,.5,1500,.01)==1,"Whole screen fits at 240 Hz repeated");double h=bandHeightForLeakage(120,Sequence::Alternating,oled,.5,1800,.01);require(h<1&&h>=.1&&std::abs(h-bandHeightForShutter(120,Sequence::Alternating,8100,1800,200,0))<.12,"Band for the leakage limit agrees with the analytic fit");
        for(double phase:{0.,1234.,8000.,16000.})require(std::abs(wrapSignedPhase(modelPhaseFromEmitter(emitterPhaseFromModel(phase,120,Sequence::Alternating,313,120),120,Sequence::Alternating,313,120)-phase,phaseCycleUs(120,Sequence::Alternating)))<1e-9,"Model/emitter phase conversion round trip");
        require(std::abs(emitterPhaseFromModel(1000,120,Sequence::Alternating,313,120)-1193)<1e-9,"A later scan start delays the emitter phase; USB latency advances it");
        Settings st;st.illumination=Illumination::Strobed;st.strobeStartUs=7000;st.strobeLengthUs=800;st.scanStartUs=313;validate(st);auto file=std::filesystem::temp_directory_path()/"vision-strobe-test.ini";saveProfile(file,st);auto loaded=loadProfile(file);
        require(loaded.illumination==Illumination::Strobed&&loaded.strobeStartUs==7000&&loaded.strobeLengthUs==800&&loaded.scanStartUs==313,"Strobe and scan-start settings survive profile reload");std::filesystem::remove(file);
        require(Settings{}.illumination==Illumination::SampleAndHold&&Settings{}.scanStartUs==0,"Sample and hold by default");
        {Settings c;c.strobeLengthUs=10;rejects([&]{validate(c);});}{Settings c;c.scanStartUs=9000;rejects([&]{validate(c);});}{Settings c;c.illumination=Illumination(5);rejects([&]{validate(c);});}
    }
    {   // Least-squares refresh clock: sample jitter is averaged, not copied into the prediction.
        TimingTracker jittery;double p=1/120.;for(unsigned i=1;i<=200;i++)jittery.observe(i,10+i*p+((i%2)?50e-6:-50e-6));
        require(jittery.samples==200&&std::abs(jittery.period-p)<1e-7,"Alternating 50 us jitter leaves the fitted period intact");
        require(std::abs(jittery.predict(201)-(10+201*p))<15e-6,"Prediction averages out the jitter");
        require(jittery.jitterRmsUs>40&&jittery.jitterRmsUs<60&&jittery.jitterMaxUs<110,"Jitter statistics report the sample scatter");
        TimingTracker drift;for(unsigned i=1;i<=400;i++)drift.observe(i,i*p*1.0001);require(std::abs(drift.period-p*1.0001)<1e-9,"The fit follows the true period across the window");
        TimingTracker jump;for(unsigned i=1;i<=50;i++)jump.observe(i,i*p);jump.observe(51,51*p+.004);require(jump.samples<8,"A 4 ms discontinuity restarts the clock");
        TimingTracker gap;for(unsigned i=1;i<=40;i++)gap.observe(i,i*p);gap.observe(43,43*p);require(gap.samples==41&&std::abs(gap.predict(44)-44*p)<1e-7,"Skipped refreshes keep the same line");
    }
    {   // Brightness: a four-slot sequence gives the emitter a two-refresh period, and the shutter
        // may use all of it. Capping the shutter at one display refresh threw away half the light.
        require(sequenceEmitterHz(120,Sequence::Alternating)==120&&sequenceEmitterHz(120,Sequence::BlackInsertion)==60&&sequenceEmitterHz(240,Sequence::Repeated)==120,"A four-slot sequence halves the emitter rate");
        require(nvidiaMaxShutterUs(sequenceEmitterHz(120,Sequence::BlackInsertion))>periodUs(120),"Black frame insertion allows a shutter longer than one display refresh");
        Settings wide;wide.refresh=120;wide.sequence=Sequence::BlackInsertion;wide.leftUs=wide.rightUs=12000;validate(wide);
        auto file=std::filesystem::temp_directory_path()/"vision-wide-shutter-test.ini";saveProfile(file,wide);require(loadProfile(file).leftUs==12000,"A two-refresh shutter survives a profile round trip");std::filesystem::remove(file);
        {Settings tooWide;tooWide.refresh=120;tooWide.sequence=Sequence::BlackInsertion;tooWide.leftUs=tooWide.rightUs=periodUs(60)-50;rejects([&]{validate(tooWide);});}
        {Settings narrow;narrow.refresh=120;narrow.sequence=Sequence::Alternating;narrow.leftUs=narrow.rightUs=9000;rejects([&]{validate(narrow);});}
        // A preset keeps a wide black-frame shutter instead of clamping it to one refresh.
        {Settings preset;preset.emitterId="nvidia:0955:0007";preset.refresh=120;preset.sequence=Sequence::BlackInsertion;preset.leftUs=preset.rightUs=12000;applyTimingPreset(preset,240);require(preset.leftUs>4000,"Rescaling a black-frame profile keeps its wide shutter");}
        PanelTiming oled{8100,200};
        const double blackCeiling=nvidiaMaxShutterUs(sequenceEmitterHz(120,Sequence::BlackInsertion));
        double widest=brightestShutterUs(120,Sequence::BlackInsertion,oled,1,.5,.01,blackCeiling);
        require(widest>periodUs(120),"The brightest clean black-frame shutter is longer than one refresh");
        double widePhase=bestModelPhaseUs(120,Sequence::BlackInsertion,oled,1,.5,widest);
        auto wideLight=estimateLeakage(120,Sequence::BlackInsertion,oled,1,.5,widePhase,widest);
        double narrowPhase=bestModelPhaseUs(120,Sequence::BlackInsertion,oled,1,.5,4000);
        auto narrowLight=estimateLeakage(120,Sequence::BlackInsertion,oled,1,.5,narrowPhase,4000);
        require(wideLight.mean<.01,"The widened black-frame shutter stays clean");
        require(wideLight.brightness*widest>narrowLight.brightness*4000*1.3,"Widening the shutter collects substantially more light");
        require(wideLight.brightness*widest<=periodUs(120)*1.02,"A row cannot give more light than its one lit refresh");
        require(wideLight.brightness<narrowLight.brightness,"The lit fraction of the shutter falls even as the collected light rises");
        // The same search must not hand a Left/Right sequence a leaking shutter to chase light.
        double alternating=brightestShutterUs(120,Sequence::Alternating,oled,.25,.5,.01,nvidiaMaxShutterUs(120));
        {   // LightBoost for LCDs: rise/fall response, per-row leak profile and the plan at every emitter rate.
            PanelTiming va{6000,4000};PanelTiming vaRise{6000,4000,Illumination::SampleAndHold,0,1500,1500};
            double bfiPhase=bestModelPhaseUs(144,Sequence::BlackInsertion,va,1,.5,8000);
            auto symmetric=estimateLeakage(144,Sequence::BlackInsertion,va,1,.5,bfiPhase,8000),asymmetric=estimateLeakage(144,Sequence::BlackInsertion,vaRise,1,.5,bfiPhase,8000);
            require(asymmetric.brightness>symmetric.brightness*1.05&&asymmetric.mean<=symmetric.mean+1e-6,"A faster rise out of black collects more own light without more leak");
            double lrPhase=bestModelPhaseUs(144,Sequence::Alternating,vaRise,1,.5,2000);
            require(std::abs(estimateLeakage(144,Sequence::Alternating,va,1,.5,lrPhase,2000).mean-estimateLeakage(144,Sequence::Alternating,vaRise,1,.5,lrPhase,2000).mean)<1e-9,"Eye-to-eye transitions use the full response whatever the rise time");
            auto profile=leakageProfile(144,Sequence::Alternating,va,1,.5,lrPhase,2000,8);require(profile.size()==8,"Profile has the requested rows");
            for(double k:profile)require(k>=0&&k<=.95,"Profile values stay within [0, 0.95]");
            auto whole=estimateLeakage(144,Sequence::Alternating,va,1,.5,lrPhase,2000);require(whole.mean>.05,"144 Hz VA Left/Right cannot be clean whole-screen: the profile has something to cancel");
            require(std::max(profile.front(),profile.back())>profile[3]+.02&&std::max(profile.front(),profile.back())>profile[4]+.02,"On a scan-limited panel the leak sits at the top and bottom rows, not the center");
            PanelTiming fast{4050,200};auto cleanProfile=leakageProfile(240,Sequence::BlackInsertion,fast,1,.5,bestModelPhaseUs(240,Sequence::BlackInsertion,fast,1,.5,4000),4000,8);
            for(double k:cleanProfile)require(k<.005,"A clean black-frame sequence has nothing to cancel");
            rejects([&]{leakageProfile(144,Sequence::Alternating,va,1,.5,lrPhase,2000,0);});rejects([&]{leakageProfile(144,Sequence::Alternating,va,1,.5,lrPhase,0,8);});
            for(double hz:{100.,120.,143.988,165.,240.}){
                double ceiling=nvidiaMaxShutterUs(hz);auto plan=planLightBoost(hz,va,1,.5,std::min(1000.,ceiling),std::min(4000.,ceiling));
                require(plan.shutterUs>=std::min(1000.,ceiling)-1e-9&&plan.shutterUs<=std::min(4000.,ceiling)+1e-9,"LightBoost shutter stays inside the requested range at every rate");
                require(plan.modelPhaseUs>=0&&plan.modelPhaseUs<phaseCycleUs(hz,Sequence::Alternating),"LightBoost phase lies inside the Left/Right cycle");
                auto check=estimateLeakage(hz,Sequence::Alternating,va,1,.5,plan.modelPhaseUs,plan.shutterUs);require(std::abs(check.mean-plan.predicted.mean)<1e-9,"LightBoost prediction matches the model at its phase");
                auto narrow=estimateLeakage(hz,Sequence::Alternating,va,1,.5,bestModelPhaseUs(hz,Sequence::Alternating,va,1,.5,std::min(1000.,ceiling)),std::min(1000.,ceiling));
                require(plan.predicted.mean<=narrow.mean+.03+1e-9,"LightBoost never leaks more than 3 % above the narrowest shutter's floor");
                Settings lb;lb.emitterId="nvidia:0955:0007";lb.refresh=hz;lb.leftUs=lb.rightUs=plan.shutterUs;lb.phaseUs=emitterPhaseFromModel(plan.modelPhaseUs,hz,Sequence::Alternating,300,120);lb.cancelCrosstalk=true;lb.panelResponseUs=4000;lb.panelRiseUs=1500;validate(lb);
            }
            PanelTiming roomy{2000,200};auto roomyPlan=planLightBoost(120,roomy,1,.5,1000,4000);require(roomyPlan.predicted.mean<=.01&&std::abs(roomyPlan.shutterUs-4000)<1e-9,"A panel with a settled window takes the widest shutter that stays within 1 % leak");
            auto slowerRate=planLightBoost(100,va,1,.5,1000,4000),fasterRate=planLightBoost(144,va,1,.5,1000,4000);require(slowerRate.predicted.mean<fasterRate.predicted.mean,"A longer hold at the same scan leaks less: the large-vertical-total route");
            rejects([&]{planLightBoost(144,va,1,.5,100,4000);});rejects([&]{planLightBoost(144,va,1,.5,3000,2000);});
            Settings saved;saved.panelRiseUs=1500;saved.cancelCrosstalk=true;saved.cancelStrength=1.25f;saved.leakProfile.fill(.3f);validate(saved);auto lbFile=std::filesystem::temp_directory_path()/"vision-lightboost-test.ini";saveProfile(lbFile,saved);auto loaded=loadProfile(lbFile);
            require(loaded.panelRiseUs==1500&&loaded.cancelCrosstalk&&std::abs(loaded.cancelStrength-1.25f)<1e-6,"LightBoost settings survive a profile reload");require(loaded.leakProfile[0]==0,"The derived leak profile is not stored");std::filesystem::remove(lbFile);
            {Settings c;c.cancelStrength=3;rejects([&]{validate(c);});}{Settings c;c.panelRiseUs=-1;rejects([&]{validate(c);});}{Settings c;c.leakProfile[2]=1.5f;rejects([&]{validate(c);});}
        }
        {   // Hold/black sequences at any rate: encoding, slots, emitter period, model and the comparison table.
            require(makeSequence(1,0)==Sequence::Alternating&&makeSequence(1,1)==Sequence::BlackInsertion&&makeSequence(2,0)==Sequence::Repeated,"Classic patterns keep their codes");
            for(unsigned hold=1;hold<=4;hold++)for(unsigned black=0;hold+black<=8;black++){Sequence s=makeSequence(hold,black);require(sequenceHold(s)==hold&&sequenceBlack(s)==black&&sequenceValid(s),"Hold/black round trip");require(cycleLength(s)==2*(hold+black),"Cycle is two runs");
                unsigned triggers=0,lefts=0,rights=0,blacks=0;for(unsigned i=0;i<cycleLength(s);i++){auto slot=sequenceSlot(s,i,false);triggers+=slot.trigger;lefts+=slot.eye==Eye::Left;rights+=slot.eye==Eye::Right;blacks+=slot.eye==Eye::Black;require(slot.pairBoundary==(i==0),"Cycle starts at slot 0");
                    require(vision::sync::slotEye(int(s),i)==int(slot.eye),"Hook slot table matches the host sequence");}
                require(triggers==2&&lefts==hold&&rights==hold&&blacks==2*black,"Each eye runs hold refreshes, is followed by its black ones and commands the emitter once");
                require(sequenceSlot(s,hold-1,false).trigger&&sequenceSlot(s,hold+black+hold-1,false).eye==Eye::Right,"The command goes out on the last refresh of each run");
                require(std::abs(sequenceEmitterHz(240,s)-240.0/(hold+black))<1e-9&&std::abs(eyeHoldUs(240,s)-periodUs(240)*hold)<1e-9&&std::abs(triggerOffsetUs(240,s)-periodUs(240)*(hold-1))<1e-9,"Emitter rate, hold and trigger offset follow the pattern");
                require(vision::sync::slotsPerFrame(int(s))==cycleLength(s)&&vision::sync::sequenceKnown(int(s)),"Hook slot count matches");
                Settings custom;custom.sequence=s;custom.refresh=240;custom.leftUs=custom.rightUs=1500;validate(custom);auto file=std::filesystem::temp_directory_path()/"vision-sequence-test.ini";saveProfile(file,custom);require(loadProfile(file).sequence==s,"Custom sequence survives a profile reload");std::filesystem::remove(file);}
            require(sequencePattern(makeSequence(2,1))=="L L B R R B"&&sequencePattern(makeSequence(1,2))=="L B B R B B"&&sequencePattern(Sequence::Alternating)=="L R","Pattern names");
            rejects([]{makeSequence(0,1);});rejects([]{makeSequence(5,0);});rejects([]{makeSequence(4,5);});require(!sequenceValid(Sequence(15))&&!sequenceValid(Sequence(48))&&!sequenceValid(Sequence(3)),"Unknown codes are rejected");{Settings c;c.sequence=Sequence(7);rejects([&]{validate(c);});}
            // A VA TV at 240 Hz: the fall time equals a refresh, so one black refresh is not enough but two are, and a second lit refresh adds light.
            PanelTiming va240{4170,5000,Illumination::SampleAndHold,0,1500,1500};auto ceiling=[](double hz){return nvidiaMaxShutterUs(hz);};
            auto table=compareSequences(240,va240,1,.5,.01,ceiling);require(table.size()==9,"Nine hold/black candidates up to four refreshes per eye pair");
            auto row=[&](unsigned hold,unsigned black)->const SequenceOption&{for(auto& o:table)if(o.sequence==makeSequence(hold,black))return o;throw std::runtime_error("Missing row");};
            require(row(1,0).estimate.mean>.01,"At 240 Hz a VA TV is not clean with Left/Right");
            require(row(2,1).estimate.brightness*row(2,1).shutterUs>row(1,1).estimate.brightness*row(1,1).shutterUs*1.5,"Two lit refreshes and a black one collect far more light than Left/Black/Right/Black on a slow panel");
            require(row(1,2).estimate.mean<=.01&&row(2,1).estimate.mean<=.01,"Two black refreshes, or two lit refreshes and one black, give a clean whole screen");
            {Sequence llbrrb=makeSequence(2,1);auto found=brightestShutter(240,llbrrb,va240,1,.5,.01,ceiling(sequenceEmitterHz(240,llbrrb)));require(estimateLeakage(240,llbrrb,va240,1,.5,found.modelPhaseUs,found.shutterUs).mean<=.01&&found.shutterUs>4000&&found.shutterUs==brightestShutterUs(240,llbrrb,va240,1,.5,.01,ceiling(sequenceEmitterHz(240,llbrrb))),"The brightest shutter search returns the phase that met the limit");}
            require(row(2,1).estimate.brightness*row(2,1).shutterUs>row(1,2).estimate.brightness*row(1,2).shutterUs,"L L B R R B collects more light than L B B R B B");
            require(std::abs(row(1,2).framesPerEye-40)<1e-9&&std::abs(row(1,0).framesPerEye-120)<1e-9,"Frames per eye follow the cycle");
            auto oledTable=compareSequences(240,PanelTiming{4050,200},1,.5,.01,ceiling);require(row(1,1).sequence==Sequence::BlackInsertion&&oledTable[1].estimate.mean<=.01&&oledTable[1].sequence==Sequence::BlackInsertion,"The OLED keeps its clean Left/Black/Right/Black at 240 Hz");
            rejects([&]{compareSequences(240,va240,1,.5,.01,nullptr);});
            // Display type and frames-per-eye choice.
            {Settings k;k.panelResponseUs=4321;k.panelRiseUs=1234;for(auto kind:{PanelKind::OLED,PanelKind::VA,PanelKind::IPS,PanelKind::MiniLED}){applyPanelKind(k,kind);require(k.panelKind==kind&&k.panelResponseUs==4321&&k.panelRiseUs==1234&&k.blackFloor==0,"Panel family must not invent response measurements or raise blacks");}
             {Settings c;c.blackFloor=.5f;rejects([&]{validate(c);});}{Settings c;c.blackFloor=-.1f;rejects([&]{validate(c);});}{Settings c;c.blackFloor=.25f;validate(c);auto file=std::filesystem::temp_directory_path()/"vision-floor-test.ini";saveProfile(file,c);require(std::abs(loadProfile(file).blackFloor-.25f)<1e-6,"Black floor survives a profile reload");std::filesystem::remove(file);}
             k.panelResponseUs=777;applyPanelKind(k,PanelKind::Custom);require(k.panelResponseUs==777&&k.panelKind==PanelKind::Custom,"Custom keeps hand-entered numbers");validate(k);
             auto file=std::filesystem::temp_directory_path()/"vision-kind-test.ini";k.panelKind=PanelKind::VA;saveProfile(file,k);require(loadProfile(file).panelKind==PanelKind::VA,"Panel kind survives a profile reload");std::filesystem::remove(file);
             {Settings c;c.panelKind=PanelKind(9);rejects([&]{validate(c);});}{Settings c;c.panelKind=PanelKind(5);rejects([&]{validate(c);});}}
            require(std::string(panelKindName(PanelKind::VA)).find("VA")!=std::string::npos,"Kind names");
            auto vaRun3=bestSequenceForRun(240,va240,1,.5,3,.01,ceiling);require(vaRun3.sequence==makeSequence(2,1)&&vaRun3.estimate.mean<=.01&&std::abs(vaRun3.framesPerEye-40)<1e-9,"40 per eye on a VA TV at 240 Hz is L L B R R B");
            auto vaRun1=bestSequenceForRun(240,va240,1,.5,1,.01,ceiling);require(vaRun1.sequence==Sequence::Alternating&&vaRun1.estimate.mean>.01,"120 per eye on a VA TV is Left/Right and leaks");
            auto oledRun2=bestSequenceForRun(240,PanelTiming{4050,200},1,.5,2,.01,ceiling);require(oledRun2.sequence==Sequence::BlackInsertion&&oledRun2.estimate.mean<=.01&&oledRun2.shutterUs>4000,"60 per eye on the OLED at 240 Hz stays Left/Black/Right/Black with its wide shutter");
            auto oledRun3=bestSequenceForRun(240,PanelTiming{4050,200},1,.5,3,.01,ceiling);require(oledRun3.estimate.mean<=.01&&sequenceHold(oledRun3.sequence)+sequenceBlack(oledRun3.sequence)==3,"40 per eye on the OLED is a clean three-refresh run");
            rejects([&]{bestSequenceForRun(240,va240,1,.5,0,.01,ceiling);});rejects([&]{bestSequenceForRun(240,va240,1,.5,9,.01,ceiling);});
            // The confirmed OLED profile (version 8 text as autosaved on 2026-09-13) must load exactly as before.
            {auto file=std::filesystem::temp_directory_path()/"vision-oled-regression.ini";{std::ofstream f(file);f<<"version 8\nname \"Odyssey_G8_OLED\"\ndisplay \"x\"\nconnection \"10:4355\"\nassessment \"Not assessed\"\nnotes \"\"\nemitter \"unassigned\"\nemitter_firmware \"unassigned\"\nwidth 3840\nheight 2160\nrefresh 239.99100000000001\nhdr 1\nswap 0\nsequence 1\nphase 2618\nleft 4501\nright 4501\ndepth 0.027000000700354576\nconvergence 0.0068000000901520252\noffset 0\npeak 400\nglasses 0\neyes 0\nvalidated 0\nband 1\nband_center 0.5\npanel_response 1000\npanel_scan 0\nillumination 0\nstrobe_start 0\nstrobe_length 1500\nscan_start 0\ngain 1\n";}
             auto o=loadProfile(file);std::filesystem::remove(file);
             require(o.sequence==Sequence::BlackInsertion&&cycleLength(o.sequence)==4&&sequenceSlot(o.sequence,0,false).eye==Eye::Left&&sequenceSlot(o.sequence,1,false).eye==Eye::Black&&sequenceSlot(o.sequence,2,false).eye==Eye::Right&&sequenceSlot(o.sequence,3,false).eye==Eye::Black,"OLED profile keeps Left/Black/Right/Black");
             require(o.phaseUs==2618&&o.leftUs==4501&&o.rightUs==4501&&std::abs(o.refresh-239.991)<1e-3&&o.hdr&&!o.cancelCrosstalk&&o.panelKind==PanelKind::Custom&&o.panelRiseUs==0&&o.imageGain==1&&o.blackFloor==0,"OLED profile timing and image path are untouched by the new fields");
             require(sequenceEmitterHz(o.refresh,o.sequence)==o.refresh/2&&sequenceSlot(o.sequence,0,false).trigger&&!sequenceSlot(o.sequence,1,false).trigger&&triggerOffsetUs(o.refresh,o.sequence)==0,"OLED profile drives the emitter as before");
             for(float k:o.leakProfile)require(k==0,"No cancellation on the OLED profile");}
        }
        require(estimateLeakage(120,Sequence::Alternating,oled,.25,.5,bestModelPhaseUs(120,Sequence::Alternating,oled,.25,.5,alternating),alternating).mean<=.01,"The brightest Left/Right shutter still respects the leakage limit");
        require(alternating<periodUs(120),"Left/Right cannot use a two-refresh shutter");
        Settings gain;gain.imageGain=2.5f;validate(gain);auto gainFile=std::filesystem::temp_directory_path()/"vision-gain-test.ini";saveProfile(gainFile,gain);require(loadProfile(gainFile).imageGain==2.5f,"Image brightness survives a profile round trip");std::filesystem::remove(gainFile);
        require(Settings{}.imageGain==1,"Image brightness defaults to unchanged");
        for(float bad:{0.5f,8.5f,std::nanf("")}){Settings c;c.imageGain=bad;rejects([&]{validate(c);});}
    }
    std::cout<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
