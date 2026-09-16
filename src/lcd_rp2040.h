#pragma once
#include "lcd_timing.h"
#include "rp2040_host.h"
#include <cmath>
namespace vision {
inline rp2040::Config lcdDeviceConfig(const LcdTiming& timing,double framePeriodUs,double scanUs=0,unsigned windowFrames=1,unsigned cadenceFrames=1) {
    if(!std::isfinite(framePeriodUs)||framePeriodUs<1||framePeriodUs>1000000)
        throw rp2040::InvalidTiming("No valid measured display period.");
    if(!windowFrames||windowFrames>cadenceFrames||cadenceFrames>8)throw rp2040::InvalidTiming("Invalid exposure sequence.");
    const auto frame=uint32_t(std::floor(framePeriodUs*cadenceFrames));
    const auto window=uint32_t(std::floor(framePeriodUs*windowFrames));
    const auto exposure=lcdExposure(timing,1e6/window,scanUs);
    if(!exposure.valid)throw rp2040::InvalidTiming(exposure.message);
    const auto width=uint32_t(std::floor(exposure.durationUs));
    const bool anchored=frame<=rp2040::Scheduler::maxAperturePeriodUs;
    // Firmware 0.4's long-cadence path timestamps the opens themselves. Keep
    // eye-offset differences inside its cadence tolerance with jitter margin.
    if(!anchored&&std::abs(exposure.openUs[0]-exposure.openUs[1])>50)
        throw rp2040::InvalidTiming("At this cadence, per-eye corrections must differ by at most 0.05 ms.");
    const rp2040::Config result{frame,width,width,anchored,uint32_t(exposure.openUs[0]),
        uint32_t(exposure.openUs[1]),uint32_t(std::ceil(timing.guardUs))};
    if(!rp2040::validConfig(result))throw rp2040::InvalidTiming("LCD aperture is outside this firmware's supported refresh range.");
    return result;
}
}
