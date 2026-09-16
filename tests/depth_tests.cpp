// CPU side of the whole-screen conversion: the network input size and the depth normaliser.
#include "screen_depth.h"
#include "depth_channel.h"
#include "core.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace vision;
static int checks=0;
static void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
int main(){try{
    // Net input: the long side is the quality, both sides are multiples of the patch, aspect kept.
    {auto n=chooseNetSize(3840,2160,518);require(n.width==518&&n.height==294,"16:9 picture at 518 gives 518 x 294 (21 patches)");
     n=chooseNetSize(2160,3840,518);require(n.width==294&&n.height==518,"Portrait keeps the long side vertical");
     n=chooseNetSize(1000,1000,700);require(n.width==700&&n.height==700,"Square picture gives a square input");
     n=chooseNetSize(3840,2160,520);require(n.width==518,"The long side rounds down to a multiple of the patch");
     n=chooseNetSize(3840,100,518);require(n.height==14,"A very wide picture still gets one patch of height");
     n=chooseNetSize(0,0,518);require(n.width==518&&n.height==518,"Unknown aspect gives a square");
     for(unsigned w:{640u,1920u,2560u,3440u,5120u})for(unsigned h:{480u,1080u,1440u,2160u}){auto s=chooseNetSize(w,h,924);require(s.width%14==0&&s.height%14==0&&s.width<=924&&s.height<=924&&s.width>=14&&s.height>=14,"Every size is patch aligned and bounded");}
     require(chooseNetSize(3840,2160,depth::maxSide).width<=depth::maxSide,"The channel buffers hold the largest input");}
    // Normaliser: relative depth of any scale becomes nearness in [0, 1], near = 1.
    {DepthNormalizer norm;norm.smoothing=0;std::vector<float> raw(100*50),out;
     for(unsigned y=0;y<50;y++)for(unsigned x=0;x<100;x++)raw[y*100+x]=x<50?3.f:12.f; // right half nearer
     require(norm.process(raw.data(),100,50,out),"A finite map normalises");
     require(out.size()==raw.size()&&out[0]<.05f&&out[99]>.95f,"Far pixels approach 0, near pixels approach 1");
     require(norm.frames()==1&&!norm.cut(),"The first frame is not a cut");
     // The same content scaled and offset (the network's per-frame scale) gives the same nearness.
     std::vector<float> scaled(raw.size());for(size_t i=0;i<raw.size();i++)scaled[i]=raw[i]*7+100;norm.process(scaled.data(),100,50,out);
     require(out[0]<.05f&&out[99]>.95f,"Scale and offset of the raw depth do not change the nearness");
     // Extreme single pixels do not set the range.
     std::vector<float> spiky=raw;spiky[0]=-1e6f;spiky[1]=1e6f;DepthNormalizer robust;robust.smoothing=0;robust.process(spiky.data(),100,50,out);
     require(out[99]>.9f&&out[10]<.1f,"Percentile range ignores a few extreme pixels");
     // Non-finite input: rejected when nothing is finite, ignored otherwise.
     std::vector<float> nan(raw.size(),NAN);DepthNormalizer empty;require(!empty.process(nan.data(),100,50,out),"An all-NaN map is rejected");
     std::vector<float> someNan=raw;someNan[5]=NAN;DepthNormalizer partial;partial.smoothing=0;require(partial.process(someNan.data(),100,50,out)&&out[5]==0&&out[99]>.95f,"A NaN pixel becomes far and the rest normalise");}
    // Smoothing: a small change is averaged over frames, a large one is followed at once, a cut resets.
    {DepthNormalizer norm;norm.smoothing=.8f;norm.rangeSmoothing=0;std::vector<float> a(64*64,0.f),out;for(unsigned i=0;i<64*64;i++)a[i]=float(i%64)/63; // a ramp
     norm.process(a.data(),64,64,out);std::vector<float> first=out;
     std::vector<float> b=a;b[10]+=.05f;norm.process(b.data(),64,64,out);
     require(std::abs(out[10]-first[10])<.03f&&out[10]>first[10],"A small change moves the smoothed value only part of the way");
     std::vector<float> c=a;c[10]=1.f;norm.process(c.data(),64,64,out);require(out[10]>.9f,"A large change is followed at once");
     std::vector<float> flipped(64*64);for(unsigned i=0;i<64*64;i++)flipped[i]=1-a[i];norm.process(flipped.data(),64,64,out);
     require(norm.cut()&&std::abs(out[0]-1)<.02f&&out[63]<.02f,"A cut takes the new frame whole");
     norm.reset();require(norm.frames()==0,"Reset clears the history");}
    // Screen settings live in the profile.
    {Settings s;s.screen.separation=.031f;s.screen.convergence=.77f;s.screen.popOut=.25f;s.screen.smoothing=.6f;s.screen.quality=700;s.screen.steps=16;s.screen.depth=false;s.screen.model="D:\\models\\depth anything large.onnx";
     auto file=std::filesystem::temp_directory_path()/"vision-screen-settings-test.ini";saveProfile(file,s);auto l=loadProfile(file);std::filesystem::remove(file);
     require(std::abs(l.screen.separation-.031f)<1e-6f&&std::abs(l.screen.convergence-.77f)<1e-6f&&std::abs(l.screen.popOut-.25f)<1e-6f&&std::abs(l.screen.smoothing-.6f)<1e-6f&&l.screen.quality==700&&l.screen.steps==16,"Screen conversion settings round-trip through the profile");
     require(l.screen.model==s.screen.model,"The chosen depth model, spaces included, round-trips through the profile");
     Settings none;saveProfile(file,none);require(loadProfile(file).screen.model.empty(),"No chosen model stays empty");std::filesystem::remove(file);
     require(l.screen.depth&&!l.screen.showDepth,"2D and depth-view toggles are session state, not profile state");
     Settings bad;bad.screen.separation=.5f;bool threw=false;try{validate(bad);}catch(const std::exception&){threw=true;}require(threw,"Separation beyond 10% of the width is rejected");
     bad=Settings{};bad.screen.quality=5000;threw=false;try{validate(bad);}catch(const std::exception&){threw=true;}require(threw,"A network input larger than the channel is rejected");
     bad=Settings{};bad.screen.convergence=NAN;threw=false;try{validate(bad);}catch(const std::exception&){threw=true;}require(threw,"A non-finite screen plane is rejected");}
    std::cout<<"PASS: "<<checks<<" screen depth checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
