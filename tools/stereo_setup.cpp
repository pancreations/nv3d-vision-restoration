// SPDX-License-Identifier: GPL-3.0-or-later
#include "stereo_compatibility.h"
#include <iostream>
#include <string>
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<3){std::cerr<<"Usage: vision_stereo_setup inspect|connect|disconnect game.exe [runtime-root]\n";return 2;}
        const std::wstring action=argv[1];
        if(action==L"inspect")std::cout<<vision::compatibility::inspect(argv[2]).description<<"\n";
        else if(action==L"connect"&&argc==4)std::cout<<vision::compatibility::connect(argv[2],argv[3])<<"\n";
        else if(action==L"disconnect")std::cout<<vision::compatibility::disconnect(argv[2])<<"\n";
        else{std::cerr<<"Invalid command or arguments\n";return 2;}
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
