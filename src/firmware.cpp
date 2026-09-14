#include "core.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace vision {
std::vector<uint8_t> readBinary(const std::filesystem::path& p){
    std::ifstream f(p,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()<0 || f.tellg()>64*1024*1024)throw std::runtime_error("Cannot read file, or file exceeds 64 MB.");
    auto size=size_t(f.tellg());std::vector<uint8_t> b(size);f.seekg(0);
    if(size && !f.read(reinterpret_cast<char*>(b.data()),size))throw std::runtime_error("Truncated file.");return b;
}
std::vector<FirmwareBlock> parseFirmware(std::span<const uint8_t> b){
    std::vector<FirmwareBlock> blocks;
    for(size_t i=0;i<b.size();) {
        if(b.size()-i<4)throw std::runtime_error("Truncated firmware header.");
        unsigned len=(b[i]<<8)|b[i+1],addr=(b[i+2]<<8)|b[i+3];i+=4;
        if(!len || len>4096 || len>b.size()-i || addr+len>65536)throw std::runtime_error("Invalid firmware block.");
        if(!(addr<0x4000 && addr+len<=0x4000) && !(addr==0xE600 && len==1))throw std::runtime_error("Firmware writes outside supported RAM/CPUCS range.");
        blocks.push_back({uint16_t(addr),{b.begin()+i,b.begin()+i+len}});i+=len;
    }
    if(blocks.size()<3 || blocks.front().address!=0xE600 || blocks.front().data!=std::vector<uint8_t>{1} || blocks.back().address!=0xE600 || blocks.back().data!=std::vector<uint8_t>{0})throw std::runtime_error("Firmware must begin/end with CPU reset/release records.");
    for(size_t i=1;i+1<blocks.size();i++)if(blocks[i].address==0xE600)throw std::runtime_error("Unexpected CPU control in firmware body.");
    return blocks;
}
std::vector<uint8_t> extractFirmware(std::span<const uint8_t> d){
    auto u16=[&](size_t i)->uint16_t{if(i>d.size() || d.size()-i<2)throw std::runtime_error("Truncated PE.");return d[i]|(d[i+1]<<8);};
    auto u32=[&](size_t i)->uint32_t{if(i>d.size() || d.size()-i<4)throw std::runtime_error("Truncated PE.");return uint32_t(d[i])|(uint32_t(d[i+1])<<8)|(uint32_t(d[i+2])<<16)|(uint32_t(d[i+3])<<24);};
    if(u16(0)!=0x5a4d)throw std::runtime_error("Expected an NVIDIA nvstusb.sys PE file.");
    size_t pe=u32(0x3c);if(u32(pe)!=0x4550)throw std::runtime_error("Invalid PE signature.");
    size_t table=pe+24+u16(pe+20);unsigned count=u16(pe+6);
    constexpr std::array<uint8_t,8> sig{0xc2,0x55,0x09,0x07,0,0,0,0};
    std::vector<uint8_t> result;int found=0;
    for(unsigned section=0;section<count;section++){
        size_t h=table+40*section;if(h>d.size() || d.size()-h<40)throw std::runtime_error("Truncated section table.");
        if(std::string(reinterpret_cast<const char*>(d.data()+h),5)!=".data")continue;
        size_t size=u32(h+16),start=u32(h+20);if(start>d.size() || size>d.size()-start)throw std::runtime_error("Invalid section bounds.");
        auto first=d.begin()+start,last=first+size;
        auto pos=std::search(first,last,sig.begin(),sig.end());
        while(pos!=last){
            ++found;if(found>1)throw std::runtime_error("Multiple firmware candidates; refusing ambiguous extraction.");
            size_t i=size_t(pos-d.begin())+8,end=start+size;
            result={0,1,0xe6,0,1};bool terminal=false;
            while(i+4<=end){
                if(d[i]&0x80){terminal=true;break;}
                unsigned len=(d[i]<<8)|d[i+1];if(!len || len>4096 || len>end-i-4)throw std::runtime_error("Malformed firmware inside driver.");
                result.insert(result.end(),d.begin()+i,d.begin()+i+4+len);i+=4+len;
            }
            if(!terminal)throw std::runtime_error("Firmware terminator missing.");
            result.insert(result.end(),{0,1,0xe6,0,0});parseFirmware(result);
            pos=std::search(pos+8,last,sig.begin(),sig.end());
        }
    }
    if(found!=1)throw std::runtime_error("No supported 0955:0007 firmware signature found. Driver was not modified.");return result;
}
}
