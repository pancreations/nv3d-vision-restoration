#pragma once
#include <cstdint>
#include <string_view>
namespace vision::capture {
enum class Mode { Disabled,Sequential,Array,Katanga,SideBySide,TopBottom };
inline Mode mode(std::string_view value){
    if(value=="sequential")return Mode::Sequential;if(value=="array")return Mode::Array;
    if(value=="katanga")return Mode::Katanga;if(value=="sbs")return Mode::SideBySide;
    if(value=="tab")return Mode::TopBottom;return Mode::Disabled;
}
struct EyeRegion {uint32_t subresource=0,left=0,top=0,right=0,bottom=0;};
// This is a declared layout, never a heuristic based on the image's contents.
inline bool regions(Mode mode,uint32_t width,uint32_t height,uint32_t layers,EyeRegion (&eyes)[2]){
    if(!width||!height)return false;
    eyes[0]={0,0,0,width,height};eyes[1]=eyes[0];
    switch(mode){
    case Mode::Sequential:return layers==1;
    case Mode::Array:eyes[1].subresource=1;return layers==2;
    case Mode::Katanga:case Mode::SideBySide:if(layers!=1||width%2)return false;eyes[0].right=width/2;eyes[1].left=width/2;return true;
    case Mode::TopBottom:if(layers!=1||height%2)return false;eyes[0].bottom=height/2;eyes[1].top=height/2;return true;
    default:return false;
    }
}
// Only advance on a confirmed successful game Present. Failed/TEST presents
// must not change eye identity. A reconnect starts at the next complete cycle.
class SequentialClock {
    uint64_t index_=0;
public:
    unsigned eye(bool rightFirst=false)const{return unsigned(index_&1)^(rightFirst?1u:0u);}
    uint64_t pair()const{return index_/2+1;}
    bool boundary()const{return (index_&1)==0;}
    void presented(){++index_;}
    void reset(){index_=0;}
};
}
