#include "output_window_order.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace vision;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static unsigned positionChanges=0;
static LRESULT CALLBACK testProc(HWND w,UINT m,WPARAM a,LPARAM b){
    if(m==WM_WINDOWPOSCHANGED&&!(reinterpret_cast<WINDOWPOS*>(b)->flags&SWP_NOZORDER))++positionChanges;
    return DefWindowProcW(w,m,a,b);
}
static bool above(HWND first,HWND second){
    for(HWND w=GetWindow(second,GW_HWNDPREV);w;w=GetWindow(w,GW_HWNDPREV))if(w==first)return true;
    return false;
}
int main(){
    // Exercise real visible Win32 windows on an isolated desktop. No flashing
    // test patterns, foreground changes, or emitter access on the user's desktop.
    const HDESK original=GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name=L"VisionWindowOrderTest-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    std::vector<HWND> windows;
    int result=0;
    try{
        require(desktop&&SetThreadDesktop(desktop),"Create isolated window-test desktop");
        WNDCLASSW wc{};wc.lpfnWndProc=testProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"VisionOrderTest";
        require(RegisterClassW(&wc)!=0,"Register window test class");
        auto make=[&](DWORD ex,int x,int y,int width,int height,HWND owner=nullptr){
            HWND w=CreateWindowExW(ex,wc.lpszClassName,L"Window order test",WS_POPUP,x,y,width,height,owner,nullptr,wc.hInstance,nullptr);
            require(w!=nullptr,"Create window order fixture");windows.push_back(w);ShowWindow(w,SW_SHOWNOACTIVATE);return w;
        };
        auto raise=[](HWND w){require(SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=FALSE,"Raise contender");};
        HWND app=make(WS_EX_TOPMOST,0,0,800,600);
        constexpr DWORD ex=WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW;
        HWND output=make(ex,0,0,800,600);
        require(SetLayeredWindowAttributes(output,0,255,LWA_ALPHA)!=FALSE,"Opaque click-through overlay");
        HWND controls=make(WS_EX_TOPMOST,20,20,300,200);
        HWND notification=make(WS_EX_TOPMOST,600,0,200,100);
        HWND otherMonitor=make(WS_EX_TOPMOST,2000,0,800,600);
        OutputWindowOrder order;
        unsigned before=positionChanges;
        for(int i=0;i<100;++i)require(order.maintain(output,controls,100)==OutputWindowOrder::Result::Unchanged,"Stable stack was reordered");
        require(positionChanges==before&&above(notification,output)&&above(otherMonitor,output),"Notifications or another monitor disturbed");
        raise(app);const HWND foreground=GetForegroundWindow();
        require(above(app,output),"Fixture did not cover output");
        require(order.maintain(output,controls,200)==OutputWindowOrder::Result::Restored,"Fullscreen takeover not repaired");
        require(above(controls,output)&&above(output,app),"Controls/output/app stack incorrect");
        require(GetForegroundWindow()==foreground,"Repair stole input focus");
        BYTE alpha=0;DWORD key=0,flags=0;
        require(GetLayeredWindowAttributes(output,&key,&alpha,&flags)&&alpha==255&&flags==LWA_ALPHA&&GetWindowLongPtrW(output,GWL_EXSTYLE)==ex,"Repair changed opacity or input passthrough");
        require(!GetWindow(output,GW_OWNER)&&!GetWindow(app,GW_OWNER),"Repair attached windows across applications");
        before=positionChanges;
        for(int i=0;i<100;++i)require(order.maintain(output,controls,300)==OutputWindowOrder::Result::Unchanged,"Repaired stack kept fighting");
        require(positionChanges==before&&order.repairs()==1,"Per-frame topmost churn");
        // A full-size owned dialog must remain usable above the controls.
        HWND dialog=make(WS_EX_TOPMOST,0,0,800,600,controls);
        require(order.maintain(output,controls,400)==OutputWindowOrder::Result::Unchanged&&above(dialog,output),"Owned dialog covered by output");
        ShowWindow(dialog,SW_HIDE);
        // Deliberately put the menu below the output; repair only that relation.
        raise(output);
        require(order.maintain(output,controls,500)==OutputWindowOrder::Result::Restored&&above(controls,output)&&order.repairs()==1,"Menu order repair changed output");
        // Repeated game activation must never close the viewer's 3D screen.
        for(ULONGLONG now:{600ULL,700ULL}){raise(app);require(order.maintain(output,controls,now)==OutputWindowOrder::Result::Restored,"Ordinary recovery failed");}
        raise(app);before=positionChanges;
        require(order.maintain(output,controls,800)==OutputWindowOrder::Result::Restored&&above(output,app),"Repeated game activation stopped restoring stereo");
        for(unsigned i=0;i<100;++i){raise(app);require(order.maintain(output,controls,900+i)==OutputWindowOrder::Result::Restored&&above(output,app)&&GetForegroundWindow()==foreground,"Game activation hid output or stole input focus");}
        require(order.maintain(output,controls,3000)==OutputWindowOrder::Result::Unchanged,"Stable output kept reordering after activation");
        order.reset();require(order.repairs()==0,"New output retained old contention state");
        ShowWindow(output,SW_HIDE);raise(app);before=positionChanges;
        require(order.maintain(output,controls,3100)==OutputWindowOrder::Result::Unchanged&&positionChanges==before,"Hidden game output was raised");
        ShowWindow(output,SW_SHOWNOACTIVATE);ShowWindow(controls,SW_HIDE);
        require(order.maintain(output,controls,3200)==OutputWindowOrder::Result::Restored&&!IsWindowVisible(controls),"Hidden controls were reopened");
        require(SetWindowPos(output,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=FALSE,"Demote output fixture");
        require(order.maintain(output,nullptr,6000)==OutputWindowOrder::Result::Restored&&(GetWindowLongPtrW(output,GWL_EXSTYLE)&WS_EX_TOPMOST),"Lost topmost style was not restored");
        require(order.maintain(nullptr,controls)==OutputWindowOrder::Result::Unchanged,"Stopped output mutated windows");
        // A focused game can be a pixel short of fullscreen or a smaller window.
        // Full-cover-only detection previously missed both cases entirely.
        order.reset();
        require(SetWindowPos(app,HWND_TOPMOST,1,1,798,598,SWP_NOACTIVATE)!=FALSE,"Inset game fixture");
        require(order.maintain(output,nullptr,6100,app)==OutputWindowOrder::Result::Restored&&above(output,app),"Inset foreground game stayed above output");
        require(SetWindowPos(app,HWND_TOPMOST,50,50,100,100,SWP_NOACTIVATE)!=FALSE,"Small game fixture");
        require(order.maintain(output,nullptr,6200,app)==OutputWindowOrder::Result::Restored&&above(output,app),"Partly overlapping foreground game stayed above output");
        require(SetWindowPos(app,HWND_TOPMOST,2000,0,800,600,SWP_NOACTIVATE)!=FALSE,"Other display fixture");
        require(order.maintain(output,nullptr,6300,app)==OutputWindowOrder::Result::Unchanged,"Foreground on another monitor reordered output");
        before=positionChanges;const HWND focusBefore=GetForegroundWindow();
        for(int i=0;i<3;++i){
            require(configureOutputOverlay(output),"Configure opaque output overlay");
            require(GetLayeredWindowAttributes(output,&key,&alpha,&flags)&&alpha==255&&flags==LWA_ALPHA,"Output allowed the underlying game through black slots");
            require(configureOutputOverlay(output),"Reapply overlay presentation policy");
            require(positionChanges==before&&GetForegroundWindow()==focusBefore&&GetWindowLongPtrW(output,GWL_EXSTYLE)==ex,"Composition change disturbed focus, z-order or passthrough");
        }
        // Reproduce the three-window stack used by display streaming. Repairing
        // only output left the game's SBS image above the 2D mirror in captures.
        order.reset();
        require(SetWindowPos(app,HWND_TOPMOST,0,0,800,600,SWP_NOACTIVATE)!=FALSE,"Restore game fixture");
        HWND mirror=make(ex,0,0,800,600);
        require(SetLayeredWindowAttributes(mirror,0,255,LWA_ALPHA)!=FALSE,"Click-through stream mirror");
        require(order.maintain(output,nullptr,7000,app,mirror)==OutputWindowOrder::Result::Restored,"Stream stack was not repaired");
        require(above(output,mirror)&&above(mirror,app),"Expected stereo / mirror / game order");
        before=positionChanges;
        for(int i=0;i<100;++i)require(order.maintain(output,nullptr,7100,app,mirror)==OutputWindowOrder::Result::Unchanged,"Stable stream stack kept reordering");
        require(positionChanges==before,"Stream mirror caused topmost churn");
        // The game comes above both windows; both outputs must be restored.
        raise(app);
        require(order.maintain(output,nullptr,7200,app,mirror)==OutputWindowOrder::Result::Restored&&above(output,mirror)&&above(mirror,app),"Game activation covered the stream after stereo recovery");
        // Also recover when only the stream is covered, with stereo still on top.
        require(SetWindowPos(app,output,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)!=FALSE,"Place game between stereo and stream");
        require(order.maintain(output,nullptr,7300,app,mirror)==OutputWindowOrder::Result::Restored&&above(output,mirror)&&above(mirror,app),"Covered stream was ignored while stereo stayed visible");
        order.reset();raise(mirror);
        require(order.maintain(output,nullptr,7400,app,mirror)==OutputWindowOrder::Result::Restored&&above(output,mirror),"Own mirror mistaken for a competing game");
        require(GetForegroundWindow()==focusBefore,"Stream repair stole input focus");
        // Changing focus or minimizing the captured game must not hide either
        // output or drag stereo to the minimized window's offscreen rectangle.
        const RECT displayArea{0,0,800,600};
        keepOutputOnDisplay(output,displayArea);RECT retained{};GetWindowRect(output,&retained);
        SetForegroundWindow(otherMonitor);
        keepOutputOnDisplay(output,displayArea);
        require(IsWindowVisible(output)&&IsWindowVisible(mirror),"Focus loss hid stereo or stream");
        ShowWindow(app,SW_MINIMIZE);keepOutputOnDisplay(output,displayArea);
        RECT minimized{};GetWindowRect(output,&minimized);
        require(EqualRect(&retained,&minimized)&&IsWindowVisible(output)&&IsWindowVisible(mirror),"Minimizing game moved or hid output");
        ShowWindow(app,SW_HIDE);keepOutputOnDisplay(output,displayArea);
        GetWindowRect(output,&minimized);
        require(EqualRect(&retained,&minimized)&&IsWindowVisible(output)&&IsWindowVisible(mirror),"Hidden game moved or hid output");
        std::cout<<"PASS: repeated game activation keeps stereo/mirror on top, stable stack, unchanged input focus, opaque click-through output, independent focus/minimize/placement, menu/dialog order\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';result=1;}
    for(auto it=windows.rbegin();it!=windows.rend();++it)DestroyWindow(*it);
    SetThreadDesktop(original);if(desktop)CloseDesktop(desktop);
    return result;
}
