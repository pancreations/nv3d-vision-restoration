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
        pe(exe,true);pe(game/L"d3d11.dll",true,"HackerSwapChainKatanga");pe(runtime/L"x64/VisionStereo11.dll",true,"VisionStereo11 initialized");pe(runtime/L"x86/VisionStereo11.dll",false,"VisionStereo11 initialized");
        pe(runtime/L"x64/VisionStereoLoader.dll",true,"VisionStereoLoader.v1");pe(runtime/L"x86/VisionStereoLoader.dll",false,"VisionStereoLoader.v1");
        const std::string fix="\xef\xbb\xbf; established fix\r\n[System]\r\n;proxy_d3d11=example.dll\r\n[Device]\r\nforce_stereo = 2\r\n[Include]\r\ninclude = ShaderFixes\\one.ini\r\ninclude = ShaderFixes\\two.ini\r\n[Constants]\r\nglobal persist $convergence = 7.5\r\n";
        const std::string provider="[Device]\n  direct_mode = tab\n[Stereo]\ndm_convergence = 2.5\n";
        put(game/L"d3dx.ini",fix);put(game/L"d3dxdm.ini",provider);put(game/L"UserShader.txt","keep this exact shader");
        auto originalExe=get(exe),originalRenderer=get(game/L"d3d11.dll");
        compat::connect(exe,runtime);require(get(game/L"d3dx.ini").find("include = ShaderFixes\\two.ini")!=std::string::npos,"Duplicate includes lost");
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
        HANDLE locked=CreateFileW((game/L"d3dxdm.ini").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);require(locked!=INVALID_HANDLE_VALUE,"Lock fixture");
        rejected([&]{compat::connect(exe,runtime);});CloseHandle(locked);
        require(get(game/L"d3dx.ini")==fix&&get(game/L"d3dxdm.ini")==provider&&!fs::exists(game/L"VisionStereo11.dll")&&!fs::exists(game/L"VisionGeo11.dll")&&get(game/L"d3d11.dll")==originalRenderer,"Partial connection did not roll back");
        std::cout<<"PASS: refuses existing wrapper conflicts and ambiguous settings; rolls back when a configuration file is locked\n";
        pe(exe,false);rejected([&]{compat::connect(exe,runtime);});pe(game/L"d3d11.dll",false,"HackerSwapChainKatanga");compat::connect(exe,runtime);require(get(game/L"VisionStereo11.dll")==get(runtime/L"x86/VisionStereo11.dll"),"32-bit executable did not select 32-bit runtime");compat::disconnect(exe);
        std::cout<<"PASS: selects runtime from executable architecture, independent of game title\n";
        // The unique, fully resolved fixture root is the only recursive target.
        require(fs::canonical(root).parent_path()==fs::canonical(fs::temp_directory_path()),"Unexpected fixture root");fs::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"; retained fixture "<<root<<"\n";return 1;}
}
