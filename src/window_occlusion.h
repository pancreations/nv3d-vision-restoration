#pragma once
#include <windows.h>
#include <string_view>

namespace vision {
// Explorer retains this composition host after Task Switching closes. Its
// inactive HWND bounds can still cover the display without visible content.
// NOREDIRECTIONBITMAP alone does not imply transparency: ordinary applications
// can draw an opaque DirectComposition visual tree with that style.
inline bool isInactiveShellCompositionHost(const wchar_t* className,LONG_PTR extendedStyle,
    DWORD windowProcessId,DWORD shellProcessId,bool foreground){
    return className&&std::wstring_view(className)==L"XamlExplorerHostIslandWindow"&&
        (extendedStyle&WS_EX_NOREDIRECTIONBITMAP)&&shellProcessId&&
        windowProcessId==shellProcessId&&!foreground;
}
inline bool isInactiveShellCompositionHost(HWND window){
    const LONG_PTR extendedStyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
    if(!(extendedStyle&WS_EX_NOREDIRECTIONBITMAP))return false;
    wchar_t className[256]{};GetClassNameW(window,className,256);
    DWORD windowProcessId=0,shellProcessId=0;
    GetWindowThreadProcessId(window,&windowProcessId);GetWindowThreadProcessId(GetShellWindow(),&shellProcessId);
    const HWND foreground=GetForegroundWindow();
    return isInactiveShellCompositionHost(className,extendedStyle,windowProcessId,shellProcessId,
        foreground==window||(foreground&&GetAncestor(foreground,GA_ROOT)==window));
}
}
