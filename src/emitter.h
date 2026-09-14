#pragma once
#include "core.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
struct libusb_context;
struct libusb_device_handle;
namespace vision {
enum class EmitterState { Disconnected, Initializing, Ready, Running, Error, Simulated };
struct UsbDeviceInfo {
    uint16_t vid=0,pid=0;uint8_t bus=0,address=0;bool supported=false;std::string description;bool rp2040=false;
    // libusb cannot see a driverless Windows USB instance. Keep the PnP identity
    // so Connect can restore the already-installed WinUSB package after a port move.
    bool driverReady=true;std::wstring instanceId;bool descriptorFailed=false;
};
struct EmitterStatus { EmitterState state=EmitterState::Disconnected;std::string message="No emitter connected";uint64_t commands=0,errors=0,late=0;double lastTransferUs=0,maxTransferUs=0;bool scheduled=false,clockReady=false;double clockUncertaintyUs=0;uint64_t deviceOpens=0,deviceCloses=0;std::string identity="unassigned",firmwareVersion="unassigned";
    // Host-side timing of the eye command: how far from its deadline the USB write started
    // (signed, positive = late), the running mean transfer time, and how many timing-block
    // writes (phase/shutter changes) reached the emitter.
    double sendErrorLastUs=0,sendErrorMaxUs=0,sendErrorRmsUs=0,meanTransferUs=0;uint64_t timingWrites=0; };
class Emitter {
public:
    Emitter();~Emitter();
    std::vector<UsbDeviceInfo> discover();
    void recoverPort(const UsbDeviceInfo& info);
    void connect(const UsbDeviceInfo& info,const std::filesystem::path& firmware,bool simulated);
    void disconnect();
    void configure(const Settings& settings);
    void submit(Eye eye,double deadline);
    void suspend();
    EmitterStatus status()const;
private:
    struct Command { Eye eye;double deadline;uint64_t generation; };
    mutable std::mutex mutex_;std::condition_variable cv_;std::jthread worker_;
    EmitterStatus status_;Settings settings_;std::vector<Command> pending_;/* FIFO: the presenter queues frames ahead, so several eye commands can wait */uint64_t generation_=0;
    std::atomic<bool> suspended_{true};
    libusb_context* context_=nullptr;libusb_device_handle* handle_=nullptr;int interface_=-1;
    void run(std::stop_token stop,UsbDeviceInfo info,std::filesystem::path firmware,bool simulated);
    void runRp2040(std::stop_token stop,UsbDeviceInfo info);
    void setStatus(EmitterState state,const std::string& message);
    void transfer(std::span<const uint8_t> bytes,uint8_t endpoint,std::stop_token stop);
};
}
