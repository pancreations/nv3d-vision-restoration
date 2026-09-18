// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_compatibility.h"
#include "geo11_output_mode.h"
#include "geo11_control_protocol.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace vision::compatibility {
namespace {
namespace fs=std::filesystem;
std::string read(const fs::path& path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read "+path.string());return {std::istreambuf_iterator<char>(f),{}};}
std::string trim(std::string s){auto ws=[](unsigned char c){return std::isspace(c)!=0;};while(!s.empty()&&ws(s.back()))s.pop_back();auto i=std::find_if_not(s.begin(),s.end(),ws);s.erase(s.begin(),i);return s;}
std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;}
struct Ini {
    std::vector<std::string> lines;std::string eol,bom;bool finalNewline=false;
    explicit Ini(const std::string& original){
        auto text=original;if(text.find('\0')!=std::string::npos)throw std::runtime_error("UTF-16/binary INI is not supported; configuration was left unchanged.");
        if(text.starts_with("\xef\xbb\xbf")){bom=text.substr(0,3);text.erase(0,3);}
        eol=text.find("\r\n")!=std::string::npos?"\r\n":"\n";finalNewline=text.ends_with('\n');
        for(size_t start=0;start<text.size();){auto end=text.find('\n',start);if(end==std::string::npos)end=text.size();auto line=text.substr(start,end-start);if(line.ends_with('\r'))line.pop_back();lines.push_back(line);start=end+1;}
    }
    std::optional<size_t> index(const std::string& section,const std::string& key)const{
        bool inside=false;std::optional<size_t> found;
        for(size_t i=0;i<lines.size();++i){auto line=trim(lines[i]);if(line.empty()||line[0]==';'||line[0]=='#')continue;
            if(line[0]=='['){auto end=line.find(']');inside=end!=std::string::npos&&lower(trim(line.substr(1,end-1)))==lower(section);continue;}
            auto equals=line.find('=');if(inside&&equals!=std::string::npos&&lower(trim(line.substr(0,equals)))==lower(key)){
                if(found)throw std::runtime_error("Ambiguous duplicate ["+section+"] "+key+"; configuration was left unchanged.");found=i;
            }
        }return found;
    }
    std::string get(const std::string& section,const std::string& key)const{auto i=index(section,key);return i?trim(lines[*i].substr(lines[*i].find('=')+1)):"";}
    void set(const std::string& section,const std::string& key,const std::optional<std::string>& raw){
        if(auto i=index(section,key)){if(raw)lines[*i]=*raw;else lines.erase(lines.begin()+*i);return;}
        if(!raw)return;
        for(size_t i=0;i<lines.size();++i){auto line=trim(lines[i]);if(line.starts_with('[')){auto end=line.find(']');if(end!=std::string::npos&&lower(trim(line.substr(1,end-1)))==lower(section)){lines.insert(lines.begin()+i+1,*raw);return;}}}
        if(!lines.empty()&&!lines.back().empty())lines.emplace_back();lines.push_back("["+section+"]");lines.push_back(*raw);finalNewline=true;
    }
    void restore(const Ini& original,const std::string& section,const std::string& key,const std::string& ours){
        if(lower(get(section,key))!=lower(ours))return;auto i=original.index(section,key);set(section,key,i?std::optional(original.lines[*i]):std::nullopt);
    }
    std::string text()const{std::string s=bom;for(size_t i=0;i<lines.size();++i){s+=lines[i];if(i+1<lines.size()||finalNewline)s+=eol;}return s;}
};
void write(const fs::path& file,const std::string& text){std::ofstream f(file,std::ios::binary|std::ios::trunc);if(!f)throw std::runtime_error("Cannot write "+file.string());f.write(text.data(),std::streamsize(text.size()));f.close();if(!f)throw std::runtime_error("Write failed: "+file.string());}
void replace(const fs::path& file,const std::string& text){
    auto temp=file;temp+=L".VisionRestoration.tmp";if(fs::exists(temp))throw std::runtime_error("A previous configuration update is unfinished: "+temp.string());
    write(temp,text);if(!MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){std::error_code ignored;fs::remove(temp,ignored);throw std::runtime_error("Cannot replace configuration; close the game and retry.");}
}
void requireStopped(const fs::path& executable){
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot check whether the game is running.");
    PROCESSENTRY32W p{sizeof(p)};bool running=false;
    if(Process32FirstW(snapshot,&p))do{if(_wcsicmp(p.szExeFile,executable.filename().c_str())!=0)continue;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,p.th32ProcessID);
        if(!process){running=true;break;}wchar_t path[32768];DWORD size=32768;
        if(WaitForSingleObject(process,0)==WAIT_OBJECT_0){CloseHandle(process);continue;}
        if(!QueryFullProcessImageNameW(process,0,path,&size)||_wcsicmp(path,executable.c_str())==0)running=true;CloseHandle(process);if(running)break;
    }while(Process32NextW(snapshot,&p));CloseHandle(snapshot);
    if(running)throw std::runtime_error("Close this game before changing its stereo output connection.");
}
fs::path canonicalExe(const fs::path& p){if(!fs::is_regular_file(p))throw std::runtime_error("Game executable not found.");return fs::canonical(p);}
constexpr char runtimeMarker[]="VisionStereo11 initialized";
constexpr char loaderMarker[]="VisionStereoLoader.v1";
constexpr char recordV1[]="VisionRestoration Geo11 output connection v1\n";
constexpr char recordV2[]="VisionRestoration Geo11 output connection v2\n";
constexpr char recordV3[]="VisionRestoration Geo11 shared hook v3\n";
}
Architecture architecture(const fs::path& executable){
    std::ifstream f(executable,std::ios::binary);IMAGE_DOS_HEADER dos{};f.read(reinterpret_cast<char*>(&dos),sizeof(dos));
    if(!f||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<sizeof(dos)||dos.e_lfanew>16*1024*1024)throw std::runtime_error("Invalid Windows executable header.");
    f.seekg(dos.e_lfanew);DWORD signature=0;IMAGE_FILE_HEADER header{};f.read(reinterpret_cast<char*>(&signature),sizeof(signature));f.read(reinterpret_cast<char*>(&header),sizeof(header));
    if(!f||signature!=IMAGE_NT_SIGNATURE)throw std::runtime_error("Invalid Windows executable signature.");
    if(header.Machine==IMAGE_FILE_MACHINE_I386)return Architecture::X86;if(header.Machine==IMAGE_FILE_MACHINE_AMD64)return Architecture::X64;
    throw std::runtime_error("This stereo backend supports x86 and x64 games.");
}
Inspection inspect(const fs::path& executable){
    auto exe=canonicalExe(executable),dir=exe.parent_path();Inspection info;info.architecture=architecture(exe);
    if(fs::exists(dir/L"d3d11.dll")&&fs::exists(dir/L"d3dx.ini")&&fs::exists(dir/L"d3dxdm.ini")){
        auto renderer=read(dir/L"d3d11.dll");
        if(renderer.find(loaderMarker)!=std::string::npos&&fs::is_regular_file(dir/L"VisionGeo11.dll"))renderer=read(dir/L"VisionGeo11.dll");
        info.geo11=renderer.find("HackerSwapChainKatanga")!=std::string::npos;
        Ini fix(read(dir/L"d3dx.ini")),provider(read(dir/L"d3dxdm.ini"));info.proxy=fix.get("System","proxy_d3d11");info.output=provider.get("Device","direct_mode");
        if(fix.get("Device","force_stereo")!="2")info.description="The installed fix has not enabled Geo11 (force_stereo=2).";
        else if(info.geo11)info.description="Existing Geo11 fix; "+std::string(info.architecture==Architecture::X64?"64-bit":"32-bit")+"; output="+info.output;
    }
    if(!info.geo11&&fs::is_regular_file(dir/L"d3d11.dll")&&fs::is_regular_file(dir/L"d3dx.ini")){
        const auto renderer=read(dir/L"d3d11.dll");info.migoto=renderer.find("3Dmigoto")!=std::string::npos||renderer.find("3DMigoto")!=std::string::npos;
        if(info.migoto)info.description="Existing 3Dmigoto fix. Direct capture can receive its rendered stereo output; a working stereo-rendering backend is still required.";
    }
    if(info.description.empty())info.description="No established Geo11 or 3Dmigoto fix detected. Native stereo can use the shared capture adapter with an explicitly selected output layout.";
    return info;
}
fs::path processExecutable(uint32_t pid){
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return {};
    wchar_t path[32768]{};DWORD length=32768;const bool ok=QueryFullProcessImageNameW(process,0,path,&length)!=FALSE;CloseHandle(process);
    return ok?fs::path(path):fs::path{};
}
Tuning tuning(const fs::path& executable){
    auto exe=canonicalExe(executable);auto info=inspect(exe);if(!info.geo11)throw std::runtime_error(info.description);
    Ini provider(read(exe.parent_path()/L"d3dxdm.ini"));Tuning result;
    auto number=[&](const char* key,float fallback){auto value=provider.get("Stereo",key);if(value.empty())return fallback;size_t end=0;float n=std::stof(value,&end);if(!std::isfinite(n))throw std::runtime_error("Invalid Geo11 tuning value");return n;};
    result.depth=number("dm_separation",50);result.convergence=number("dm_convergence",2);result.autoConvergence=number("dm_auto_convergence",0)!=0;
    return result;
}
std::string setTuning(const fs::path& executable,const Tuning& values){
    if(!std::isfinite(values.depth)||values.depth<0||values.depth>100||!std::isfinite(values.convergence)||values.convergence<0||values.convergence>10000)throw std::runtime_error("Depth must be 0-100% and convergence must be 0-10000.");
    auto exe=canonicalExe(executable);auto current=tuning(exe);auto file=exe.parent_path()/L"d3dxdm.ini";Ini provider(read(file));
    auto format=[](float value){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9)<<value;return out.str();};
    provider.set("Stereo","dm_separation","dm_separation = "+format(values.depth));
    if(!current.autoConvergence)provider.set("Stereo","dm_convergence","dm_convergence = "+format(values.convergence));
    replace(file,provider.text());
    bool sent=false;HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot!=INVALID_HANDLE_VALUE){PROCESSENTRY32W p{sizeof(p)};if(Process32FirstW(snapshot,&p))do{
        if(_wcsicmp(p.szExeFile,exe.filename().c_str())!=0||_wcsicmp(processExecutable(p.th32ProcessID).c_str(),exe.c_str())!=0)continue;
        const auto name=geo11::controlsName(p.th32ProcessID);HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());if(!mapping)continue;
        auto* control=static_cast<geo11::Controls*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(geo11::Controls)));
        if(control&&control->magic==geo11::controlsMagic&&control->version==1&&control->pid==p.th32ProcessID){InterlockedIncrement(reinterpret_cast<volatile LONG*>(&control->request));sent=true;}
        if(control)UnmapViewOfFile(control);CloseHandle(mapping);
    }while(Process32NextW(snapshot,&p));CloseHandle(snapshot);}
    return sent?"Saved game depth; return to the game to apply it. Geo11's reload may briefly pause rendering.":"Saved game depth. It takes effect at the next game launch or with the fix's reload shortcut.";
}
std::string connect(const fs::path& executable,const fs::path& runtimeRoot){
    auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);auto info=inspect(exe);
    if(!info.geo11)throw std::runtime_error(info.description);
    if(geo11::outputMode(info.output)==geo11::OutputMode::Unsupported)throw std::runtime_error("The shared Geo11 hook cannot consume the installed output mode ("+info.output+"). Supported modes are sbs, tab, sbs_reversed, tab_reversed and katanga_vr. The existing fix was left unchanged.");
    auto source=runtimeRoot/(info.architecture==Architecture::X64?L"x64":L"x86")/L"VisionStereo11.dll";
    if(!fs::is_regular_file(source)||architecture(source)!=info.architecture||read(source).find(runtimeMarker)==std::string::npos)throw std::runtime_error("The matching VisionStereo11 runtime is missing or invalid.");
    auto loader=source.parent_path()/L"VisionStereoLoader.dll";
    if(!fs::is_regular_file(loader)||architecture(loader)!=info.architecture||read(loader).find(loaderMarker)==std::string::npos)throw std::runtime_error("The matching early stereo loader is missing or invalid.");
    auto fixPath=dir/L"d3dx.ini",providerPath=dir/L"d3dxdm.ini",runtimePath=dir/L"VisionStereo11.dll",backup=dir/L"VisionRestoration.OutputBackup";
    auto fixBefore=read(fixPath),providerBefore=read(providerPath);Ini fix(fixBefore),provider(providerBefore);
    if(fix.get("Device","force_stereo")!="2")throw std::runtime_error(info.description);
    if(!info.proxy.empty()&&lower(info.proxy)!="visionstereo11.dll")throw std::runtime_error("The fix already chains another D3D11 wrapper ("+info.proxy+"). That chain needs an explicit supported adapter; no files were changed.");
    if(fs::exists(backup)){
        if(fs::is_regular_file(backup/L"connection.txt")&&read(backup/L"connection.txt")==recordV3&&lower(info.proxy)=="visionstereo11.dll"&&fs::is_regular_file(runtimePath)&&read(runtimePath)==read(source)&&read(dir/L"d3d11.dll")==read(loader)&&fs::is_regular_file(dir/L"VisionGeo11.dll")&&read(dir/L"VisionGeo11.dll")==read(backup/L"Geo11.original.dll"))return "This existing Geo11 fix is already connected. Leave Vision Restoration open and launch the game normally.";
        throw std::runtime_error("An output connection backup already exists. Disconnect that connection before updating it.");
    }
    if(lower(info.proxy)=="visionstereo11.dll")throw std::runtime_error("The fix references our adapter without a connection record; configuration was left unchanged.");
    if(fs::exists(runtimePath))throw std::runtime_error("VisionStereo11.dll already exists without our connection record; no files were changed.");
    auto originalPath=dir/L"VisionGeo11.dll",entryPath=dir/L"d3d11.dll";
    if(fs::exists(originalPath)||read(entryPath).find(loaderMarker)!=std::string::npos)throw std::runtime_error("An early stereo loader already exists without a matching connection record; no files were changed.");
    if(architecture(entryPath)!=info.architecture)throw std::runtime_error("The installed Geo11 renderer does not match the game's architecture.");
    auto originalRenderer=read(entryPath);
    // Only establish the common hook chain. Keep d3dxdm.ini byte-for-byte,
    // including output mode, compatibility options and convergence settings.
    fix.set("System","proxy_d3d11","proxy_d3d11=VisionStereo11.dll");
    fs::create_directory(backup);write(backup/L"d3dx.ini.before",fixBefore);write(backup/L"d3dxdm.ini.before",providerBefore);
    fs::copy_file(source,backup/L"VisionStereo11.installed.dll");
    fs::copy_file(loader,backup/L"VisionStereoLoader.installed.dll");write(backup/L"Geo11.original.dll",originalRenderer);
    bool copied=false,fixChanged=false,originalCopied=false,entryChanged=false;
    try{
        fs::copy_file(source,runtimePath);copied=true;fs::copy_file(entryPath,originalPath);originalCopied=true;
        replace(fixPath,fix.text());fixChanged=true;
        replace(entryPath,read(loader));entryChanged=true;write(backup/L"connection.txt",recordV3);
    }catch(...){
        if(entryChanged)replace(entryPath,originalRenderer);if(originalCopied)fs::remove(originalPath);
        if(fixChanged)replace(fixPath,fixBefore);if(copied)fs::remove(runtimePath);
        for(const auto* name:{L"d3dx.ini.before",L"d3dxdm.ini.before",L"VisionStereo11.installed.dll",L"VisionStereoLoader.installed.dll",L"Geo11.original.dll"})fs::remove(backup/name);fs::remove(backup);throw;
    }
    return "Enabled the shared Geo11 hook. Leave Vision Restoration open and launch normally. The existing output mode, compatibility settings and shader fix were preserved. The game keeps its window and input; the app controls eye/black timing.";
}
std::string connectCapture(const fs::path& executable,const fs::path& runtimeRoot,CaptureMode mode,bool rightFirst){
    const auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);const auto arch=architecture(exe);
    const auto runtime=runtimeRoot/(arch==Architecture::X64?L"x64":L"x86");
    const wchar_t* addonName=arch==Architecture::X64?L"VisionStereoCapture.addon64":L"VisionStereoCapture.addon32";
    const auto addon=runtime/addonName,backup=dir/L"VisionRestoration.CaptureBackup",config=dir/L"VisionStereoCapture.ini";
    if(fs::exists(backup)||fs::exists(dir/addonName)||fs::exists(config))throw std::runtime_error("A capture connection or its files already exist. Disconnect it before changing the adapter.");
    if(fs::exists(dir/L"VisionRestoration.OutputBackup")||fs::exists(dir/L"VisionGameHook.addon64")||fs::exists(dir/L"VisionGameHook.addon32"))throw std::runtime_error("Disconnect the existing in-game presentation hook before enabling independent capture.");
    const char* modes[]{"sequential","array","katanga","sbs","tab"};const auto index=unsigned(mode);if(index>=5)throw std::runtime_error("Unknown capture layout.");
    const auto providerInfo=inspect(exe);
    if(providerInfo.migoto&&!providerInfo.geo11)throw std::runtime_error("This classic 3Dmigoto fix needs a working NVIDIA stereo renderer. Adding ReShade cannot provide it. No game files were changed; use a compatible Geo11 version of the community fix on modern GPUs.");
    if(providerInfo.geo11){
        const auto geoRuntime=runtime/L"VisionStereo11.dll";
        if(!fs::is_regular_file(geoRuntime)||read(geoRuntime).find("Geo11 direct-eye capture ready")==std::string::npos)throw std::runtime_error("Update the matching Geo11 output runtime before connecting direct capture.");
        if(fs::exists(dir/L"dxgi.dll"))throw std::runtime_error("An existing DXGI wrapper needs a verified Geo11 chain; it was left unchanged.");
        const auto settings=std::string("[VISION_CAPTURE]\r\nMode=geo11\r\nRightFirst=")+(rightFirst?"1":"0")+"\r\n";
        bool connected=false;
        fs::create_directory(backup);
        try{
            write(backup/L"connection.txt","VisionCapture.v1\n");write(backup/L"loader.txt","geo11");write(backup/L"config.installed",settings);
            connect(exe,runtimeRoot);connected=true;write(config,settings);
        }catch(...){
            if(connected)disconnect(exe);
            std::error_code ignored;fs::remove(config,ignored);
            for(const auto* name:{L"connection.txt",L"loader.txt",L"config.installed"})fs::remove(backup/name,ignored);
            if(fs::is_empty(backup,ignored))fs::remove(backup,ignored);throw;
        }
        return "Connected the existing Geo11 renderer to Direct 3D eyes through its proxy interface. Its output mode and shader fixes were retained. Use Start game 3D under Input/Games when its provider is ready.";
    }
    if(!fs::is_regular_file(addon)||architecture(addon)!=arch||read(addon).find("VisionStereoCapture.v1")==std::string::npos)throw std::runtime_error("The matching shared capture add-on is missing from runtime/x86 or runtime/x64.");
    auto reshade=[](const fs::path& path){return fs::is_regular_file(path)&&read(path).find("ReShadeRegisterAddon")!=std::string::npos;};
    fs::path loader;
    const auto dxgi=dir/L"dxgi.dll",d3d11=dir/L"d3d11.dll";
    const bool existing=reshade(dxgi)||reshade(d3d11);
    if(existing){const auto path=reshade(dxgi)?dxgi:d3d11;if(architecture(path)!=arch)throw std::runtime_error("The installed ReShade loader has the wrong architecture.");}
    else{
        loader=dxgi;if(fs::exists(loader))throw std::runtime_error("A different DXGI loader is already installed; it was left unchanged.");
        // Load ReShade through DXGI before a legacy stereo renderer initializes.
        // Late proxy_d3d11 loading can invalidate ReShade's original-function
        // trampolines. Keep the renderer and its configuration byte-for-byte.
        const auto source=runtime/L"ReShade.dll";
        if(!reshade(source)||architecture(source)!=arch)throw std::runtime_error("ReShade with full add-on support is missing. Run tools/Get-CaptureRuntime.ps1 to download it from the official site.");
    }
    const auto settings=std::string("[VISION_CAPTURE]\r\nMode=")+modes[index]+"\r\nRightFirst="+(rightFirst?"1":"0")+"\r\n";
    const auto reshadeConfig=dir/L"ReShade.ini";const bool createReShadeConfig=!fs::exists(reshadeConfig);
    const std::string reshadeSettings="[GENERAL]\r\nNoDebugInfo=1\r\n";
    fs::create_directory(backup);bool addonWritten=false,configWritten=false,loaderWritten=false,reshadeWritten=false;
    try{
        write(backup/L"connection.txt","VisionCapture.v1\n");write(backup/L"loader.txt",loader.empty()?"existing":"dxgi");
        fs::copy_file(addon,backup/L"addon.installed");write(backup/L"config.installed",settings);
        if(!loader.empty())fs::copy_file(runtime/L"ReShade.dll",backup/L"loader.installed");
        if(createReShadeConfig)write(backup/L"reshade.ini.installed",reshadeSettings);
        fs::copy_file(addon,dir/addonName);addonWritten=true;write(config,settings);configWritten=true;
        if(!loader.empty()){fs::copy_file(runtime/L"ReShade.dll",loader);loaderWritten=true;}
        // Seed a minimal configuration only when none exists.
        if(createReShadeConfig){write(reshadeConfig,reshadeSettings);reshadeWritten=true;}
    }catch(...){
        std::error_code ignored;if(addonWritten)fs::remove(dir/addonName,ignored);if(configWritten)fs::remove(config,ignored);if(loaderWritten)fs::remove(loader,ignored);if(reshadeWritten)fs::remove(reshadeConfig,ignored);
        for(const auto* name:{L"connection.txt",L"loader.txt",L"addon.installed",L"config.installed",L"loader.installed",L"reshade.ini.installed"})fs::remove(backup/name,ignored);
        if(fs::is_empty(backup,ignored))fs::remove(backup,ignored);throw;
    }
    return "Connected shared 3D capture. Launch the game, then select its window under Input > Direct 3D eyes and start the source. The existing stereo renderer and shader fixes were preserved.";
}
std::string disconnectCapture(const fs::path& executable){
    const auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);const auto backup=dir/L"VisionRestoration.CaptureBackup";
    if(!fs::is_regular_file(backup/L"connection.txt")||read(backup/L"connection.txt")!="VisionCapture.v1\n")throw std::runtime_error("No valid shared capture connection record.");
    const auto arch=architecture(exe);const auto addon=dir/(arch==Architecture::X64?L"VisionStereoCapture.addon64":L"VisionStereoCapture.addon32");
    const auto config=dir/L"VisionStereoCapture.ini";const auto kind=read(backup/L"loader.txt");
    if(kind=="geo11"){
        if(!fs::is_regular_file(config)||read(config)!=read(backup/L"config.installed"))throw std::runtime_error("The capture configuration changed; automatic removal stopped to preserve it.");
        disconnect(exe);fs::remove(config);
        for(const auto* name:{L"connection.txt",L"loader.txt",L"config.installed"})fs::remove(backup/name);
        if(fs::is_empty(backup))fs::remove(backup);
        return "Disconnected Geo11 capture; the existing renderer, output mode and shader fixes were restored.";
    }
    if(kind!="existing"&&kind!="dxgi")throw std::runtime_error("Invalid capture loader record.");
    const auto loader=kind=="dxgi"?dir/L"dxgi.dll":fs::path{};
    for(const auto& pair:{std::pair{addon,backup/L"addon.installed"},std::pair{config,backup/L"config.installed"}})
        if(!fs::is_regular_file(pair.first)||read(pair.first)!=read(pair.second))throw std::runtime_error("An installed capture file was changed; automatic removal stopped to preserve it.");
    if(!loader.empty()&&(!fs::is_regular_file(loader)||read(loader)!=read(backup/L"loader.installed")))throw std::runtime_error("The installed capture loader was changed; automatic removal stopped.");
    fs::remove(addon);fs::remove(config);if(!loader.empty())fs::remove(loader);
    if(fs::is_regular_file(backup/L"reshade.ini.installed")&&fs::is_regular_file(dir/L"ReShade.ini")&&read(dir/L"ReShade.ini")==read(backup/L"reshade.ini.installed"))fs::remove(dir/L"ReShade.ini");
    for(const auto* name:{L"connection.txt",L"loader.txt",L"addon.installed",L"config.installed",L"loader.installed",L"reshade.ini.installed"})fs::remove(backup/name);
    if(fs::is_empty(backup))fs::remove(backup);
    return "Disconnected shared capture; the game's existing renderer and shader fixes were retained.";
}
std::string disconnect(const fs::path& executable){
    auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);auto backup=dir/L"VisionRestoration.OutputBackup";
    if(!fs::is_regular_file(backup/L"connection.txt"))throw std::runtime_error("No completed output connection record was found.");
    const auto record=read(backup/L"connection.txt");const bool sharedHook=record==recordV3,early=record==recordV2||sharedHook;
    if(!early&&record!=recordV1)throw std::runtime_error("Unsupported output connection record; no files were changed.");
    auto fixPath=dir/L"d3dx.ini",providerPath=dir/L"d3dxdm.ini",runtime=dir/L"VisionStereo11.dll";
    if(fs::exists(runtime)&&read(runtime)!=read(backup/L"VisionStereo11.installed.dll"))throw std::runtime_error("The installed runtime changed since connection; no files were removed.");
    if(early&&(read(dir/L"d3d11.dll")!=read(backup/L"VisionStereoLoader.installed.dll")||read(dir/L"VisionGeo11.dll")!=read(backup/L"Geo11.original.dll")))throw std::runtime_error("The loader or community renderer changed since connection; preserve the updated fix before reconnecting. No files were changed.");
    auto fixBefore=read(fixPath),providerBefore=read(providerPath);Ini fix(fixBefore),provider(providerBefore);
    fix.restore(Ini(read(backup/L"d3dx.ini.before")),"System","proxy_d3d11","VisionStereo11.dll");
    if(!sharedHook)provider.restore(Ini(read(backup/L"d3dxdm.ini.before")),"Device","direct_mode","katanga_vr");
    replace(fixPath,fix.text());try{if(!sharedHook)replace(providerPath,provider.text());}catch(...){replace(fixPath,fixBefore);throw;}
    if(early){try{replace(dir/L"d3d11.dll",read(backup/L"Geo11.original.dll"));}catch(...){replace(fixPath,fixBefore);if(!sharedHook)replace(providerPath,providerBefore);throw;}fs::remove(dir/L"VisionGeo11.dll");}
    if(fs::exists(runtime))fs::remove(runtime);
    // Only remove files owned by this connection, retaining any user additions.
    for(const auto* name:{L"connection.txt",L"d3dx.ini.before",L"d3dxdm.ini.before",L"VisionStereo11.installed.dll",L"VisionStereoLoader.installed.dll",L"Geo11.original.dll"})fs::remove(backup/name);
    if(fs::is_empty(backup))fs::remove(backup);
    return "Disconnected our output adapter and restored the previous output settings. Other fix settings and shader files were retained.";
}
}
