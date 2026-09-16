// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include "sync_protocol.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
int main(int argc,char** argv){
    wchar_t name[128]{};DWORD bytes=0;auto desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    if(desktop){GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&bytes);CloseDesktop(desktop);}
    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
    std::printf("Input desktop=%ls foreground PID=%lu\n",name,foreground);
    auto mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,vision::sync::mappingName);if(!mapping){std::puts("Host absent");return 1;}
    auto* shared=static_cast<const vision::sync::Shared*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(vision::sync::Shared)));if(!shared){CloseHandle(mapping);return 1;}
    vision::sync::Shared copy{};bool valid=false;
    for(int n=0;n<100;++n){auto seq=shared->seq;if(seq&1)continue;MemoryBarrier();memcpy(&copy,shared,sizeof(copy));MemoryBarrier();if(seq==shared->seq){valid=true;break;}}
    if(!valid){UnmapViewOfFile(shared);CloseHandle(mapping);return 2;}
    LARGE_INTEGER now,f;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&f);
    std::printf("Host PID=%u enabled=%d state=%d age=%.3fs sequence=%d\n",copy.hostPid,copy.enabled,copy.hostState,double(now.QuadPart-copy.hostHeartbeatQpc)/f.QuadPart,copy.sequence);
    std::printf("Game PID=%u HWND=%llx active=%u size=%ux%u source frames=%llu physical presents=%llu errors=%llu\n",copy.gamePid,copy.hwnd,copy.active,copy.width,copy.height,copy.frames,copy.nativePresents,copy.presentErrors);
    std::printf("Timing valid=%u composed=%u present=%llu refresh=%llu sync refresh=%llu\nStatus: %.128s\n",copy.statsValid,copy.composed,copy.presentCount,copy.presentRefreshCount,copy.syncRefreshCount,copy.message);
    std::printf("Output flags=0x%x; fixed refresh slots=%s\n",copy.hookFlags,(copy.hookFlags&vision::sync::HookRefreshSlots)?"yes":"no");
    if(argc==3&&std::strcmp(argv[1],"--monitor")==0){
        const int seconds=std::atoi(argv[2]);if(seconds<1||seconds>60){UnmapViewOfFile(shared);CloseHandle(mapping);return 3;}
        uint64_t observed=0,wrong=0,unmapped=0,off=0,last=0,report=GetTickCount64();uint32_t pid=copy.gamePid;
        const auto end=GetTickCount64()+uint64_t(seconds)*1000;
        while(GetTickCount64()<end){
            bool stable=false;for(int n=0;n<16;++n){const auto seq=shared->seq;if(seq&1)continue;MemoryBarrier();memcpy(&copy,shared,sizeof(copy));MemoryBarrier();if(seq==shared->seq){stable=true;break;}}
            if(stable){
                if(pid!=copy.gamePid){pid=copy.gamePid;last=0;}
                if(copy.statsValid&&copy.presentCount!=last){
                    last=copy.presentCount;
                    if(!copy.active)++off;
                    else if((copy.hookFlags&vision::sync::HookRefreshSlots)&&vision::sync::sequenceKnown(copy.sequence)){
                        int slot=-1;for(uint32_t k=0;k<copy.ringHead&&k<vision::sync::ringSize;++k){const auto r=copy.ring[(copy.ringHead-1-k)%vision::sync::ringSize];if(r.presentId==uint32_t(last)){slot=r.eye;break;}}
                        if(slot<0)++unmapped;else{++observed;if(uint32_t(slot)!=copy.presentRefreshCount%vision::sync::slotsPerFrame(copy.sequence))++wrong;}
                    }
                }
            }
            if(GetTickCount64()-report>=2000){std::printf("Observed scanouts=%llu wrong-slot=%llu unrecorded=%llu inactive=%llu\n",observed,wrong,unmapped,off);std::fflush(stdout);report=GetTickCount64();}
            Sleep(2);
        }
        std::printf("Monitor result: observed=%llu wrong-slot=%llu unrecorded=%llu inactive=%llu (sampled timing, not optical validation)\n",observed,wrong,unmapped,off);
    }
    UnmapViewOfFile(shared);CloseHandle(mapping);
    return 0;
}
