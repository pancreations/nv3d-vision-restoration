#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vision {
enum class KeyAction { Menu,Fullscreen,Stop,Pause,PhaseUp,PhaseDown,ShutterUp,ShutterDown,Swap,Relock,DepthUp,DepthDown,PlaneNear,PlaneFar,DepthToggle,BandUp,BandDown,BandMoveUp,BandMoveDown,Sweep,Mark,Accept,Bfi,Crosstalk,Target0,Target1,Target2,Target3,BrightnessUp,BrightnessDown,Hdr,ConvergenceUp,ConvergenceDown };
struct KeyBinding { KeyAction action;const char* label;UINT key,modifiers;bool enabled=true,global=false,registered=false; };
class KeyBindings {
    HWND window_=nullptr;std::filesystem::path path_;bool enabled_=true,active_=false;int capture_=-1;
    std::vector<KeyBinding> bindings_;std::function<void(KeyAction)> dispatch_;
    void refresh();
public:
    std::string message;
    KeyBindings(HWND window,std::filesystem::path path,std::function<void(KeyAction)> dispatch);
    ~KeyBindings();
    const std::vector<KeyBinding>& bindings()const{return bindings_;}
    bool enabled()const{return enabled_;}
    bool active()const{return active_;}
    void activate(bool active){if(active_!=active){active_=active;refresh();}}
    int capturing()const{return capture_;}
    void enable(bool value);
    void edit(size_t index,bool enabled,bool global);
    void beginCapture(size_t index);
    void cancelCapture();
    void clear(size_t index);
    void defaults();
    bool keyDown(UINT key,LPARAM data);
    bool hotkey(UINT id);
    void save()const;
    static std::string chord(const KeyBinding& binding);
    static UINT modifiers();
};
}
