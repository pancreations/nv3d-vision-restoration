// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_compatibility.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>
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
    if(info.description.empty())info.description="No established Geo11 fix detected. Install the game's suggested fix with 3D Fix Manager first. Legacy NVAPI/Helix backends need a separate renderer.";
    return info;
}
std::string connect(const fs::path& executable,const fs::path& runtimeRoot){
    auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);auto info=inspect(exe);
    if(!info.geo11)throw std::runtime_error(info.description);
    auto source=runtimeRoot/(info.architecture==Architecture::X64?L"x64":L"x86")/L"VisionStereo11.dll";
    if(!fs::is_regular_file(source)||architecture(source)!=info.architecture||read(source).find(runtimeMarker)==std::string::npos)throw std::runtime_error("The matching VisionStereo11 runtime is missing or invalid.");
    auto loader=source.parent_path()/L"VisionStereoLoader.dll";
    if(!fs::is_regular_file(loader)||architecture(loader)!=info.architecture||read(loader).find(loaderMarker)==std::string::npos)throw std::runtime_error("The matching early stereo loader is missing or invalid.");
    auto fixPath=dir/L"d3dx.ini",providerPath=dir/L"d3dxdm.ini",runtimePath=dir/L"VisionStereo11.dll",backup=dir/L"VisionRestoration.OutputBackup";
    auto fixBefore=read(fixPath),providerBefore=read(providerPath);Ini fix(fixBefore),provider(providerBefore);
    if(fix.get("Device","force_stereo")!="2")throw std::runtime_error(info.description);
    if(!info.proxy.empty()&&lower(info.proxy)!="visionstereo11.dll")throw std::runtime_error("The fix already chains another D3D11 wrapper ("+info.proxy+"). That chain needs an explicit supported adapter; no files were changed.");
    if(fs::exists(backup)){
        if(fs::is_regular_file(backup/L"connection.txt")&&read(backup/L"connection.txt")==recordV2&&lower(info.proxy)=="visionstereo11.dll"&&lower(info.output)=="katanga_vr"&&fs::is_regular_file(runtimePath)&&read(runtimePath)==read(source)&&read(dir/L"d3d11.dll")==read(loader))return "This existing Geo11 fix is already connected. Leave Vision Restoration open and launch the game normally.";
        throw std::runtime_error("An output connection backup already exists. Disconnect that connection before updating it.");
    }
    if(lower(info.proxy)=="visionstereo11.dll")throw std::runtime_error("The fix references our adapter without a connection record; configuration was left unchanged.");
    if(fs::exists(runtimePath))throw std::runtime_error("VisionStereo11.dll already exists without our connection record; no files were changed.");
    auto originalPath=dir/L"VisionGeo11.dll",entryPath=dir/L"d3d11.dll";
    if(fs::exists(originalPath)||read(entryPath).find(loaderMarker)!=std::string::npos)throw std::runtime_error("An early stereo loader already exists without a matching connection record; no files were changed.");
    if(architecture(entryPath)!=info.architecture)throw std::runtime_error("The installed Geo11 renderer does not match the game's architecture.");
    auto originalRenderer=read(entryPath);
    fix.set("System","proxy_d3d11","proxy_d3d11=VisionStereo11.dll");provider.set("Device","direct_mode","direct_mode = katanga_vr");
    fs::create_directory(backup);write(backup/L"d3dx.ini.before",fixBefore);write(backup/L"d3dxdm.ini.before",providerBefore);
    fs::copy_file(source,backup/L"VisionStereo11.installed.dll");
    fs::copy_file(loader,backup/L"VisionStereoLoader.installed.dll");write(backup/L"Geo11.original.dll",originalRenderer);
    bool copied=false,fixChanged=false,providerChanged=false,originalCopied=false,entryChanged=false;
    try{
        fs::copy_file(source,runtimePath);copied=true;fs::copy_file(entryPath,originalPath);originalCopied=true;
        replace(fixPath,fix.text());fixChanged=true;replace(providerPath,provider.text());providerChanged=true;
        replace(entryPath,read(loader));entryChanged=true;write(backup/L"connection.txt",recordV2);
    }catch(...){
        if(entryChanged)replace(entryPath,originalRenderer);if(originalCopied)fs::remove(originalPath);
        if(providerChanged)replace(providerPath,providerBefore);if(fixChanged)replace(fixPath,fixBefore);if(copied)fs::remove(runtimePath);
        for(const auto* name:{L"d3dx.ini.before",L"d3dxdm.ini.before",L"VisionStereo11.installed.dll",L"VisionStereoLoader.installed.dll",L"Geo11.original.dll"})fs::remove(backup/name);fs::remove(backup);throw;
    }
    return "Connected the existing Geo11 fix. Leave Vision Restoration open and launch the game normally. The game keeps its own window and input; the host controls eye/black timing. Original output settings were backed up.";
}
std::string disconnect(const fs::path& executable){
    auto exe=canonicalExe(executable),dir=exe.parent_path();requireStopped(exe);auto backup=dir/L"VisionRestoration.OutputBackup";
    if(!fs::is_regular_file(backup/L"connection.txt"))throw std::runtime_error("No completed output connection record was found.");
    const auto record=read(backup/L"connection.txt");const bool early=record==recordV2;
    if(!early&&record!=recordV1)throw std::runtime_error("Unsupported output connection record; no files were changed.");
    auto fixPath=dir/L"d3dx.ini",providerPath=dir/L"d3dxdm.ini",runtime=dir/L"VisionStereo11.dll";
    if(fs::exists(runtime)&&read(runtime)!=read(backup/L"VisionStereo11.installed.dll"))throw std::runtime_error("The installed runtime changed since connection; no files were removed.");
    if(early&&(read(dir/L"d3d11.dll")!=read(backup/L"VisionStereoLoader.installed.dll")||read(dir/L"VisionGeo11.dll")!=read(backup/L"Geo11.original.dll")))throw std::runtime_error("The loader or community renderer changed since connection; preserve the updated fix before reconnecting. No files were changed.");
    auto fixBefore=read(fixPath),providerBefore=read(providerPath);Ini fix(fixBefore),provider(providerBefore);
    fix.restore(Ini(read(backup/L"d3dx.ini.before")),"System","proxy_d3d11","VisionStereo11.dll");
    provider.restore(Ini(read(backup/L"d3dxdm.ini.before")),"Device","direct_mode","katanga_vr");
    replace(fixPath,fix.text());try{replace(providerPath,provider.text());}catch(...){replace(fixPath,fixBefore);throw;}
    if(early){try{replace(dir/L"d3d11.dll",read(backup/L"Geo11.original.dll"));}catch(...){replace(fixPath,fixBefore);replace(providerPath,providerBefore);throw;}fs::remove(dir/L"VisionGeo11.dll");}
    if(fs::exists(runtime))fs::remove(runtime);
    // Only remove files owned by this connection, retaining any user additions.
    for(const auto* name:{L"connection.txt",L"d3dx.ini.before",L"d3dxdm.ini.before",L"VisionStereo11.installed.dll",L"VisionStereoLoader.installed.dll",L"Geo11.original.dll"})fs::remove(backup/name);
    if(fs::is_empty(backup))fs::remove(backup);
    return "Disconnected our output adapter and restored the previous output settings. Other fix settings and shader files were retained.";
}
}
