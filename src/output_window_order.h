#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <cstdint>

namespace vision {
// Black stereo slots must completely cover the application's image beneath.
// Alpha 254 exposed packed game pixels and forced expensive composition.
inline bool configureOutputOverlay(HWND window){
    COLORREF key=0;BYTE alpha=0;DWORD flags=0;
    constexpr BYTE desired=255;
    if(GetLayeredWindowAttributes(window,&key,&alpha,&flags)&&alpha==desired&&flags==LWA_ALPHA)return true;
    return SetLayeredWindowAttributes(window,0,desired,LWA_ALPHA)!=FALSE;
}
// The output covers the display independently of game focus, size or position.
inline void keepOutputOnDisplay(HWND output,const RECT& area){
    if(!IsWindow(output)||area.right<=area.left||area.bottom<=area.top)return;
    RECT current{};
    if(GetWindowRect(output,&current)&&!EqualRect(&current,&area))
        SetWindowPos(output,nullptr,area.left,area.top,area.right-area.left,area.bottom-area.top,SWP_NOACTIVATE|SWP_NOZORDER);
}
// Restore priority only when another window actually covers output. Keep input
// focus with the application; repeated takeovers must never stop stereo output.
class OutputWindowOrder {
public:
    enum class Result { Unchanged, Restored, Failed };
    void reset(){*this=OutputWindowOrder{};}
    uint64_t repairs()const{return repairs_;}

    Result maintain(HWND output,HWND controls,ULONGLONG now=GetTickCount64(),HWND foreground=GetForegroundWindow(),HWND mirror=nullptr){
        (void)now;
        if(!IsWindow(output)||!IsWindowVisible(output)||IsIconic(output)||
           (GetWindowLongPtrW(output,GWL_STYLE)&WS_CHILD))return Result::Unchanged;
        RECT area{};if(!GetWindowRect(output,&area))return Result::Failed;
        foreground=GetAncestor(foreground,GA_ROOT);
        HWND blocker=nullptr;
        // A notification or a window on another monitor is not a fullscreen
        // competitor. Leave it alone, including dialogs owned by our controls.
        const auto competes=[&](HWND above){
            if(above==output||above==mirror||above==controls||(controls&&GetAncestor(above,GA_ROOTOWNER)==controls)||
               !IsWindowVisible(above)||IsIconic(above))return false;
            DWORD cloaked=0;
            if(SUCCEEDED(DwmGetWindowAttribute(above,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked)return false;
            RECT rect{},overlap{};
            return GetWindowRect(above,&rect)&&IntersectRect(&overlap,&rect,&area)&&
               (above==foreground||(rect.left<=area.left&&rect.top<=area.top&&
                                    rect.right>=area.right&&rect.bottom>=area.bottom));
        };
        for(HWND above=GetWindow(output,GW_HWNDPREV);above;above=GetWindow(above,GW_HWNDPREV))
            if(competes(above)){blocker=above;break;}
        const bool raiseOutput=blocker||!(GetWindowLongPtrW(output,GWL_EXSTYLE)&WS_EX_TOPMOST);
        // Screen capture removes the stereo HWND. The mirror must remain above
        // the game as well as below stereo, including after a game activation.
        // Otherwise gameplay is repaired but the stream alternates game/mirror.
        bool raiseMirror=false;
        if(mirror&&IsWindowVisible(mirror)&&!IsIconic(mirror)){
            bool outputAbove=false;
            raiseMirror=raiseOutput||!(GetWindowLongPtrW(mirror,GWL_EXSTYLE)&WS_EX_TOPMOST);
            for(HWND above=GetWindow(mirror,GW_HWNDPREV);above;above=GetWindow(above,GW_HWNDPREV)){
                if(above==output)outputAbove=true;
                if(competes(above)){raiseMirror=true;if(!blocker)blocker=above;}
            }
            raiseMirror=raiseMirror||!outputAbove;
        }
        bool raiseControls=false;
        if(controls&&IsWindowVisible(controls)&&!IsIconic(controls)){
            raiseControls=raiseOutput||!(GetWindowLongPtrW(controls,GWL_EXSTYLE)&WS_EX_TOPMOST);
            if(!raiseControls){
                for(HWND above=GetWindow(controls,GW_HWNDPREV);above;above=GetWindow(above,GW_HWNDPREV))
                    if(above==output){raiseControls=true;break;}
            }
        }
        if(!raiseOutput&&!raiseControls&&!raiseMirror)return Result::Unchanged;
        const bool repairCompetition=raiseOutput||(raiseMirror&&blocker);
        constexpr UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER;
        HDWP changes=BeginDeferWindowPos(3);
        if(raiseOutput&&changes)changes=DeferWindowPos(changes,output,HWND_TOPMOST,0,0,0,0,flags);
        if(raiseMirror&&changes)changes=DeferWindowPos(changes,mirror,output,0,0,0,0,flags);
        if(raiseControls&&changes)changes=DeferWindowPos(changes,controls,HWND_TOPMOST,0,0,0,0,flags);
        if(!changes||!EndDeferWindowPos(changes))return Result::Failed;
        if(repairCompetition)++repairs_;
        return Result::Restored;
    }
private:
    uint64_t repairs_=0;
};
}
