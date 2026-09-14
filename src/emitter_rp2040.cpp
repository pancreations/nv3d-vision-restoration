// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter.h"
#include "rp2040_usb.h"
#include "platform.h"
#include <avrt.h>
#include <algorithm>
#include <cmath>
namespace vision {
void Emitter::runRp2040(std::stop_token stopToken,UsbDeviceInfo info) {
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    std::unique_ptr<rp2040::Link> link;std::unique_ptr<rp2040::Client> client;
    try {
        link=rp2040::openUsb(info.bus,info.address);client=std::make_unique<rp2040::Client>(*link,rp2040::hostMicroseconds);
        client->inspect(stopToken);client->stop(stopToken);
        {std::lock_guard lock(mutex_);status_.identity=link->identity();status_.firmwareVersion=link->firmwareVersion();}
        unsigned attempts=0;
        while(!stopToken.stop_requested()&&!client->status().clockReady&&attempts++<100){client->sampleClock(stopToken);Sleep(2);}
        if(!client->status().clockReady)throw std::runtime_error("RP2040 clock uncertainty is too high. Run vision_rp2040_probe --clock to collect USB timing before calibration.");
        setStatus(EmitterState::Ready,"RP2040 USB/clock ready - experimental IR; optical calibration pending");
        uint64_t activeGeneration=UINT64_MAX;double lastClock=qpc(),lastDiagnostic=qpc();
        while(!stopToken.stop_requested()) {
            std::optional<Command> command;Settings settings;uint64_t generation;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(2),[&]{return stopToken.stop_requested()||!pending_.empty();});
                if(!pending_.empty()){command=pending_.front();pending_.erase(pending_.begin());}settings=settings_;generation=generation_;}
            if(stopToken.stop_requested())break;
            if(suspended_||activeGeneration!=generation) {
                client->stop(stopToken);activeGeneration=generation;
            }
            if(command&&!suspended_&&command->generation==generation) {
                if(settings.sequence!=Sequence::Alternating||settings.refresh<119.5||settings.refresh>120.5)
                    throw std::runtime_error("RP2040 firmware currently supports fixed 120 Hz LR only");
                if(settings.leftUs<250||settings.rightUs<250)throw std::runtime_error("RP2040 openings must be at least 250 us to keep IR tokens separate");
                if(!client->status().session)client->start({uint32_t(std::llround(periodUs(settings.refresh))),uint32_t(std::llround(settings.leftUs)),uint32_t(std::llround(settings.rightUs))},stopToken);
                const double before=qpc();
                {std::lock_guard lock(mutex_);if(command->generation!=generation_||suspended_)continue;}
                // Deadline is a predicted future display refresh, not a USB arrival.
                const double target=command->deadline*1e6+wrapPhase(settings.phaseUs,periodUs(settings.refresh));
                client->schedule(command->eye==Eye::Left?rp2040::Eye::Left:rp2040::Eye::Right,target,stopToken);
                std::lock_guard lock(mutex_);++status_.commands;status_.lastTransferUs=(qpc()-before)*1e6;
                status_.maxTransferUs=std::max(status_.maxTransferUs,status_.lastTransferUs);status_.state=EmitterState::Running;
            }
            if(qpc()-lastClock>.005) {client->sampleClock(stopToken);lastClock=qpc();}
            if(qpc()-lastDiagnostic>.25) {
                auto d=client->diagnostics(stopToken);const auto p=std::span(d.payload);
                {std::lock_guard lock(mutex_);status_.deviceOpens=rp2040::get64(p.subspan(8));status_.deviceCloses=rp2040::get64(p.subspan(16));}
                if(client->status().session&&(d.payload[2]&1)==0)throw std::runtime_error("RP2040 stopped after a missed deadline or watchdog timeout; restart output after inspecting diagnostics");
                lastDiagnostic=qpc();
            }
            auto cs=client->status();{std::lock_guard lock(mutex_);status_.clockReady=cs.clockReady;status_.clockUncertaintyUs=cs.uncertaintyUs;}
        }
        client->stop();
    }catch(const std::exception& e) {
        if(client)try{client->stop();}catch(...){}
        std::lock_guard lock(mutex_);status_.state=EmitterState::Error;status_.message=e.what();++status_.errors;status_.clockReady=false;
    }
    if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
}
}
