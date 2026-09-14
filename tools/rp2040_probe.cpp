// SPDX-License-Identifier: GPL-3.0-or-later
#include "rp2040_usb.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc,char** argv) {
    using namespace vision::rp2040;
    try {
        const std::string mode=argc>1?argv[1]:"--list";
        if(mode!="--list"&&mode!="--clock"){std::cerr<<"Usage: vision_rp2040_probe [--list | --clock]\nBoth modes are read-only; no IR commands or flashing.\n";return 1;}
        auto devices=discoverUsb();
        for(auto& d:devices)std::cout<<unsigned(d.bus)<<':'<<unsigned(d.address)<<' '<<d.label<<'\n';
        if(devices.empty()){std::cout<<"No Vision RP2040 firmware device found. This is expected before the board is connected and flashed.\n";return mode=="--list"?0:2;}
        if(mode=="--list")return 0;
        if(devices.size()!=1)throw std::runtime_error("Connect only the emitter being tested before running --clock");
        auto link=openUsb(devices[0].bus,devices[0].address);Client client(*link,hostMicroseconds);client.inspect();
        std::vector<double> rtt;unsigned usable=0;
        for(unsigned i=0;i<200;++i){if(client.sampleClock())++usable;rtt.push_back(client.status().roundTripUs);std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        std::sort(rtt.begin(),rtt.end());auto status=client.status();
        std::cout<<"Boot ID: "<<status.bootId<<"\nClock samples usable: "<<usable<<"/200\nUSB RTT us p50/p95/max: "<<rtt[100]<<'/'<<rtt[190]<<'/'<<rtt.back()<<"\nLast clock uncertainty us: "<<status.uncertaintyUs<<"\n";
        auto diagnostic=client.diagnostics();auto p=std::span(diagnostic.payload);
        std::cout<<"Device open/close commands: "<<get64(p.subspan(8))<<'/'<<get64(p.subspan(16))<<"\nLast device fault: "<<get64(p.subspan(24))<<"\nRead-only probe finished. No shutter test performed.\n";
        return status.clockReady?0:2;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
