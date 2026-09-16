#pragma once
#include "core.h"
namespace vision {
struct LcdCalibrationUi {
    bool fine=false,sweeping=false,seen=false;
    double seenHz=0;
    size_t candidate=0;
    double lastAdvance=0;
    std::vector<LcdTiming> candidates;
    std::array<int,6> ratings{};
    std::vector<LcdObservation> observations;
    LcdTiming ratedTiming;
    std::string context,message;
};
void prepareLcdCalibration(LcdCalibrationUi&,const Settings&);
bool startLcdSweep(LcdCalibrationUi&,Settings&,double hz,double now);
bool drawLcdObservations(LcdCalibrationUi&,Settings&,double hz,const std::filesystem::path& reports);
bool tickLcdCalibration(LcdCalibrationUi&,Settings&,double hz,double now,bool running);
bool drawLcdCalibration(LcdCalibrationUi&,Settings&,const Settings& active,double hz,bool running,float dpi,const std::filesystem::path& reports,double now);
bool drawImageControls(Settings&,bool hdrAvailable,bool aiDesktop=false);
bool drawTimingControls(Settings&,double phaseMin,double phaseMax,double shutterMax,bool& fine);
}
