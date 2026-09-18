// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
// Compiled only into VisionStereo11Test.dll. Production has neither the export
// nor the private emitter-free test channel.
struct VisionOutputTestStats {
    uint64_t samples[3]{};
    uint32_t pixel[3]{};
    uint64_t eyeHash[2]{},eyeContent[2]{};
    double eyeCentroidX[2]{};
    uint64_t pairs=0;
    uint64_t acceptedPairs=0,lastAcceptedSerial=0,outOfOrderPairs=0;
    uint64_t presents=0;
    uint64_t producerStalls=0;
    uint64_t outputStalls=0,alignedRun=0,misaligned=0;
    uint32_t observedPresent=0;
    uint32_t width=0,height=0;
    int32_t presentResult=0;
    uint32_t windowVisible=0,windowIconic=0;
};
