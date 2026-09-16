// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamesync.h"
#include "platform.h"
#include "sync_protocol.h"
#include <avrt.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
namespace vision {
namespace {
double qpcFrequency(){static double f=[]{LARGE_INTEGER v;QueryPerformanceFrequency(&v);return double(v.QuadPart);}();return f;}
LONGLONG qpcRaw(){LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
bool snapshot(const sync::Shared* shared,sync::Shared& out){
    for(int attempt=0;attempt<16;attempt++){
        uint32_t s1=shared->seq;if(s1&1){YieldProcessor();continue;}
        MemoryBarrier();memcpy(&out,shared,sizeof(out));MemoryBarrier();
        if(shared->seq==s1)return true;
    }
    return false;
}
}
void GameSync::start(){if(worker_.joinable())return;worker_=std::jthread([this](std::stop_token stop){run(stop);});}
void GameSync::stop(){if(worker_.joinable()){worker_.request_stop();worker_.join();}}
void GameSync::enable(bool on){std::lock_guard lock(mutex_);enabled_=on;++generation_;}
void GameSync::configure(const Settings& s,HMONITOR display){std::lock_guard lock(mutex_);settings_=s;monitor_=display;++generation_;}
GameSyncStatus GameSync::status()const{std::lock_guard lock(mutex_);return status_;}
void GameSync::run(std::stop_token stop){
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(sync::Shared),sync::mappingName);
    auto* shared=mapping?static_cast<sync::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(sync::Shared))):nullptr;
    HANDLE frameEvent=CreateEventW(nullptr,FALSE,FALSE,sync::frameEventName);
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!shared||!frameEvent){std::lock_guard lock(mutex_);status_.message="Game hook channel could not be created";if(shared)UnmapViewOfFile(shared);if(mapping)CloseHandle(mapping);if(frameEvent)CloseHandle(frameEvent);if(timer)CloseHandle(timer);if(mmcss)AvRevertMmThreadCharacteristics(mmcss);return;}
    if(shared->magic!=sync::magic||shared->version!=sync::version){memset(shared,0,sizeof(*shared));shared->magic=sync::magic;shared->version=sync::version;}
    shared->hostPid=GetCurrentProcessId();
    GameSyncStatus local;local.hosting=true;local.message="Waiting for a hooked game";
    TimingTracker clock;uint64_t nextRefresh=0,lastRefresh=0,lastObserved=0,generation=0;double lastTarget=0,lastPublish=0,lastConfigure=-1;bool driving=false,configured=false;uint32_t seenPid=0;Sequence configuredSequence=Sequence::Alternating;
    int lockedPhase=-1,candidatePhase=-1,phaseVotes=0;unsigned phaseSlots=0;
    const double lead=0.0015,minimum=0.0005;
    while(!stop.stop_requested()){
        Settings settings;bool enabled;uint64_t gen;HMONITOR monitor;{std::lock_guard lock(mutex_);settings=settings_;enabled=enabled_;gen=generation_;monitor=monitor_;}
        const Sequence sequenceRequested=settings.sequence;
        auto es=emitter_.status();bool emitterReady=es.state==EmitterState::Ready||es.state==EmitterState::Running||es.state==EmitterState::Simulated;
        bool allow=enabled&&emitterReady;
        shared->hostHeartbeatQpc=qpcRaw();shared->enabled=allow?1:0;shared->hostState=allow?sync::HostReady:!enabled?sync::HostPausedByOutput:sync::HostEmitterNotReady;shared->packing=0;shared->swapHalves=0;shared->convergenceMicrounits=int32_t(settings.convergence*1000000);shared->bandHeightMicrounits=int32_t(settings.bandHeight*1000000);shared->bandCenterMicrounits=int32_t(settings.bandCenter*1000000);shared->sequence=int32_t(settings.sequence);shared->hostSeq++;
        sync::Shared snap{};bool valid=snapshot(shared,snap);
        double now=qpc();double age=valid?double(qpcRaw()-snap.gameHeartbeatQpc)/qpcFrequency():1e9;
        bool hooked=valid&&snap.gamePid&&age<1.0;
        if(gen!=generation){generation=gen;configured=false;}
        if(!hooked||!snap.active||!allow){
            if(driving){emitter_.suspend();driving=false;}
            clock.reset();nextRefresh=0;lastObserved=lastRefresh=0;lockedPhase=candidatePhase=-1;phaseVotes=0;
            local.hooked=hooked;local.driving=false;local.frames=hooked?snap.frames:0;local.game=hooked?snap.gameName:"";local.pid=hooked?snap.gamePid:0;local.api=hooked?snap.api:0;local.width=hooked?snap.width:0;local.height=hooked?snap.height:0;local.measuredHz=0;local.samples=0;local.frameAge=hooked?age:0;
            if(!hooked)local.message=enabled?"Waiting for a hooked game (start a game with the hook installed)":"Paused while the test output runs";
            else if(!enabled)local.message=std::string(snap.gameName)+" is hooked; paused while the test output runs";
            else if(!emitterReady)local.message=std::string(snap.gameName)+" is hooked; connect the emitter to drive the glasses";
            else local.message=std::string(snap.gameName)+": "+snap.message;
            if(hooked&&seenPid!=snap.gamePid){seenPid=snap.gamePid;local.triggers=local.misses=local.guesses=0;}
            {std::lock_guard lock(mutex_);status_=local;}
            WaitForSingleObject(frameEvent,100);continue;
        }
        // A hook without sequence support presents one Left/Right pair per game frame whatever the
        // profile says, so the emitter and the slot interpretation follow the hook's capability.
        // An older hook with sequence support knows the three classic codes only; encoded hold/black
        // sequences need HookPatterns as well.
        const bool hookSequences=(snap.hookFlags&sync::HookSequences)!=0&&(int(settings.sequence)<=2||(snap.hookFlags&sync::HookPatterns)!=0);
        if(!hookSequences&&settings.sequence!=Sequence::Alternating)settings.sequence=Sequence::Alternating;
        if(!configured||lastConfigure<0||configuredSequence!=settings.sequence){emitter_.configure(settings);configured=true;lastConfigure=now;configuredSequence=settings.sequence;}
        local.sequenceLimited=!hookSequences&&sequenceRequested!=Sequence::Alternating;
        // A phase-locked hook shows every slot on the refresh its number implies. Once that phase is
        // known the glasses follow the refresh clock alone, so a late game frame changes what the
        // display repeats but never when the emitter switches.
        const bool phaseLocked=hookSequences&&(snap.hookFlags&sync::HookPhaseLocked)!=0;
        const bool refreshSlots=phaseLocked&&(snap.hookFlags&sync::HookRefreshSlots)!=0;
        const unsigned cycle=cycleLength(settings.sequence);
        if(!phaseLocked||cycle!=phaseSlots){lockedPhase=candidatePhase=-1;phaseVotes=0;phaseSlots=cycle;}
        // The new output backend schedules absolute refresh slots. A dropped
        // physical frame must not teach the emitter a new left/right phase.
        if(refreshSlots){lockedPhase=0;candidatePhase=-1;phaseVotes=0;}
        if(snap.statsValid&&snap.syncQpc){
            clock.observe(snap.syncRefreshCount,double(snap.syncQpc)/qpcFrequency());
            if(snap.presentCount!=lastObserved){
                lastObserved=snap.presentCount;lastRefresh=snap.presentRefreshCount;
                if(phaseLocked&&!refreshSlots&&snap.ringHead){
                    int shown=-1;const uint32_t count=std::min<uint32_t>(snap.ringHead,sync::ringSize);
                    for(uint32_t k=0;k<count;k++){auto& r=snap.ring[(snap.ringHead-1-k)%sync::ringSize];if(r.presentId==uint32_t(lastObserved)){shown=r.eye;break;}if(r.presentId<uint32_t(lastObserved))break;}
                    if(shown>=0){
                        const int observed=int((lastRefresh+cycle-uint64_t(unsigned(shown)%cycle))%cycle);
                        if(observed==candidatePhase)++phaseVotes;else{candidatePhase=observed;phaseVotes=1;}
                        // After a slip a few queued slots show off phase before the hook realigns; only a
                        // phase that holds for eight presents in a row replaces the locked one.
                        if(phaseVotes>=(lockedPhase<0?4:8))lockedPhase=candidatePhase;
                    }
                }
            }
        }
        bool composed=snap.composed!=0;
        local.hooked=true;local.game=snap.gameName;local.pid=snap.gamePid;local.api=snap.api;local.width=snap.width;local.height=snap.height;local.frames=snap.frames;local.composed=composed;local.samples=clock.samples;local.measuredHz=clock.period>0?1/clock.period:0;local.frameAge=age;
        local.onDisplay=!monitor||!snap.hwnd||MonitorFromWindow(reinterpret_cast<HWND>(snap.hwnd),MONITOR_DEFAULTTONULL)==monitor;
        if(clock.samples<8||!lastObserved){
            local.driving=false;local.message=std::string(snap.gameName)+": acquiring the game's refresh timing";
            if(now-lastPublish>.1){std::lock_guard lock(mutex_);status_=local;lastPublish=now;}
            WaitForSingleObject(frameEvent,20);continue;
        }
        // DXGI's refresh/present pair already includes composition. Use that same
        // mapping as the presenter; subtracting a refresh selects the opposite eye.
        if(nextRefresh==0||clock.predict(nextRefresh)<now-0.02){nextRefresh=lastRefresh+1;while(clock.predict(nextRefresh)<now+lead)++nextRefresh;}
        double target=clock.predict(nextRefresh);
        if(target-lead>now){
            double wait=std::min(target-lead-now,0.004);
            if(timer&&wait>0.0008){LARGE_INTEGER due;due.QuadPart=-LONGLONG(wait*1e7);SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE);HANDLE handles[2]{frameEvent,timer};WaitForMultipleObjects(2,handles,FALSE,10);}
            else if(wait>0.0002)WaitForSingleObject(frameEvent,1);else YieldProcessor();
            if(now-lastPublish>.1){local.driving=driving;local.message=std::string("Driving the glasses for ")+snap.gameName+(composed?" (DWM composed)":" (direct flip)")+(lockedPhase>=0?", phase-locked to the refresh clock":"")+(local.onDisplay?"":" - game window is not on the 3D display")+(local.sequenceLimited?" - this hook build presents Left/Right only; the profile's four-slot sequence is not applied in the game":"");std::lock_guard lock(mutex_);status_=local;lastPublish=now;}
            continue;
        }
        if(target-now<minimum){local.misses++;++nextRefresh;continue;}
        int slot=-1;bool guessed=false;
        if(lockedPhase>=0)slot=int((nextRefresh+cycle-uint64_t(lockedPhase))%cycle);
        else if(snap.ringHead){
            uint64_t presentId=presentForRefresh(nextRefresh,uint32_t(lastObserved),lastRefresh);
            uint32_t newest=snap.ring[(snap.ringHead-1)%sync::ringSize].presentId;
            if(presentId>newest){slot=snap.ring[(snap.ringHead-1)%sync::ringSize].eye;guessed=true;} // game has not presented that far: assume the last frame repeats
            else{uint32_t count=std::min<uint32_t>(snap.ringHead,sync::ringSize);for(uint32_t k=0;k<count;k++){auto& r=snap.ring[(snap.ringHead-1-k)%sync::ringSize];if(r.presentId==uint32_t(presentId)){slot=r.eye;break;}if(r.presentId<uint32_t(presentId))break;}}
        }
        if(slot<0){local.misses++;++nextRefresh;continue;}
        // The hook records the slot of the sequence each present carries. Black frames and the
        // settling frame of a repeated pair send no command; the emitter free-runs across them.
        // A repeated last frame in a four-slot sequence is left alone for the same reason: a
        // second command one refresh later would drag the emitter's period lock.
        auto sequenced=sequenceSlot(settings.sequence,uint64_t(slot),false);
        if(!sequenced.trigger||sequenced.eye==Eye::Black||(guessed&&cycleLength(settings.sequence)>2)){++nextRefresh;continue;}
        if(guessed)local.guesses++;
        Eye commandEye=sequenced.eye;if(settings.swapEyes)commandEye=commandEye==Eye::Left?Eye::Right:Eye::Left;
        emitter_.submit(commandEye,target);driving=true;local.triggers++;lastTarget=target;++nextRefresh;(void)lastTarget;
    }
    if(driving)emitter_.suspend();
    shared->enabled=0;shared->hostHeartbeatQpc=0;
    UnmapViewOfFile(shared);CloseHandle(mapping);CloseHandle(frameEvent);if(timer)CloseHandle(timer);if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
    std::lock_guard lock(mutex_);status_=GameSyncStatus{};
}
namespace {
bool fileContains(const std::filesystem::path& path,const char* needle){std::ifstream f(path,std::ios::binary);if(!f)return false;std::string data((std::istreambuf_iterator<char>(f)),{});return data.find(needle)!=std::string::npos;}
// Sets key=value in [section] of an INI file and leaves every other line as it was.
void setIniValue(const std::filesystem::path& path,const std::string& section,const std::string& key,const std::string& value){
    std::string data;{std::ifstream in(path,std::ios::binary);data.assign(std::istreambuf_iterator<char>(in),{});}
    const std::string eol=data.find("\r\n")!=std::string::npos?"\r\n":"\n";
    std::vector<std::string> lines;
    for(size_t start=0;start<data.size();){size_t end=data.find('\n',start);if(end==std::string::npos)end=data.size();std::string line=data.substr(start,end-start);if(!line.empty()&&line.back()=='\r')line.pop_back();lines.push_back(line);start=end+1;}
    bool inSection=false,found=false,replaced=false;size_t insertAt=lines.size();
    for(size_t i=0;i<lines.size();++i){
        const std::string& line=lines[i];
        if(!line.empty()&&line[0]=='['){if(inSection)break;inSection=line=="["+section+"]";if(inSection){found=true;insertAt=i+1;}continue;}
        if(!inSection)continue;
        if(line.rfind(key+"=",0)==0){lines[i]=key+"="+value;replaced=true;break;}
        if(!line.empty()&&line[0]!=';')insertAt=i+1;
    }
    if(!replaced){
        if(!found){lines.push_back("["+section+"]");insertAt=lines.size();}
        lines.insert(lines.begin()+std::ptrdiff_t(insertAt),key+"="+value);
    }
    std::ofstream out(path,std::ios::binary);for(const auto& line:lines)out<<line<<eol;
}
}
std::string installGameHook(const std::filesystem::path& gameExe,const std::filesystem::path& addon){
    namespace fs=std::filesystem;std::string log;auto dir=gameExe.parent_path();
    if(!fs::exists(gameExe))throw std::runtime_error("Game executable not found.");
    if(!fs::exists(addon))throw std::runtime_error("Hook not built: "+utf8(addon.wstring()));
    auto dxgi=dir/L"dxgi.dll";fs::path source=L"C:\\ProgramData\\ReShade\\ReShade64.dll";
    auto requireReShade=[&]{if(!fs::exists(source)||!fileContains(source,"ReShadeRegisterAddon"))throw std::runtime_error("ReShade with add-on support is not installed on this PC. Run ReShade_Setup_*_Addon.exe once (it is in Downloads), then retry.");};
    if(fs::exists(dxgi)&&fileContains(dxgi,"ReShadeRegisterAddon")){
        log+="Existing ReShade dxgi.dll kept.\n";
    }else if(fs::exists(dxgi)&&fs::exists(dir/L"OptiScaler.ini")){
        // OptiScaler owns dxgi.dll and loads ReShade itself from ReShade64.dll when LoadReshade=true.
        auto target=dir/L"ReShade64.dll";
        if(fs::exists(target)&&fileContains(target,"ReShadeRegisterAddon"))log+="Existing ReShade64.dll kept.\n";
        else{requireReShade();fs::copy_file(source,target,fs::copy_options::overwrite_existing);log+="Installed ReShade (add-on build) as ReShade64.dll next to OptiScaler.\n";}
        setIniValue(dir/L"OptiScaler.ini","Plugins","LoadReshade","true");log+="Set LoadReshade=true in OptiScaler.ini so OptiScaler loads ReShade.\n";
    }else if(fs::exists(dxgi)){
        throw std::runtime_error("dxgi.dll in the game folder is not ReShade with add-on support. Remove it, or run the ReShade add-on installer for this game first.");
    }else{
        requireReShade();fs::copy_file(source,dxgi,fs::copy_options::overwrite_existing);log+="Installed ReShade (add-on build) as dxgi.dll.\n";
    }
    for(auto* name:{L"d3d11.dll",L"d3d12.dll"})if(fs::exists(dir/name)&&!fileContains(dir/name,"ReShadeRegisterAddon"))log+="Note: "+utf8(std::wstring(name))+" wrapper present (geo-11 or similar); ReShade loads after it.\n";
    fs::copy_file(addon,dir/addon.filename(),fs::copy_options::overwrite_existing);log+="Installed "+utf8(addon.filename().wstring())+".\n";
    // The hook finds depth itself; ReShade's own Generic Depth would only repeat that work.
    auto ini=dir/L"ReShade.ini";if(!fs::exists(ini)){std::ofstream f(ini);f<<"[GENERAL]\nEffectSearchPaths=.\\\nTextureSearchPaths=.\\\nNoReloadOnInit=1\n[OVERLAY]\nShowFPS=0\nTutorialProgress=4\n[ADDON]\nAddonPath=.\\\nDisabledAddons=Generic Depth\n[VISION]\nStereoSource=auto\n";log+="Wrote a minimal ReShade.ini (no effects).\n";}
    log+="Leave this app open and start the game. Direct3D 11 games with side-by-side output (Dolphin, geo-11) are split into eyes; Direct3D 12 games get depth-based 3D from their own depth buffer. ReShade.ini [VISION] StereoSource=packed|depth overrides that.";return log;
}
std::string removeGameHook(const std::filesystem::path& gameExe,const std::filesystem::path& addon){
    namespace fs=std::filesystem;auto dir=gameExe.parent_path();std::error_code ec;
    bool removed=fs::remove(dir/addon.filename(),ec);
    return removed?"Removed the hook from the game folder. ReShade itself (dxgi.dll) was left in place.":"No hook was installed in that folder.";
}
}
