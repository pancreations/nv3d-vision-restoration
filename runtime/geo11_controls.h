// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "geo11_control_protocol.h"
#include <intrin.h>
#include <sstream>

namespace vision::geo11::control {
inline decltype(&GetAsyncKeyState) realKey=nullptr;
inline Controls* shared=nullptr;
inline uintptr_t rendererBegin=0,rendererEnd=0;
inline int32_t request=0;
inline ULONGLONG pulseUntil=0;
inline SRWLOCK stateLock=SRWLOCK_INIT;
inline bool reloadKey(const std::filesystem::path& ini,uint32_t& key,uint32_t& modifiers){
    wchar_t value[256]{};GetPrivateProfileStringW(L"Hunting",L"reload_config",L"no_modifiers VK_F10",value,256,ini.c_str());
    std::wstring binding=value;binding=binding.substr(0,binding.find_first_of(L";#"));
    std::wistringstream words(binding);std::wstring token;key=modifiers=0;
    while(words>>token){
        for(auto& c:token)c=wchar_t(towupper(c));
        if(token==L"CTRL")modifiers|=1;else if(token==L"SHIFT")modifiers|=2;else if(token==L"ALT")modifiers|=4;
        else if(token==L"NO_MODIFIERS"||token==L"NO_CTRL"||token==L"NO_SHIFT"||token==L"NO_ALT")continue;
        else{
            if(key)return false;if(token.starts_with(L"VK_"))token.erase(0,3);
            if(token.size()==1&&((token[0]>=L'A'&&token[0]<=L'Z')||(token[0]>=L'0'&&token[0]<=L'9')))key=uint32_t(token[0]);
            else if(token.size()>=2&&token[0]==L'F'){
                const auto digits=token.substr(1);if(digits.find_first_not_of(L"0123456789")!=std::wstring::npos)return false;
                const int n=_wtoi(digits.c_str());if(n<1||n>24)return false;key=VK_F1+n-1;
            }else return false;
        }
    }
    return key!=0;
}
inline SHORT WINAPI keyState(int key){
    if(!shared||shared->request==shared->acknowledged)return realKey(key);
    struct Lock {Lock(){AcquireSRWLockExclusive(&stateLock);}~Lock(){ReleaseSRWLockExclusive(&stateLock);}} lock;
    if(!rendererBegin){auto renderer=GetModuleHandleW(L"VisionGeo11.dll");if(!renderer)return realKey(key);
        MODULEINFO info{};if(!K32GetModuleInformation(GetCurrentProcess(),renderer,&info,sizeof(info)))return realKey(key);
        rendererBegin=reinterpret_cast<uintptr_t>(renderer);rendererEnd=rendererBegin+info.SizeOfImage;
    }
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(caller<rendererBegin||caller>=rendererEnd)return realKey(key);
    const auto pending=shared->request;const auto now=GetTickCount64();
    if(pending!=request){request=pending;pulseUntil=now+250;}
    if(now>=pulseUntil){InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->acknowledged),request);return 0;}
    // Only Geo11 sees this virtual chord. No synthetic keyboard input reaches
    // the game, the desktop or the app; the user's own bindings stay intact.
    const auto modifiers=shared->modifiers;
    if(key==int(shared->key)||(key==VK_CONTROL&&(modifiers&1))||(key==VK_SHIFT&&(modifiers&2))||(key==VK_MENU&&(modifiers&4)))return SHORT(-32768);
    return 0;
}
inline void initialize(const std::filesystem::path& directory){
    uint32_t key=0,modifiers=0;if(!reloadKey(directory/L"d3dx.ini",key,modifiers))return;
    auto entry=GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetAsyncKeyState");if(!entry)return;
    if(MH_CreateHook(reinterpret_cast<void*>(entry),reinterpret_cast<void*>(keyState),reinterpret_cast<void**>(&realKey))!=MH_OK)return;
    const auto name=controlsName(GetCurrentProcessId());auto mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Controls),name.c_str());
    auto* view=mapping?static_cast<Controls*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Controls))):nullptr;
    if(!view){if(mapping)CloseHandle(mapping);return;}
    *view={controlsMagic,1,GetCurrentProcessId(),key,modifiers,0,0};shared=view;
    if(MH_EnableHook(reinterpret_cast<void*>(entry))!=MH_OK){shared=nullptr;UnmapViewOfFile(view);CloseHandle(mapping);return;}
    // Process-lifetime hook: Windows releases the mapping at process exit.
    // No static destructor can invalidate it while Geo11 still polls keys.
}
}
