#include "core.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace vision {
void applyPanelDrive(Settings& s, PanelDrive drive) {
    Settings next=s;
    next.guardLevel=0;
    switch(drive) {
    case PanelDrive::Direct: next.sequence=Sequence::Alternating; break;
    case PanelDrive::BlackGuard: next.sequence=Sequence::BlackInsertion; break;
    case PanelDrive::NeutralGuard: next.sequence=Sequence::BlackInsertion; next.guardLevel=.5f; break;
    case PanelDrive::Preload: next.sequence=Sequence::Repeated; break;
    default: throw std::runtime_error("Unknown panel drive experiment.");
    }
    next.cancelCrosstalk=false; next.blackFloor=0; next.imageGain=1;
    next.bandHeight=1; next.bandCenter=.5f; next.rightOffsetUs=0;
    // A starting probe width, not a claim about this panel's response or lens latency.
    next.leftUs=next.rightUs=std::min(1500.,nvidiaMaxShutterUs(sequenceEmitterHz(next.refresh,next.sequence)));
    next.phaseUs=wrapPhase(next.phaseUs,phaseCycleUs(next.refresh,next.sequence));
    next.validated=next.glassesConfirmed=next.eyeConfirmed=false;
    next.assessment="Experimental panel drive; optical result not assessed";
    validate(next); s=std::move(next);
}

std::vector<PhaseWindow> intersectPhaseWindows(double cycle, std::span<const PhaseWindow> windows) {
    if(!std::isfinite(cycle)||cycle<=0)throw std::runtime_error("Invalid optical calibration cycle.");
    if(windows.empty())return {};
    std::vector<PhaseWindow> result{{0,cycle}};
    for(auto w:windows) {
        if(!std::isfinite(w.beginUs)||!std::isfinite(w.endUs)||w.beginUs<0||w.endUs<0||w.beginUs>=cycle||w.endUs>=cycle)
            throw std::runtime_error("Optical phase marks must lie inside the current cycle.");
        if(w.beginUs==w.endUs)return {};
        std::vector<PhaseWindow> pieces;
        if(w.beginUs<w.endUs)pieces.push_back(w);
        else {pieces.push_back({0,w.endUs});pieces.push_back({w.beginUs,cycle});}
        std::vector<PhaseWindow> next;
        for(auto a:result)for(auto b:pieces){double lo=std::max(a.beginUs,b.beginUs),hi=std::min(a.endUs,b.endUs);if(hi>lo)next.push_back({lo,hi});}
        result=std::move(next);
    }
    std::sort(result.begin(),result.end(),[](auto a,auto b){return a.beginUs<b.beginUs;});
    // Join the two ends of one circular interval; preserve disconnected alternatives.
    if(result.size()>1&&result.front().beginUs==0&&result.back().endUs==cycle){
        auto joined=PhaseWindow{result.back().beginUs,result.front().endUs};
        result.erase(result.begin());result.back()=joined;
    }
    return result;
}

std::string opticalCalibrationKey(const Settings& s) {
    std::ostringstream o;o<<std::setprecision(17);
    o<<std::quoted(s.displayId)<<' '<<std::quoted(s.connection)<<' '<<std::quoted(s.emitterId)<<' '<<std::quoted(s.emitterFirmware)
     <<' '<<s.width<<' '<<s.height<<' '<<s.refresh<<' '<<s.hdr<<' '<<s.swapEyes<<' '<<int(s.sequence)
     <<' '<<s.leftUs<<' '<<s.rightUs<<' '<<s.rightOffsetUs<<' '<<s.bandHeight<<' '<<s.bandCenter
     <<' '<<s.guardLevel<<' '<<s.blackFloor<<' '<<s.imageGain<<' '<<s.peakNits<<' '<<s.cancelCrosstalk<<' '<<s.cancelStrength
     <<' '<<int(s.illumination)<<' '<<s.strobeStartUs<<' '<<s.strobeLengthUs
     <<' '<<s.panelResponseUs<<' '<<s.panelRiseUs<<' '<<s.panelScanUs<<' '<<s.scanStartUs;
    return o.str();
}
}
