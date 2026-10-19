// SPDX-License-Identifier: GPL-3.0-or-later
// CPU side of the whole-screen conversion: the network input size, and the turning of the
// network's raw relative depth into a stable nearness map.
#pragma once
#include <cstdint>
#include <vector>
namespace vision {
// Levels operate on normalized nearness, before stereo reprojection. Defaults are neutral.
struct DepthMapSettings {
    float farPoint=0,nearPoint=1,contrast=1,gamma=1;
    float detail=0;           // local depth enhancement, 0 to 2
    unsigned detailRadius=2;  // radius in depth-map pixels, 1 to 8
    bool invert=false;
    bool operator==(const DepthMapSettings&)const=default;
};
// Separate from temporal normalization: edits never feed back into its history.
class DepthMapProcessor {
public:
    void process(const std::vector<float>& normalized,unsigned width,unsigned height,
                 const DepthMapSettings&,std::vector<float>& out);
private:
    std::vector<float> horizontal_;
};
struct NetSize { unsigned width=0,height=0; };
// Network input for a picture of this aspect ratio: the longer side becomes maxSide rounded down to
// a multiple of `patch`, the shorter side follows the aspect ratio rounded to a multiple of `patch`;
// both at least one patch.
NetSize chooseNetSize(unsigned sourceWidth,unsigned sourceHeight,unsigned maxSide,unsigned patch=14);
// Turns raw relative depth (larger = nearer; scale and offset differ from frame to frame) into
// nearness in [0, 1]. Normalize each model result in its own scale, then align corresponding
// surfaces to the preceding estimate. Raw ranges from different inferences cannot be averaged:
// the model's arbitrary scale/offset would move the whole scene. Spatial denoising and temporal
// smoothing suppress flicker; only a structural scene cut bypasses the temporal history.
class DepthNormalizer {
public:
    float smoothing=.5f;       // history retained at 30 updates/s; 0 disables denoising
    bool stabilizeRange=true; // align to corresponding unchanged surfaces
    float cutThreshold=.2f;    // mean nearness change that counts as a cut
    // Returns false (and leaves `out` untouched) when the input has no finite values.
    bool process(const float* raw,unsigned width,unsigned height,std::vector<float>& out,
                 float elapsedSeconds=1.f/30);
    void reset();
    float low()const{return low_;}
    float high()const{return high_;}
    bool cut()const{return cut_;}
    unsigned frames()const{return frames_;}
private:
    std::vector<float> previous_,previousInput_,current_,filtered_,scratch_;
    unsigned width_=0,height_=0,frames_=0;float low_=0,high_=1;bool cut_=false;
};
}
