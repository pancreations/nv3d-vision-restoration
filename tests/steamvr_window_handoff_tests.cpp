#include "steamvr_window_handoff.h"
#include <iostream>
#include <stdexcept>
#include <atomic>

using namespace vision;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

// Keep a real, independently queued window thread paused until the test has
// enqueued its moves. Same-thread fixtures cannot exercise SWP_ASYNCWINDOWPOS.
struct AsyncWindowFixture {
    HDESK desktop=nullptr;HWND window=nullptr;
    HANDLE ready=nullptr,resume=nullptr,stop=nullptr,worker=nullptr;
    std::atomic<unsigned> positionChanges{0};
    static LRESULT CALLBACK windowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
        if(message==WM_NCCREATE)SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        const auto result=DefWindowProcW(hwnd,message,wparam,lparam);
        if(message==WM_WINDOWPOSCHANGED){
            auto* self=reinterpret_cast<AsyncWindowFixture*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
            if(self)self->positionChanges.fetch_add(1,std::memory_order_release);
        }
        return result;
    }
    static DWORD WINAPI run(void* context){
        auto& self=*static_cast<AsyncWindowFixture*>(context);
        if(SetThreadDesktop(self.desktop)){
            self.window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOPMOST,L"VisionSteamVrAsyncFixture",L"WibbleWobble",
                WS_POPUP,20,30,320,180,nullptr,nullptr,GetModuleHandleW(nullptr),&self);
            if(self.window)ShowWindow(self.window,SW_SHOWNOACTIVATE);
        }
        SetEvent(self.ready);
        if(!self.window)return 0;
        HANDLE waits[]{self.stop,self.resume};
        if(WaitForMultipleObjects(2,waits,FALSE,INFINITE)==WAIT_OBJECT_0+1){
            bool running=true;
            while(running){
                const DWORD wait=MsgWaitForMultipleObjects(1,&self.stop,FALSE,INFINITE,QS_ALLINPUT);
                if(wait==WAIT_OBJECT_0||wait==WAIT_FAILED)break;
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                    if(message.message==WM_QUIT){running=false;break;}
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
            }
        }
        DestroyWindow(self.window);return 0;
    }
    void start(HDESK target){
        desktop=target;
        ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        require(ready&&resume&&stop,"Create cross-thread fixture events");
        worker=CreateThread(nullptr,0,run,this,0,nullptr);
        require(worker&&WaitForSingleObject(ready,5000)==WAIT_OBJECT_0&&window,"Create cross-thread handoff fixture");
    }
    void pump(){require(SetEvent(resume)!=FALSE,"Resume cross-thread window pump");}
    void waitForPosition(const RECT& expected,unsigned minimumChanges){
        // A posted WM_APP checkpoint may be delivered before User32 handles
        // an asynchronous SetWindowPos. Observe completed position messages
        // and the requested geometry instead of assuming queue ordering.
        const ULONGLONG deadline=GetTickCount64()+2000;
        do{
            RECT actual{};
            if(positionChanges.load(std::memory_order_acquire)>=minimumChanges&&
               GetWindowRect(window,&actual)&&EqualRect(&actual,&expected))return;
            Sleep(1);
        }while(GetTickCount64()<deadline);
        require(false,"Cross-thread position did not reach the requested geometry after completed moves");
    }
    ~AsyncWindowFixture(){
        if(stop)SetEvent(stop);
        if(worker){WaitForSingleObject(worker,INFINITE);CloseHandle(worker);}
        for(HANDLE event:{ready,resume,stop})if(event)CloseHandle(event);
    }
};
static bool hasPositionLease(HWND window){
    bool found=false;
    EnumPropsExW(window,[](HWND,LPWSTR name,HANDLE,ULONG_PTR context)->int{
        if(!IS_INTRESOURCE(name)&&std::wstring_view(name).starts_with(L"VisionRestoration.PositionLease."))
            *reinterpret_cast<bool*>(context)=true;
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
int main(){
    const HDESK originalDesktop=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=nullptr;std::vector<HWND> windows;int result=0;
    try{
        require(isWibbleOutputWindow(L"WWWindowClass",L"C:\\SteamVR\\vrserver.exe"),"Known Wibble runtime window rejected");
        require(isWibbleOutputWindow(L"wwwindowclass",L"C:/Apps/WIBBLEWOBBLE.EXE"),"Known process/class case variant rejected");
        require(!isWibbleOutputWindow(L"WibbleWobble",L"vrserver.exe"),"Window title accepted as a class");
        require(!isWibbleOutputWindow(L"WWWindowClass",L"game.exe"),"Unrelated executable accepted");
        require(!isWibbleOutputWindow(L"WWWindowClass",L"fakevrserver.exe"),"Process suffix accepted instead of exact basename");
        require(!isWibbleOutputWindow(L"VRto3D_PresentWindow",L"vrserver.exe"),"Unsupported presenter class accepted");
        require(!isWibbleOutputWindow(L"",L"vrserver.exe")&&!isWibbleOutputWindow(L"WWWindowClass",L""),"Missing identity accepted");
        const RECT monitorUnion{-1920,-1080,3840,2160},original{20,30,340,210};
        const RECT parked=wibbleParkingRect(original,monitorUnion);RECT overlap{};
        require(!IntersectRect(&overlap,&parked,&monitorUnion)&&parked.left==monitorUnion.right+64&&
            parked.right-parked.left==320&&parked.bottom-parked.top==180,"Parking changed size or overlaps virtual desktop");

        // All position tests run on an isolated desktop. The production
        // predicate sees this test executable, never an injected fake process.
        const auto name=L"VisionSteamVrHandoffTest-"+std::to_wstring(GetCurrentProcessId());
        desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        require(desktop&&SetThreadDesktop(desktop),"Create isolated handoff test desktop");
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=DefWindowProcW;cls.lpszClassName=L"WWWindowClass";
        require(RegisterClassW(&cls)!=0,"Register handoff fixture class");
        const auto make=[&](bool visible){
            HWND window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOPMOST,cls.lpszClassName,L"WibbleWobble",WS_POPUP,20,30,320,180,nullptr,nullptr,cls.hInstance,nullptr);
            require(window!=nullptr,"Create position fixture");windows.push_back(window);
            if(visible)ShowWindow(window,SW_SHOWNOACTIVATE);return window;
        };
        HWND visible=make(true),hidden=make(false),minimized=make(true);
        ShowWindow(minimized,SW_SHOWMINNOACTIVE);require(IsIconic(minimized),"Minimized fixture did not minimize");
        const HWND foreground=GetForegroundWindow(),active=GetActiveWindow();
        const auto style=GetWindowLongPtrW(visible,GWL_STYLE),extended=GetWindowLongPtrW(visible,GWL_EXSTYLE);
        require(!isWibbleOutputWindow(visible),"Production predicate accepted the unrelated test process");
        SteamVrWindowHandoff handoff;handoff.update(true,0);handoff.update(true,50);handoff.update(true,100);
        RECT current{};GetWindowRect(visible,&current);
        require(EqualRect(&current,&original)&&handoff.parkedCount()==0,"Production guard moved an unrelated process");
        handoff.update(false,101);
        WindowPositionLease lease;
        require(!lease.park(hidden,monitorUnion)&&!lease.park(minimized,monitorUnion)&&lease.parkedCount()==0,"Lease adopted a hidden or minimized window");
        require(lease.park(visible,monitorUnion)&&lease.parkedCount()==1,"Visible window was not parked");
        GetWindowRect(visible,&current);require(EqualRect(&current,&parked)&&IsWindowVisible(visible),"Parking changed visibility or dimensions");
        // Wibble's periodic loop reasserts topmost + SHOWWINDOW but does not
        // move or resize. Its output must remain outside the desktop afterward.
        require(SetWindowPos(visible,HWND_TOPMOST,0,0,0,0,SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW),"Reassert Wibble window visibility");
        require(lease.park(visible,monitorUnion)&&lease.parkedCount()==1,"Repeated scan duplicated ownership");
        GetWindowRect(visible,&current);require(EqualRect(&current,&parked)&&!IntersectRect(&overlap,&current,&monitorUnion),"Topmost SHOWWINDOW brought the presenter onto the desktop");
        require(GetWindowLongPtrW(visible,GWL_STYLE)==style&&GetWindowLongPtrW(visible,GWL_EXSTYLE)==extended,"Parking changed styles or topmost state");
        require(GetForegroundWindow()==foreground&&GetActiveWindow()==active,"Parking activated a window");
        lease.restore();GetWindowRect(visible,&current);
        require(EqualRect(&current,&original)&&IsWindowVisible(visible)&&lease.parkedCount()==0,"Release did not restore original placement");
        require(!IsWindowVisible(hidden)&&IsIconic(minimized),"Release changed hidden or minimized windows");
        require(GetForegroundWindow()==foreground&&GetActiveWindow()==active,"Restoration activated a window");
        {WindowPositionLease scoped;require(scoped.park(visible,monitorUnion),"Scoped parking failed");}
        GetWindowRect(visible,&current);require(EqualRect(&current,&original),"Scoped lease failed to restore on destruction");
        require(lease.park(visible,monitorUnion),"Manual-move fixture could not be parked");
        require(SetWindowPos(visible,nullptr,90,100,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE),"Move parked fixture explicitly");
        require(!lease.park(visible,monitorUnion),"Lease overwrote an explicit subsequent move");
        lease.restore();GetWindowRect(visible,&current);require(current.left==90&&current.top==100,"Release overwrote an explicit subsequent move");
        require(lease.park(visible,monitorUnion),"Destroyed-window fixture could not be parked");
        DestroyWindow(visible);windows.front()=nullptr;
        require(lease.parkedCount()==0,"Destroyed HWND retained valid ownership");
        HWND replacement=make(true);lease.restore();GetWindowRect(replacement,&current);
        require(EqualRect(&current,&original),"Release moved a replacement window");

        WNDCLASSW asyncClass=cls;asyncClass.lpszClassName=L"VisionSteamVrAsyncFixture";asyncClass.lpfnWndProc=AsyncWindowFixture::windowProc;
        require(RegisterClassW(&asyncClass)!=0,"Register asynchronous handoff fixture class");
        {
            AsyncWindowFixture async;async.start(desktop);
            WindowPositionLease queued;
            unsigned changes=async.positionChanges.load(std::memory_order_acquire);
            require(queued.park(async.window,monitorUnion),"Queue cross-thread parking");
            GetWindowRect(async.window,&current);
            require(EqualRect(&current,&original)&&queued.parkedCount()==0&&hasPositionLease(async.window),"Paused owner thread did not retain a pending asynchronous move");
            // Stop immediately, before the owning thread sees the park. Both
            // queued operations must finish in order and leave no stale cookie.
            queued.restore();require(!hasPositionLease(async.window),"Immediate restoration retained a lease cookie");
            async.pump();async.waitForPosition(original,changes+2);GetWindowRect(async.window,&current);
            require(EqualRect(&current,&original)&&queued.parkedCount()==0,"Immediate restore lost a race with queued cross-thread parking");

            changes=async.positionChanges.load(std::memory_order_acquire);
            require(queued.park(async.window,monitorUnion),"Queue running-thread parking");
            async.waitForPosition(parked,changes+1);GetWindowRect(async.window,&current);
            require(EqualRect(&current,&parked)&&queued.parkedCount()==1&&IsWindowVisible(async.window),"Cross-thread parking did not complete at the requested size");
            changes=async.positionChanges.load(std::memory_order_acquire);
            queued.restore();async.waitForPosition(original,changes+1);GetWindowRect(async.window,&current);
            require(EqualRect(&current,&original)&&!hasPositionLease(async.window),"Cross-thread release did not restore placement and remove ownership");
            {
                changes=async.positionChanges.load(std::memory_order_acquire);
                WindowPositionLease scoped;require(scoped.park(async.window,monitorUnion),"Queue scoped cross-thread parking");
                async.waitForPosition(parked,changes+1);
                require(scoped.parkedCount()==1,"Scoped cross-thread parking did not complete");
                changes=async.positionChanges.load(std::memory_order_acquire);
            }
            async.waitForPosition(original,changes+1);GetWindowRect(async.window,&current);
            require(EqualRect(&current,&original)&&!hasPositionLease(async.window),"Cross-thread destruction failed to restore placement");
        }
        std::cout<<"PASS: exact Wibble identity and reversible offscreen placement on an isolated desktop\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    for(HWND window:windows)if(window)DestroyWindow(window);
    SetThreadDesktop(originalDesktop);if(desktop)CloseDesktop(desktop);return result;
}
