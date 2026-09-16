#include "control_menu.h"
#include "key_bindings.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace vision;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    const auto instance=GetModuleHandleW(nullptr);
    HWND controls=CreateWindowW(L"STATIC",L"Menu test",WS_OVERLAPPEDWINDOW,100,100,800,600,nullptr,nullptr,instance,nullptr);
    HWND output=CreateWindowW(L"STATIC",L"Independent output",WS_POPUP,0,0,1200,800,nullptr,nullptr,instance,nullptr);
    require(controls&&output,"Create test windows");
    RECT original{},outputRect{};GetWindowRect(controls,&original);GetWindowRect(output,&outputRect);const auto style=GetWindowLongPtrW(controls,GWL_STYLE);
    ControlMenu menu(controls,true);menu.toggle();require(!menu.visible()&&!menu.active(),"Menu activated outside fullscreen");
    const RECT display{0,0,1920,1080};menu.enterFullscreen(display);require(menu.active()&&!menu.visible(),"Fullscreen must begin with the normal app hidden");
    for(int i=0;i<6;++i){menu.toggle();require(menu.visible()==(i%2==0),"Menu visibility toggle");RECT actual{};GetWindowRect(output,&actual);require(IsWindow(output)&&!GetParent(output)&&EqualRect(&actual,&outputRect),"Menu mutated the output window");}
    menu.leaveFullscreen();RECT restored{};GetWindowRect(controls,&restored);require(!menu.active()&&GetWindowLongPtrW(controls,GWL_STYLE)==style&&EqualRect(&restored,&original),"Normal app placement/style not restored");
    const auto path=std::filesystem::temp_directory_path()/("vision-shortcuts-"+std::to_string(GetCurrentProcessId())+".ini");
    {int calls=0;KeyAction last=KeyAction::Menu;KeyBindings bindings(controls,path,[&](KeyAction a){last=a;++calls;});
        require(!bindings.active()&&!bindings.keyDown(VK_SPACE,0)&&calls==0,"Shortcuts active outside fullscreen");
        bindings.beginCapture(0);require(bindings.keyDown(VK_F24,0),"Shortcut capture");require(bindings.bindings()[0].key==VK_F24,"Assigned key was not retained");
        bindings.activate(true);require(bindings.bindings()[0].registered,"Global shortcut did not register");
        require(bindings.keyDown(VK_SPACE,0)&&calls==1&&last==KeyAction::Pause,"Local shortcut dispatch");
        bindings.keyDown(VK_SPACE,1LL<<30);require(calls==1,"Toggle repeated while key held");
        bindings.hotkey(500);require(calls==2&&last==KeyAction::Menu,"Global menu shortcut dispatch");
        bindings.enable(false);require(!bindings.bindings()[0].registered&&!bindings.keyDown(VK_SPACE,0),"Disabled shortcuts still consume keys");bindings.hotkey(500);require(calls==2,"Queued shortcut executed after disable");
        require(RegisterHotKey(controls,9000,MOD_NOREPEAT,VK_F24)!=FALSE,"Disabled global shortcut was not released");UnregisterHotKey(controls,9000);
        bindings.enable(true);bindings.edit(0,false,true);require(!bindings.bindings()[0].registered,"Individual disable failed");
        bindings.edit(0,true,true);bindings.beginCapture(1);bindings.keyDown(VK_F24,0);require(bindings.bindings()[1].key==VK_F11&&bindings.capturing()==1,"Duplicate shortcut accepted");bindings.cancelCapture();
        bindings.activate(false);bindings.hotkey(500);require(calls==2&&!bindings.bindings()[0].registered,"Fullscreen exit left shortcuts active");
        bindings.enable(false);
    }
    {KeyBindings loaded(controls,path,[](KeyAction){});require(!loaded.enabled()&&loaded.bindings()[0].key==VK_F24,"Shortcut preferences did not survive restart");}
    std::filesystem::remove(path);DestroyWindow(output);DestroyWindow(controls);
    std::cout<<"PASS: fullscreen-only menu, independent output, restored window, rebind/disable/release/conflict/persistence checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
