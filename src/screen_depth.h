// SPDX-License-Identifier: GPL-3.0-or-later
// CPU side of the whole-screen conversion: the network input size, and the turning of the
// network's raw relative depth into a stable nearness map.
#pragma once
#include <cstdint>
#include <vector>
namespace vision {
struct NetSize { unsigned width=0,height=0; };
// Network input for a picture of this aspect ratio: the longer side becomes maxSide rounded down to
// a multiple of `patch`, the shorter side follows the aspect ratio rounded to a multiple of `patch`;
// both at least one patch.
NetSize chooseNetSize(unsigned sourceWidth,unsigned sourceHeight,unsigned maxSide,unsigned patch=14);
// Turns raw relative depth (larger = nearer; scale and offset differ from frame to frame) into
// nearness in [0, 1]. The range comes from the 1st and 99th percentiles and is tracked over time,
// so a small change of content does not pump the whole picture's depth; each pixel is smoothed
// over time with a weight that follows large changes at once and averages small ones; a cut (most
// of the picture changed) takes the new frame whole, with its own range.
class DepthNormalizer {
public:
    float smoothing=.5f;       // 0 = every frame stands alone, towards 1 = slower
    float rangeSmoothing=.9f;  // share of the previous range kept per frame
    float cutThreshold=.2f;    // mean nearness change that counts as a cut
    // Returns false (and leaves `out` untouched) when the input has no finite values.
    bool process(const float* raw,unsigned width,unsigned height,std::vector<float>& out);
    void reset();
    float low()const{return low_;}
    float high()const{return high_;}
    bool cut()const{return cut_;}
    unsigned frames()const{return frames_;}
private:
    std::vector<float> previous_,scratch_;unsigned width_=0,height_=0,frames_=0;float low_=0,high_=1;bool cut_=false;
};
}
