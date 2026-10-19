// Temporal regressions for relative AI depth. These sequences keep scene geometry separate
// from arbitrary network scale/offset, isolated noise, real motion, and scene changes.
#include "screen_depth.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using namespace vision;
namespace {
constexpr unsigned width=96,height=64;
int checks=0,failures=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::vector<float> ramp(){
    std::vector<float> v(size_t(width)*height);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
        v[size_t(y)*width+x]=10.f+.75f*float(x)/float(width-1)+.25f*float(y)/float(height-1);
    return v;
}
float difference(const std::vector<float>& a,const std::vector<float>& b){
    double sum=0;for(size_t i=0;i<a.size();++i)sum+=std::abs(a[i]-b[i]);
    return float(sum/double(a.size()));
}
void patch(std::vector<float>& v,unsigned x0,unsigned x1,unsigned y0,unsigned y1,float change){
    for(unsigned y=y0;y<y1;++y)for(unsigned x=x0;x<x1;++x)v[size_t(y)*width+x]+=change;
}
template<class Test>void run(const char* name,Test test){
    try{test();std::cout<<"PASS: "<<name<<'\n';}
    catch(const std::exception& e){++failures;std::cerr<<"FAIL: "<<name<<": "<<e.what()<<'\n';}
}
int benchmark(){
    for(const NetSize size:std::vector<NetSize>{{518,294},{1036,588}}){
        DepthNormalizer norm;std::vector<float> raw(size_t(size.width)*size.height),out;
        std::vector<double> samples;uint32_t random=0x6a97b3d1u;
        for(unsigned frame=0;frame<55;++frame){
            for(unsigned y=0;y<size.height;++y)for(unsigned x=0;x<size.width;++x){
                random=random*1664525u+1013904223u;
                raw[size_t(y)*size.width+x]=10.f+.75f*float(x)/float(size.width-1)+.25f*float(y)/float(size.height-1)+
                    (float(random>>8)/16777215.f-.5f)*.02f;
            }
            const auto start=std::chrono::steady_clock::now();
            if(!norm.process(raw.data(),size.width,size.height,out))return 1;
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(frame>=5)samples.push_back(ms);
        }
        double total=0;for(double ms:samples)total+=ms;std::sort(samples.begin(),samples.end());
        std::cout<<size.width<<'x'<<size.height<<" CPU normalization, 50 updates after warmup: mean "
                 <<total/double(samples.size())<<" ms, p95 "<<samples[47]<<" ms, max "<<samples.back()<<" ms\n";
    }
    return 0;
}
}
int main(int argc,char** argv){
    if(argc==2&&std::string(argv[1])=="--bench")return benchmark();
    run("static scene with changing network scale and offset",[]{
        const auto raw=ramp();
        for(float smoothing:{.16f,.8f}){
            DepthNormalizer norm;norm.smoothing=smoothing;std::vector<float> out;
            require(norm.process(raw.data(),width,height,out),"Initial ramp accepted");const auto reference=out;
            float worst=0;
            for(unsigned frame=0;frame<48;++frame){
                const float scale=frame%3==0?1.01f:frame%3==1?.99f:7.f;
                const float offset=frame%3==2?100.f:(frame%2?.02f:-.02f);
                auto drifting=raw;for(float& value:drifting)value=value*scale+offset;
                require(norm.process(drifting.data(),width,height,out),"Affine frame accepted");
                worst=std::max(worst,difference(out,reference));
                require(!norm.cut(),"A positive affine rescaling of the same scene must not trigger a cut");
            }
            std::cout<<"  affine mean depth error, smoothing "<<smoothing<<": "<<worst<<'\n';
            require(worst<.003f,"Arbitrary network scale and offset must not make a stationary scene bounce");
        }
    });
    run("minority foreground does not pump stationary background",[]{
        const auto raw=ramp();DepthNormalizer norm;norm.smoothing=.16f;std::vector<float> out;
        norm.process(raw.data(),width,height,out);const auto reference=out;
        float worst=0;
        for(unsigned frame=0;frame<24;++frame){
            auto moving=raw;if(frame<12)patch(moving,36,60,24,40,1.5f);
            norm.process(moving.data(),width,height,out);require(!norm.cut(),"A small foreground object is not a scene cut");
            double sum=0;size_t count=0;
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                if(x>=33&&x<63&&y>=21&&y<43)continue;
                const size_t i=size_t(y)*width+x;sum+=std::abs(out[i]-reference[i]);++count;
            }
            worst=std::max(worst,float(sum/double(count)));
        }
        std::cout<<"  unaffected background mean depth error: "<<worst<<'\n';
        require(worst<.015f,"Foreground percentile changes must not remap the stationary background");
    });
    run("long noisy sequence retains scene depth contrast",[]{
        const auto raw=ramp();DepthNormalizer norm;std::vector<float> out;
        norm.process(raw.data(),width,height,out);
        auto deviation=[](const std::vector<float>& values){
            double sum=0,squared=0;for(float value:values){sum+=value;squared+=double(value)*value;}
            const double mean=sum/double(values.size());
            return std::sqrt(std::max(0.,squared/double(values.size())-mean*mean));
        };
        const double originalContrast=deviation(out);double minimumRatio=1,maximumRatio=1;unsigned cuts=0;
        uint32_t random=0x16b7a93du;
        for(unsigned frame=0;frame<2400;++frame){
            auto noisy=raw;
            for(float& value:noisy){random=random*1664525u+1013904223u;value+=(float(random>>8)/16777215.f-.5f)*.05f;}
            norm.process(noisy.data(),width,height,out);if(norm.cut())++cuts;
            const double ratio=deviation(out)/originalContrast;
            minimumRatio=std::min(minimumRatio,ratio);maximumRatio=std::max(maximumRatio,ratio);
        }
        std::cout<<"  noisy-sequence contrast ratio: "<<minimumRatio<<" to "<<maximumRatio<<", cuts "<<cuts<<'\n';
        require(!cuts,"A static scene with small inference noise must not create scene cuts");
        require(minimumRatio>.95&&maximumRatio<1.05,"Repeated noisy alignment must preserve scene contrast rather than flattening or inflating it");
    });
    run("isolated spikes and alternating coherent noise",[]{
        const auto raw=ramp();const size_t center=size_t(height/2)*width+width/2;
        for(float smoothing:{.5f,.8f}){
            DepthNormalizer norm;norm.smoothing=smoothing;std::vector<float> out;
            norm.process(raw.data(),width,height,out);const float reference=out[center];
            auto spike=raw;spike[center]+=1.f;norm.process(spike.data(),width,height,out);
            require(std::abs(out[center]-reference)<.1f,"An isolated one-frame depth spike must not pass straight through");
            norm.reset();norm.process(raw.data(),width,height,out);
            float low=1.f,high=0.f;
            for(unsigned frame=0;frame<40;++frame){
                auto noisy=raw;patch(noisy,36,60,24,40,frame%2?.08f:-.08f);
                norm.process(noisy.data(),width,height,out);
                require(!norm.cut(),"Local inference noise must not trigger a scene cut");
                if(frame>=20){low=std::min(low,out[center]);high=std::max(high,out[center]);}
            }
            std::cout<<"  alternating patch peak-to-peak, smoothing "<<smoothing<<": "<<high-low<<'\n';
            require(high-low<.09f,"Alternating coherent depth noise must be damped at normal smoothing levels");
        }
    });
    run("sustained moving foreground follows without a long ghost",[]{
        const auto raw=ramp();DepthNormalizer norm;norm.smoothing=.16f;std::vector<float> out;
        norm.process(raw.data(),width,height,out);const auto reference=out;
        const size_t oldCenter=size_t(32)*width+30,newCenter=size_t(32)*width+64;
        auto first=raw;patch(first,24,36,26,38,.4f);
        for(unsigned frame=0;frame<4;++frame)norm.process(first.data(),width,height,out);
        require(out[oldCenter]-reference[oldCenter]>.30f,"A sustained object must acquire its new depth within four updates");
        auto moved=raw;patch(moved,58,70,26,38,.4f);
        for(unsigned frame=0;frame<4;++frame){norm.process(moved.data(),width,height,out);require(!norm.cut(),"Moving foreground is not a cut");}
        require(std::abs(out[oldCenter]-reference[oldCenter])<.06f,"Vacated background must recover within four updates");
        require(out[newCenter]-reference[newCenter]>.20f,"The moved object must follow within four updates");
    });
    run("temporal response uses elapsed time",[]{
        const auto raw=ramp();auto changed=raw;patch(changed,36,60,24,40,.08f);
        const size_t center=size_t(height/2)*width+width/2;std::vector<float> results;
        for(unsigned rate:{15u,30u,60u}){
            DepthNormalizer norm;norm.smoothing=.9f;std::vector<float> out;
            norm.process(raw.data(),width,height,out);
            // All rates observe the same stationary new surface for one fifth of a second.
            for(unsigned frame=0;frame<rate/5;++frame)
                norm.process(changed.data(),width,height,out,1.f/float(rate));
            results.push_back(out[center]);
        }
        const auto bounds=std::minmax_element(results.begin(),results.end());
        std::cout<<"  15/30/60 Hz response spread: "<<*bounds.second-*bounds.first<<'\n';
        require(*bounds.second-*bounds.first<.02f,"Changing inference rate must not greatly change damping duration");
    });
    run("denoising preserves supported thin geometry",[]{
        const auto raw=ramp();auto thin=raw;patch(thin,48,49,12,52,.3f);
        DepthNormalizer background,surface;std::vector<float> reference,out;
        background.process(raw.data(),width,height,reference);surface.process(thin.data(),width,height,out);
        const size_t center=size_t(height/2)*width+48;
        require(out[center]-reference[center]>.25f,"A supported one-pixel-wide surface must survive spatial denoising");
        require(std::abs(out[center-2]-reference[center-2])<.02f,"A thin surface must not blur into surrounding background");
    });
    run("scene cuts reset temporal history",[]{
        const auto raw=ramp();DepthNormalizer norm;norm.smoothing=.8f;std::vector<float> out;
        norm.process(raw.data(),width,height,out);auto reversed=raw;for(float& value:reversed)value=21.f-value;
        DepthNormalizer fresh;std::vector<float> expected;fresh.process(reversed.data(),width,height,expected);
        norm.process(reversed.data(),width,height,out);
        require(norm.cut(),"A spatially reversed scene must be detected as a cut");
        require(difference(out,expected)<.003f,"A real cut must take the new scene without old geometry ghosting");
        norm.reset();require(norm.frames()==0&&!norm.cut(),"Reset clears frame and cut history");
        norm.process(reversed.data(),width,height,out);
        require(!norm.cut()&&difference(out,expected)<.003f,"The first frame after reset stands alone");
    });
    run("invalid and textureless input remain stable",[]{
        const auto raw=ramp();DepthNormalizer norm;std::vector<float> out;
        norm.process(raw.data(),width,height,out);const auto reference=out;const auto frames=norm.frames();
        std::vector<float> invalid(raw.size(),std::numeric_limits<float>::quiet_NaN());
        require(!norm.process(invalid.data(),width,height,out),"An all-invalid map is rejected");
        require(out==reference&&norm.frames()==frames,"A rejected map must preserve output and history");
        require(!norm.process(raw.data(),0,height,out)&&out==reference,"Zero width is rejected without changing output");
        const size_t center=size_t(height/2)*width+width/2;auto partial=raw;partial[center]=std::numeric_limits<float>::infinity();
        norm.process(partial.data(),width,height,out);
        require(std::abs(out[center]-reference[center])<.02f,"A missing depth sample must not punch a hole into a valid map");
        norm.reset();std::vector<float> flat(raw.size(),10.f);norm.process(flat.data(),width,height,out);
        for(unsigned frame=0;frame<8;++frame){
            for(size_t i=0;i<flat.size();++i)flat[i]=10.f+(i%2==frame%2?1e-5f:-1e-5f);
            norm.process(flat.data(),width,height,out);
            const auto bounds=std::minmax_element(out.begin(),out.end());
            require(std::isfinite(*bounds.first)&&std::isfinite(*bounds.second),"Flat input must remain finite");
            require(*bounds.second-*bounds.first<.05f,"Near-flat numerical noise must not expand into full scene depth");
        }
        const std::vector<float> small{0.f,1.f,0.f,1.f};
        require(norm.process(small.data(),2,2,out)&&out.size()==4&&!norm.cut(),"Changing map dimensions resets incompatible history");
    });
    std::cout<<(failures?"FAIL: ":"PASS: ")<<checks<<" depth stability checks; "<<failures<<" failed sequences\n";
    return failures?1:0;
}
