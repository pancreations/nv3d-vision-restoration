// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_depth.h"
#include <algorithm>
#include <cmath>
namespace vision {
NetSize chooseNetSize(unsigned w,unsigned h,unsigned maxSide,unsigned patch){
    if(!patch)patch=1;
    const unsigned longest=std::max(maxSide/patch,1u)*patch;
    if(!w||!h)return {longest,longest};
    const double aspect=double(w)/double(h);
    NetSize n;
    if(w>=h){n.width=longest;n.height=unsigned(std::lround(longest/aspect/patch))*patch;}
    else{n.height=longest;n.width=unsigned(std::lround(longest*aspect/patch))*patch;}
    n.width=std::max(n.width,patch);n.height=std::max(n.height,patch);
    return n;
}
void DepthNormalizer::reset(){previous_.clear();width_=height_=frames_=0;low_=0;high_=1;cut_=false;}
bool DepthNormalizer::process(const float* raw,unsigned width,unsigned height,std::vector<float>& out){
    const size_t count=size_t(width)*height;if(!raw||!count)return false;
    // Robust range: the 1st and 99th percentiles, so a few extreme pixels never set the scale.
    scratch_.clear();scratch_.reserve(count);for(size_t i=0;i<count;i++)if(std::isfinite(raw[i]))scratch_.push_back(raw[i]);
    if(scratch_.empty())return false;
    const size_t finite=scratch_.size(),i1=finite/100,i99=finite-1-finite/100;
    std::nth_element(scratch_.begin(),scratch_.begin()+i1,scratch_.end());float lo=scratch_[i1];
    std::nth_element(scratch_.begin()+i1,scratch_.begin()+i99,scratch_.end());float hi=scratch_[i99];
    if(hi-lo<1e-6f)hi=lo+1e-6f;
    const bool sameSize=width==width_&&height==height_&&previous_.size()==count;
    float low=sameSize?low_*rangeSmoothing+lo*(1-rangeSmoothing):lo,high=sameSize?high_*rangeSmoothing+hi*(1-rangeSmoothing):hi;
    if(high-low<1e-6f)high=low+1e-6f;
    out.resize(count);
    auto nearness=[](float v,float l,float h){return std::isfinite(v)?std::clamp((v-l)/(h-l),0.f,1.f):0.f;};
    cut_=false;
    if(sameSize){
        double change=0;for(size_t i=0;i<count;i++)change+=std::abs(nearness(raw[i],low,high)-previous_[i]);
        cut_=change/double(count)>cutThreshold;
    }
    if(!sameSize||cut_){
        // First frame or a cut: the new frame stands alone, with its own range.
        low=lo;high=hi;for(size_t i=0;i<count;i++)out[i]=nearness(raw[i],low,high);
    }else{
        const float base=1-std::clamp(smoothing,0.f,.99f);
        for(size_t i=0;i<count;i++){const float n=nearness(raw[i],low,high),p=previous_[i];const float a=std::clamp(base+std::abs(n-p)*4,base,1.f);out[i]=p+(n-p)*a;}
    }
    previous_=out;width_=width;height_=height;low_=low;high_=high;frames_++;
    return true;
}
}
