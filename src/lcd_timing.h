#pragma once
#include <array>
#include <string>
#include <vector>

namespace vision {
// The scheduler admits successive frame anchors up to 100 us early/late.
// Reserve that early-arrival margin ahead of the next frame's guard.
inline constexpr double lcdCadenceMarginUs=100;
// Microseconds, relative to the predicted start of ONE displayed refresh.
// No manufacturer's response-time number is an input to this model.
struct LcdTiming {
    bool enabled=false;
    double settleUs=4000, durationUs=750, phaseUs=0;
    double leftAdjustUs=0, rightAdjustUs=0, guardUs=250;
    bool compensateScanout=false;
    double scanoutUs=0, referencePosition=.5; // zero scanout uses signal timing
    int target=0; // full panel, top, center, bottom; visual selection only
    bool operator==(const LcdTiming&) const = default;
};
struct LcdExposure {
    bool valid=false;
    double periodUs=0, scanShiftUs=0, maxDurationUs=0, durationUs=0;
    std::array<double,2> openUs{},closeUs{};
    std::string message;
};
inline bool sameLcdAperture(LcdTiming a,LcdTiming b){a.target=b.target=0;return a==b;}
LcdExposure lcdExposure(const LcdTiming&,double measuredHz,double signalScanUs=0);
// Automatic sweep/preset helper ONLY: explicitly fit a candidate's duration.
// Manual sliders use constrainLcdAdjustment and never change another control.
bool fitLcdDuration(LcdTiming&,double measuredHz,double signalScanUs=0);
// Keep a live edit within the nearest reachable valid aperture. An invalid
// request never replaces the last working timing or requires destroying output.
bool constrainLcdAdjustment(LcdTiming&,const LcdTiming& previous,double measuredHz,double signalScanUs=0);
// The editor may hold an invalid draft while other controls are being moved.
// Only a complete valid request replaces the last applied timing.
bool applyLcdRequest(const LcdTiming& requested,LcdTiming& applied,double measuredHz,double signalScanUs=0);
std::vector<LcdTiming> lcdSettleSweep(const LcdTiming&,double measuredHz,double signalScanUs=0);
struct LcdObservation {
    LcdTiming timing;
    double refresh=0,signalScanUs=0;
    std::array<int,6> crosstalk{}; // 0 unset, 1 clear, 2 mild, 3 strong; L top/mid/bottom, R top/mid/bottom
    unsigned windowFrames=1;
};
bool lcdObservationComplete(const LcdObservation&);
bool lcdObservationBetter(const LcdObservation&,const LcdObservation&);
}
