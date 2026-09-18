// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_compatibility.h"
#include <iostream>
#include <string>
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<3){std::cerr<<"Usage: vision_stereo_setup inspect|connect|disconnect|tuning game.exe [runtime-root]\n       vision_stereo_setup depth game.exe depth-percent convergence\n       vision_stereo_setup capture game.exe runtime-root sequential|array|katanga|sbs|tab [right-first]\n       vision_stereo_setup uncapture game.exe\n";return 2;}
        const std::wstring action=argv[1];
        if(action==L"inspect")std::cout<<vision::compatibility::inspect(argv[2]).description<<"\n";
        else if(action==L"connect"&&argc==4)std::cout<<vision::compatibility::connect(argv[2],argv[3])<<"\n";
        else if(action==L"disconnect")std::cout<<vision::compatibility::disconnect(argv[2])<<"\n";
        else if(action==L"capture"&&(argc==5||argc==6)){
            const std::wstring name=argv[4];vision::compatibility::CaptureMode mode;
            if(name==L"sequential")mode=vision::compatibility::CaptureMode::Sequential;else if(name==L"array")mode=vision::compatibility::CaptureMode::Array;
            else if(name==L"katanga")mode=vision::compatibility::CaptureMode::Katanga;else if(name==L"sbs")mode=vision::compatibility::CaptureMode::SideBySide;else if(name==L"tab")mode=vision::compatibility::CaptureMode::TopBottom;else throw std::runtime_error("Unknown capture mode");
            if(argc==6&&std::wstring(argv[5])!=L"right-first")throw std::runtime_error("Expected right-first");
            std::cout<<vision::compatibility::connectCapture(argv[2],argv[3],mode,argc==6)<<"\n";
        }
        else if(action==L"uncapture")std::cout<<vision::compatibility::disconnectCapture(argv[2])<<"\n";
        else if(action==L"tuning"){const auto values=vision::compatibility::tuning(argv[2]);std::cout<<"Depth="<<values.depth<<"% convergence="<<values.convergence<<" auto-convergence="<<values.autoConvergence<<"\n";}
        else if(action==L"depth"&&argc==5){auto values=vision::compatibility::tuning(argv[2]);values.depth=std::stof(argv[3]);values.convergence=std::stof(argv[4]);std::cout<<vision::compatibility::setTuning(argv[2],values)<<"\n";}
        else{std::cerr<<"Invalid command or arguments\n";return 2;}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
