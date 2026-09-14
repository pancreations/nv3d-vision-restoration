#include "core.h"
#include <fstream>
#include <iostream>
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=3){std::cerr<<"Usage: vision_firmware.exe <nvstusb.sys> <output.fw>\nNo driver is installed. Only the known 0955:0007 firmware signature is supported.\n";return 2;}
        if(std::filesystem::exists(argv[2]))throw std::runtime_error("Output already exists; choose a new filename.");
        auto data=vision::extractFirmware(vision::readBinary(argv[1]));
        std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),data.size());if(!out)throw std::runtime_error("Output write failed.");
        std::cout<<"Extracted "<<data.size()<<" bytes. No USB writes performed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
