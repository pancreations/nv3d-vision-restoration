#include "emitter.h"
#include "platform.h"
#include "rp2040_usb.h"
#include <algorithm>
#include <avrt.h>
#include <chrono>
#include <cmath>
#include <cfgmgr32.h>
#include <iomanip>
#include <shellapi.h>
#include <setupapi.h>
#include <sstream>
#include <libusb.h>
namespace vision {
namespace {
struct PnpEmitter { uint16_t pid=0;std::wstring instance;std::string location;bool descriptorFailed=false; };
std::vector<PnpEmitter> pnpEmitters(bool presentOnly){
    DWORD flags=DIGCF_ALLCLASSES|(presentOnly?DIGCF_PRESENT:0);
    HDEVINFO devices=SetupDiGetClassDevsW(nullptr,L"USB",nullptr,flags);std::vector<PnpEmitter> result;
    if(devices==INVALID_HANDLE_VALUE)return result;
    for(DWORD index=0;;++index){
        SP_DEVINFO_DATA device{sizeof(device)};if(!SetupDiEnumDeviceInfo(devices,index,&device)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}
        wchar_t id[4096]{};if(!SetupDiGetDeviceInstanceIdW(devices,&device,id,DWORD(std::size(id)),nullptr))continue;
        std::wstring instance=id,upper=instance;std::transform(upper.begin(),upper.end(),upper.begin(),towupper);
        uint16_t pid=upper.rfind(L"USB\\VID_0955&PID_7003\\",0)==0?0x7003:upper.rfind(L"USB\\VID_0955&PID_0007\\",0)==0?0x0007:0;
        bool failed=upper.rfind(L"USB\\VID_0000&PID_0002\\",0)==0;
        if(failed){ULONG status=0,problem=0;if(CM_Get_DevNode_Status(&status,&problem,device.DevInst,0)!=CR_SUCCESS||problem!=CM_PROB_FAILED_POST_START)failed=false;}
        if(pid||failed){wchar_t where[512]{};DWORD type=0,bytes=sizeof(where);std::string location;
            if(SetupDiGetDeviceRegistryPropertyW(devices,&device,SPDRP_LOCATION_INFORMATION,&type,reinterpret_cast<PBYTE>(where),bytes,&bytes))location=utf8(where);
            result.push_back({pid,instance,location,failed});}
    }
    SetupDiDestroyDeviceInfoList(devices);return result;
}
void launchDriverRepair(uint16_t pid){
    if(pid!=0x7003&&pid!=0x0007)throw std::runtime_error("Refusing WinUSB installation for an unsupported device.");
    wchar_t executable[32768]{};DWORD length=GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));if(!length||length==std::size(executable))throw std::runtime_error("Cannot locate the portable WinUSB installer.");
    auto helper=std::filesystem::path(executable).parent_path()/L"vision_winusb_installer.exe";if(!std::filesystem::exists(helper))throw std::runtime_error("Portable WinUSB installer is missing from the application folder. Bind WinUSB to the emitter with Zadig instead.");
    auto destination=std::filesystem::temp_directory_path()/(L"VisionRestoration-WinUSB-"+std::to_wstring(pid));std::filesystem::create_directories(destination);
    std::wostringstream parameters;parameters<<L"--name \"NVIDIA stereo controller\" --manufacturer \"NVIDIA Corp.\" --vid 0x0955 --pid 0x"<<std::hex<<std::setw(4)<<std::setfill(L'0')<<pid<<L" --type 0 --inf VisionNvidiaEmitter.inf --dest \""<<destination.wstring()<<L"\" --silent";
    // libwdi generates and signs an exact VID/PID WinUSB package on the target
    // machine. No driver-store name, USB port, GPU or local device fingerprint is reused.
    auto parameterText=parameters.str();SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=helper.c_str();launch.lpParameters=parameterText.c_str();launch.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch)){
        if(GetLastError()==ERROR_CANCELLED)throw std::runtime_error("Administrator approval is required once per emitter identity on this PC to install WinUSB.");
        throw std::runtime_error("Could not start the portable WinUSB installer.");
    }
    DWORD wait=WaitForSingleObject(launch.hProcess,30000),exitCode=1;if(wait==WAIT_OBJECT_0)GetExitCodeProcess(launch.hProcess,&exitCode);CloseHandle(launch.hProcess);
    if(wait!=WAIT_OBJECT_0 || exitCode!=0)throw std::runtime_error("Portable WinUSB installation failed for the NVIDIA emitter.");
}

void restartBootDevice(const std::wstring& instance){
    wchar_t systemDir[MAX_PATH]{};UINT n=GetSystemDirectoryW(systemDir,MAX_PATH);
    if(!n || n>=MAX_PATH)throw std::runtime_error("Cannot locate Windows PnP restart utility.");
    std::wstring pnputil=std::wstring(systemDir)+L"\\pnputil.exe";
    std::wstring params=L"/restart-device \""+instance+L"\"";
    SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=pnputil.c_str();launch.lpParameters=params.c_str();launch.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch)){
        if(GetLastError()==ERROR_CANCELLED)throw std::runtime_error("Administrator approval was required to restart the emitter after firmware upload.");
        throw std::runtime_error("Could not request the Windows PnP restart for the emitter.");
    }
    DWORD wait=WaitForSingleObject(launch.hProcess,15000);DWORD exitCode=1;
    if(wait==WAIT_OBJECT_0)GetExitCodeProcess(launch.hProcess,&exitCode);
    CloseHandle(launch.hProcess);
    if(wait!=WAIT_OBJECT_0 || exitCode!=0)throw std::runtime_error("Windows could not restart the emitter after firmware upload.");
}
}
Emitter::Emitter(){int r=libusb_init(&context_);if(r<0)throw std::runtime_error("libusb initialization failed.");}
Emitter::~Emitter(){disconnect();libusb_exit(context_);}
std::vector<UsbDeviceInfo> Emitter::discover(){
    auto pnp=pnpEmitters(true);
    libusb_device** list=nullptr;auto count=libusb_get_device_list(context_,&list);if(count<0)throw std::runtime_error("USB enumeration failed.");
    std::vector<UsbDeviceInfo> out;
    for(ptrdiff_t i=0;i<count;i++){libusb_device_descriptor desc{};if(libusb_get_device_descriptor(list[i],&desc)<0 || desc.idVendor!=0x0955)continue;
        std::ostringstream text;text<<"NVIDIA USB "<<std::hex<<desc.idVendor<<":"<<desc.idProduct;
        // 0955:0007 is the runtime identity used by libnvstusb. 0955:7003 is what the
        // emitter enumerates as on this PC before RAM firmware is loaded (Windows bus
        // description "NVIDIA stereo controller"; NVIDIA's nvstusb.inf binds both IDs).
        // connect() still inspects the descriptors and only uploads firmware to a
        // boot interface with no endpoints; runtime commands go only to EP1/EP2.
        bool runtime=desc.idProduct==0x0007,boot=desc.idProduct==0x7003;bool supported=runtime||boot;
        UsbDeviceInfo info{desc.idVendor,desc.idProduct,libusb_get_bus_number(list[i]),libusb_get_device_address(list[i]),supported,text.str()+(runtime?" (IR emitter)":boot?" (IR emitter, boot state: firmware upload required)":" (unverified device; no writes)")};
        for(auto& device:pnp)if(device.pid==desc.idProduct){info.instanceId=device.instance;break;}out.push_back(std::move(info));
    }libusb_free_device_list(list,1);
    // A device with no usable driver is intentionally absent from libusb. Surface
    // it through Windows PnP so the UI can repair the new port instead of saying
    // that no emitter exists.
    for(auto& device:pnp){bool visible=false;for(auto& info:out)if(!info.rp2040&&info.pid==device.pid){visible=true;break;}if(visible)continue;
        if(device.descriptorFailed){UsbDeviceInfo info;info.driverReady=false;info.instanceId=device.instance;info.descriptorFailed=true;info.description="USB descriptor failure"+(device.location.empty()?std::string{}:" at "+device.location)+"; Windows cannot identify the emitter";out.push_back(std::move(info));continue;}
        std::ostringstream text;text<<"NVIDIA USB 0955:" <<std::hex<<std::setfill('0')<<std::setw(4)<<device.pid<<" (IR emitter detected; WinUSB will be repaired on Connect)";
        UsbDeviceInfo info{0x0955,device.pid,0,0,true,text.str()};info.driverReady=false;info.instanceId=device.instance;out.push_back(std::move(info));
    }
    for(auto& d:rp2040::discoverUsb())out.push_back({0xcafe,0x3d02,d.bus,d.address,true,d.label,true});
    return out;
}
void Emitter::recoverPort(const UsbDeviceInfo& info){
    if(!info.descriptorFailed||info.instanceId.empty())throw std::runtime_error("Refusing to reset a USB port that is not in descriptor-failure state.");
    wchar_t executable[32768]{};DWORD length=GetModuleFileNameW(nullptr,executable,DWORD(std::size(executable)));if(!length||length==std::size(executable))throw std::runtime_error("Cannot locate the USB recovery helper.");
    auto helper=std::filesystem::path(executable).parent_path()/L"vision_usb_recover.exe";if(!std::filesystem::exists(helper))throw std::runtime_error("USB recovery helper is missing from the application folder.");
    std::wstring parameters=L"\""+info.instanceId+L"\"";SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;launch.lpVerb=L"runas";launch.lpFile=helper.c_str();launch.lpParameters=parameters.c_str();launch.nShow=SW_HIDE;
    if(!ShellExecuteExW(&launch)){if(GetLastError()==ERROR_CANCELLED)throw std::runtime_error("Administrator approval is required to power-cycle the failed USB port.");throw std::runtime_error("Could not start USB port recovery.");}
    DWORD wait=WaitForSingleObject(launch.hProcess,20000),exitCode=1;if(wait==WAIT_OBJECT_0)GetExitCodeProcess(launch.hProcess,&exitCode);CloseHandle(launch.hProcess);
    if(wait!=WAIT_OBJECT_0)throw std::runtime_error("USB port recovery timed out.");if(exitCode!=0)throw std::runtime_error("Windows could not power-cycle the failed USB port (recovery code "+std::to_string(exitCode)+").");
}
void Emitter::setStatus(EmitterState state,const std::string& message){std::lock_guard lock(mutex_);status_.state=state;status_.message=message;}
EmitterStatus Emitter::status()const{std::lock_guard lock(mutex_);return status_;}
void Emitter::connect(const UsbDeviceInfo& info,const std::filesystem::path& fw,bool simulated){
    disconnect();if(!simulated && !info.supported)throw std::runtime_error("This USB model has no verified protocol profile; no writes performed.");
    {std::lock_guard lock(mutex_);status_={};status_.scheduled=info.rp2040&&!simulated;
        if(!status_.scheduled){status_.identity=simulated?"simulated":"nvidia:0955:0007";status_.firmwareVersion=simulated?"simulation-v1":"libnvstusb-runtime";}}
    setStatus(EmitterState::Initializing,"Opening emitter...");worker_=std::jthread([this,info,fw,simulated](std::stop_token s){run(s,info,fw,simulated);});
}
void Emitter::disconnect(){if(worker_.joinable()){worker_.request_stop();cv_.notify_all();worker_.join();}suspended_=true;std::lock_guard lock(mutex_);pending_.clear();++generation_;status_.state=EmitterState::Disconnected;status_.message="Emitter disconnected";}
void Emitter::configure(const Settings& s){validate(s);std::lock_guard lock(mutex_);
    // NVIDIA applies register changes live and keeps its queued eye trigger.
    // RP2040 fixes durations and cadence for a session: restart that session
    // when its timing changes, including phase/eye changes that would otherwise
    // violate the spacing or alternation of already-scheduled deadlines.
    if(status_.scheduled&&(s.refresh!=settings_.refresh||s.sequence!=settings_.sequence||
        s.phaseUs!=settings_.phaseUs||s.leftUs!=settings_.leftUs||s.rightUs!=settings_.rightUs||s.swapEyes!=settings_.swapEyes||!sameLcdAperture(s.lcd,settings_.lcd)||s.signalScanUs!=settings_.signalScanUs)){
        if(s.lcd.enabled&&settings_.lcd.enabled&&s.refresh==settings_.refresh&&s.sequence==settings_.sequence&&s.swapEyes==settings_.swapEyes){
            // A drag produces many intermediate values. Keep the current valid
            // device session running until the hand pauses for 60 ms, then apply
            // the latest aperture once. The picture never needs to be recreated.
            lcdEditPending_=true;lcdEditAt_=qpc();
        }else{lcdEditPending_=false;pending_.clear();++generation_;}
        cv_.notify_one();
    }
    settings_=s;
}
void Emitter::submit(Eye eye,double deadline,double framePeriodUs){if(eye==Eye::Black)return;std::lock_guard lock(mutex_);if(status_.state!=EmitterState::Ready && status_.state!=EmitterState::Running && status_.state!=EmitterState::Simulated)return;if(pending_.size()>=4){pending_.erase(pending_.begin());status_.late++;}pending_.push_back(Command{eye,deadline,generation_,framePeriodUs});suspended_=false;cv_.notify_one();}
void Emitter::suspend(){suspended_=true;std::lock_guard lock(mutex_);pending_.clear();++generation_;cv_.notify_one();}
void Emitter::transfer(std::span<const uint8_t> bytes,uint8_t endpoint,std::stop_token stop){
    struct Completion{bool done=false;libusb_transfer_status status{};int actual=0;};Completion done;
    auto* t=libusb_alloc_transfer(0);if(!t)throw std::runtime_error("USB allocation failed.");
    std::vector<uint8_t> buffer(bytes.begin(),bytes.end());
    libusb_fill_bulk_transfer(t,handle_,endpoint,buffer.data(),int(buffer.size()),[](libusb_transfer* x){auto* c=static_cast<Completion*>(x->user_data);c->status=x->status;c->actual=x->actual_length;c->done=true;},&done,20);
    int rc=libusb_submit_transfer(t);if(rc<0){libusb_free_transfer(t);throw std::runtime_error(std::string("USB submit: ")+libusb_error_name(rc));}
    bool cancel=false;while(!done.done){if(stop.stop_requested() && !cancel){libusb_cancel_transfer(t);cancel=true;}timeval timeout{0,1000};libusb_handle_events_timeout(context_,&timeout);}
    libusb_free_transfer(t);
    if(done.status!=LIBUSB_TRANSFER_COMPLETED || done.actual!=int(bytes.size()))throw std::runtime_error("USB transfer failed/timed out. Stop and reconnect the emitter; inspect WinUSB binding.");
}
void Emitter::run(std::stop_token stop,UsbDeviceInfo info,std::filesystem::path firmware,bool simulated){
    if(info.rp2040&&!simulated){runRp2040(stop,info);return;}
    DWORD task=0;HANDLE mmcss=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);if(!timer)timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
    bool initialized=false;
    try{
        if(!simulated){
            // Uploading RAM firmware changes the emitter's descriptors. Complete
            // the PnP restart and reconnect here so no physical USB port is baked
            // into the recovery path.
            for(;;){
            // Both the boot and runtime identities create port-specific devnodes.
            // Repair again after the firmware transition if Windows did not attach
            // the already-staged WinUSB package to the runtime devnode either.
            if(!info.driverReady){
                setStatus(EmitterState::Initializing,"Emitter found on a new USB port; requesting one-time WinUSB repair...");launchDriverRepair(info.pid);
                bool ready=false;for(int attempt=0;attempt<50&&!stop.stop_requested();++attempt){
                    for(auto& candidate:discover())if(!candidate.rp2040&&candidate.pid==info.pid&&candidate.driverReady){info=candidate;ready=true;break;}
                    if(ready)break;Sleep(100);
                }
                if(!ready)throw std::runtime_error("Windows repaired the driver, but the emitter did not become available. Unplug/replug it in the same port and press Refresh USB.");
            }
            libusb_device** devices=nullptr;auto count=libusb_get_device_list(context_,&devices);int openResult=LIBUSB_ERROR_NO_DEVICE;
            for(ptrdiff_t i=0;i<count;i++)if(libusb_get_bus_number(devices[i])==info.bus && libusb_get_device_address(devices[i])==info.address){openResult=libusb_open(devices[i],&handle_);break;}
            if(devices)libusb_free_device_list(devices,1);
            if(openResult<0)throw std::runtime_error(std::string("Cannot open emitter: ")+libusb_error_name(openResult)+". Bind WinUSB to this emitter only (see docs/EMITTER.md).");
            libusb_config_descriptor* config=nullptr;
            int rc=libusb_get_active_config_descriptor(libusb_get_device(handle_),&config);
            if(rc<0)rc=libusb_get_config_descriptor(libusb_get_device(handle_),0,&config);
            if(rc<0 || !config)throw std::runtime_error("Cannot inspect emitter interfaces.");
            bool boot=false,found=false;
            for(int i=0;i<config->bNumInterfaces;i++)for(int a=0;a<config->interface[i].num_altsetting;a++){
                auto& alt=config->interface[i].altsetting[a];if(alt.bNumEndpoints==0)boot=true;
                bool eye=false,cmd=false;
                for(int e=0;e<alt.bNumEndpoints;e++){auto& ep=alt.endpoint[e];if((ep.bmAttributes&3)!=LIBUSB_TRANSFER_TYPE_BULK)continue;eye|=ep.bEndpointAddress==0x01;cmd|=ep.bEndpointAddress==0x02;}
                if(eye&&cmd && alt.bAlternateSetting==0){found=true;interface_=alt.bInterfaceNumber;}
            }libusb_free_config_descriptor(config);
            if(!found && boot){
                if(firmware.empty())throw std::runtime_error("Emitter needs firmware. Select an extracted .fw file or use vision_firmware on a matching nvstusb.sys.");
                auto blocks=parseFirmware(readBinary(firmware));
                for(auto& b:blocks){if(stop.stop_requested())throw std::runtime_error("Firmware initialization cancelled. Unplug/replug emitter before retrying.");
                    int sent=libusb_control_transfer(handle_,0x40,0xA0,b.address,0,b.data.data(),uint16_t(b.data.size()),200);
                    if(sent!=int(b.data.size()))throw std::runtime_error("Firmware upload failed. Unplug/replug emitter; no persistent firmware was written.");
                }
                auto instance=info.instanceId;
                if(instance.empty())for(auto& device:pnpEmitters(true))if(device.pid==0x7003){instance=device.instance;break;}
                libusb_close(handle_);handle_=nullptr;interface_=-1;
                if(instance.empty())throw std::runtime_error("Firmware loaded, but Windows no longer exposes the emitter's boot device.");
                setStatus(EmitterState::Initializing,"Firmware loaded; restarting emitter automatically...");
                restartBootDevice(instance);
                bool runtime=false;
                for(int attempt=0;attempt<50 && !stop.stop_requested();attempt++){
                    for(auto& candidate:discover())if(!candidate.rp2040 && candidate.supported && candidate.pid==0x0007){info=candidate;runtime=true;break;}
                    if(runtime)break;
                    Sleep(100);
                }
                if(!runtime)throw std::runtime_error("Emitter did not re-enumerate as runtime device 0955:0007 after automatic restart.");
                continue;
            }
            if(!found)throw std::runtime_error("Emitter endpoints do not match the supported runtime protocol. No timing commands sent.");
            if(libusb_claim_interface(handle_,interface_)<0)throw std::runtime_error("Cannot claim emitter. Close other 3D software and verify WinUSB binding.");
            break;
            }
        }
        setStatus(simulated?EmitterState::Simulated:EmitterState::Ready,simulated?"SIMULATED emitter - no glasses connected":"USB ready - optical synchronization unverified");
        double lastDuration=-1,lastPhase=-1,lastRate=-1;bool wasSuspended=true,lastPerEye=false,lastSwap=false;double sendErrorSq_=0;
        auto rejectLate=[&]{
            // A late eye command is dropped, never sent into the wrong image. The firmware's shutters
            // free-run through that one period and the next on-time command corrects them. Disabling
            // the emitter and re-initializing here shut the glasses off and re-sent the whole setup on
            // every late command, which flashed the glasses whenever the output window was unfocused
            // behind the game (background scheduling makes commands late). Re-locking is manual only.
            std::lock_guard lock(mutex_);status_.late++;
        };
        while(!stop.stop_requested()){
            std::optional<Command> command;Settings settings;
            {std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop.stop_requested() || !pending_.empty() || (!wasSuspended && suspended_.load());});
                if(stop.stop_requested())break;if(!pending_.empty()){command=pending_.front();pending_.erase(pending_.begin());}settings=settings_;}
            if(suspended_){if(!wasSuspended && initialized && !simulated){std::array<uint8_t,5> disable{1,0x1b,1,0,0};transfer(disable,2,stop);initialized=false;}wasSuspended=true;continue;}
            wasSuspended=false;if(!command)continue;
            constexpr double maximumLateness=.0005;
            if(qpc()-command->deadline>maximumLateness){rejectLate();continue;}
            if(settings.lcd.enabled) {
                const double rate=command->framePeriodUs>0?1e6/command->framePeriodUs:settings.refresh;
                auto aperture=lcdExposure(settings.lcd,apertureWindowHz(rate,settings.sequence),settings.signalScanUs);
                if(!aperture.valid)throw std::runtime_error(aperture.message);
                settings.phaseUs=aperture.openUs[0];settings.rightOffsetUs=float(aperture.openUs[1]-aperture.openUs[0]);
                settings.leftUs=settings.rightUs=std::min(aperture.durationUs,nvidiaMaxShutterUs(sequenceEmitterHz(rate,settings.sequence)));
                {std::lock_guard lock(mutex_);status_.aperturePeriodUs=aperture.periodUs;status_.apertureDurationUs=settings.leftUs;}
            }
            double hz=sequenceEmitterHz(settings.refresh,settings.sequence);
            // The 28-byte block carries the left eye timing. With a right-eye offset or unequal
            // durations, X/Y are rewritten just before every eye command (after the previous
            // boundary, before this one), so each eye gets its own window. Each write puts the
            // firmware back into unclamped acquisition, which snaps the boundary to this command.
            double duration=settings.leftUs;
            double phase=settings.phaseUs;
            bool perEye=!simulated&&settings.sequence==Sequence::Alternating&&(settings.rightOffsetUs!=0||settings.leftUs!=settings.rightUs);bool wroteEye=false;
            auto writeEyeTiming=[&]{auto physicalEye=nvidiaCommandEye(command->eye,hz,phase);auto t=nvidiaEyeTiming(hz,phase,physicalEye,settings.leftUs,settings.rightUs,settings.rightOffsetUs);transfer(eyeTimingPacket(hz,t.delayUs,t.durationUs),2,stop);wroteEye=true;};
            // Configuration transfers are not eye triggers. Send changed X/Y timers while
            // there is still lead time, then wait for the predicted vblank before EP1.
            // Sending the 28-byte timing block at the deadline made slider movement delay
            // the eye packet and produced an apparent range-dependent loss of sync.
            if(!simulated && (duration!=lastDuration || phase!=lastPhase || hz!=lastRate || perEye!=lastPerEye || settings.swapEyes!=lastSwap || !initialized)){
                auto packet=timingPacket(hz,phase,duration);transfer(packet,2,stop);lastDuration=duration;lastPhase=phase;lastRate=hz;{std::lock_guard lock(mutex_);status_.timingWrites++;}
                if(!initialized){
                    std::array<uint8_t,6> counter{1,0x1c,2,0,2,0};transfer(counter,2,stop);
                    uint16_t timeout=uint16_t(hz*4);std::array<uint8_t,6> watchdog{1,0x1e,2,0,uint8_t(timeout),uint8_t(timeout>>8)};transfer(watchdog,2,stop);
                    std::array<uint8_t,5> enable{1,0x1b,1,0,7};transfer(enable,2,stop);initialized=true;
                }
            }
            lastPerEye=perEye;lastSwap=settings.swapEyes;
            // The emitter's X timer is the single source of NVIDIA phase.
            // High-resolution wait: Sleep(1) can overshoot by a whole scheduler tick (up to 15.6 ms),
            // which dropped ~30% of predictive triggers. Wait on a 100 ns waitable timer until ~0.7 ms
            // before the deadline, then spin.
            while(qpc()<command->deadline && !stop.stop_requested() && !suspended_){
                double remaining=command->deadline-qpc();
                if(perEye&&!wroteEye&&remaining<0.0006){writeEyeTiming();continue;}
                if(remaining>0.0012 && timer){LARGE_INTEGER due;due.QuadPart=-LONGLONG((remaining-0.0007)*1e7);if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,10);else Sleep(0);}
                else YieldProcessor();
            }
            {std::lock_guard lock(mutex_);if(command->generation!=generation_ || suspended_)continue;}
            // Configuration writes and scheduler stalls can cross the deadline after
            // the first check. Never send a stale eye command into the next image.
            if(stop.stop_requested())break;
            if(qpc()-command->deadline>maximumLateness){rejectLate();continue;}
            double start=qpc();
            if(!simulated){
                if(perEye&&!wroteEye)writeEyeTiming();
                if(qpc()-command->deadline>maximumLateness){rejectLate();continue;}
                transfer(eyePacket(command->eye,hz,phase),1,stop);
            }
            {std::lock_guard lock(mutex_);status_.commands++;status_.lastTransferUs=(qpc()-start)*1e6;status_.maxTransferUs=std::max(status_.maxTransferUs,status_.lastTransferUs);status_.state=simulated?EmitterState::Simulated:EmitterState::Running;
                // Command timing error: the write started this far after (positive) or before the
                // predicted vblank. This is host scheduling jitter that the emitter's phase lock sees.
                double error=(start-command->deadline)*1e6;status_.sendErrorLastUs=error;status_.sendErrorMaxUs=std::max(status_.sendErrorMaxUs,std::abs(error));
                sendErrorSq_=sendErrorSq_?sendErrorSq_*.98+error*error*.02:error*error;status_.sendErrorRmsUs=std::sqrt(sendErrorSq_);
                status_.meanTransferUs=status_.meanTransferUs?status_.meanTransferUs*.95+status_.lastTransferUs*.05:status_.lastTransferUs;}
        }
    }catch(const std::exception& e){std::lock_guard lock(mutex_);status_.state=EmitterState::Error;status_.message=e.what();status_.errors++;}
    // Fail-safe: always try to disable 3D before releasing the interface, even
    // when initialization, a phase update, or another USB transfer threw.
    if(handle_&&!simulated){unsigned char off[]{1,0x1b,1,0,0};bool disabled=false;for(int attempt=0;attempt<3;attempt++){int sent=0;if(libusb_bulk_transfer(handle_,2,off,5,&sent,50)==0&&sent==5){disabled=true;break;}Sleep(10);}if(disabled)Sleep(250);}
    if(handle_){if(interface_>=0)libusb_release_interface(handle_,interface_);libusb_close(handle_);handle_=nullptr;}interface_=-1;
    if(timer)CloseHandle(timer);if(mmcss)AvRevertMmThreadCharacteristics(mmcss);
}
}
