// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_compatibility.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
namespace compat=vision::compatibility;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void put(const fs::path& p,const std::string& s){std::ofstream f(p,std::ios::binary);f<<s;require(bool(f),"Fixture write");}
std::string get(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void pe(const fs::path& p,bool x64,const std::string& extra=""){
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=sizeof(dos);DWORD signature=IMAGE_NT_SIGNATURE;IMAGE_FILE_HEADER header{};header.Machine=x64?IMAGE_FILE_MACHINE_AMD64:IMAGE_FILE_MACHINE_I386;
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(&dos),sizeof(dos));f.write(reinterpret_cast<const char*>(&signature),sizeof(signature));f.write(reinterpret_cast<const char*>(&header),sizeof(header));f<<extra;
}
template<class F>void rejected(F&& action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,"Unsafe connection was accepted");}
int main(){
    const auto root=fs::temp_directory_path()/(L"VisionFixConnectionTests-"+std::to_wstring(GetCurrentProcessId()));
    try{
        fs::create_directories(root/L"game");fs::create_directories(root/L"runtime/x64");fs::create_directories(root/L"runtime/x86");
        auto game=root/L"game",exe=game/L"FixtureGame.exe",runtime=root/L"runtime";
        pe(exe,true);pe(game/L"d3d11.dll",true,"HackerSwapChainKatanga");pe(runtime/L"x64/VisionStereo11.dll",true,"VisionStereo11 initialized Geo11 direct-eye capture ready");pe(runtime/L"x86/VisionStereo11.dll",false,"VisionStereo11 initialized Geo11 direct-eye capture ready");
        pe(runtime/L"x64/VisionStereoLoader.dll",true,"VisionStereoLoader.v1");pe(runtime/L"x86/VisionStereoLoader.dll",false,"VisionStereoLoader.v1");
        const std::string fix="\xef\xbb\xbf; established fix\r\n[System]\r\n;proxy_d3d11=example.dll\r\n[Device]\r\nforce_stereo = 2\r\n[Include]\r\ninclude = ShaderFixes\\one.ini\r\ninclude = ShaderFixes\\two.ini\r\n[Constants]\r\nglobal persist $convergence = 7.5\r\n";
        const std::string provider="[Device]\n  direct_mode = tab\nallow_platform_update = 1\n[Stereo]\ndm_convergence = 2.5\n";
        put(game/L"d3dx.ini",fix);put(game/L"d3dxdm.ini",provider);put(game/L"UserShader.txt","keep this exact shader");
        auto originalExe=get(exe),originalRenderer=get(game/L"d3d11.dll");
        compat::connect(exe,runtime);require(get(game/L"d3dx.ini").find("include = ShaderFixes\\two.ini")!=std::string::npos,"Duplicate includes lost");
        require(get(game/L"d3dxdm.ini")==provider,"Connecting changed the established provider settings");
        require(get(game/L"d3dx.ini").starts_with("\xef\xbb\xbf"),"UTF8 BOM lost");
        require(get(game/L"VisionStereo11.dll")==get(runtime/L"x64/VisionStereo11.dll"),"Wrong runtime architecture");
        require(get(game/L"VisionGeo11.dll")==originalRenderer,"Community renderer bytes changed");
        require(get(game/L"d3d11.dll")==get(runtime/L"x64/VisionStereoLoader.dll"),"Early loader was not installed");
        compat::connect(exe,runtime); // Idempotent connection with exact same runtime.
        put(game/L"d3dxdm.ini",get(game/L"d3dxdm.ini")+"user_tuning = 123\n");
        compat::disconnect(exe);require(get(game/L"d3dx.ini")==fix,"Original fix text was not restored");
        require(get(game/L"d3dxdm.ini")==provider+"user_tuning = 123\n","Output restore clobbered subsequent user tuning");
        require(get(exe)==originalExe&&get(game/L"d3d11.dll")==originalRenderer&&get(game/L"UserShader.txt")=="keep this exact shader","A game or community shader file was modified");
        std::cout<<"PASS: preserves existing fix, repeated includes, encoding, shader files and later user edits; idempotent connect and clean disconnect\n";
        put(game/L"d3dx.ini",fix+"[System]\nproxy_d3d11=AnotherWrapper.dll\n");auto conflict=get(game/L"d3dx.ini");
        rejected([&]{compat::connect(exe,runtime);});require(get(game/L"d3dx.ini")==conflict&&!fs::exists(game/L"VisionStereo11.dll"),"Wrapper collision changed files");
        put(game/L"d3dx.ini",fix+"[System]\nproxy_d3d11=first.dll\nproxy_d3d11=second.dll\n");rejected([&]{compat::connect(exe,runtime);});
        put(game/L"d3dx.ini",fix);put(game/L"d3dxdm.ini",provider);
        HANDLE locked=CreateFileW((game/L"d3d11.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(locked!=INVALID_HANDLE_VALUE,"Lock fixture");
        rejected([&]{compat::connect(exe,runtime);});CloseHandle(locked);
        require(get(game/L"d3dx.ini")==fix&&get(game/L"d3dxdm.ini")==provider&&!fs::exists(game/L"VisionStereo11.dll")&&!fs::exists(game/L"VisionGeo11.dll")&&get(game/L"d3d11.dll")==originalRenderer,"Partial connection did not roll back");
        std::cout<<"PASS: refuses existing wrapper conflicts and ambiguous settings; rolls back when a configuration file is locked\n";
        pe(exe,false);rejected([&]{compat::connect(exe,runtime);});pe(game/L"d3d11.dll",false,"HackerSwapChainKatanga");compat::connect(exe,runtime);require(get(game/L"VisionStereo11.dll")==get(runtime/L"x86/VisionStereo11.dll"),"32-bit executable did not select 32-bit runtime");compat::disconnect(exe);
        std::cout<<"PASS: selects runtime from executable architecture, independent of game title\n";
        for(const auto* mode:{"sbs","tab","sbs_reversed","tab_reversed","katanga_vr","TAB ; existing comment",""}){
            const auto settings=std::string("[Device]\nallow_platform_update = 1\n")+(*mode?std::string("direct_mode = ")+mode+"\n":"")+"[Stereo]\ndm_separation = 47.25\ndm_convergence = 2.5\n";
            put(game/L"d3dxdm.ini",settings);
            // Prove neither connection nor removal attempts a provider write.
            locked=CreateFileW((game/L"d3dxdm.ini").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(locked!=INVALID_HANDLE_VALUE,"Read-only provider fixture");
            compat::connect(exe,runtime);require(get(game/L"d3dxdm.ini")==settings,"Connection rewrote provider configuration");
            compat::disconnect(exe);CloseHandle(locked);require(get(game/L"d3dxdm.ini")==settings,"Removal rewrote provider configuration");
        }
        for(const auto* mode:{"nvidia_dx11","nvidia_dx9","anaglyph","interlaced","unknown"}){
            const auto settings=std::string("[Device]\ndirect_mode = ")+mode+"\n";put(game/L"d3dxdm.ini",settings);
            rejected([&]{compat::connect(exe,runtime);});
            require(get(game/L"d3dx.ini")==fix&&get(game/L"d3dxdm.ini")==settings&&!fs::exists(game/L"VisionRestoration.OutputBackup"),"Unsupported mode changed the fix");
        }
        std::cout<<"PASS: consumes existing packed/Katanga modes with read-only provider settings; unsupported modes leave the fix untouched\n";
        put(game/L"d3dxdm.ini",provider);auto values=compat::tuning(exe);require(values.depth==50&&values.convergence==2.5f,"Tuning defaults or existing values were lost");
        values.depth=32.5f;values.convergence=4.75f;compat::setTuning(exe,values);auto readback=compat::tuning(exe);
        require(readback.depth==32.5f&&readback.convergence==4.75f,"Depth slider settings were not persisted");
        require(get(game/L"d3dxdm.ini").find("direct_mode = tab")!=std::string::npos&&get(game/L"d3dxdm.ini").find("allow_platform_update = 1")!=std::string::npos&&get(game/L"d3dx.ini")==fix,"Depth controls changed output or compatibility settings");
        auto saved=get(game/L"d3dxdm.ini");values.depth=-1;rejected([&]{compat::setTuning(exe,values);});require(get(game/L"d3dxdm.ini")==saved,"Invalid depth changed configuration");
        put(game/L"d3dxdm.ini",provider+"dm_auto_convergence = 1\n");values=compat::tuning(exe);values.depth=40;values.convergence=999;compat::setTuning(exe,values);
        require(compat::tuning(exe).autoConvergence&&compat::tuning(exe).convergence==2.5f,"Depth control overrode the fix's auto-convergence");
        std::cout<<"PASS: game depth controls persist only requested tuning and preserve auto-convergence, output mode and compatibility options\n";
        for(bool x64:{false,true}){
            auto captureGame=root/(x64?L"capture64":L"capture32");fs::create_directory(captureGame);auto captureExe=captureGame/L"GenericGame.exe";pe(captureExe,x64);
            auto runtimeArch=runtime/(x64?L"x64":L"x86");const auto addon=x64?L"VisionStereoCapture.addon64":L"VisionStereoCapture.addon32";
            pe(runtimeArch/addon,x64,"VisionStereoCapture.v1");pe(runtimeArch/L"ReShade.dll",x64,"ReShadeRegisterAddon");
            compat::connectCapture(captureExe,runtime,compat::CaptureMode::Sequential,true);
            require(get(captureGame/L"VisionStereoCapture.ini").find("Mode=sequential")!=std::string::npos&&get(captureGame/L"VisionStereoCapture.ini").find("RightFirst=1")!=std::string::npos,"Declared eye layout lost");
            require(get(captureGame/addon)==get(runtimeArch/addon)&&get(captureGame/L"dxgi.dll")==get(runtimeArch/L"ReShade.dll"),"Native capture architecture or loader mismatch");
            rejected([&]{compat::connectCapture(captureExe,runtime,compat::CaptureMode::Array);});
            compat::disconnectCapture(captureExe);require(!fs::exists(captureGame/L"dxgi.dll")&&!fs::exists(captureGame/addon),"Capture disconnect retained owned files");
            pe(captureGame/L"dxgi.dll",x64,"foreign wrapper");const auto foreign=get(captureGame/L"dxgi.dll");rejected([&]{compat::connectCapture(captureExe,runtime,compat::CaptureMode::Sequential);});require(get(captureGame/L"dxgi.dll")==foreign,"Foreign loader overwritten");fs::remove(captureGame/L"dxgi.dll");
            for(const auto& marker:{std::string("3Dmigoto"),std::string("HackerSwapChainKatanga")}){
                pe(captureGame/L"d3d11.dll",x64,marker);const auto renderer=get(captureGame/L"d3d11.dll");put(captureGame/L"d3dx.ini",fix);put(captureGame/L"d3dxdm.ini",provider);
                const bool geo=marker=="HackerSwapChainKatanga";
                if(!geo){
                    rejected([&]{compat::connectCapture(captureExe,runtime,compat::CaptureMode::Katanga);});
                    require(!fs::exists(captureGame/L"dxgi.dll")&&!fs::exists(captureGame/addon)&&!fs::exists(captureGame/L"VisionRestoration.CaptureBackup"),"Classic fix received an unsupported ReShade connection");
                    require(get(captureGame/L"d3d11.dll")==renderer&&get(captureGame/L"d3dx.ini")==fix&&get(captureGame/L"d3dxdm.ini")==provider,"Unsupported renderer rejection changed game files");
                    continue;
                }
                compat::connectCapture(captureExe,runtime,compat::CaptureMode::Katanga);
                if(geo){require(!fs::exists(captureGame/L"dxgi.dll")&&!fs::exists(captureGame/addon),"Geo11 capture incorrectly chained ReShade");require(get(captureGame/L"VisionGeo11.dll")==renderer&&get(captureGame/L"VisionStereoCapture.ini").find("Mode=geo11")!=std::string::npos,"Geo11 capture did not use its proxy route");}
                else require(get(captureGame/L"d3dx.ini")==fix&&fs::is_regular_file(captureGame/L"dxgi.dll")&&get(captureGame/L"d3d11.dll")==renderer,"Legacy capture changed provider or loader");
                require(get(captureGame/L"d3dxdm.ini")==provider,"Capture changed provider settings");
                put(captureGame/L"d3dx.ini",get(captureGame/L"d3dx.ini")+"; later user edit\r\n");
                compat::disconnectCapture(captureExe);require(get(captureGame/L"d3dx.ini")==fix+"; later user edit\r\n","Disconnect lost later fix edits");
                put(captureGame/L"d3dx.ini",fix);
                // Even a read-only community fix must connect successfully.
                HANDLE fixLock=CreateFileW((captureGame/L"d3dx.ini").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(fixLock!=INVALID_HANDLE_VALUE,"Capture read-only fix");
                if(geo){rejected([&]{compat::connectCapture(captureExe,runtime,compat::CaptureMode::Katanga);});require(!fs::exists(captureGame/L"VisionRestoration.CaptureBackup")&&!fs::exists(captureGame/L"VisionStereo11.dll")&&get(captureGame/L"d3d11.dll")==renderer,"Geo11 failed connection did not roll back");}
                else{compat::connectCapture(captureExe,runtime,compat::CaptureMode::Katanga);compat::disconnectCapture(captureExe);}
                CloseHandle(fixLock);
                require(get(captureGame/L"d3dx.ini")==fix,"Capture wrote a locked fix");
            }
        }
        std::cout<<"PASS: shared capture connects native and Geo11 fixtures and rejects unsupported classic 3Dmigoto for x86/x64; preserves providers and user edits, rejects foreign loaders, and accepts read-only provider settings\n";
        // The unique, fully resolved fixture root is the only recursive target.
        require(fs::canonical(root).parent_path()==fs::canonical(fs::temp_directory_path()),"Unexpected fixture root");fs::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"; retained fixture "<<root<<"\n";return 1;}
}
