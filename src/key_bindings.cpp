#include "key_bindings.h"
#include "platform.h"
#include <fstream>
#include <sstream>

namespace vision {
namespace {
constexpr UINT firstHotkey=500;
std::vector<KeyBinding> defaultBindings(){
    constexpr UINT ca=MOD_CONTROL|MOD_ALT;
    return {
        {KeyAction::Menu,"Show / hide controls",VK_HOME,0,true,true},
        {KeyAction::Fullscreen,"Fullscreen / windowed output",VK_F11,0},
        {KeyAction::Stop,"Stop output",VK_F8,ca,true,true},
        {KeyAction::Pause,"Pause / resume output",VK_SPACE,0},
        {KeyAction::PhaseUp,"Increase phase",VK_UP,ca,true,true},
        {KeyAction::PhaseDown,"Decrease phase",VK_DOWN,ca,true,true},
        {KeyAction::ShutterUp,"Increase shutter",VK_RIGHT,ca,true,true},
        {KeyAction::ShutterDown,"Decrease shutter",VK_LEFT,ca,true,true},
        {KeyAction::Swap,"Swap eyes",'X',ca,true,true},
        {KeyAction::Relock,"Re-lock synchronization",'R',ca,true,true},
        {KeyAction::DepthUp,"Increase AI depth",VK_PRIOR,ca,true,true},
        {KeyAction::DepthDown,"Decrease AI depth",VK_NEXT,ca,true,true},
        {KeyAction::PlaneNear,"Move screen plane nearer",VK_HOME,ca,true,true},
        {KeyAction::PlaneFar,"Move screen plane farther",VK_END,ca,true,true},
        {KeyAction::DepthToggle,"Toggle AI 2D / 3D",VK_INSERT,ca,true,true},
        {KeyAction::BandUp,"Increase stereo area",VK_PRIOR,0},
        {KeyAction::BandDown,"Decrease stereo area",VK_NEXT,0},
        {KeyAction::BandMoveUp,"Move stereo area up",0,0},
        {KeyAction::BandMoveDown,"Move stereo area down",VK_END,0},
        {KeyAction::Sweep,"Start / stop phase sweep",'S',0},
        {KeyAction::Mark,"Mark sweep phase",'M',0},
        {KeyAction::Accept,"Keep sweep phase",VK_RETURN,0},
        {KeyAction::Bfi,"Toggle black frame insertion",'B',0},
        {KeyAction::Crosstalk,"Toggle ghost subtraction",'C',0},
        {KeyAction::Target0,"LCD target 0",'0',0},
        {KeyAction::Target1,"LCD target 1",'1',0},
        {KeyAction::Target2,"LCD target 2",'2',0},
        {KeyAction::Target3,"LCD target 3",'3',0},
        {KeyAction::BrightnessUp,"Increase image brightness",0,0},
        {KeyAction::BrightnessDown,"Decrease image brightness",0,0},
        {KeyAction::Hdr,"Toggle HDR output",0,0},
        {KeyAction::ConvergenceUp,"Increase convergence",0,0},
        {KeyAction::ConvergenceDown,"Decrease convergence",0,0}
    };
}
bool repeats(KeyAction action){return action==KeyAction::PhaseUp||action==KeyAction::PhaseDown||action==KeyAction::ShutterUp||action==KeyAction::ShutterDown||action==KeyAction::DepthUp||action==KeyAction::DepthDown||action==KeyAction::PlaneNear||action==KeyAction::PlaneFar||action==KeyAction::BandUp||action==KeyAction::BandDown||action==KeyAction::BandMoveUp||action==KeyAction::BandMoveDown;}
}
KeyBindings::KeyBindings(HWND window,std::filesystem::path path,std::function<void(KeyAction)> dispatch):window_(window),path_(std::move(path)),bindings_(defaultBindings()),dispatch_(std::move(dispatch)){
    std::ifstream input(path_);std::string line;
    while(std::getline(input,line)){std::istringstream in(line);std::string kind;in>>kind;
        if(kind=="enabled"){int value;if(in>>value)enabled_=value!=0;}
        else if(kind=="bind"){unsigned action,key,mods;int enabled,global;if(in>>action>>key>>mods>>enabled>>global&&action<bindings_.size()&&key<=255&&(mods&~(MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_WIN))==0){auto& b=bindings_[action];b.key=key;b.modifiers=mods;b.enabled=enabled!=0;b.global=global!=0;}}
    }
    refresh();
}
KeyBindings::~KeyBindings(){for(size_t i=0;i<bindings_.size();++i)if(bindings_[i].registered)UnregisterHotKey(window_,firstHotkey+UINT(i));}
void KeyBindings::refresh(){
    for(size_t i=0;i<bindings_.size();++i){auto& b=bindings_[i];if(b.registered)UnregisterHotKey(window_,firstHotkey+UINT(i));b.registered=false;}
    if(!enabled_||!active_||capture_>=0)return;
    for(size_t i=0;i<bindings_.size();++i){auto& b=bindings_[i];if(b.enabled&&b.global&&b.key)b.registered=RegisterHotKey(window_,firstHotkey+UINT(i),b.modifiers|(repeats(b.action)?0:MOD_NOREPEAT),b.key)!=FALSE;}
}
void KeyBindings::save()const{
    if(path_.empty())return;std::ofstream out(path_);if(!out)return;out<<"enabled "<<enabled_<<'\n';
    for(const auto& b:bindings_)out<<"bind "<<int(b.action)<<' '<<b.key<<' '<<b.modifiers<<' '<<b.enabled<<' '<<b.global<<'\n';
}
void KeyBindings::enable(bool value){enabled_=value;capture_=-1;refresh();save();}
void KeyBindings::edit(size_t index,bool enabled,bool global){auto& b=bindings_.at(index);b.enabled=enabled;b.global=global;refresh();save();}
void KeyBindings::beginCapture(size_t index){capture_=int(index);message="Press the new shortcut. Use Cancel to keep the current binding.";refresh();}
void KeyBindings::cancelCapture(){capture_=-1;message.clear();refresh();}
void KeyBindings::clear(size_t index){bindings_.at(index).key=0;capture_=-1;refresh();save();}
void KeyBindings::defaults(){for(size_t i=0;i<bindings_.size();++i)if(bindings_[i].registered)UnregisterHotKey(window_,firstHotkey+UINT(i));bindings_=defaultBindings();capture_=-1;message.clear();refresh();save();}
UINT KeyBindings::modifiers(){UINT mods=0;if(GetKeyState(VK_CONTROL)&0x8000)mods|=MOD_CONTROL;if(GetKeyState(VK_MENU)&0x8000)mods|=MOD_ALT;if(GetKeyState(VK_SHIFT)&0x8000)mods|=MOD_SHIFT;if((GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000)mods|=MOD_WIN;return mods;}
bool KeyBindings::keyDown(UINT key,LPARAM data){
    if(capture_>=0){
        if(key==VK_SHIFT||key==VK_CONTROL||key==VK_MENU||key==VK_LWIN||key==VK_RWIN)return true;
        if(data&(1LL<<30))return true;const UINT mods=modifiers();
        for(size_t i=0;i<bindings_.size();++i)if(int(i)!=capture_&&bindings_[i].enabled&&bindings_[i].key==key&&bindings_[i].modifiers==mods){message=std::string("Already assigned to ")+bindings_[i].label;return true;}
        auto& b=bindings_.at(capture_);b.key=key;b.modifiers=mods;capture_=-1;message.clear();refresh();save();return true;
    }
    if(!enabled_||!active_)return false;const UINT mods=modifiers();
    for(const auto& b:bindings_)if(b.enabled&&!b.global&&b.key==key&&b.modifiers==mods){if(!(data&(1LL<<30))||repeats(b.action))dispatch_(b.action);return true;}
    return false;
}
bool KeyBindings::hotkey(UINT id){
    if(id<firstHotkey||id>=firstHotkey+bindings_.size())return false;
    const auto& b=bindings_[id-firstHotkey];if(enabled_&&active_&&capture_<0&&b.enabled&&b.registered)dispatch_(b.action);return true;
}
std::string KeyBindings::chord(const KeyBinding& b){
    if(!b.key)return "Unassigned";std::string name;
    if(b.modifiers&MOD_CONTROL)name+="Ctrl+";if(b.modifiers&MOD_ALT)name+="Alt+";if(b.modifiers&MOD_SHIFT)name+="Shift+";if(b.modifiers&MOD_WIN)name+="Win+";
    LONG scan=LONG(MapVirtualKeyW(b.key,MAPVK_VK_TO_VSC)<<16);if(b.key==VK_HOME||b.key==VK_END||b.key==VK_PRIOR||b.key==VK_NEXT||b.key==VK_LEFT||b.key==VK_RIGHT||b.key==VK_UP||b.key==VK_DOWN||b.key==VK_INSERT||b.key==VK_DELETE)scan|=1<<24;
    wchar_t text[128]{};GetKeyNameTextW(scan,text,128);return name+(*text?utf8(text):std::to_string(b.key));
}
}
