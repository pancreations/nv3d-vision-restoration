#include "lcd_timing.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace vision {
LcdExposure lcdExposure(const LcdTiming& t,double hz,double signalScanUs) {
    LcdExposure e;
    auto fail=[&](const char* message){e.message=message;return e;};
    for(double x:{hz,signalScanUs,t.settleUs,t.durationUs,t.phaseUs,t.leftAdjustUs,t.rightAdjustUs,t.guardUs,t.scanoutUs,t.referencePosition})
        if(!std::isfinite(x))return fail("Timing values must be finite.");
    if(hz<24||hz>500)return fail("Refresh rate must be between 24 and 500 Hz.");
    e.periodUs=1e6/hz;
    if(t.settleUs<0||t.settleUs>8000||t.durationUs<250||t.durationUs>8000||
       std::abs(t.phaseUs)>e.periodUs+1e-6||std::abs(t.leftAdjustUs)>1000||std::abs(t.rightAdjustUs)>1000||
       t.guardUs<250||t.guardUs>2000||t.scanoutUs<0||t.scanoutUs>50000||signalScanUs<0||
       t.referencePosition<0||t.referencePosition>1||t.target<0||t.target>3)
        return fail("LCD timing is outside its control range (minimum guard: 0.25 ms).");
    if(t.compensateScanout) {
        const double scan=t.scanoutUs>0?t.scanoutUs:signalScanUs;
        if(scan<=0)return fail("Scanout compensation needs a measured or signal scan time.");
        e.scanShiftUs=scan*t.referencePosition;
    }
    const double base=t.settleUs+t.phaseUs+e.scanShiftUs;
    e.openUs={std::ceil(base+t.leftAdjustUs),std::ceil(base+t.rightAdjustUs)};
    e.maxDurationUs=std::min(8000.,std::floor(e.periodUs)-std::ceil(t.guardUs)-lcdCadenceMarginUs-std::max(e.openUs[0],e.openUs[1]));
    e.durationUs=t.durationUs;
    for(unsigned eye=0;eye<2;++eye)e.closeUs[eye]=e.openUs[eye]+t.durationUs;
    // Compare the same integer microseconds that the device receives. A tiny
    // fractional guard still rounds up; accepting it with an epsilon can send
    // an opening one microsecond inside the firmware's guard.
    if(std::min(e.openUs[0],e.openUs[1])<std::ceil(t.guardUs))
        return fail("Opening crosses the start guard. Increase settle delay or phase.");
    if(e.maxDurationUs<250)
        return fail("No 0.25 ms exposure fits before the next eye. Reduce settle delay, phase or scan compensation.");
    if(t.durationUs>e.maxDurationUs)
        return fail("Exposure reaches the next eye's guard. Shorten the shutter duration.");
    e.valid=true;e.message="Exposure fits inside each refresh.";return e;
}
bool fitLcdDuration(LcdTiming& t,double hz,double scan) {
    auto e=lcdExposure(t,hz,scan);
    if(std::isfinite(e.maxDurationUs)&&e.maxDurationUs>=250&&t.durationUs>e.maxDurationUs)
        t.durationUs=e.maxDurationUs;
    return lcdExposure(t,hz,scan).valid;
}
bool applyLcdRequest(const LcdTiming& requested,LcdTiming& applied,double hz,double scan) {
    if(!lcdExposure(requested,hz,scan).valid)return false;
    applied=requested;return true;
}
bool constrainLcdAdjustment(LcdTiming& t,const LcdTiming& previous,double hz,double scan) {
    if(lcdExposure(t,hz,scan).valid)return true;
    auto base=previous;base.enabled=t.enabled;base.target=t.target;
    if(!lcdExposure(base,hz,scan).valid)return false;
    // Only the control being edited may move. Invalid compound requests (a
    // profile or scan toggle) retain the last working timing as a whole.
    double LcdTiming::* edited=nullptr;unsigned count=0;
    for(auto member:{&LcdTiming::settleUs,&LcdTiming::durationUs,&LcdTiming::phaseUs,
        &LcdTiming::leftAdjustUs,&LcdTiming::rightAdjustUs,&LcdTiming::guardUs,
        &LcdTiming::scanoutUs,&LcdTiming::referencePosition})
        if(t.*member!=base.*member){edited=member;++count;}
    if(count!=1||t.compensateScanout!=base.compensateScanout||!std::isfinite(t.*edited)){t=base;return true;}
    const double distance=t.*edited-base.*edited;double low=0,high=1;
    for(int n=0;n<48;++n){
        const double fraction=(low+high)/2;auto candidate=base;
        candidate.*edited+=fraction*distance;
        if(lcdExposure(candidate,hz,scan).valid)low=fraction;else high=fraction;
    }
    // Keep a real fine-step margin from the boundary rather than storing a
    // binary-search epsilon such as 250.000000999 us in a profile.
    const double step=edited==&LcdTiming::referencePosition?.01:10.;
    t=base;t.*edited+=std::copysign(std::floor(std::abs(distance)*low/step)*step,distance);
    if(!lcdExposure(t,hz,scan).valid)t=base;
    return true;
}
std::vector<LcdTiming> lcdSettleSweep(const LcdTiming& origin,double hz,double scan) {
    if(!std::isfinite(hz)||hz<24||hz>500)throw std::runtime_error("Invalid calibration refresh rate.");
    std::vector<LcdTiming> steps;const double p=1e6/hz;
    for(double delay=0;delay<=std::min(8000.,p);delay+=250) {
        auto t=origin;t.settleUs=delay;
        // Separation first: progressively later, shorter probes, with the
        // guard and measured refresh imposing the final exposure ceiling.
        t.durationUs=std::clamp(1500.*(1.-delay/p),250.,1500.);
        if(fitLcdDuration(t,hz,scan))steps.push_back(t);
    }
    return steps;
}
bool lcdObservationComplete(const LcdObservation& o) {
    return o.windowFrames>=1&&o.windowFrames<=8&&lcdExposure(o.timing,o.refresh/o.windowFrames,o.signalScanUs).valid&&
        std::all_of(o.crosstalk.begin(),o.crosstalk.end(),[](int v){return v>=1&&v<=3;});
}
bool lcdObservationBetter(const LcdObservation& a,const LcdObservation& b) {
    if(!lcdObservationComplete(a))return false;
    if(!lcdObservationComplete(b))return true;
    const auto worst=[](const auto& x){return *std::max_element(x.crosstalk.begin(),x.crosstalk.end());};
    if(worst(a)!=worst(b))return worst(a)<worst(b);
    const auto total=[](const auto& x){return std::accumulate(x.crosstalk.begin(),x.crosstalk.end(),0);};
    if(total(a)!=total(b))return total(a)<total(b);
    return a.timing.durationUs>b.timing.durationUs;
}
}
