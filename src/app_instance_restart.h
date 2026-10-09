#pragma once
#include <windows.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vision {
inline bool isVisionRestorationExecutable(std::wstring_view path){
    const auto separator=path.find_last_of(L"\\/");
    if(separator!=std::wstring_view::npos)path.remove_prefix(separator+1);
    constexpr std::wstring_view prefix=L"VisionRestoration";
    if(path.size()<prefix.size())return false;
    const auto lower=[](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c-L'A'+L'a'):c;};
    for(size_t i=0;i<prefix.size();++i)if(lower(path[i])!=lower(prefix[i]))return false;
    return true; // A deployed running image may have been renamed before restart.
}

// Used only by the app's explicit restart option before constructing its UI.
// WM_CLOSE lets the existing instance save state, stop output and release USB;
// timeout never terminates a process or attempts any other window operation.
inline void closeVisionRestorationForRestart(HWND existing,DWORD timeoutMs=30000){
    if(timeoutMs>30000)timeoutMs=30000;
    const auto correctClass=[](HWND window){
        wchar_t name[256]{};
        return GetClassNameW(window,name,int(std::size(name)))&&
            std::wstring_view(name)==L"VisionRestorationControl";
    };
    if(!correctClass(existing))throw std::runtime_error("Restart refused: the existing window is not a Vision Restoration control window.");
    DWORD pid=0;const DWORD thread=GetWindowThreadProcessId(existing,&pid);
    if(!thread||!pid||pid==GetCurrentProcessId())throw std::runtime_error("Restart refused: the previous Vision Restoration process could not be identified.");
    struct ProcessHandle {HANDLE value=nullptr;~ProcessHandle(){if(value)CloseHandle(value);}} process;
    process.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process.value)throw std::runtime_error("Cannot restart the existing Vision Restoration instance: Windows denied process access (error "+std::to_string(GetLastError())+"). Relaunch with matching permissions.");
    wchar_t path[32768]{};DWORD length=DWORD(std::size(path));
    if(!QueryFullProcessImageNameW(process.value,0,path,&length))
        throw std::runtime_error("Restart refused: Windows could not verify the existing process executable (error "+std::to_string(GetLastError())+").");
    if(!isVisionRestorationExecutable(std::wstring_view(path,length)))
        throw std::runtime_error("Restart refused: the control window belongs to another application.");
    if(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0)return;
    DWORD currentPid=0;const DWORD currentThread=GetWindowThreadProcessId(existing,&currentPid);
    if(currentPid!=pid||currentThread!=thread||!correctClass(existing))
        throw std::runtime_error("Restart refused: the previous application's control window changed while its identity was being checked.");
    if(!PostMessageW(existing,WM_CLOSE,0,0))
        throw std::runtime_error("Windows blocked Vision Restoration's normal close request (error "+std::to_string(GetLastError())+"). Close the existing instance or relaunch with matching permissions.");
    const DWORD waited=WaitForSingleObject(process.value,timeoutMs);
    if(waited==WAIT_TIMEOUT)
        throw std::runtime_error("The previous Vision Restoration instance did not finish its normal shutdown within "+std::to_string(timeoutMs)+" ms. It has not been force-closed; finish closing it before restarting.");
    if(waited!=WAIT_OBJECT_0)
        throw std::runtime_error("Could not confirm the previous Vision Restoration instance exited (error "+std::to_string(GetLastError())+"). Restart was stopped.");
}
}
