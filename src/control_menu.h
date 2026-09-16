#pragma once
#include <windows.h>
#include <algorithm>

namespace vision {
// Reuses the complete application UI only while an independent fullscreen
// output is active. It never owns or modifies that output or its source.
class ControlMenu {
    HWND window_,previousFocus_=nullptr;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};LONG_PTR style_=0;DWORD affinity_=0;
    bool active_=false,visible_=false,hiddenForTest_=false,normalVisible_=false;
public:
    explicit ControlMenu(HWND window,bool hiddenForTest=false):window_(window),hiddenForTest_(hiddenForTest){}
    bool active()const{return active_;}
    bool visible()const{return visible_;}
    void enterFullscreen(const RECT& display){
        if(active_)return;GetWindowPlacement(window_,&placement_);style_=GetWindowLongPtrW(window_,GWL_STYLE);normalVisible_=IsWindowVisible(window_)!=FALSE;
        RECT size{};GetWindowRect(window_,&size);active_=true;visible_=false;
        GetWindowDisplayAffinity(window_,&affinity_);SetWindowDisplayAffinity(window_,0x11);
        SetWindowLongPtrW(window_,GWL_STYLE,WS_POPUP|WS_CLIPCHILDREN);
        SetWindowPos(window_,HWND_TOPMOST,display.left+16,display.top+16,std::min(size.right-size.left,display.right-display.left-32L),std::min(size.bottom-size.top,display.bottom-display.top-32L),SWP_FRAMECHANGED|SWP_NOACTIVATE);
        ShowWindow(window_,SW_HIDE);
    }
    void leaveFullscreen(){
        if(!active_)return;active_=visible_=false;
        SetWindowLongPtrW(window_,GWL_STYLE,style_);
        SetWindowPos(window_,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        SetWindowPlacement(window_,&placement_);if(hiddenForTest_||!normalVisible_)ShowWindow(window_,SW_HIDE);
        SetWindowDisplayAffinity(window_,affinity_);
    }
    void show(){
        if(!active_)return;
        if(!visible_)previousFocus_=GetForegroundWindow();visible_=true;
        if(!hiddenForTest_){ShowWindow(window_,SW_RESTORE);SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetForegroundWindow(window_);}
    }
    void hide(){
        if(!visible_)return;visible_=false;const bool focused=GetForegroundWindow()==window_;
        ShowWindow(window_,SW_HIDE);
        if(!hiddenForTest_&&focused&&IsWindow(previousFocus_)&&previousFocus_!=window_)SetForegroundWindow(previousFocus_);
    }
    void toggle(){if(!active_)return;if(visible_)hide();else show();}
};
}
