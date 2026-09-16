// SPDX-License-Identifier: GPL-3.0-or-later
#include "emitter.h"
#include "rp2040_usb.h"
#include "rp2040_timing.h"
#include "lcd_rp2040.h"
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
        {std::lock_guard lock(mutex_);status_.identity=link->identity();status_.firmwareVersion=link->firmwareVersion();status_.extendedCadence=client->status().extendedCadence;status_.aperture=client->status().aperture;status_.fastCadence=client->status().fastCadence;}
        unsigned attempts=0;
        while(!stopToken.stop_requested()&&!client->status().clockReady&&attempts++<100){client->sampleClock(stopToken);Sleep(2);}
        if(!client->status().clockReady)throw std::runtime_error("RP2040 clock uncertainty is too high. Run vision_rp2040_probe --clock to collect USB timing before calibration.");
        {auto cs=client->status();std::lock_guard lock(mutex_);status_.clockReady=cs.clockReady;status_.clockUncertaintyUs=cs.uncertaintyUs;}
        setStatus(EmitterState::Ready,"RP2040 USB/clock ready - experimental IR; optical calibration pending");
        uint64_t activeGeneration=UINT64_MAX;double lastClock=qpc(),lastDiagnostic=qpc(),lastFramePeriod=0;
        std::optional<LcdTiming> appliedLcd;
        std::optional<rp2040::Config> appliedApertureConfig;
        while(!stopToken.stop_requested()) {
            std::optional<Command> command;Settings settings;uint64_t generation;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(2),[&]{return stopToken.stop_requested()||!pending_.empty();});
                if(lcdEditPending_&&qpc()-lcdEditAt_>=.060){
                    lcdEditPending_=false;
                    try{
                        // Validate against the measured device period BEFORE
                        // cancelling the currently working shutter session.
                        lcdDeviceConfig(settings_.lcd,lastFramePeriod>0?lastFramePeriod:periodUs(settings_.refresh),settings_.signalScanUs,1+sequenceBlack(settings_.sequence),sequenceHold(settings_.sequence)+sequenceBlack(settings_.sequence));
                        pending_.clear();++generation_;
                    }catch(const rp2040::InvalidTiming& e){status_.message=std::string("Keeping applied timing: ")+e.what();}
                }
                if(!pending_.empty()){command=pending_.front();pending_.erase(pending_.begin());}settings=settings_;generation=generation_;}
            if(command&&command->framePeriodUs>0)lastFramePeriod=command->framePeriodUs;
            if(stopToken.stop_requested())break;
            if(suspended_||activeGeneration!=generation) {
                client->stop(stopToken);activeGeneration=generation;
            }
            if(command&&!suspended_&&command->generation==generation) {
                if(!settings.lcd.enabled&&!rp2040TimingSupported(settings.refresh,settings.sequence,client->status().extendedCadence,client->status().fastCadence))
                    throw std::runtime_error(client->status().extendedCadence?"RP2040 needs 30-120 eye openings/s; choose a compatible repeated/black sequence":"Update RP2040 firmware for LCD preload; the connected build supports 120 Hz LR only");
                if(settings.leftUs<250||settings.rightUs<250)throw std::runtime_error("RP2040 openings must be at least 250 us to keep IR tokens separate");
                if(!client->status().session) {
                    if(settings.lcd.enabled) {
                        const double measuredPeriod=command->framePeriodUs>0?command->framePeriodUs:periodUs(settings.refresh);
                        rp2040::Config config;
                        auto makeConfig=[&](const LcdTiming& timing){return lcdDeviceConfig(timing,measuredPeriod,settings.signalScanUs,1+sequenceBlack(settings.sequence),sequenceHold(settings.sequence)+sequenceBlack(settings.sequence));};
                        try{config=makeConfig(settings.lcd);}
                        catch(const rp2040::InvalidTiming& e){
                            {std::lock_guard lock(mutex_);status_.state=EmitterState::Ready;status_.message=std::string("Timing not applied: ")+e.what();}
                            if(!appliedLcd)continue;
                            try{config=makeConfig(*appliedLcd);settings.lcd=*appliedLcd;}
                            catch(const rp2040::InvalidTiming&){continue;}
                        }
                        client->start(config,stopToken);appliedLcd=settings.lcd;appliedApertureConfig=config;
                        std::lock_guard lock(mutex_);status_.aperturePeriodUs=config.periodUs;status_.apertureDurationUs=config.leftUs;++status_.timingWrites;
                        status_.message="Shutter timing active";
                    } else client->start({uint32_t(std::llround(periodUs(sequenceEmitterHz(settings.refresh,settings.sequence)))),uint32_t(std::llround(settings.leftUs)),uint32_t(std::llround(settings.rightUs))},stopToken);
                }
                const double before=qpc();
                {std::lock_guard lock(mutex_);if(command->generation!=generation_||suspended_)continue;}
                // Deadline is a predicted future display refresh, not a USB arrival.
                const double openOffset=settings.lcd.enabled&&appliedApertureConfig&&!appliedApertureConfig->frameAnchored?
                    (command->eye==Eye::Right?appliedApertureConfig->rightOpenUs:appliedApertureConfig->leftOpenUs):0;
                const auto phase=settings.lcd.enabled?Rp2040Phase{openOffset,false}:client->status().extendedCadence?rp2040Phase(settings.phaseUs,settings.refresh,settings.sequence):Rp2040Phase{wrapPhase(settings.phaseUs,periodUs(settings.refresh)),false};
                const double target=command->deadline*1e6+phase.delayUs;
                // A long phase at low refresh can exceed the USB scheduling
                // horizon. Wait on the host, maintaining clock samples and stop
                // responsiveness, instead of queuing distant device events.
                bool stale=false;
                while(!stopToken.stop_requested()&&target-qpc()*1e6>30000) {
                    {std::lock_guard lock(mutex_);stale=command->generation!=generation_||suspended_;}
                    if(stale)break;
                    if(qpc()-lastClock>.005){client->sampleClock(stopToken);lastClock=qpc();}
                    Sleep(1);
                }
                {std::lock_guard lock(mutex_);stale|=command->generation!=generation_||suspended_;}
                if(stale||stopToken.stop_requested())continue;
                bool right=command->eye==Eye::Right;right^=phase.flipEye;
                try{client->schedule(right?rp2040::Eye::Right:rp2040::Eye::Left,target,stopToken);}
                catch(const rp2040::ScheduleMiss& miss){
                    client->stop(stopToken);suspend();
                    std::lock_guard lock(mutex_);++status_.late;status_.state=EmitterState::Ready;
                    status_.message=std::string("Timing missed; reacquiring: ")+miss.what();continue;
                }
                std::lock_guard lock(mutex_);++status_.commands;status_.lastTransferUs=(qpc()-before)*1e6;
                status_.maxTransferUs=std::max(status_.maxTransferUs,status_.lastTransferUs);status_.state=EmitterState::Running;
            }
            if(qpc()-lastClock>.005) {client->sampleClock(stopToken);lastClock=qpc();}
            if(qpc()-lastDiagnostic>.25) {
                auto d=client->diagnostics(stopToken);const auto p=std::span(d.payload);
                {std::lock_guard lock(mutex_);status_.deviceOpens=rp2040::get64(p.subspan(8));status_.deviceCloses=rp2040::get64(p.subspan(16));}
                if(client->status().session&&(d.payload[2]&1)==0){
                    client->stop(stopToken);suspend();
                    std::lock_guard lock(mutex_);++status_.late;status_.state=EmitterState::Ready;
                    status_.message="LCD timing reacquiring after a device deadline miss";
                }
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
