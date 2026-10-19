// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen_depth.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace vision {
void DepthMapProcessor::process(const std::vector<float>& input,unsigned width,unsigned height,
                                const DepthMapSettings& s,std::vector<float>& out){
    const size_t count=size_t(width)*height;
    if(!width||!height||input.size()!=count||&input==&out)
        throw std::invalid_argument("Depth map processing needs distinct, correctly sized input and output.");
    out.resize(count);
    // A separable box average with clamped borders costs O(pixels), regardless of radius.
    // Extract detail before levels and cap its contribution around large depth jumps.
    const int radius=int(std::clamp(s.detailRadius,1u,8u));
    const float divisor=float(radius*2+1);
    if(s.detail>0){
        horizontal_.resize(count);
        for(unsigned y=0;y<height;y++){
            const size_t row=size_t(y)*width;
            auto sample=[&](int x){return input[row+std::clamp(x,0,int(width)-1)];};
            float sum=0;for(int dx=-radius;dx<=radius;dx++)sum+=sample(dx);
            for(unsigned x=0;x<width;x++){
                horizontal_[row+x]=sum/divisor;
                sum+=sample(int(x)+radius+1)-sample(int(x)-radius);
            }
        }
        for(unsigned x=0;x<width;x++){
            auto sample=[&](int y){return horizontal_[size_t(std::clamp(y,0,int(height)-1))*width+x];};
            float sum=0;for(int dy=-radius;dy<=radius;dy++)sum+=sample(dy);
            for(unsigned y=0;y<height;y++){
                const size_t i=size_t(y)*width+x;
                out[i]=input[i]+s.detail*std::clamp(input[i]-sum/divisor,-.05f,.05f);
                sum+=sample(int(y)+radius+1)-sample(int(y)-radius);
            }
        }
    }else out=input;
    const float range=std::max(s.nearPoint-s.farPoint,.01f);
    for(float& value:out){
        float n=s.invert?1-value:value;
        n=std::clamp((n-s.farPoint)/range,0.f,1.f);
        n=std::clamp((n-.5f)*s.contrast+.5f,0.f,1.f);
        value=s.gamma==1?n:std::pow(n,1/s.gamma);
    }
}
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
void DepthNormalizer::reset(){previous_.clear();previousInput_.clear();width_=height_=frames_=0;low_=0;high_=1;cut_=false;}
bool DepthNormalizer::process(const float* raw,unsigned width,unsigned height,std::vector<float>& out,float elapsedSeconds){
    const size_t count=size_t(width)*height;if(!raw||!count)return false;
    // Robust range: the 1st and 99th percentiles, so a few extreme pixels never set the scale.
    scratch_.clear();scratch_.reserve(count);for(size_t i=0;i<count;i++)if(std::isfinite(raw[i]))scratch_.push_back(raw[i]);
    if(scratch_.empty())return false;
    const size_t finite=scratch_.size(),i1=finite/100,i99=finite-1-finite/100;
    std::nth_element(scratch_.begin(),scratch_.begin()+i1,scratch_.end());float lo=scratch_[i1];
    std::nth_element(scratch_.begin()+i1,scratch_.begin()+i99,scratch_.end());float hi=scratch_[i99];
    const bool sameSize=width==width_&&height==height_&&previous_.size()==count;
    // A nearly constant model result has no usable geometry. Stretching its rounding noise
    // across [0,1] produces a full-strength speckled map even on a blank picture.
    const double span=double(hi)-lo;
    const bool flat=span<=1e-4*std::max({1.,std::abs(double(lo)),std::abs(double(hi))});
    current_.resize(count);
    for(size_t i=0;i<count;i++)current_[i]=std::isfinite(raw[i])?
        (flat?.5f:float(std::clamp((double(raw[i])-lo)/span,0.,1.))):
        (sameSize?previousInput_[i]:0.f);

    float scale=1,offset=0;
    if(sameSize&&!flat&&stabilizeRange){
        // Fit corresponding surfaces, not the raw range from the previous inference. A new
        // foreground object can change the percentiles without changing background geometry.
        // Robust reweighting excludes those moving surfaces. Work on a bounded sample, and
        // fit to the preceding estimate (not the temporally averaged output, which would
        // progressively flatten moving depth). A cut must never be fitted to a constant.
        const size_t stride=std::max(size_t(1),count/8192);
        double a=1,b=0;bool usable=true;
        for(int pass=0;pass<5;pass++){
            double sw=0,sx=0,sy=0,sxx=0,sxy=0,syy=0;
            for(size_t i=0;i<count;i+=stride){
                if(!std::isfinite(raw[i])||raw[i]<lo||raw[i]>hi)continue;
                const double x=current_[i],y=previousInput_[i];
                if(x<=.02||x>=.98||y<=.02||y>=.98)continue;
                const double residual=std::abs(a*x+b-y);
                const double w=pass==0?1.:std::min(1.,.025/std::max(residual,1e-9));
                sw+=w;sx+=w*x;sy+=w*y;sxx+=w*x*x;sxy+=w*x*y;syy+=w*y*y;
            }
            const double vx=sxx-sx*sx/std::max(sw,1.),vy=syy-sy*sy/std::max(sw,1.);
            if(sw<8||vx/sw<.0004||vy/sw<.0004){usable=false;break;}
            if(sxy-sx*sy/sw<=0){usable=false;break;}
            a=std::sqrt(vy/vx);b=(sy-a*sx)/sw;
            if(!std::isfinite(a)||!std::isfinite(b)||a<.25||a>4){usable=false;break;}
        }
        if(usable){
            size_t good=0,total=0;double sx=0,sy=0,sxx=0,syy=0,sxy=0;
            for(size_t i=0;i<count;i+=stride)if(std::isfinite(raw[i])){
                ++total;const double x=current_[i],y=previousInput_[i];
                if(x>.02&&x<.98&&y>.02&&y<.98&&std::abs(a*x+b-y)<.04){++good;sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;}
            }
            if(good>=8&&double(good)/total>=.6){
                // Refit only agreeing surfaces so moving outliers cannot drag the range.
                // Use a symmetric variance ratio: ordinary regression shrinks contrast
                // a little on every noisy frame because its x values also contain noise.
                const double vx=sxx-sx*sx/good,vy=syy-sy*sy/good,cov=sxy-sx*sy/good;
                if(vx/good>.0004&&vy/good>.0004&&cov>.9*std::sqrt(vx*vy)){
                    a=std::sqrt(vy/vx);b=(sy-a*sx)/good;
                    if(a>=.25&&a<=4){scale=float(a);offset=float(b);}
                }
            }
        }
    }
    for(size_t i=0;i<count;i++)current_[i]=sameSize&&!std::isfinite(raw[i])?
        previousInput_[i]:std::clamp(current_[i]*scale+offset,0.f,1.f);
    cut_=false;
    if(sameSize){
        double change=0;size_t valid=0;
        for(size_t i=0;i<count;i++)if(std::isfinite(raw[i])){change+=std::abs(current_[i]-previousInput_[i]);++valid;}
        cut_=valid&&change/double(valid)>cutThreshold;
        if(cut_){
            // A structurally different scene gets its own range immediately.
            scale=1;offset=0;
            for(size_t i=0;i<count;i++)current_[i]=std::isfinite(raw[i])?
                (flat?.5f:float(std::clamp((double(raw[i])-lo)/span,0.,1.))):0.f;
        }
    }
    const float smooth=std::clamp(smoothing,0.f,.99f);
    filtered_=current_;
    if(smooth>0){
        // Small, edge-aware spatial average. Depth boundaries keep their contrast while
        // low-amplitude pixel noise on a surface is not magnified by stereo reprojection.
        for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
            const size_t i=size_t(y)*width+x;const float center=current_[i];float sum=0,weight=0;
            float neighbors[9];unsigned neighborCount=0,support=0;
            for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
                const int xx=int(x)+dx,yy=int(y)+dy;
                if(xx<0||yy<0||xx>=int(width)||yy>=int(height))continue;
                const size_t j=size_t(yy)*width+xx;
                const float v=current_[j],edge=std::max(0.f,1-std::abs(v-center)/.12f);
                neighbors[neighborCount++]=v;if(edge>0)++support;
                const float w=float((dx==0?2:1)*(dy==0?2:1))*edge*edge;
                sum+=v*w;weight+=w;
            }
            if(support<(neighborCount+2)/3){
                // An isolated impulse has no supporting surface. Keep thin edges/lines
                // with multiple agreeing neighbors, but suppress single-pixel sparkles.
                std::nth_element(neighbors,neighbors+neighborCount/2,neighbors+neighborCount);
                filtered_[i]=neighbors[neighborCount/2];
            }else filtered_[i]=sum/weight;
        }
    }
    out.resize(count);
    if(!sameSize||cut_)out=filtered_;
    else{
        // The same smoothing has the same time constant at 15, 30 or 60 depth updates/s.
        // Magnitude alone cannot distinguish real motion from a bad inference. Only a
        // repeated, consistent large change gets faster catch-up on the next estimate.
        const float dt=std::isfinite(elapsedSeconds)&&elapsedSeconds>0?elapsedSeconds:1.f/30;
        const float duration=std::clamp(dt,1.f/240,.25f)*30;
        const float base=1-std::pow(smooth,duration),catchUp=1-std::pow(.35f,duration);
        for(size_t i=0;i<count;i++){
            if(!std::isfinite(raw[i])){out[i]=previous_[i];continue;}
            const float n=filtered_[i],p=previous_[i],delta=n-p;
            const bool persistent=std::abs(delta)>.12f&&std::abs(current_[i]-previousInput_[i])<.03f&&
                (previousInput_[i]-p)*delta>0;
            const float alpha=persistent?std::max(base,catchUp):base;
            out[i]=p+delta*alpha;
        }
    }
    previous_=out;previousInput_=current_;width_=width;height_=height;
    low_=flat?lo:float(double(lo)-offset*span/scale);
    high_=flat?hi:float(double(lo)+(1-offset)*span/scale);frames_++;
    return true;
}
}
