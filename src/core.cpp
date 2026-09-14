#include "core.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace vision {
uint64_t refreshForPresent(uint32_t present,uint32_t observedPresent,uint64_t observedRefresh) {
    return observedRefresh+uint32_t(present-observedPresent);
}
uint32_t presentForRefresh(uint64_t refresh,uint32_t observedPresent,uint64_t observedRefresh) {
    return observedPresent+uint32_t(refresh-observedRefresh);
}
unsigned cycleLength(Sequence s) { return s == Sequence::Alternating ? 2 : 4; }
Slot sequenceSlot(Sequence s, uint64_t index, bool swap) {
    unsigned i = unsigned(index % cycleLength(s));
    Slot result{};
    if(s == Sequence::Alternating) result = {i == 0 ? Eye::Left : Eye::Right, true, i == 0};
    else if(s == Sequence::BlackInsertion) result = {i % 2 ? Eye::Black : (i == 0 ? Eye::Left : Eye::Right), i % 2 == 0, i == 0};
    else result = {i < 2 ? Eye::Left : Eye::Right, i % 2 == 1, i == 0};
    if(swap && result.eye != Eye::Black) result.eye = result.eye == Eye::Left ? Eye::Right : Eye::Left;
    return result;
}
double periodUs(double hz) {
    if(!std::isfinite(hz) || hz < 24 || hz > 500) throw std::runtime_error("Invalid display refresh rate.");
    return 1000000.0 / hz;
}
double phaseCycleUs(double refresh,Sequence sequence){return periodUs(refresh)*cycleLength(sequence);}
double wrapPhase(double phase, double period) {
    if(!std::isfinite(phase) || !std::isfinite(period) || period <= 0) throw std::runtime_error("Invalid phase.");
    return std::fmod(std::fmod(phase, period) + period, period);
}
double wrapSignedPhase(double phase, double period) {
    double result=wrapPhase(phase + period * .5, period) - period * .5;
    return std::abs(result)<1e-9?0.0:result;
}
double eyeHoldUs(double refresh,Sequence sequence){
    // How long the panel keeps receiving the same eye image: one refresh for Left/Right and
    // Left/Black/Right/Black, two for Left/Left/Right/Right (the repeated scan rewrites identical rows).
    return periodUs(refresh)*(sequence==Sequence::Repeated?2:1);
}
SettledWindow settledWindow(double refresh,Sequence sequence,double scanUs,double bandHeight,double bandCenter,double panelResponseUs){
    if(!std::isfinite(scanUs)||scanUs<0||!std::isfinite(panelResponseUs)||panelResponseUs<0)throw std::runtime_error("Invalid panel timing.");
    double top=std::clamp(bandCenter-bandHeight/2,0.,1.),bottom=std::clamp(bandCenter+bandHeight/2,0.,1.);
    return {scanUs*bottom+panelResponseUs,eyeHoldUs(refresh,sequence)+scanUs*top};
}
double suggestedPhaseUs(double refresh,Sequence sequence,const SettledWindow& w,double shutterUs){
    // Center the shutter in the window; if the window is shorter, open with it and overlap the end.
    double length=w.closeUs-w.openUs;double start=length>=shutterUs?w.openUs+(length-shutterUs)/2:w.openUs;
    return wrapPhase(start,phaseCycleUs(refresh,sequence));
}
double bandHeightForShutter(double refresh,Sequence sequence,double scanUs,double shutterUs,double panelResponseUs,double marginUs){
    // Window length = hold - response - scan*height, independent of the band position.
    if(!(scanUs>0))return 1;
    return std::clamp((eyeHoldUs(refresh,sequence)-panelResponseUs-shutterUs-marginUs)/scanUs,.1,1.);
}
double triggerOffsetUs(double refresh,Sequence sequence){
    // Left/Left/Right/Right commands the emitter at the second refresh of each pair (the first is settling).
    return sequence==Sequence::Repeated?periodUs(refresh):0;
}
double emitterPhaseFromModel(double modelPhaseUs,double refresh,Sequence sequence,double scanStartUs,double usbLatencyUs){
    if(!std::isfinite(modelPhaseUs)||!std::isfinite(scanStartUs)||!std::isfinite(usbLatencyUs))throw std::runtime_error("Invalid phase conversion.");
    return wrapPhase(modelPhaseUs+scanStartUs-usbLatencyUs-triggerOffsetUs(refresh,sequence),phaseCycleUs(refresh,sequence));
}
double modelPhaseFromEmitter(double emitterPhaseUs,double refresh,Sequence sequence,double scanStartUs,double usbLatencyUs){
    if(!std::isfinite(emitterPhaseUs)||!std::isfinite(scanStartUs)||!std::isfinite(usbLatencyUs))throw std::runtime_error("Invalid phase conversion.");
    return wrapPhase(emitterPhaseUs-scanStartUs+usbLatencyUs+triggerOffsetUs(refresh,sequence),phaseCycleUs(refresh,sequence));
}
namespace {
// Light of the left eye's image and of the right eye's image reaching a shutter that is open
// over [phaseUs, phaseUs+shutterUs] (model origin), integrated over the rows of the band.
struct Collected { double own=0,other=0; };
void collectLight(double refresh,Sequence sequence,const PanelTiming& panel,double bandHeight,double bandCenter,double phaseUs,double shutterUs,unsigned rowCount,Collected* perRow){
    const double p=periodUs(refresh);const unsigned n=cycleLength(sequence);const double cycle=p*n;
    const double top=std::clamp(bandCenter-bandHeight/2,0.,1.),bottom=std::clamp(bandCenter+bandHeight/2,0.,1.);
    Eye content[4];for(unsigned k=0;k<n;k++)content[k]=sequenceSlot(sequence,k,false).eye;
    const double dt=std::max(5.,shutterUs/200);const unsigned steps=std::max(1u,unsigned(shutterUs/dt));
    const bool strobed=panel.illumination==Illumination::Strobed;
    for(unsigned r=0;r<rowCount;r++){
        const double y=rowCount==1?(top+bottom)/2:top+(bottom-top)*(r+.5)/rowCount;
        double own=0,other=0;
        for(unsigned i=0;i<steps;i++){
            const double t=wrapPhase(phaseUs+(i+.5)*dt,cycle);
            // Illumination of this refresh: continuous, or a pulse that starts strobeStartUs after each
            // scan start and may run past the refresh boundary.
            double light=1;if(strobed){double sincePulse=wrapPhase(t-panel.strobeStartUs,p);light=sincePulse<panel.strobeLengthUs?1:0;if(light==0)continue;}
            // Latest refresh whose scan has reached this row, and the previous content it replaces.
            unsigned k=unsigned(std::floor(t/p));double elapsed=t-(k*p+panel.scanUs*y);
            if(elapsed<0){elapsed+=p;k=(k+n-1)%n;}
            const double progress=panel.responseUs>0?std::clamp(elapsed/panel.responseUs,0.,1.):1.;
            const Eye now=content[k%n],before=content[(k+n-1)%n];
            auto weight=[&](Eye e,double w){if(e==Eye::Left)own+=w*light;else if(e==Eye::Right)other+=w*light;};
            weight(now,progress);weight(before,1-progress);
        }
        perRow[r]={own*dt,other*dt};
    }
}
}
LeakageEstimate estimateLeakage(double refresh,Sequence sequence,const PanelTiming& panel,double bandHeight,double bandCenter,double phaseUs,double shutterUs){
    if(!std::isfinite(panel.scanUs)||panel.scanUs<0||!std::isfinite(panel.responseUs)||panel.responseUs<0||!std::isfinite(phaseUs)||!std::isfinite(shutterUs)||shutterUs<=0)throw std::runtime_error("Invalid panel timing.");
    if(panel.illumination==Illumination::Strobed&&(!std::isfinite(panel.strobeStartUs)||!std::isfinite(panel.strobeLengthUs)||panel.strobeLengthUs<=0))throw std::runtime_error("Invalid strobe timing.");
    constexpr unsigned rows=33;Collected rowLight[rows];collectLight(refresh,sequence,panel,bandHeight,bandCenter,phaseUs,shutterUs,rows,rowLight);
    LeakageEstimate e;double own=0,other=0;auto ratio=[](double o,double x){return o>1e-9?std::min(x/o,10.):10.;};
    double regionOwn[3]{},regionOther[3]{};
    for(unsigned r=0;r<rows;r++){own+=rowLight[r].own;other+=rowLight[r].other;unsigned region=r*3/rows;regionOwn[region]+=rowLight[r].own;regionOther[region]+=rowLight[r].other;}
    e.brightness=own/(shutterUs*rows);e.mean=ratio(own,other);e.top=ratio(regionOwn[0],regionOther[0]);e.center=ratio(regionOwn[1],regionOther[1]);e.bottom=ratio(regionOwn[2],regionOther[2]);
    return e;
}
double bestModelPhaseUs(double refresh,Sequence sequence,const PanelTiming& panel,double bandHeight,double bandCenter,double shutterUs){
    const double cycle=phaseCycleUs(refresh,sequence);
    // Least leakage first; among phases within 0.2 % of it, the brightest. Coarse pass, then refine.
    auto better=[](const LeakageEstimate& a,const LeakageEstimate& b){if(std::abs(a.mean-b.mean)>.002)return a.mean<b.mean;return a.brightness>b.brightness;};
    double best=0;LeakageEstimate bestEstimate;bool first=true;
    auto consider=[&](double phase){auto e=estimateLeakage(refresh,sequence,panel,bandHeight,bandCenter,phase,shutterUs);if(first||better(e,bestEstimate)){first=false;best=phase;bestEstimate=e;}};
    for(double phase=0;phase<cycle;phase+=50)consider(phase);
    const double coarse=best;for(double phase=coarse-50;phase<=coarse+50;phase+=5)consider(wrapPhase(phase,cycle));
    return wrapPhase(best,cycle);
}
double bandHeightForLeakage(double refresh,Sequence sequence,const PanelTiming& panel,double bandCenter,double shutterUs,double leakageLimit){
    for(double h=1;h>.1+1e-9;h-=.05){double center=std::clamp(bandCenter,h/2,1-h/2);double phase=bestModelPhaseUs(refresh,sequence,panel,h,center,shutterUs);if(estimateLeakage(refresh,sequence,panel,h,center,phase,shutterUs).mean<=leakageLimit)return h;}
    return .1;
}
double sequenceEmitterHz(double refresh,Sequence sequence){
    if(!std::isfinite(refresh)||refresh<=0)throw std::runtime_error("Invalid display refresh rate.");
    return refresh/(sequence==Sequence::Alternating?1:2);
}
double brightestShutterUs(double refresh,Sequence sequence,const PanelTiming& panel,double bandHeight,double bandCenter,double leakageLimit,double maxShutterUs){
    if(!std::isfinite(maxShutterUs)||maxShutterUs<=minimumShutterUs)return minimumShutterUs;
    const double cycle=phaseCycleUs(refresh,sequence),step=std::max(100.0,(maxShutterUs-minimumShutterUs)/48);
    // Start from the best phase of the narrowest shutter, then widen. Adding `step` to the width
    // can move the clean window's start earlier by at most `step`, so a local search follows it
    // instead of repeating the whole-cycle search at every width.
    double phase=bestModelPhaseUs(refresh,sequence,panel,bandHeight,bandCenter,minimumShutterUs);
    double best=minimumShutterUs,bestLight=0;
    for(double shutter=minimumShutterUs;shutter<=maxShutterUs+1e-9;shutter+=step){
        double localPhase=phase,localLight=0;bool found=false;
        for(double offset=-step-50;offset<=50;offset+=25){
            const double candidate=wrapPhase(phase+offset,cycle);
            const auto estimate=estimateLeakage(refresh,sequence,panel,bandHeight,bandCenter,candidate,shutter);
            if(estimate.mean>leakageLimit)continue;
            const double light=estimate.brightness*shutter;
            if(!found||light>localLight){found=true;localPhase=candidate;localLight=light;}
        }
        // No phase of this width is clean; a wider shutter cannot become clean again.
        if(!found)break;
        phase=localPhase;
        if(localLight>bestLight*1.002){bestLight=localLight;best=shutter;}
    }
    return best;
}
double calibrationMaxShutterUs(double refresh) {
    // Keep the everyday control in a bounded calibration range without making
    // its value or slider position depend on phase.
    return std::min(3000.0,periodUs(refresh)-500.0);
}
double nvidiaBandCorrectionUs(double refresh){
    // Firmware subtracts ticks from the NEGATIVE X reload: this increases delay.
    double p=periodUs(refresh);return p>9523.75?91.0:p>8695.6?66.0:53.5;
}
double nvidiaMaxShutterUs(double refresh){
    double p=periodUs(refresh),maxX=nvidiaDelayCenterUs+nvidiaBoundaryMarginUs;
    // The firmware allows the window to run up to the sequence guard; no artificial 3000 us cap here
    // because at 100/120 Hz a wider window is the only way to keep the picture bright.
    return std::max(p-maxX-nvidiaBandCorrectionUs(refresh)-nvidiaSequenceGuardUs,minimumShutterUs);
}
NvidiaSchedule nvidiaSchedule(double refresh,double phaseUs){
    double p=periodUs(refresh),bc=nvidiaBandCorrectionUs(refresh);
    if(!std::isfinite(phaseUs))throw std::runtime_error("Invalid phase.");
    double phase=wrapPhase(phaseUs,p),lo=nvidiaBoundaryMarginUs,hi=p-nvidiaBoundaryMarginUs;
    if(hi<=lo)throw std::runtime_error("Refresh period too short for the NVIDIA emitter schedule.");
    // open-token start = boundary - bias + X + bandCorrection.
    // Prefer X at its center and move the boundary;
    // when the boundary would leave its safe range, put the remainder into X instead.
    double x=nvidiaDelayCenterUs,boundary=wrapPhase(phase-(x+bc)+nvidiaLockBiasUs,p);
    if(boundary<lo){x-=lo-boundary;boundary=lo;}
    else if(boundary>hi){x+=boundary-hi;boundary=hi;}
    return {boundary,x,boundary-nvidiaLockBiasUs+x+bc};
}
static uint32_t timer(double us, double mhz) {
    if(!std::isfinite(us) || us < 0 || us > 100000) throw std::runtime_error("Invalid emitter timer.");
    return uint32_t(int32_t(1 - us*mhz));
}
static void append32(std::vector<uint8_t>& out, uint32_t value) {
    for(int i=0;i<4;i++) out.push_back(uint8_t(value >> (i*8)));
}
NvidiaEyeTiming nvidiaEyeTiming(double refresh,double phaseUs,Eye eye,double leftUs,double rightUs,double rightOffsetUs){
    double p=periodUs(refresh);auto s=nvidiaSchedule(refresh,phaseUs);
    double x=s.delayUs+(eye==Eye::Right?rightOffsetUs:0),y=eye==Eye::Right?rightUs:leftUs;
    if(!std::isfinite(x)||!std::isfinite(y))throw std::runtime_error("Invalid eye timing.");
    double guard=nvidiaSequenceGuardUs+nvidiaBandCorrectionUs(refresh);
    x=std::clamp(x,150.0,p-guard-minimumShutterUs);y=std::clamp(y,minimumShutterUs,p-guard-x);
    return {x,y};
}
std::vector<uint8_t> eyeTimingPacket(double refresh,double x,double y){
    double p=periodUs(refresh);if(!std::isfinite(x)||!std::isfinite(y)||x<0||y<minimumShutterUs||x+y+nvidiaBandCorrectionUs(refresh)+nvidiaSequenceGuardUs>p+1e-6)throw std::runtime_error("Eye timing outside the emitter sequence guard.");
    std::vector<uint8_t> out{0x01,0x04,0x08,0x00};append32(out,timer(x,4));append32(out,timer(y,4));return out;
}
double nvidiaLegacyPhaseUs(double refresh,double registerPhaseUs){
    // Profiles up to version 3 stored the raw X register with the upstream period/1.8 boundary.
    double p=periodUs(refresh);return wrapPhase(p/1.8-nvidiaLockBiasUs+registerPhaseUs+nvidiaBandCorrectionUs(refresh),p);
}
void applyTimingPreset(Settings& settings,unsigned hz){
    if(hz!=100&&hz!=120&&hz!=144&&hz!=165&&hz!=240)throw std::runtime_error("Unknown IR timing preset.");
    validate(settings);Settings next=settings;double scale=settings.refresh/hz;
    next.refresh=hz;next.phaseUs*=scale;
    double maximum=settings.emitterId.rfind("nvidia",0)==0?nvidiaMaxShutterUs(sequenceEmitterHz(hz,settings.sequence)):calibrationMaxShutterUs(hz);
    next.leftUs=std::clamp(next.leftUs*scale,minimumShutterUs,maximum);
    next.rightUs=std::clamp(next.rightUs*scale,minimumShutterUs,maximum);
    next.validated=false;validate(next);settings=std::move(next);
}
bool refreshRatesMatch(double presetHz,double displayHz){
    // Fractional monitor rates such as 119.88/143.988 match nominal 120/144.
    return std::isfinite(presetHz)&&std::isfinite(displayHz)&&presetHz>0&&displayHz>0&&std::abs(presetHz-displayHz)<.5;
}
void validate(const Settings& s) {
    if(s.width < 64 || s.height < 64 || s.width > 16384 || s.height > 16384) throw std::runtime_error("Invalid output size.");
    if(int(s.sequence) < 0 || int(s.sequence) > 2) throw std::runtime_error("Invalid sequence.");
    if(!std::isfinite(s.phaseUs) || std::abs(s.phaseUs) > phaseCycleUs(s.refresh,s.sequence)) throw std::runtime_error("Phase exceeds a stereo cycle.");
    // The emitter period spans one stereo slot pair, so a four-slot sequence may hold the shutter
    // open across two display refreshes. Bounding this by one refresh threw away half the light.
    const double emitterPeriod = periodUs(sequenceEmitterHz(s.refresh, s.sequence));
    for(double duration : {s.leftUs, s.rightUs})
        if(!std::isfinite(duration) || duration < 50 || duration > emitterPeriod - 100) throw std::runtime_error("Shutter duration must leave a 100 us guard inside one emitter period.");
    if(!std::isfinite(s.depth) || s.depth < 0 || s.depth > 0.15f) throw std::runtime_error("Invalid test scene depth.");
    if(!std::isfinite(s.convergence) || std::abs(s.convergence) > .05f) throw std::runtime_error("Convergence must be between -5% and +5% of the eye image width.");
    if(!std::isfinite(s.peakNits) || s.peakNits < 80 || s.peakNits > 2000) throw std::runtime_error("Invalid HDR peak target.");
    if(!std::isfinite(s.imageGain) || s.imageGain < 1 || s.imageGain > 8) throw std::runtime_error("Image brightness must be between 1x and 8x.");
    if(!std::isfinite(s.rightOffsetUs) || std::abs(s.rightOffsetUs) > 2000) throw std::runtime_error("Right eye offset must stay within 2000 us.");
    if(!std::isfinite(s.bandHeight) || s.bandHeight < .1f || s.bandHeight > 1) throw std::runtime_error("Stereo area height must be between 10% and 100% of the output.");
    if(!std::isfinite(s.bandCenter) || s.bandCenter < s.bandHeight/2-1e-4f || s.bandCenter > 1-s.bandHeight/2+1e-4f) throw std::runtime_error("Stereo area must stay inside the output.");
    if(!std::isfinite(s.panelResponseUs) || s.panelResponseUs < 0 || s.panelResponseUs > 8000) throw std::runtime_error("Panel response must be between 0 and 8000 us.");
    if(!std::isfinite(s.panelScanUs) || s.panelScanUs < 0 || s.panelScanUs > 50000) throw std::runtime_error("Panel scan time must be between 0 and 50000 us.");
    if(int(s.illumination) < 0 || int(s.illumination) > 1) throw std::runtime_error("Invalid panel illumination.");
    if(!std::isfinite(s.strobeStartUs) || s.strobeStartUs < 0 || s.strobeStartUs > 50000) throw std::runtime_error("Strobe start must be between 0 and 50000 us.");
    if(!std::isfinite(s.strobeLengthUs) || s.strobeLengthUs < 50 || s.strobeLengthUs > 50000) throw std::runtime_error("Strobe length must be between 50 and 50000 us.");
    if(!std::isfinite(s.scanStartUs) || std::abs(s.scanStartUs) > 5000) throw std::runtime_error("Scan start offset must stay within 5000 us.");
}
// Protocol reference: libnvstusb, Bjoern Paetzel / Johann Baudy (LGPL).
// W is deliberately held at the upstream value; its physical meaning is unknown.
std::vector<uint8_t> timingPacket(double hz, double phase, double duration) {
    double p = periodUs(hz);
    if(duration < 50 || duration >= p || !std::isfinite(phase)) throw std::runtime_error("Invalid shutter timing packet.");
    auto schedule=nvidiaSchedule(hz,phase);double x=schedule.delayUs;
    // Last line of defence: the open/close token sequence must end before the next boundary
    // or the firmware skips the other eye entirely (one lens goes dark).
    duration=std::min(duration,std::max(minimumShutterUs,p-x-nvidiaBandCorrectionUs(hz)-nvidiaSequenceGuardUs));
    std::vector<uint8_t> out{0x01,0x00,0x18,0x00};
    append32(out,timer(4568.5,12)); append32(out,timer(x,4)); append32(out,timer(duration,4));
    out.insert(out.end(),{0x30,0x28,0x24,0x22,0x0a,0x08,0x05,0x04});
    append32(out,timer(p,12)); return out;
}
Eye nvidiaCommandEye(Eye eye,double hz,double phase){
    if(eye == Eye::Black) throw std::runtime_error("Black is not an emitter eye.");
    auto schedule=nvidiaSchedule(hz,phase);
    // The firmware assigns the commanded eye at the NEXT period boundary.
    // If X carries its token into the following image refresh, command that
    // refresh's eye. Modulo phase alone loses this essential parity bit.
    // Keep the requested phase over BOTH eyes. One additional refresh changes
    // eye parity; two refreshes repeat the same stereo phase.
    double p=periodUs(hz),cyclePhase=wrapPhase(phase,2*p);
    if(std::llround((schedule.openAfterCommandUs-cyclePhase)/p)%2!=0)
        eye=eye==Eye::Left?Eye::Right:Eye::Left;
    return eye;
}
std::array<uint8_t,8> eyePacket(Eye eye, double hz, double phase) {
    eye=nvidiaCommandEye(eye,hz,phase);
    // Expected timer-2 count at command time: distance to the next period boundary.
    auto schedule=nvidiaSchedule(hz,phase);
    uint32_t r = timer(schedule.boundaryUs,12);
    return {0xAA, uint8_t(eye == Eye::Right ? 0xFE : 0xFF),0,0,uint8_t(r),uint8_t(r>>8),uint8_t(r>>16),uint8_t(r>>24)};
}
void saveProfile(const std::filesystem::path& path, const Settings& s) {
    validate(s);
    if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    auto temp = path; temp += L".tmp";
    std::ofstream f(temp); if(!f) throw std::runtime_error("Cannot write profile.");
    f << std::setprecision(17) << "version 8\n";
    auto str=[&](const char* k,const std::string& v){ f << k << ' ' << std::quoted(v) << '\n'; };
    str("name",s.name);str("display",s.displayId);str("connection",s.connection);str("assessment",s.assessment);str("notes",s.monitorNotes);
    str("emitter",s.emitterId);str("emitter_firmware",s.emitterFirmware);
    f << "width " << s.width << "\nheight " << s.height << "\nrefresh " << s.refresh << "\nhdr " << s.hdr << "\nswap " << s.swapEyes
      << "\nsequence " << int(s.sequence) << "\nphase " << s.phaseUs << "\nleft " << s.leftUs << "\nright " << s.rightUs
      << "\ndepth " << s.depth << "\nconvergence " << s.convergence << "\noffset " << s.rightOffsetUs << "\npeak " << s.peakNits << "\nglasses " << s.glassesConfirmed << "\neyes " << s.eyeConfirmed << "\nvalidated " << s.validated
      << "\nband " << s.bandHeight << "\nband_center " << s.bandCenter << "\npanel_response " << s.panelResponseUs << "\npanel_scan " << s.panelScanUs
      << "\nillumination " << int(s.illumination) << "\nstrobe_start " << s.strobeStartUs << "\nstrobe_length " << s.strobeLengthUs << "\nscan_start " << s.scanStartUs
      << "\ngain " << s.imageGain << '\n';
    f.close(); if(!f) throw std::runtime_error("Profile write failed.");
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace profile; previous profile retained.");
}
Settings loadProfile(const std::filesystem::path& path) {
    std::ifstream f(path); if(!f) throw std::runtime_error("Cannot open profile.");
    Settings s; std::string key; bool version=false; unsigned fields=0; int fileVersion=0;
    while(f >> key) {
        if(key=="version") { int v=0; f>>v; if(v<1||v>8)throw std::runtime_error("Unsupported profile version."); version=true; fileVersion=v; }
        else if(key=="name") f>>std::quoted(s.name);
        else if(key=="display") f>>std::quoted(s.displayId);
        else if(key=="connection") f>>std::quoted(s.connection);
        else if(key=="emitter") f>>std::quoted(s.emitterId);
        else if(key=="emitter_firmware") f>>std::quoted(s.emitterFirmware);
        else if(key=="assessment") f>>std::quoted(s.assessment);
        else if(key=="notes") f>>std::quoted(s.monitorNotes);
        else if(key=="width") {f>>s.width;fields|=1;}
        else if(key=="height") {f>>s.height;fields|=2;}
        else if(key=="refresh") {f>>s.refresh;fields|=4;}
        else if(key=="hdr") f>>s.hdr;
        else if(key=="swap") f>>s.swapEyes;
        else if(key=="sequence") {int v=-1;f>>v;s.sequence=Sequence(v);}
        else if(key=="phase") f>>s.phaseUs;
        else if(key=="left") f>>s.leftUs;
        else if(key=="right") f>>s.rightUs;
        else if(key=="depth") f>>s.depth;
        else if(key=="convergence") f>>s.convergence;
        // Read old profiles without restoring removed cross-eye processing.
        else if(key=="ghost_left"||key=="ghost_right"||key=="cross") {double retired=0;f>>retired;if(!std::isfinite(retired))throw std::runtime_error("Invalid legacy image adjustment.");}
        else if(key=="offset") f>>s.rightOffsetUs;
        else if(key=="peak") f>>s.peakNits;
        else if(key=="glasses") f>>s.glassesConfirmed;
        else if(key=="eyes") f>>s.eyeConfirmed;
        else if(key=="validated") f>>s.validated;
        else if(key=="band") f>>s.bandHeight;
        else if(key=="band_center") f>>s.bandCenter;
        else if(key=="panel_response") f>>s.panelResponseUs;
        else if(key=="panel_scan") f>>s.panelScanUs;
        else if(key=="illumination") {int v=-1;f>>v;s.illumination=Illumination(v);}
        else if(key=="strobe_start") f>>s.strobeStartUs;
        else if(key=="strobe_length") f>>s.strobeLengthUs;
        else if(key=="scan_start") f>>s.scanStartUs;
        else if(key=="gain") f>>s.imageGain;
        else throw std::runtime_error("Unknown profile field: "+key);
        if(!f)throw std::runtime_error("Malformed profile field: "+key);
    }
    if(!version || fields!=7)throw std::runtime_error("Incomplete profile.");
    // Version 4 stores the NVIDIA phase as the window start after the vblank; older files
    // stored the raw emitter X register. Convert so a tuned profile keeps its optical position.
    if(fileVersion<4 && s.emitterId.rfind("nvidia",0)==0 && s.sequence==Sequence::Alternating) s.phaseUs=nvidiaLegacyPhaseUs(s.refresh,std::clamp(s.phaseUs,0.0,periodUs(s.refresh)-1));
    if(fileVersion<5)s.validated=false;
    validate(s);return s;
}
std::string profileKey(const Settings& s) {
    uint64_t hash=14695981039346656037ull;
    for(unsigned char c:s.displayId+"|"+s.connection+"|"+s.emitterId+"|"+s.emitterFirmware) {hash^=c;hash*=1099511628211ull;}
    std::ostringstream out;out<<std::hex<<hash<<std::dec<<'_'<<s.width<<'x'<<s.height<<'_'<<std::fixed<<std::setprecision(3)<<s.refresh<<(s.hdr?"_HDR":"_SDR")<<".ini";return out.str();
}
void TimingTracker::reset(){*this={};}
void TimingTracker::refit(){
    // Least squares of time against refresh index over the window. With N samples the
    // predicted vblank carries roughly 1/sqrt(N) of one sample's jitter instead of all of it.
    const size_t n=history_.size();if(n<2){period=0;return;}
    const uint64_t n0=history_.front().first;const double t0=history_.front().second;double sx=0,sy=0,sxx=0,sxy=0;
    for(auto& [refresh,time]:history_){double x=double(refresh-n0),y=time-t0;sx+=x;sy+=y;sxx+=x*x;sxy+=x*y;}
    const double denominator=double(n)*sxx-sx*sx;if(denominator<=0){period=0;return;}
    period=(double(n)*sxy-sx*sy)/denominator;const double intercept=(sy-period*sx)/double(n);
    epoch=t0+intercept+period*double(lastRefresh-n0);
}
bool TimingTracker::observe(uint64_t n,double t) {
    if(!std::isfinite(t))return false;
    if(samples && n==lastRefresh)return false;
    if(samples && (n<lastRefresh || t<=epoch-period*0.5)){reset();}
    if(samples) {
        double sample=(t-epoch)/double(n-lastRefresh);
        if(sample<0.002 || sample>0.05){reset();return false;}
        // A different period or a sample far from the fitted line is a mode change or a
        // discontinuity: start over rather than average two clocks.
        if(period && std::abs(sample-period)>period*0.08){reset();}
        else if(period && history_.size()>=8 && std::abs(t-(epoch+double(n-lastRefresh)*period))>period*0.25){reset();}
    }
    if(samples&&period){lastResidualUs=(t-(epoch+double(n-lastRefresh)*period))*1e6;if(history_.size()>=8){sumResidualSq_+=lastResidualUs*lastResidualUs;++residualCount_;jitterRmsUs=std::sqrt(sumResidualSq_/residualCount_);jitterMaxUs=std::max(jitterMaxUs,std::abs(lastResidualUs));}}
    history_.emplace_back(n,t);if(history_.size()>window)history_.erase(history_.begin());
    lastRefresh=n;++samples;refit();if(!period)epoch=t;
    return samples>=8 && period>0;
}
double TimingTracker::predict(uint64_t n)const{return epoch+(double(n)-double(lastRefresh))*period;}
}
