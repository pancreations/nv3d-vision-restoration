#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace vision {
inline bool isWibbleOutputWindow(std::wstring_view className,std::wstring_view executable){
    const auto equal=[](std::wstring_view a,std::wstring_view b){
        if(a.size()!=b.size())return false;
        const auto lower=[](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c-L'A'+L'a'):c;};
        for(size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;
        return true;
    };
    const auto separator=executable.find_last_of(L"\\/");
    if(separator!=std::wstring_view::npos)executable.remove_prefix(separator+1);
    return equal(className,L"WWWindowClass")&&
        (equal(executable,L"vrserver.exe")||equal(executable,L"WibbleWobble.exe"));
}
inline bool isWibbleOutputWindow(HWND window){
    wchar_t className[256]{};
    if(!GetClassNameW(window,className,int(std::size(className)))||
       !isWibbleOutputWindow(className,L"vrserver.exe"))return false;
    DWORD pid=0;if(!GetWindowThreadProcessId(window,&pid)||!pid)return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return false;
    wchar_t path[32768]{};DWORD length=DWORD(std::size(path));
    const bool queried=QueryFullProcessImageNameW(process,0,path,&length)!=FALSE;
    CloseHandle(process);
    return queried&&isWibbleOutputWindow(className,std::wstring_view(path,length));
}
inline RECT wibbleParkingRect(const RECT& original,const RECT& desktop){
    const LONG left=desktop.right+64;
    return {left,original.top,left+(original.right-original.left),original.bottom};
}
inline RECT virtualDesktopRect(){
    const LONG left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {left,top,left+GetSystemMetrics(SM_CXVIRTUALSCREEN),top+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

// Temporarily moves a presentation window outside the desktop. Keeping it
// visible lets its rendering and focus loop continue without covering output.
// The cookie disappears on destruction, protecting even same-thread HWND reuse.
class WindowPositionLease {
    struct Entry {HWND window;DWORD pid,thread;std::wstring className;RECT original,parked;bool parkedSeen=false,relinquished=false;};
    std::vector<Entry> entries_;
    const std::wstring property_=L"VisionRestoration.PositionLease."+
        std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
    HANDLE cookie()const{return reinterpret_cast<HANDLE>(const_cast<WindowPositionLease*>(this));}
    bool owns(const Entry& entry)const{
        DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(entry.window,&pid);
        wchar_t className[256]{};
        return thread==entry.thread&&pid==entry.pid&&
            GetClassNameW(entry.window,className,int(std::size(className)))&&entry.className==className&&
            GetPropW(entry.window,property_.c_str())==cookie();
    }
    static bool samePosition(const RECT& a,const RECT& b){return a.left==b.left&&a.top==b.top;}
    static bool move(HWND window,const RECT& rect,bool restoreSize){
        return SetWindowPos(window,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,
            SWP_ASYNCWINDOWPOS|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER|(restoreSize?0:SWP_NOSIZE))!=FALSE;
    }
public:
    WindowPositionLease()=default;
    WindowPositionLease(const WindowPositionLease&)=delete;
    WindowPositionLease& operator=(const WindowPositionLease&)=delete;
    ~WindowPositionLease(){restore();}
    bool park(HWND window,const RECT& desktop=virtualDesktopRect()){
        std::erase_if(entries_,[&](const Entry& entry){return !owns(entry);});
        auto found=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.window==window;});
        if(found!=entries_.end()){
            if(found->relinquished)return false;
            RECT current{};if(!GetWindowRect(window,&current))return false;
            if(samePosition(current,found->parked)){found->parkedSeen=true;return true;}
            if(!found->parkedSeen&&samePosition(current,found->original))return true; // queued async move
            found->relinquished=true;return false; // respect a subsequent user/application move
        }
        if(!IsWindowVisible(window)||IsIconic(window))return false;
        DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(window,&pid);
        wchar_t className[256]{};RECT original{};
        if(!thread||!pid||!GetClassNameW(window,className,int(std::size(className)))||!GetWindowRect(window,&original))return false;
        entries_.push_back({window,pid,thread,className,original,wibbleParkingRect(original,desktop)});
        if(!SetPropW(window,property_.c_str(),cookie())){entries_.pop_back();return false;}
        if(!owns(entries_.back())||!move(window,entries_.back().parked,false)){
            if(GetPropW(window,property_.c_str())==cookie())RemovePropW(window,property_.c_str());
            entries_.pop_back();return false;
        }
        RECT current{};if(GetWindowRect(window,&current)&&samePosition(current,entries_.back().parked))entries_.back().parkedSeen=true;
        return true;
    }
    void restore(){
        for(const auto& entry:entries_)if(owns(entry)){
            RECT current{};
            if(!entry.relinquished&&!IsIconic(entry.window)&&GetWindowRect(entry.window,&current)&&
               (samePosition(current,entry.parked)||(!entry.parkedSeen&&samePosition(current,entry.original))))
                move(entry.window,entry.original,true);
            RemovePropW(entry.window,property_.c_str());
        }
        entries_.clear();
    }
    size_t parkedCount()const{
        return size_t(std::count_if(entries_.begin(),entries_.end(),[&](const Entry& entry){
            RECT current{};return !entry.relinquished&&owns(entry)&&GetWindowRect(entry.window,&current)&&samePosition(current,entry.parked);
        }));
    }
};

class SteamVrWindowHandoff {
    WindowPositionLease lease_;
    ULONGLONG lastScan_=0;bool scanned_=false;
public:
    void update(bool active,ULONGLONG now=GetTickCount64()){
        if(!active){restore();return;}
        if(scanned_&&now-lastScan_<100)return;
        scanned_=true;lastScan_=now;
        EnumWindows([](HWND window,LPARAM context)->BOOL{
            if(IsWindowVisible(window)&&!IsIconic(window)&&isWibbleOutputWindow(window))
                reinterpret_cast<SteamVrWindowHandoff*>(context)->lease_.park(window);
            return TRUE;
        },reinterpret_cast<LPARAM>(this));
    }
    void restore(){lease_.restore();scanned_=false;}
    size_t parkedCount()const{return lease_.parkedCount();}
};
}
